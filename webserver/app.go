package main

import (
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"errors"
	"fmt"
	"net"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"

	"github.com/skip2/go-qrcode"
)

// Backend of the Android floppy app (android/), and its pairing.
//
// The app is paired by scanning a QR code on android_app.html (or tapping the same link on the phone), which holds the
// addresses + port of this server, the app token and the fingerprint of the local CA from tls.go. The app trusts only that
// CA, so nothing has to be installed on the phone. IP or token changed? Just scan the code again.
//
// The app token survives restarts (the sessions of the web UI don't) and it allows only /app/v1/*, which is limited to
// choosing floppy images. Folders can be browsed anywhere like in the file selector of the web UI, but it shows only
// the names of sub folders and floppy images, and nothing else can be inserted or read.

const (
	appTokenFileName = "app_token"
	appAPKFile       = "static/app/ce_floppy.apk"
	appPairScheme    = "cosmosex://pair"
)

var appTokenMu sync.Mutex

func appTokenPath() string {
	return filepath.Join(getSettingsDir(), appTokenFileName)
}

// loadAppToken returns the stored app token, "" if there's none yet.
func loadAppToken() string {
	content, err := os.ReadFile(appTokenPath())
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(content))
}

// storeNewAppToken creates a new token and stores it (via temp file + rename, so it's never half-written).
func storeNewAppToken() (string, error) {
	var buf [32]byte
	if _, err := rand.Read(buf[:]); err != nil {
		return "", err
	}
	token := base64.RawURLEncoding.EncodeToString(buf[:])

	path := appTokenPath()
	tmpPath := path + ".tmp"
	if err := os.WriteFile(tmpPath, []byte(token+"\n"), 0o600); err != nil {
		return "", err
	}
	if err := os.Rename(tmpPath, path); err != nil {
		return "", err
	}
	return token, nil
}

// appTokenForPairing returns the token to put in the QR code, it's created when it's needed for the first time.
func appTokenForPairing() (string, error) {
	appTokenMu.Lock()
	defer appTokenMu.Unlock()
	if token := loadAppToken(); token != "" {
		return token, nil
	}
	return storeNewAppToken()
}

// appAuthMiddleware lets through only requests with the app token. It answers 401 and not a redirect to the login page
// like authMiddleware, so the app can tell the user to pair again.
func appAuthMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		got := parseBearerToken(r.Header.Get("Authorization"))
		want := loadAppToken()
		if got == "" || want == "" || subtle.ConstantTimeCompare([]byte(got), []byte(want)) != 1 {
			logWarningf("appAuthMiddleware - invalid app token for path %s", r.URL.Path)
			http.Error(w, "invalid token", http.StatusUnauthorized)
			return
		}
		next.ServeHTTP(w, r)
	})
}

// caFingerprint returns SHA-256 of the local CA certificate (of its DER form), in hex.
func caFingerprint() (string, error) {
	content, err := os.ReadFile(caCertPath())
	if err != nil {
		return "", err
	}
	block, _ := pem.Decode(content)
	if block == nil {
		return "", errors.New("CA certificate is not in PEM format")
	}
	sum := sha256.Sum256(block.Bytes)
	return hex.EncodeToString(sum[:]), nil
}

// orderAppAddresses puts the addresses in the order the app should try them: first the one the user reached this server at,
// which is known to work, unless it's a loopback one - then the local IPs, the likely LAN ones first, as 172.16/12 is
// often only a docker bridge. Duplicates are removed.
func orderAppAddresses(requestHost string, ips []net.IP) []string {
	result := make([]string, 0, len(ips)+1)
	seen := map[string]bool{}
	add := func(a string) {
		if a != "" && !seen[a] {
			seen[a] = true
			result = append(result, a)
		}
	}

	if ip := net.ParseIP(requestHost); requestHost != "localhost" && (ip == nil || !ip.IsLoopback()) {
		add(requestHost)
	}

	rank := func(ip net.IP) int {
		if v4 := ip.To4(); v4 != nil && v4[0] == 172 && v4[1] >= 16 && v4[1] < 32 {
			return 1
		}
		return 0
	}
	sorted := append([]net.IP(nil), ips...)
	sort.SliceStable(sorted, func(i, j int) bool { return rank(sorted[i]) < rank(sorted[j]) })
	for _, ip := range sorted {
		add(ip.String())
	}
	return result
}

// localIPv4s returns the IPv4 addresses of this machine, without loopback and link-local ones.
func localIPv4s() []net.IP {
	ips := []net.IP{}
	addrs, err := net.InterfaceAddrs()
	if err != nil {
		return ips
	}
	for _, addr := range addrs {
		ipnet, ok := addr.(*net.IPNet)
		if !ok || ipnet.IP.To4() == nil || ipnet.IP.IsLoopback() || ipnet.IP.IsLinkLocalUnicast() {
			continue
		}
		ips = append(ips, ipnet.IP)
	}
	return ips
}

// appPairURI creates the link which is in the QR code, based on how this request reached the server.
func appPairURI(r *http.Request) (string, error) {
	token, err := appTokenForPairing()
	if err != nil {
		return "", fmt.Errorf("app token: %w", err)
	}
	fingerprint, err := caFingerprint()
	if err != nil {
		return "", fmt.Errorf("CA fingerprint: %w", err)
	}

	host, port, err := net.SplitHostPort(r.Host)
	if err != nil { // no port in the request, so it's the one we listen on
		host = strings.Trim(r.Host, "[]")
		port = os.Getenv("WEBSERVER_PORT")
		if port == "" {
			port = "8080"
		}
	}

	addresses := orderAppAddresses(host, localIPv4s())
	if len(addresses) == 0 {
		return "", errors.New("no usable address of this server found")
	}

	q := url.Values{}
	q.Set("a", strings.Join(addresses, ","))
	q.Set("p", port)
	q.Set("t", token)
	q.Set("f", fingerprint)
	return appPairScheme + "?" + q.Encode(), nil
}

// handleGetAppPair returns what android_app.html shows - the pairing link and if the APK is included in this build.
func (s *Server) handleGetAppPair(w http.ResponseWriter, r *http.Request) {
	uri, err := appPairURI(r)
	if err != nil {
		logErrorf("handleGetAppPair - %v", err)
		http.Error(w, "cannot create pairing link", http.StatusInternalServerError)
		return
	}
	info, err := os.Stat(appAPKFile)
	writeJSON(w, http.StatusOK, map[string]interface{}{"uri": uri, "apkAvailable": err == nil && info.Mode().IsRegular()})
}

// handleGetAppPairQR returns the pairing link as a QR code.
func (s *Server) handleGetAppPairQR(w http.ResponseWriter, r *http.Request) {
	uri, err := appPairURI(r)
	if err != nil {
		logErrorf("handleGetAppPairQR - %v", err)
		http.Error(w, "cannot create pairing link", http.StatusInternalServerError)
		return
	}
	png, err := qrcode.Encode(uri, qrcode.Medium, 400)
	if err != nil {
		logErrorf("handleGetAppPairQR - cannot create QR code: %v", err)
		http.Error(w, "cannot create QR code", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "image/png")
	_, _ = w.Write(png)
}

// handlePostAppPairReset replaces the app token - the phones paired so far stop working until they scan the new code.
func (s *Server) handlePostAppPairReset(w http.ResponseWriter, r *http.Request) {
	appTokenMu.Lock()
	_, err := storeNewAppToken()
	appTokenMu.Unlock()
	if err != nil {
		logErrorf("handlePostAppPairReset - cannot store app token: %v", err)
		http.Error(w, "cannot store app token", http.StatusInternalServerError)
		return
	}
	logInfof("handlePostAppPairReset - app token replaced")
	w.WriteHeader(http.StatusNoContent)
}

// appDevice is a device with the FDD feature, as the app sees it.
type appDevice struct {
	MAC       string `json:"mac"` // without colons
	Name      string `json:"name"`
	Connected bool   `json:"connected"`
	Image     string `json:"image"` // current image, full path, "" if there's none
	ImageName string `json:"imageName"`
	Dir       string `json:"dir"` // folder of the current image
}

// handleGetAppState returns the devices with the FDD feature, with their current images. The app polls it while it's open.
func (s *Server) handleGetAppState(w http.ResponseWriter, r *http.Request) {
	devices, err := readDevices()
	if err != nil {
		logErrorf("handleGetAppState - %v", err)
		http.Error(w, "cannot read settings directory", http.StatusInternalServerError)
		return
	}

	result := make([]appDevice, 0, len(devices))
	for _, d := range devices {
		mac := stripMACColons(d.MAC)
		if !deviceFeatures(mac)["fdd"] {
			continue
		}
		dev := appDevice{MAC: mac, Name: d.Name, Connected: d.Connected}
		if image, err := s.readFddImage(mac); err == nil && image != "" {
			dev.Image = image
			dev.ImageName = filepath.Base(image)
			dev.Dir = filepath.Dir(image)
		}
		result = append(result, dev)
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{"devices": result})
}

// appDeviceExists accepts only MACs of known devices (and nothing which could leave SETTINGS_DIR).
func appDeviceExists(mac string) bool {
	if !isValidMACFormat(mac) {
		return false
	}
	info, err := os.Stat(filepath.Join(getSettingsDir(), mac))
	return err == nil && info.IsDir()
}

type appDirEntry struct {
	Name string `json:"name"`
	Path string `json:"path"`
}

// handleGetAppImages returns the images and sub folders of a folder, by default of the folder the device browses. The
// images are in the order the device has them, prev / next in the app go through this list.
func (s *Server) handleGetAppImages(w http.ResponseWriter, r *http.Request) {
	mac := stripMACColons(r.URL.Query().Get("mac"))
	if !appDeviceExists(mac) {
		http.Error(w, "unknown device", http.StatusNotFound)
		return
	}

	dir := r.URL.Query().Get("dir")
	if dir == "" { // like the device: the folder of the current image, if it's gone or there's no image then FLOPPY_IMAGES_DIR
		dir = floppyImagesDir()
		if image, err := s.readFddImage(mac); err == nil && image != "" {
			if info, err := os.Stat(filepath.Dir(image)); err == nil && info.IsDir() {
				dir = filepath.Dir(image)
			}
		}
		if dir == "" {
			http.Error(w, "no image inserted and FLOPPY_IMAGES_DIR not set", http.StatusNotFound)
			return
		}
	} else if !filepath.IsAbs(dir) {
		http.Error(w, "folder must be a full path", http.StatusBadRequest)
		return
	}
	dir = filepath.Clean(dir)

	entries, err := os.ReadDir(dir)
	if err != nil {
		logErrorf("handleGetAppImages - cannot read directory %q: %v", dir, err)
		http.Error(w, "cannot read folder", http.StatusNotFound)
		return
	}

	dirs := make([]appDirEntry, 0)
	for _, e := range entries {
		if strings.HasPrefix(e.Name(), ".") {
			continue
		}
		path := filepath.Join(dir, e.Name())
		if info, err := os.Stat(path); err == nil && info.IsDir() { // os.Stat, so symlinks to folders count too
			dirs = append(dirs, appDirEntry{Name: e.Name(), Path: path})
		}
	}
	sort.Slice(dirs, func(i, j int) bool { return floppyImageNameLess(dirs[i].Name, dirs[j].Name) })

	parent := filepath.Dir(dir)
	if parent == dir { // root of the disk
		parent = ""
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"dir":    dir,
		"parent": parent,
		"dirs":   dirs,
		"images": floppyImageNames(dir, entries),
	})
}

// handlePostAppInsert inserts an image into the device.
func (s *Server) handlePostAppInsert(w http.ResponseWriter, r *http.Request) {
	var body struct {
		MAC  string `json:"mac"`
		Dir  string `json:"dir"`
		Name string `json:"name"`
	}
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		http.Error(w, "invalid request body", http.StatusBadRequest)
		return
	}

	mac := stripMACColons(body.MAC)
	if !appDeviceExists(mac) {
		http.Error(w, "unknown device", http.StatusNotFound)
		return
	}
	if !filepath.IsAbs(body.Dir) {
		http.Error(w, "folder must be a full path", http.StatusBadRequest)
		return
	}
	if !isFloppyImageName(body.Name) || filepath.Base(body.Name) != body.Name || strings.ContainsAny(body.Name, `/\`) {
		http.Error(w, "not a floppy image", http.StatusBadRequest)
		return
	}

	image := filepath.Join(filepath.Clean(body.Dir), body.Name)
	if info, err := os.Stat(image); err != nil || !info.Mode().IsRegular() {
		http.Error(w, "image not found", http.StatusNotFound)
		return
	}

	if err := s.writeFddImage(mac, image); err != nil {
		logErrorf("handlePostAppInsert - cannot write FLOPPY_IMAGE: %v", err)
		http.Error(w, "cannot write FLOPPY_IMAGE", http.StatusInternalServerError)
		return
	}
	if err := handleFloppyAction("insert", mac); err != nil {
		http.Error(w, "image stored, but ce_cores did not get the command", http.StatusBadGateway)
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"image": image})
}
