package main

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strings"
	"testing"

	"github.com/go-chi/chi/v5"
)

// The order must match ImageList in ce_cores/fdd/imagelist.cpp - the same cases as in ce_cores/tests/test_imagelist.cpp.

func TestFloppyImageNameLess(t *testing.T) {
	v := []string{"zork.st", "Arkanoid.st", "arkanoid.st", "B.st", "a.st", "ab.st", "A.st", "_x.st", "1.st"}
	sort.Slice(v, func(i, j int) bool { return floppyImageNameLess(v[i], v[j]) })

	expected := []string{"1.st", "_x.st", "A.st", "a.st", "ab.st", "Arkanoid.st", "arkanoid.st", "B.st", "zork.st"}
	if !reflect.DeepEqual(v, expected) {
		t.Fatalf("got %v, want %v", v, expected)
	}
}

func TestIsFloppyImageName(t *testing.T) {
	for name, want := range map[string]bool{
		"game.st": true, "GAME.ST": true, "disk 1.Msa": true, "a.b.st": true,
		"game.stx": false, "game.zip": false, "readme": false, ".st": false, "st": false, "game.ſt": false,
	} {
		if got := isFloppyImageName(name); got != want {
			t.Errorf("isFloppyImageName(%q) = %v, want %v", name, got, want)
		}
	}
}

func TestFloppyImageNames(t *testing.T) {
	dir := t.TempDir()
	for _, n := range []string{"Zork.st", "arkanoid.MSA", "readme.txt", "Barbarian.st"} {
		if err := os.WriteFile(filepath.Join(dir, n), nil, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.Mkdir(filepath.Join(dir, "dir.st"), 0o755); err != nil {
		t.Fatal(err)
	}
	links := map[string]string{"link.st": "Zork.st", "linkdir.st": "dir.st", "broken.st": "missing.st"}
	for link, target := range links {
		if err := os.Symlink(filepath.Join(dir, target), filepath.Join(dir, link)); err != nil {
			t.Fatal(err)
		}
	}

	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatal(err)
	}

	got := floppyImageNames(dir, entries)
	expected := []string{"arkanoid.MSA", "Barbarian.st", "link.st", "Zork.st"}
	if !reflect.DeepEqual(got, expected) {
		t.Fatalf("got %v, want %v", got, expected)
	}
}

// GET /fdd/{mac}/image/next|prev - from the inserted image, from FLOPPY_IMAGES_DIR without one or when the image's
// folder is gone
func TestFDDImageStep(t *testing.T) {
	settings, images := t.TempDir(), t.TempDir()
	for _, n := range []string{"b.st", "a.st", "c.msa", "readme.txt"} {
		if err := os.WriteFile(filepath.Join(images, n), nil, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	t.Setenv("SETTINGS_DIR", settings)
	t.Setenv("FLOPPY_IMAGES_DIR", images)
	t.Setenv("CMD_UDP_PORT", "") // don't send the insert to a running core

	s := &Server{}
	const mac = "aa:bb"
	step := func(current string, dir int) (int, string) {
		t.Helper()
		if err := s.writeFddImage(mac, current); err != nil {
			t.Fatal(err)
		}
		rc := chi.NewRouteContext()
		rc.URLParams.Add("mac", mac)
		r := httptest.NewRequest(http.MethodGet, "/fdd/"+mac+"/image/next", nil)
		r = r.WithContext(context.WithValue(r.Context(), chi.RouteCtxKey, rc))
		w := httptest.NewRecorder()
		s.handleGetFDDImageStep(w, r, dir)
		if w.Code != http.StatusOK {
			return w.Code, ""
		}
		var resp map[string]string
		if err := json.Unmarshal(w.Body.Bytes(), &resp); err != nil {
			t.Fatal(err)
		}
		return w.Code, resp["image"]
	}

	gone := filepath.Join(t.TempDir(), "moved", "x.st")
	for _, tc := range []struct {
		current string
		dir     int
		want    string
	}{
		{filepath.Join(images, "a.st"), +1, "b.st"},
		{filepath.Join(images, "c.msa"), +1, "a.st"}, // wraps
		{filepath.Join(images, "a.st"), -1, "c.msa"},
		{filepath.Join(images, "deleted.st"), +1, "a.st"}, // not in the folder any more - first / last
		{filepath.Join(images, "deleted.st"), -1, "c.msa"},
		{"", +1, "a.st"}, // no image - FLOPPY_IMAGES_DIR
		{"", -1, "c.msa"},
		{gone, +1, "a.st"}, // its folder is gone - FLOPPY_IMAGES_DIR
	} {
		code, got := step(tc.current, tc.dir)
		if code != http.StatusOK || got != filepath.Join(images, tc.want) {
			t.Errorf("step(%q, %d) = %d %q, want %q", tc.current, tc.dir, code, got, tc.want)
		}
	}

	t.Setenv("FLOPPY_IMAGES_DIR", "")
	if code, _ := step("", +1); code != http.StatusNotFound {
		t.Errorf("no image, no FLOPPY_IMAGES_DIR: %d, want 404", code)
	}
	if code, _ := step(gone, +1); code != http.StatusNotFound {
		t.Errorf("folder gone, no FLOPPY_IMAGES_DIR: %d, want 404", code)
	}
}

func TestFDDSettings(t *testing.T) {
	settings := t.TempDir()
	t.Setenv("SETTINGS_DIR", settings)
	t.Setenv("CMD_UDP_PORT", "") // don't send to a running core

	s := &Server{}
	const mac = "2c:cf:67:c4:b1:e5"
	deviceDir := filepath.Join(settings, "2ccf67c4b1e5")

	call := func(method, body string) (int, map[string]int) {
		t.Helper()
		rc := chi.NewRouteContext()
		rc.URLParams.Add("mac", mac)
		r := httptest.NewRequest(method, "/fdd/"+mac+"/settings", strings.NewReader(body))
		r = r.WithContext(context.WithValue(r.Context(), chi.RouteCtxKey, rc))
		w := httptest.NewRecorder()
		if method == http.MethodGet {
			s.handleGetFDDSettings(w, r)
		} else {
			s.handlePutFDDSettings(w, r)
		}
		var resp map[string]int
		if method == http.MethodGet {
			if err := json.Unmarshal(w.Body.Bytes(), &resp); err != nil {
				t.Fatal(err)
			}
		}
		return w.Code, resp
	}
	file := func(name string) string {
		t.Helper()
		b, err := os.ReadFile(filepath.Join(deviceDir, name))
		if err != nil {
			return "<missing>"
		}
		return strings.TrimSpace(string(b))
	}

	// nothing stored yet - both on, as the device defaults
	if code, got := call(http.MethodGet, ""); code != http.StatusOK || got["beep_on_step"] != 1 || got["ikbd_enabled"] != 1 {
		t.Fatalf("defaults: %d %v", code, got)
	}

	// one setting changed - the other one isn't touched, marked pending for the core
	if code, _ := call(http.MethodPut, `{"beep_on_step":0}`); code != http.StatusNoContent {
		t.Fatalf("put beep: %d", code)
	}
	if file(fddSettingBeepOnStep) != "0" || file(fddSettingIkbdEnabled) != "<missing>" || file(fddSettingPending) != "1" {
		t.Errorf("after put beep: beep %s, ikbd %s, pending %s", file(fddSettingBeepOnStep), file(fddSettingIkbdEnabled), file(fddSettingPending))
	}
	if _, got := call(http.MethodGet, ""); got["beep_on_step"] != 0 || got["ikbd_enabled"] != 1 {
		t.Errorf("get after put beep: %v", got)
	}

	if code, _ := call(http.MethodPut, `{"ikbd_enabled":0}`); code != http.StatusNoContent {
		t.Fatalf("put ikbd: %d", code)
	}
	if _, got := call(http.MethodGet, ""); got["beep_on_step"] != 0 || got["ikbd_enabled"] != 0 {
		t.Errorf("get after put ikbd: %v", got)
	}

	// the core writes 0 / 1 with a newline
	if err := os.WriteFile(filepath.Join(deviceDir, fddSettingBeepOnStep), []byte("1\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, got := call(http.MethodGet, ""); got["beep_on_step"] != 1 {
		t.Errorf("value written by the core: %v", got)
	}

	for _, bad := range []string{`{}`, `{"beep_on_step":2}`, `{"ikbd_enabled":-1}`, `not json`} {
		if code, _ := call(http.MethodPut, bad); code != http.StatusBadRequest {
			t.Errorf("put %s: %d, want 400", bad, code)
		}
	}
}

func TestEmptyFloppyImageContent(t *testing.T) {
	for _, tracks := range []int{80, 81} {
		for _, sectors := range []int{8, 9, 10, 11} {
			for _, sides := range []int{1, 2} {
				image, err := makeEmptyFloppyImage(tracks, sectors, sides)
				if err != nil {
					t.Fatal(err)
				}

				total := tracks * sectors * sides
				word := func(at int) int { return int(image[at]) | int(image[at+1])<<8 }

				if len(image) != total*512 || word(11) != 512 || image[13] != 2 || word(14) != 1 || image[16] != 2 ||
					word(17) != 112 || word(19) != total || word(22) != 5 || word(24) != sectors || word(26) != sides {
					t.Errorf("%d/%d/%d: bad boot sector % x", tracks, sectors, sides, image[:30])
				}
				if floppyBootSectorSum(image) == 0x1234 {
					t.Errorf("%d/%d/%d: boot sector is executable", tracks, sectors, sides)
				}

				media := byte(0xF8 + sides - 1)
				if image[21] != media {
					t.Errorf("%d/%d/%d: media %x", tracks, sectors, sides, image[21])
				}
				for _, fat := range []int{512, 512 + 5*512} {
					if image[fat] != media || image[fat+1] != 0xFF || image[fat+2] != 0xFF {
						t.Errorf("%d/%d/%d: FAT at %d: % x", tracks, sectors, sides, fat, image[fat:fat+3])
					}
				}

				for i, val := range image[1536*4:] { // after the FATs: nothing, the root dir and the data are empty
					if val != 0 {
						t.Fatalf("%d/%d/%d: byte %d is %x", tracks, sectors, sides, 1536*4+i, val)
					}
				}
			}
		}
	}

	a, _ := makeEmptyFloppyImage(80, 9, 2)
	b, _ := makeEmptyFloppyImage(80, 9, 2)
	if string(a[8:11]) == string(b[8:11]) && string(a[8:11]) == "\x00\x00\x00" {
		t.Error("serial numbers aren't random")
	}
}

func TestCreateEmptyFloppyImage(t *testing.T) {
	settings := t.TempDir()
	images := t.TempDir()
	t.Setenv("SETTINGS_DIR", settings)
	t.Setenv("CMD_UDP_PORT", "") // don't send to a running core

	s := &Server{}
	const mac = "2ccf67c4b1e5"

	create := func(body map[string]interface{}) (int, string) {
		t.Helper()
		data, _ := json.Marshal(body)
		rc := chi.NewRouteContext()
		rc.URLParams.Add("mac", mac)
		r := httptest.NewRequest(http.MethodPost, "/fdd/"+mac+"/image/new", strings.NewReader(string(data)))
		r = r.WithContext(context.WithValue(r.Context(), chi.RouteCtxKey, rc))
		w := httptest.NewRecorder()
		s.handlePostFDDImageNew(w, r)
		return w.Code, w.Body.String()
	}
	request := func(name string, insert bool) map[string]interface{} {
		return map[string]interface{}{"dir": images, "name": name, "tracks": 80, "sectors": 9, "sides": 2, "insert": insert}
	}

	// not inserted: file created, what's inserted stays
	if err := s.writeFddImage(mac, "/old/image.st"); err != nil {
		t.Fatal(err)
	}
	if code, body := create(request("first", false)); code != http.StatusOK {
		t.Fatalf("create: %d %s", code, body)
	}
	if info, err := os.Stat(filepath.Join(images, "first.st")); err != nil || info.Size() != 737280 {
		t.Fatalf("file: %v %v", info, err)
	}
	if current, _ := s.readFddImage(mac); current != "/old/image.st" {
		t.Errorf("image inserted: %q", current)
	}

	// inserted, name already with the extension
	if code, body := create(request("Second.ST", true)); code != http.StatusOK {
		t.Fatalf("create: %d %s", code, body)
	}
	if current, _ := s.readFddImage(mac); current != filepath.Join(images, "Second.ST") {
		t.Errorf("not inserted: %q", current)
	}

	// an existing file stays as it is
	existing := filepath.Join(images, "games.st")
	if err := os.WriteFile(existing, []byte("my game"), 0o644); err != nil {
		t.Fatal(err)
	}
	if code, _ := create(request("games.st", true)); code != http.StatusConflict {
		t.Errorf("existing file: %d, want 409", code)
	}
	if content, _ := os.ReadFile(existing); string(content) != "my game" {
		t.Errorf("existing file changed: %q", content)
	}
	if current, _ := s.readFddImage(mac); current != filepath.Join(images, "Second.ST") {
		t.Errorf("failed create changed the inserted image: %q", current)
	}

	// asked and confirmed: the file gets replaced, the access rights stay
	if err := os.Chmod(existing, 0o600); err != nil {
		t.Fatal(err)
	}
	overwrite := request("games.st", false)
	overwrite["overwrite"] = true
	overwrite["sides"] = 1
	if code, body := create(overwrite); code != http.StatusOK {
		t.Fatalf("overwrite: %d %s", code, body)
	}
	info, err := os.Stat(existing)
	if err != nil || info.Size() != 368640 || info.Mode().Perm() != 0o600 {
		t.Errorf("overwritten file: %v %v", info, err)
	}
	if current, _ := s.readFddImage(mac); current != filepath.Join(images, "Second.ST") {
		t.Errorf("overwrite without insert changed the inserted image: %q", current)
	}

	// overwriting the inserted image: stays inserted
	overwrite = request("Second.ST", false)
	overwrite["overwrite"] = true
	overwrite["sectors"] = 10
	if code, body := create(overwrite); code != http.StatusOK {
		t.Fatalf("overwrite inserted: %d %s", code, body)
	}
	if info, err := os.Stat(filepath.Join(images, "Second.ST")); err != nil || info.Size() != 819200 {
		t.Errorf("overwritten inserted file: %v %v", info, err)
	}
	if current, _ := s.readFddImage(mac); current != filepath.Join(images, "Second.ST") {
		t.Errorf("not inserted anymore: %q", current)
	}

	// a symlink stays, the file it points to is replaced
	if err := os.Symlink(existing, filepath.Join(images, "link.st")); err != nil {
		t.Fatal(err)
	}
	overwrite = request("link.st", false)
	overwrite["overwrite"] = true
	if code, body := create(overwrite); code != http.StatusOK {
		t.Fatalf("overwrite link: %d %s", code, body)
	}
	if info, err := os.Lstat(filepath.Join(images, "link.st")); err != nil || info.Mode()&os.ModeSymlink == 0 {
		t.Errorf("symlink gone: %v %v", info, err)
	}
	if info, err := os.Stat(existing); err != nil || info.Size() != 737280 {
		t.Errorf("file behind the symlink: %v %v", info, err)
	}

	// a folder can't be overwritten
	if err := os.Mkdir(filepath.Join(images, "folder.st"), 0o755); err != nil {
		t.Fatal(err)
	}
	overwrite = request("folder.st", false)
	overwrite["overwrite"] = true
	if code, _ := create(overwrite); code == http.StatusOK {
		t.Errorf("folder overwritten")
	}

	// no temp files left
	if names, _ := filepath.Glob(filepath.Join(images, ".new_floppy_*")); len(names) != 0 {
		t.Errorf("left: %v", names)
	}

	for _, name := range []string{"", "  ", ".", "..", "../up", "a/b", `a\b`, "c:x", strings.Repeat("x", 300)} {
		if code, _ := create(request(name, false)); code != http.StatusBadRequest {
			t.Errorf("name %q: %d, want 400", name, code)
		}
	}
	if _, err := os.Stat(filepath.Join(filepath.Dir(images), "up.st")); err == nil {
		t.Error("file created out of the folder")
	}

	for _, tc := range []struct{ tracks, sectors, sides int }{{79, 9, 2}, {82, 9, 2}, {80, 7, 2}, {80, 12, 2}, {80, 9, 0}, {80, 9, 3}} {
		body := request("bad", false)
		body["tracks"], body["sectors"], body["sides"] = tc.tracks, tc.sectors, tc.sides
		if code, _ := create(body); code != http.StatusBadRequest {
			t.Errorf("geometry %v: %d, want 400", tc, code)
		}
	}

	body := request("x", false)
	body["dir"] = filepath.Join(images, "no such folder")
	if code, _ := create(body); code != http.StatusBadRequest {
		t.Errorf("missing folder: %d, want 400", code)
	}
	body["dir"] = existing
	if code, _ := create(body); code != http.StatusBadRequest {
		t.Errorf("folder is a file: %d, want 400", code)
	}
}
