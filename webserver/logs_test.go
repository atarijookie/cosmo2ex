package main

import (
	"archive/zip"
	"bytes"
	"encoding/json"
	"io"
	"log"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"sort"
	"testing"
)

// makeLogDir creates LOG_DIR with current, rotated and non-log files
func makeLogDir(t *testing.T) string {
	dir := t.TempDir()
	t.Setenv("LOG_DIR", dir)

	files := map[string]string{
		"core_hdd.log":         "hdd current\n",
		"core_hdd.log.1":       "hdd rotated\n",
		"libdospath.log":       "ldp current\n",
		"CA4E3EA40030.txt":     "device status - not a log\n",
		"core_hdd.log.1.extra": "not a log either\n",
	}
	for name, content := range files {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(content), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.Mkdir(filepath.Join(dir, "subdir.log"), 0o755); err != nil { // dir with log-like name - ignored
		t.Fatal(err)
	}
	return dir
}

func TestHandleGetLogsZip(t *testing.T) {
	makeLogDir(t)
	s := &Server{}

	rec := httptest.NewRecorder()
	s.handleGetLogs(rec, httptest.NewRequest(http.MethodGet, "/settings/logs", nil))

	if rec.Code != http.StatusOK {
		t.Fatalf("status %d", rec.Code)
	}
	if ct := rec.Header().Get("Content-Type"); ct != "application/zip" {
		t.Fatalf("content type %q", ct)
	}

	zr, err := zip.NewReader(bytes.NewReader(rec.Body.Bytes()), int64(rec.Body.Len()))
	if err != nil {
		t.Fatal(err)
	}

	got := map[string]string{}
	for _, f := range zr.File {
		rc, err := f.Open()
		if err != nil {
			t.Fatal(err)
		}
		b, _ := io.ReadAll(rc)
		rc.Close()
		got[f.Name] = string(b)
	}

	want := map[string]string{
		"core_hdd.log":   "hdd current\n",
		"core_hdd.log.1": "hdd rotated\n",
		"libdospath.log": "ldp current\n",
	}
	if len(got) != len(want) {
		t.Fatalf("got files %v, want %v", got, want)
	}
	for name, content := range want {
		if got[name] != content {
			t.Fatalf("%s: got %q, want %q", name, got[name], content)
		}
	}
}

func TestHandleDeleteLogs(t *testing.T) {
	dir := makeLogDir(t)
	s := &Server{}

	// a writer keeping the log open in append mode, like ce_cores does - must continue at start of the emptied file
	f, err := os.OpenFile(filepath.Join(dir, "core_hdd.log"), os.O_APPEND|os.O_WRONLY, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()

	rec := httptest.NewRecorder()
	s.handleDeleteLogs(rec, httptest.NewRequest(http.MethodDelete, "/settings/logs", nil))

	if rec.Code != http.StatusOK {
		t.Fatalf("status %d: %s", rec.Code, rec.Body.String())
	}

	var resp WipeLogsResponse
	if err := json.Unmarshal(rec.Body.Bytes(), &resp); err != nil {
		t.Fatal(err)
	}
	sort.Strings(resp.Wiped)
	if len(resp.Wiped) != 3 || resp.Wiped[0] != "core_hdd.log" || resp.Wiped[1] != "core_hdd.log.1" || resp.Wiped[2] != "libdospath.log" {
		t.Fatalf("wiped %v", resp.Wiped)
	}

	if _, err := os.Stat(filepath.Join(dir, "core_hdd.log.1")); !os.IsNotExist(err) {
		t.Fatalf("rotated log not deleted: %v", err)
	}
	for _, name := range []string{"core_hdd.log", "libdospath.log"} {
		if info, err := os.Stat(filepath.Join(dir, name)); err != nil || info.Size() != 0 {
			t.Fatalf("%s not emptied: %v %v", name, info, err)
		}
	}
	for _, name := range []string{"CA4E3EA40030.txt", "core_hdd.log.1.extra"} {
		if info, err := os.Stat(filepath.Join(dir, name)); err != nil || info.Size() == 0 {
			t.Fatalf("%s should be untouched: %v", name, err)
		}
	}

	f.WriteString("after wipe\n")
	if b, _ := os.ReadFile(filepath.Join(dir, "core_hdd.log")); string(b) != "after wipe\n" {
		t.Fatalf("append writer after wipe: %q", string(b))
	}
}

// webserver's own log after rotation is opened without O_APPEND - wipe must rewind it, not leave a hole of zeros
func TestWipeLogsWebserverWriterAfterRotation(t *testing.T) {
	dir := t.TempDir()
	t.Setenv("LOG_DIR", dir)

	w, err := newRotatingFileWriter(dir, "webserver.log", 64)
	if err != nil {
		t.Fatal(err)
	}
	old := webLogWriter
	webLogWriter = w
	defer func() { webLogWriter = old }()

	w.Write(bytes.Repeat([]byte("x"), 60))
	w.Write(bytes.Repeat([]byte("y"), 30)) // rotates - webserver.log reopened with O_TRUNC, no O_APPEND

	if _, failed := wipeLogs(dir); len(failed) > 0 {
		t.Fatal(failed)
	}

	logger := log.New(w, "", 0)
	logger.Print("fresh")

	b, _ := os.ReadFile(filepath.Join(dir, "webserver.log"))
	if string(b) != "fresh\n" {
		t.Fatalf("webserver.log after wipe: %q", string(b))
	}
	if w.size != int64(len("fresh\n")) {
		t.Fatalf("size counter %d", w.size)
	}
	if _, err := os.Stat(filepath.Join(dir, "webserver.log.1")); !os.IsNotExist(err) {
		t.Fatalf("rotated webserver log not deleted")
	}
}
