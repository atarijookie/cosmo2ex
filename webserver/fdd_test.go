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
