package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

func TestHostDirHidesDownloadMarks(t *testing.T) {
	dir := t.TempDir()
	files := map[string]int{"A_000.zip": 0, "A_000.MSA": 10, "B.ZIP": 0, "real.zip": 5, "empty.st": 0}
	for name, size := range files {
		if err := os.WriteFile(filepath.Join(dir, name), make([]byte, size), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.Mkdir(filepath.Join(dir, "sub.zip"), 0o755); err != nil {
		t.Fatal(err)
	}

	s := &Server{}
	rec := httptest.NewRecorder()
	s.handleHostDir(rec, httptest.NewRequest(http.MethodGet, "/host/dir?path="+url.QueryEscape(dir), nil))

	var resp struct {
		Dirs  []string `json:"dirs"`
		Files []string `json:"files"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &resp); err != nil {
		t.Fatal(err)
	}
	// empty zips (the marks) hidden; a zip with data, an empty image and a dir named .zip stay
	if want := []string{"A_000.MSA", "empty.st", "real.zip"}; !reflect.DeepEqual(resp.Files, want) {
		t.Errorf("files %v, want %v", resp.Files, want)
	}
	if want := []string{"sub.zip"}; !reflect.DeepEqual(resp.Dirs, want) {
		t.Errorf("dirs %v, want %v", resp.Dirs, want)
	}
}
