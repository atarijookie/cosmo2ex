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

// GET /host/dir?path=...&q=... - q: only the names containing it, case insensitive
func TestHostDirSearch(t *testing.T) {
	dir := t.TempDir()
	for _, n := range []string{"A-097_Dungeon-Master_Oids.msa", "dungeon.st", "Zork.st", "readme.txt"} {
		if err := os.WriteFile(filepath.Join(dir, n), nil, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	for _, d := range []string{"Dungeons", "other"} {
		if err := os.Mkdir(filepath.Join(dir, d), 0o755); err != nil {
			t.Fatal(err)
		}
	}

	s := &Server{}
	list := func(q string) (dirs, files []string) {
		t.Helper()
		r := httptest.NewRequest(http.MethodGet, "/host/dir?path="+url.QueryEscape(dir)+"&q="+url.QueryEscape(q), nil)
		w := httptest.NewRecorder()
		s.handleHostDir(w, r)
		if w.Code != http.StatusOK {
			t.Fatalf("q=%q: HTTP %d", q, w.Code)
		}
		var resp struct{ Dirs, Files []string }
		if err := json.Unmarshal(w.Body.Bytes(), &resp); err != nil {
			t.Fatal(err)
		}
		return resp.Dirs, resp.Files
	}

	for _, tc := range []struct {
		q           string
		dirs, files []string
	}{
		{"", []string{"Dungeons", "other"}, []string{"A-097_Dungeon-Master_Oids.msa", "Zork.st", "dungeon.st", "readme.txt"}},
		{"DUNGEON", []string{"Dungeons"}, []string{"A-097_Dungeon-Master_Oids.msa", "dungeon.st"}},
		{"n-mas", []string{}, []string{"A-097_Dungeon-Master_Oids.msa"}},
		{"nothing", []string{}, []string{}},
	} {
		dirs, files := list(tc.q)
		if !reflect.DeepEqual(dirs, tc.dirs) || !reflect.DeepEqual(files, tc.files) {
			t.Errorf("q=%q: got %v %v, want %v %v", tc.q, dirs, files, tc.dirs, tc.files)
		}
	}
}
