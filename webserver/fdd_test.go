package main

import (
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"testing"
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
