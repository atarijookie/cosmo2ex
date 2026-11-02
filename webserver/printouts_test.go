package main

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/go-chi/chi/v5"
)

func TestIsPrintoutName(t *testing.T) {
	cases := map[string]bool{
		"2026-11-02_10-42-31.png": true, "2026-11-02_10-42-31_2.PNG": true,
		"": false, "x.png.tmp": false, "../x.png": false, `..\x.png`: false, "a/b.png": false,
		"C:x.png": false, ".hidden.png": false, "notes.txt": false,
	}
	for name, want := range cases {
		if got := isPrintoutName(name); got != want {
			t.Errorf("isPrintoutName(%q) = %v, want %v", name, got, want)
		}
	}
}

// list: newest first, only complete .png files; file: served, download header, names outside rejected
func TestPrintouts(t *testing.T) {
	dir := t.TempDir()
	t.Setenv("PRINTOUTS_DIR", dir)
	s := &Server{}

	write := func(name string, age time.Duration) {
		p := filepath.Join(dir, name)
		if err := os.WriteFile(p, []byte("\x89PNG "+name), 0o644); err != nil {
			t.Fatal(err)
		}
		mt := time.Now().Add(-age)
		os.Chtimes(p, mt, mt)
	}
	write("2026-11-01_09-00-00.png", 2*time.Hour)
	write("2026-11-02_10-00-00.png", time.Hour)
	write("2026-11-02_11-00-00.png.tmp", 0)
	write("readme.txt", 0)
	os.WriteFile(filepath.Join(filepath.Dir(dir), "secret.png"), []byte("no"), 0o644)

	rec := httptest.NewRecorder()
	s.handleGetPrintouts(rec, httptest.NewRequest(http.MethodGet, "/printouts", nil))
	var resp PrintoutsResponse
	if err := json.Unmarshal(rec.Body.Bytes(), &resp); err != nil {
		t.Fatal(err)
	}
	if !resp.Exists || resp.Dir != dir {
		t.Errorf("dir %q exists %v", resp.Dir, resp.Exists)
	}
	if len(resp.Files) != 2 || resp.Files[0].Name != "2026-11-02_10-00-00.png" || resp.Files[1].Name != "2026-11-01_09-00-00.png" {
		t.Errorf("files %+v", resp.Files)
	}

	get := func(name, query string) *httptest.ResponseRecorder {
		req := httptest.NewRequest(http.MethodGet, "/printouts/file/x"+query, nil)
		rctx := chi.NewRouteContext()
		rctx.URLParams.Add("name", name)
		req = req.WithContext(context.WithValue(req.Context(), chi.RouteCtxKey, rctx))
		rec := httptest.NewRecorder()
		s.handleGetPrintoutFile(rec, req)
		return rec
	}

	rec = get("2026-11-02_10-00-00.png", "?download=1")
	if rec.Code != http.StatusOK || rec.Header().Get("Content-Type") != "image/png" ||
		rec.Header().Get("Content-Disposition") != `attachment; filename="2026-11-02_10-00-00.png"` {
		t.Errorf("download: %d %v", rec.Code, rec.Header())
	}
	if rec = get("2026-11-02_10-00-00.png", ""); rec.Header().Get("Content-Disposition") != "" {
		t.Errorf("view has Content-Disposition")
	}
	for _, bad := range []string{"../secret.png", "readme.txt", "missing.png"} {
		if rec = get(bad, ""); rec.Code == http.StatusOK {
			t.Errorf("%q served", bad)
		}
	}

	t.Setenv("PRINTOUTS_DIR", filepath.Join(dir, "not-yet"))
	rec = httptest.NewRecorder()
	s.handleGetPrintouts(rec, httptest.NewRequest(http.MethodGet, "/printouts", nil))
	json.Unmarshal(rec.Body.Bytes(), &resp)
	if resp.Exists || len(resp.Files) != 0 {
		t.Errorf("missing dir: %+v", resp)
	}
}
