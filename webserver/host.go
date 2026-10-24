package main

import (
	"log"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// Host-related handlers.

func (s *Server) handleHostDir(w http.ResponseWriter, r *http.Request) {
	log.Printf("handleHostDir %s %s", r.Method, r.URL.Path)

	path := r.URL.Query().Get("path")
	if path == "" {
		path = "."
	}
	entries, err := os.ReadDir(path)
	if err != nil {
		http.Error(w, "cannot read directory", http.StatusBadRequest)
		return
	}

	// Separate directories and files
	dirs := make([]string, 0)
	files := make([]string, 0)
	for _, e := range entries {
		if e.IsDir() {
			dirs = append(dirs, e.Name())
		} else if !isDownloadMark(e) {
			files = append(files, e.Name())
		}
	}

	// Sort both lists
	sort.Strings(dirs)
	sort.Strings(files)

	// Return in the requested format
	response := map[string]interface{}{
		"dirs":  dirs,
		"files": files,
	}
	writeJSON(w, http.StatusOK, response)
}

// isDownloadMark - an empty .zip is the mark of a zip unpacked by the floppy image downloader (see floppydownload.go),
// nothing to select
func isDownloadMark(e os.DirEntry) bool {
	if !strings.EqualFold(filepath.Ext(e.Name()), ".zip") {
		return false
	}
	info, err := e.Info()
	return err == nil && info.Mode().IsRegular() && info.Size() == 0
}
