package main

import (
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/go-chi/chi/v5"
)

// Printouts - pages printed on the emulated SLM laser printer, saved by ce_cores as PNG files (hdd/slm/slmprinter.cpp).

// printoutsDir - PRINTOUTS_DIR from .env, with the same default as ce_cores; absolute, so the web UI can show it
func printoutsDir() string {
	dir := os.Getenv("PRINTOUTS_DIR")
	if dir == "" {
		dir = "./printouts"
	}
	if abs, err := filepath.Abs(dir); err == nil {
		dir = abs
	}
	return dir
}

type PrintoutFile struct {
	Name string `json:"name"`
	Size int64  `json:"size"`
	Time int64  `json:"time"` // modification time, unix seconds
}

type PrintoutsResponse struct {
	Dir    string         `json:"dir"`
	Exists bool           `json:"exists"` // the dir is created with the first printed page
	Files  []PrintoutFile `json:"files"`  // newest first
}

// isPrintoutName - just a .png file name, no path - nothing else can be read through /printouts/file/
func isPrintoutName(name string) bool {
	return name != "" && name == filepath.Base(name) && !strings.ContainsAny(name, `/\:`) &&
		!strings.HasPrefix(name, ".") && strings.HasSuffix(strings.ToLower(name), ".png")
}

func (s *Server) handleGetPrintouts(w http.ResponseWriter, r *http.Request) {
	logDebugf("handleGetPrintouts %s %s", r.Method, r.URL.Path)

	resp := PrintoutsResponse{Dir: printoutsDir(), Files: []PrintoutFile{}}

	entries, err := os.ReadDir(resp.Dir)
	if err == nil {
		resp.Exists = true
	}

	for _, e := range entries {
		if e.IsDir() || !isPrintoutName(e.Name()) { // also skips the .png.tmp files still being written
			continue
		}
		info, err := e.Info()
		if err != nil {
			continue
		}
		resp.Files = append(resp.Files, PrintoutFile{Name: e.Name(), Size: info.Size(), Time: info.ModTime().Unix()})
	}

	sort.Slice(resp.Files, func(i, j int) bool {
		if resp.Files[i].Time != resp.Files[j].Time {
			return resp.Files[i].Time > resp.Files[j].Time
		}
		return resp.Files[i].Name > resp.Files[j].Name
	})

	writeJSON(w, http.StatusOK, resp)
}

// handleGetPrintoutFile serves one page; with ?download=1 the browser saves it to its download folder
func (s *Server) handleGetPrintoutFile(w http.ResponseWriter, r *http.Request) {
	name := chi.URLParam(r, "name")
	logDebugf("handleGetPrintoutFile %s %s", r.Method, name)

	if !isPrintoutName(name) {
		http.Error(w, "invalid file name", http.StatusBadRequest)
		return
	}

	f, err := os.Open(filepath.Join(printoutsDir(), name))
	if err != nil {
		http.Error(w, "file not found", http.StatusNotFound)
		return
	}
	defer f.Close()

	info, err := f.Stat()
	if err != nil || info.IsDir() {
		http.Error(w, "file not found", http.StatusNotFound)
		return
	}

	w.Header().Set("Content-Type", "image/png")
	w.Header().Set("Cache-Control", "no-cache")
	if r.URL.Query().Get("download") == "1" {
		w.Header().Set("Content-Disposition", `attachment; filename="`+name+`"`)
	}
	http.ServeContent(w, r, name, info.ModTime(), f)
}
