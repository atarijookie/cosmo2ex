package main

import (
	"encoding/json"
	"log"
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
	log.Printf("handleGetFDDImage %s %s", r.Method, r.URL.Path)

	mac := chi.URLParam(r, "mac")

	image, err := s.readFddImage(mac)
	if err != nil {
		image = ""
	}

	// defaultDir - where the file selector starts without an image (the downloaded images)
	writeJSON(w, http.StatusOK, map[string]string{"mac": mac, "image": image, "defaultDir": floppyImagesDir()})
}

func (s *Server) handlePutFDDImage(w http.ResponseWriter, r *http.Request) {
	log.Printf("handlePutFDDImage %s %s", r.Method, r.URL.Path)

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
			log.Printf("handlePutFDDImage - failed to send cmd socket: %v", err)
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
	log.Printf("handleGetFDDImageStep %s %s step=%d", r.Method, r.URL.Path, step)

	mac := chi.URLParam(r, "mac")

	current, err := s.readFddImage(mac)
	if err != nil || strings.TrimSpace(current) == "" {
		http.Error(w, "current image not set", http.StatusBadRequest)
		return
	}

	// Stored paths are native paths of this machine ("/home/..." or "C:\Atari\..." on Windows), so use filepath.* for split/join.
	dir := filepath.Dir(current)
	base := filepath.Base(current)
	if dir == "." || base == "." || base == string(filepath.Separator) || base == "" {
		http.Error(w, "invalid current image path", http.StatusBadRequest)
		return
	}

	entries, err := os.ReadDir(dir)
	if err != nil {
		http.Error(w, "cannot read image directory", http.StatusInternalServerError)
		return
	}

	names := floppyImageNames(dir, entries)
	if len(names) == 0 {
		http.Error(w, "no floppy images in image directory", http.StatusBadRequest)
		return
	}

	// Find current file index; if not found, treat as 0.
	idx := 0
	for i := 0; i < len(names); i++ {
		if names[i] == base {
			idx = i
			break
		}
	}

	nextIdx := (idx + step) % len(names)
	if nextIdx < 0 {
		nextIdx += len(names)
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
			log.Printf("handleGetFDDImageStep - failed to send cmd socket: %v", err)
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

// handleFloppyAction sends a floppy action command to the core
func handleFloppyAction(action, mac string) error {
	cmd := map[string]string{"module": "floppy", "action": action, "mac": mac}
	cmdJSON, err := json.Marshal(cmd)
	if err != nil {
		return err
	}
	if err := sendToCmdSocket(string(cmdJSON)); err != nil {
		log.Printf("handleFloppyAction - failed to send cmd socket: %v", err)
		return err
	}
	return nil
}

func (s *Server) handleDeleteFDDImage(w http.ResponseWriter, r *http.Request) {
	log.Printf("handleDeleteFDDImage %s %s", r.Method, r.URL.Path)

	mac := chi.URLParam(r, "mac")

	// Send eject command to core (log error but don't fail the request)
	if err := handleFloppyAction("eject", mac); err != nil {
		log.Printf("handleDeleteFDDImage - failed to send eject command: %v", err)
	}

	// Clear the floppy image setting file
	if err := s.writeFddImage(mac, ""); err != nil {
		log.Printf("handleDeleteFDDImage - failed to write empty FLOPPY_IMAGE: %v", err)
		http.Error(w, "cannot write FLOPPY_IMAGE", http.StatusInternalServerError)
		return
	}

	w.WriteHeader(http.StatusNoContent)
}
