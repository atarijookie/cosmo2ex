package main

import (
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

const appTestMAC = "AABBCCDDEE01"

// appTestEnv creates SETTINGS_DIR with one FDD device + FLOPPY_IMAGES_DIR, and returns the server and the app token.
func appTestEnv(t *testing.T) (*Server, string, string) {
	t.Helper()
	root := t.TempDir()
	settings := filepath.Join(root, "settings")
	images := filepath.Join(root, "images")
	for _, d := range []string{filepath.Join(settings, appTestMAC), filepath.Join(images, "sub")} {
		if err := os.MkdirAll(d, 0o755); err != nil {
			t.Fatal(err)
		}
	}
	for _, f := range []string{"b.st", "A.msa", "c.stx", "readme.txt", "sub/d.st"} {
		if err := os.WriteFile(filepath.Join(images, f), []byte("x"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	t.Setenv("SETTINGS_DIR", settings)
	t.Setenv("FLOPPY_IMAGES_DIR", images)
	t.Setenv("CMD_UDP_PORT", "")

	token, err := appTokenForPairing()
	if err != nil {
		t.Fatal(err)
	}
	return newServer(), token, images
}

func appRequest(s *Server, method, target, token, body string) *httptest.ResponseRecorder {
	req := httptest.NewRequest(method, target, strings.NewReader(body))
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	rec := httptest.NewRecorder()
	s.router.ServeHTTP(rec, req)
	return rec
}

func TestAppTokenLimitedToAppRoutes(t *testing.T) {
	s, token, _ := appTestEnv(t)

	if rec := appRequest(s, "GET", "/app/v1/state", "", ""); rec.Code != http.StatusUnauthorized {
		t.Fatalf("no token: got %d", rec.Code)
	}
	if rec := appRequest(s, "GET", "/app/v1/state", "wrong", ""); rec.Code != http.StatusUnauthorized {
		t.Fatalf("wrong token: got %d", rec.Code)
	}
	if rec := appRequest(s, "GET", "/app/v1/state", token, ""); rec.Code != http.StatusOK {
		t.Fatalf("good token: got %d", rec.Code)
	}
	// the app token is not a session of the web UI
	for _, path := range []string{"/host/dir?path=/", "/devices", "/app/pair", "/settings/logs"} {
		if rec := appRequest(s, "GET", path, token, ""); rec.Code != http.StatusFound {
			t.Errorf("%s with app token: got %d, expected redirect to login", path, rec.Code)
		}
	}
	// and a session of the web UI is not the app token
	s.authTokens["session"] = struct{}{}
	if rec := appRequest(s, "GET", "/app/v1/state", "session", ""); rec.Code != http.StatusUnauthorized {
		t.Fatalf("web session on app route: got %d", rec.Code)
	}
}

func TestAppTokenSurvivesAndResets(t *testing.T) {
	_, token, _ := appTestEnv(t)
	again, err := appTokenForPairing()
	if err != nil || again != token {
		t.Fatalf("token changed: %q, %v", again, err)
	}
	if strings.ContainsAny(token, "+/=") {
		t.Fatalf("token should not need escaping in a link: %q", token)
	}
	info, err := os.Stat(appTokenPath())
	if err != nil || info.Mode().Perm() != 0o600 {
		t.Fatalf("token file: %v, %v", info, err)
	}

	s := newServer()
	s.authTokens["session"] = struct{}{}
	if err := storePassword("secret"); err != nil {
		t.Fatal(err)
	}
	if rec := appRequest(s, "POST", "/app/pair/reset", "session", ""); rec.Code != http.StatusNoContent {
		t.Fatalf("reset: got %d", rec.Code)
	}
	if rec := appRequest(s, "GET", "/app/v1/state", token, ""); rec.Code != http.StatusUnauthorized {
		t.Fatalf("old token after reset: got %d", rec.Code)
	}
}

func TestAppStateOnlyFDDDevices(t *testing.T) {
	s, token, images := appTestEnv(t)
	settings := os.Getenv("SETTINGS_DIR")

	noFDD := filepath.Join(settings, "AABBCCDDEE02")
	if err := os.MkdirAll(noFDD, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(noFDD, "features"), []byte("AS"), 0o644); err != nil {
		t.Fatal(err)
	}
	image := filepath.Join(images, "b.st")
	if err := s.writeFddImage(appTestMAC, image); err != nil {
		t.Fatal(err)
	}

	var got struct {
		Devices []appDevice `json:"devices"`
	}
	rec := appRequest(s, "GET", "/app/v1/state", token, "")
	if err := json.Unmarshal(rec.Body.Bytes(), &got); err != nil {
		t.Fatal(err)
	}
	want := []appDevice{{MAC: appTestMAC, Name: appTestMAC, Image: image, ImageName: "b.st", Dir: images}}
	if !reflect.DeepEqual(got.Devices, want) {
		t.Fatalf("got %+v, want %+v", got.Devices, want)
	}
}

func TestAppImages(t *testing.T) {
	s, token, images := appTestEnv(t)

	var got struct {
		Dir    string        `json:"dir"`
		Parent string        `json:"parent"`
		Dirs   []appDirEntry `json:"dirs"`
		Images []string      `json:"images"`
	}
	rec := appRequest(s, "GET", "/app/v1/images?mac="+appTestMAC, token, "")
	if err := json.Unmarshal(rec.Body.Bytes(), &got); err != nil {
		t.Fatalf("%v: %s", err, rec.Body.String())
	}
	if got.Dir != images || got.Parent != filepath.Dir(images) || !reflect.DeepEqual(got.Images, []string{"A.msa", "b.st", "c.stx"}) {
		t.Fatalf("got %+v", got)
	}
	if !reflect.DeepEqual(got.Dirs, []appDirEntry{{Name: "sub", Path: filepath.Join(images, "sub")}}) {
		t.Fatalf("got dirs %+v", got.Dirs)
	}

	rec = appRequest(s, "GET", "/app/v1/images?mac="+appTestMAC+"&dir="+url.QueryEscape(filepath.Join(images, "sub")), token, "")
	got.Images = nil
	if err := json.Unmarshal(rec.Body.Bytes(), &got); err != nil {
		t.Fatal(err)
	}
	if got.Parent != images || !reflect.DeepEqual(got.Images, []string{"d.st"}) {
		t.Fatalf("sub folder: got %+v", got)
	}

	// going above FLOPPY_IMAGES_DIR works - and shows only folders and floppy images, up to the root
	rec = appRequest(s, "GET", "/app/v1/images?mac="+appTestMAC+"&dir="+url.QueryEscape(images+"/.."), token, "")
	got.Images = nil
	if err := json.Unmarshal(rec.Body.Bytes(), &got); err != nil {
		t.Fatal(err)
	}
	if got.Dir != filepath.Dir(images) || len(got.Images) != 0 {
		t.Fatalf("parent folder: got %+v", got)
	}
	rec = appRequest(s, "GET", "/app/v1/images?mac="+appTestMAC+"&dir=/", token, "")
	got.Parent = "not set"
	if err := json.Unmarshal(rec.Body.Bytes(), &got); err != nil {
		t.Fatal(err)
	}
	if got.Parent != "" {
		t.Fatalf("root has a parent: %q", got.Parent)
	}

	rec = appRequest(s, "GET", "/app/v1/images?mac="+appTestMAC+"&dir=relative", token, "")
	if rec.Code != http.StatusBadRequest {
		t.Errorf("relative dir: got %d", rec.Code)
	}
	// only what's a floppy image is listed, whatever else is in the folder
	if strings.Contains(appRequest(s, "GET", "/app/v1/images?mac="+appTestMAC, token, "").Body.String(), "readme.txt") {
		t.Errorf("non image file listed")
	}
	for _, mac := range []string{"..", "000000000000", "AABBCCDDEE0G", "../settings"} {
		rec := appRequest(s, "GET", "/app/v1/images?mac="+url.QueryEscape(mac), token, "")
		if rec.Code != http.StatusNotFound {
			t.Errorf("mac %q: got %d", mac, rec.Code)
		}
	}
}

// the image of a device can be outside of FLOPPY_IMAGES_DIR - browsing starts in its folder
func TestAppImagesFolderOfCurrentImage(t *testing.T) {
	s, token, _ := appTestEnv(t)
	other := filepath.Join(t.TempDir(), "other")
	if err := os.MkdirAll(other, 0o755); err != nil {
		t.Fatal(err)
	}
	image := filepath.Join(other, "x.st")
	if err := os.WriteFile(image, []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := s.writeFddImage(appTestMAC, image); err != nil {
		t.Fatal(err)
	}

	var got struct {
		Dir    string   `json:"dir"`
		Parent string   `json:"parent"`
		Images []string `json:"images"`
	}
	rec := appRequest(s, "GET", "/app/v1/images?mac="+appTestMAC, token, "")
	if err := json.Unmarshal(rec.Body.Bytes(), &got); err != nil {
		t.Fatal(err)
	}
	if got.Dir != other || got.Parent != filepath.Dir(other) || !reflect.DeepEqual(got.Images, []string{"x.st"}) {
		t.Fatalf("got %+v", got)
	}
}

func TestAppInsertRejectsBadRequests(t *testing.T) {
	s, token, images := appTestEnv(t)
	before, _ := s.readFddImage(appTestMAC)

	cases := []struct {
		mac, dir, name string
		code           int
	}{
		{"000000000000", images, "b.st", http.StatusNotFound},
		{appTestMAC, "/etc", "passwd", http.StatusBadRequest},
		{appTestMAC, "/etc", "missing.st", http.StatusNotFound},
		{appTestMAC, "relative", "b.st", http.StatusBadRequest},
		{appTestMAC, images, "readme.txt", http.StatusBadRequest},
		{appTestMAC, images, "../../x.st", http.StatusBadRequest},
		{appTestMAC, images, "missing.st", http.StatusNotFound},
	}
	for _, c := range cases {
		body, _ := json.Marshal(map[string]string{"mac": c.mac, "dir": c.dir, "name": c.name})
		if rec := appRequest(s, "POST", "/app/v1/insert", token, string(body)); rec.Code != c.code {
			t.Errorf("%+v: got %d", c, rec.Code)
		}
	}
	if after, _ := s.readFddImage(appTestMAC); after != before {
		t.Fatalf("image changed to %q", after)
	}
}

func TestAppInsertStoresImageAndTellsCore(t *testing.T) {
	s, token, images := appTestEnv(t)

	// ce_cores is not reachable - image stays stored for the next start, but the app is told
	body, _ := json.Marshal(map[string]string{"mac": appTestMAC, "dir": images, "name": "c.stx"})
	if rec := appRequest(s, "POST", "/app/v1/insert", token, string(body)); rec.Code != http.StatusBadGateway {
		t.Fatalf("got %d", rec.Code)
	}
	if got, _ := s.readFddImage(appTestMAC); got != filepath.Join(images, "c.stx") {
		t.Fatalf("stored %q", got)
	}
}

func TestOrderAppAddresses(t *testing.T) {
	ips := []net.IP{net.ParseIP("172.17.0.1"), net.ParseIP("192.168.1.5"), net.ParseIP("10.0.0.2")}
	cases := []struct {
		host string
		want []string
	}{
		{"192.168.1.5", []string{"192.168.1.5", "10.0.0.2", "172.17.0.1"}},
		{"atari.lan", []string{"atari.lan", "192.168.1.5", "10.0.0.2", "172.17.0.1"}},
		{"localhost", []string{"192.168.1.5", "10.0.0.2", "172.17.0.1"}},
		{"127.0.0.1", []string{"192.168.1.5", "10.0.0.2", "172.17.0.1"}},
		{"::1", []string{"192.168.1.5", "10.0.0.2", "172.17.0.1"}},
	}
	for _, c := range cases {
		if got := orderAppAddresses(c.host, ips); !reflect.DeepEqual(got, c.want) {
			t.Errorf("%s: got %v, want %v", c.host, got, c.want)
		}
	}
}
