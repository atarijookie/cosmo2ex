package main

import (
	"encoding/json"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/go-chi/chi/v5"
)

// FDD-related handlers.

func getSettingsDir() string {
	settingsDir := os.Getenv("SETTINGS_DIR")
	if settingsDir == "" {
		settingsDir = "."
	}
	return settingsDir
}

func (s *Server) readFddImage(mac string) (string, error) {
	imageFile := filepath.Join(getSettingsDir(), mac, "FLOPPY_IMAGE")
	content, err := os.ReadFile(imageFile)
	if err != nil {
		return "", err
	}
	return strings.TrimSpace(string(content)), nil
}

func (s *Server) writeFddImage(mac, image string) error {
	deviceDir := filepath.Join(getSettingsDir(), mac)
	if err := os.MkdirAll(deviceDir, 0o755); err != nil {
		return err
	}
	imageFile := filepath.Join(deviceDir, "FLOPPY_IMAGE")
	return os.WriteFile(imageFile, []byte(image), 0o644)
}

func (s *Server) handleGetFDDImage(w http.ResponseWriter, r *http.Request) {
	logDebugf("handleGetFDDImage %s %s", r.Method, r.URL.Path)

	mac := chi.URLParam(r, "mac")

	image, err := s.readFddImage(mac)
	if err != nil {
		image = ""
	}

	// defaultDir - where the file selector starts without an image
	writeJSON(w, http.StatusOK, map[string]string{"mac": mac, "image": image, "defaultDir": floppyImagesDir()})
}

func (s *Server) handlePutFDDImage(w http.ResponseWriter, r *http.Request) {
	logDebugf("handlePutFDDImage %s %s", r.Method, r.URL.Path)

	mac := chi.URLParam(r, "mac")
	var body struct {
		Image string `json:"image"`
	}
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		http.Error(w, "invalid request body", http.StatusBadRequest)
		return
	}
	if body.Image == "" {
		http.Error(w, "image is required", http.StatusBadRequest)
		return
	}

	if err := s.writeFddImage(mac, body.Image); err != nil {
		http.Error(w, "cannot write FLOPPY_IMAGE", http.StatusInternalServerError)
		return
	}

	// Send insert command to core
	cmd := map[string]string{"module": "floppy", "action": "insert", "mac": mac}
	if cmdJSON, err := json.Marshal(cmd); err == nil {
		if err := sendToCmdSocket(string(cmdJSON)); err != nil {
			logErrorf("handlePutFDDImage - failed to send cmd socket: %v", err)
		}
	}

	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) handleGetFDDImageNext(w http.ResponseWriter, r *http.Request) {
	s.handleGetFDDImageStep(w, r, +1)
}

func (s *Server) handleGetFDDImagePrev(w http.ResponseWriter, r *http.Request) {
	s.handleGetFDDImageStep(w, r, -1)
}

func (s *Server) handleGetFDDImageStep(w http.ResponseWriter, r *http.Request, step int) {
	logDebugf("handleGetFDDImageStep %s %s step=%d", r.Method, r.URL.Path, step)

	mac := chi.URLParam(r, "mac")

	current, _ := s.readFddImage(mac)

	// Stored paths are native paths of this machine ("/home/..." or "C:\Atari\..." on Windows), so use filepath.* for split/join.
	// No image: browse FLOPPY_IMAGES_DIR, like the device does with an empty slot.
	dir, base := floppyImagesDir(), ""
	if current != "" {
		dir = filepath.Dir(current)
		base = filepath.Base(current)
		if dir == "." || base == "." || base == string(filepath.Separator) || base == "" {
			http.Error(w, "invalid current image path", http.StatusBadRequest)
			return
		}
	}
	if dir == "" {
		http.Error(w, "no image inserted and FLOPPY_IMAGES_DIR not set", http.StatusNotFound)
		return
	}

	entries, err := os.ReadDir(dir)
	if os.IsNotExist(err) && base != "" && floppyImagesDir() != "" { // the image's folder is gone (moved, deleted) - browse FLOPPY_IMAGES_DIR
		dir, base = floppyImagesDir(), ""
		entries, err = os.ReadDir(dir)
	}
	if err != nil {
		logErrorf("handleGetFDDImageStep - cannot read image directory %q: %v", dir, err)
		if os.IsNotExist(err) {
			http.Error(w, "image directory not found: "+dir, http.StatusNotFound)
		} else {
			http.Error(w, "cannot read image directory: "+dir, http.StatusInternalServerError)
		}
		return
	}

	names := floppyImageNames(dir, entries)
	if len(names) == 0 {
		http.Error(w, "no floppy images in "+dir, http.StatusNotFound)
		return
	}

	// Current file index; not in the list (no image, or it was deleted) - next is the first one, prev the last one.
	idx := -1
	for i := 0; i < len(names); i++ {
		if names[i] == base {
			idx = i
			break
		}
	}

	var nextIdx int
	switch {
	case idx < 0 && step > 0:
		nextIdx = 0
	case idx < 0:
		nextIdx = len(names) - 1
	default:
		nextIdx = (idx + step + len(names)) % len(names)
	}

	newImage := filepath.Join(dir, names[nextIdx])
	if err := s.writeFddImage(mac, newImage); err != nil {
		http.Error(w, "cannot write FLOPPY_IMAGE", http.StatusInternalServerError)
		return
	}

	// Send insert command to core
	cmd := map[string]string{"module": "floppy", "action": "insert", "mac": mac}
	if cmdJSON, err := json.Marshal(cmd); err == nil {
		if err := sendToCmdSocket(string(cmdJSON)); err != nil {
			logErrorf("handleGetFDDImageStep - failed to send cmd socket: %v", err)
		}
	}

	writeJSON(w, http.StatusOK, map[string]string{"mac": mac, "image": newImage})
}

// floppyImageNames returns the floppy images in dir, in the order the device browses them - must stay the same as
// ImageList in ce_cores/fdd/imagelist.cpp: only .st / .msa (any case), regular files (symlinks followed), names up to
// 255 bytes, sorted by the ASCII lower case bytes, names which differ only in case by their bytes.
func floppyImageNames(dir string, entries []os.DirEntry) []string {
	names := make([]string, 0, len(entries))
	for _, e := range entries {
		name := e.Name()
		if !isFloppyImageName(name) || len(name) > 255 {
			continue
		}
		if !e.Type().IsRegular() {
			if e.Type()&os.ModeSymlink == 0 { // dir or something special
				continue
			}
			info, err := os.Stat(filepath.Join(dir, name)) // symlink - to a file?
			if err != nil || !info.Mode().IsRegular() {
				continue
			}
		}
		names = append(names, name)
	}
	sort.Slice(names, func(i, j int) bool { return floppyImageNameLess(names[i], names[j]) })
	return names
}

func isFloppyImageName(name string) bool {
	dot := strings.LastIndexByte(name, '.')
	if dot <= 0 { // no extension, or nothing before it
		return false
	}
	ext := []byte(name[dot:]) // ASCII only, like strcasecmp() in the core
	for i := range ext {
		ext[i] = lowerASCII(ext[i])
	}
	return string(ext) == ".st" || string(ext) == ".msa"
}

func lowerASCII(c byte) byte {
	if c >= 'A' && c <= 'Z' {
		return c + ('a' - 'A')
	}
	return c
}

func floppyImageNameLess(a, b string) bool {
	n := len(a)
	if len(b) < n {
		n = len(b)
	}
	for i := 0; i < n; i++ {
		ca, cb := lowerASCII(a[i]), lowerASCII(b[i])
		if ca != cb {
			return ca < cb
		}
	}
	if len(a) != len(b) {
		return len(a) < len(b)
	}
	return a < b // differ only in case - by bytes, so the order is always the same
}

// floppyImagesDir - where the file selector starts without an image (FLOPPY_IMAGES_DIR in .env, next to SETTINGS_DIR),
// "" if not set
func floppyImagesDir() string {
	return os.Getenv("FLOPPY_IMAGES_DIR")
}

// handleFloppyAction sends a floppy action command to the core
func handleFloppyAction(action, mac string) error {
	cmd := map[string]string{"module": "floppy", "action": action, "mac": mac}
	cmdJSON, err := json.Marshal(cmd)
	if err != nil {
		return err
	}
	if err := sendToCmdSocket(string(cmdJSON)); err != nil {
		logErrorf("handleFloppyAction - failed to send cmd socket: %v", err)
		return err
	}
	return nil
}

func (s *Server) handleDeleteFDDImage(w http.ResponseWriter, r *http.Request) {
	logDebugf("handleDeleteFDDImage %s %s", r.Method, r.URL.Path)

	mac := chi.URLParam(r, "mac")

	// Send eject command to core (log error but don't fail the request)
	if err := handleFloppyAction("eject", mac); err != nil {
		logErrorf("handleDeleteFDDImage - failed to send eject command: %v", err)
	}

	// Clear the floppy image setting file
	if err := s.writeFddImage(mac, ""); err != nil {
		logErrorf("handleDeleteFDDImage - failed to write empty FLOPPY_IMAGE: %v", err)
		http.Error(w, "cannot write FLOPPY_IMAGE", http.StatusInternalServerError)
		return
	}

	w.WriteHeader(http.StatusNoContent)
}
