package main

import (
	"archive/zip"
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strings"
	"sync"
	"testing"
)

func msaData(fill byte) []byte {
	d := make([]byte, 1000)
	for i := range d {
		d[i] = fill
	}
	d[0], d[1] = 0x0e, 0x0f
	return d
}

func zipData(t *testing.T, files map[string][]byte) []byte {
	t.Helper()
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	names := make([]string, 0, len(files))
	for n := range files {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, n := range names {
		w, err := zw.Create(n)
		if err != nil {
			t.Fatal(err)
		}
		w.Write(files[n])
	}
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

// fake file server - counts the requests per path
type fakeFiles struct {
	mu    sync.Mutex
	files map[string][]byte
	hits  map[string]int
	srv   *httptest.Server
}

func newFakeFiles(t *testing.T) *fakeFiles {
	f := &fakeFiles{files: map[string][]byte{}, hits: map[string]int{}}
	f.srv = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		f.mu.Lock()
		f.hits[r.URL.Path]++
		data, ok := f.files[r.URL.Path]
		f.mu.Unlock()
		if r.URL.Path == "/moved.zip" { // like www.stonish.net now - redirect to a html page
			http.Redirect(w, r, "/index.html", http.StatusMovedPermanently)
			return
		}
		if !ok {
			http.NotFound(w, r)
			return
		}
		w.Write(data)
	}))
	t.Cleanup(f.srv.Close)
	return f
}

func (f *fakeFiles) hitCount(p string) int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.hits[p]
}

func testDownloader(dir string, listURL string, enabled *bool, free *uint64) *floppyDownloader {
	d := newFloppyDownloader()
	d.pause = 0
	d.dir = func() string { return dir }
	d.settings = func() generalSettings {
		return generalSettings{FloppyDownloadEnabled: *enabled, FloppyDownloadURL: listURL, FloppyDownloadDecided: true}
	}
	d.diskFree = func(string) (uint64, error) { return *free, nil }
	return d
}

func dirContent(t *testing.T, dir string) map[string]int {
	t.Helper()
	got := map[string]int{}
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	for _, e := range entries {
		info, _ := e.Info()
		got[e.Name()] = int(info.Size())
	}
	return got
}

func TestParseFloppyImageList(t *testing.T) {
	list := "2026-08-14\n" +
		"https://exxosforum.co.uk/atari/games/automation/files/A_000.zip,0x0000,Sinbad and the Throne of the Falcon,Myth\n" +
		"\n" +
		"http://www.stonish.net/download/ACO_A.ZIP,0x0000,Vroom,Grav\n" +
		"https://example.com/disks/plain.st,0x0000\n" +
		"ftp://example.com/x.zip,0x0000,No\n" + // not http
		"https://example.com/readme.txt,0x0000\n" + // not an image / zip
		"https://example.com/.hidden.zip,0x0000\n" +
		"https://example.com/,0x0000\n" +
		"<head><title>301 Moved Permanently</title></head>\n" + // the tail of the real list
		"<center><h1>301 Moved Permanently</h1></center>\n" +
		"<hr><center>nginx</center>\n"

	version, entries, skipped, err := parseFloppyImageList(strings.NewReader(list))
	if err != nil {
		t.Fatal(err)
	}
	if version != "2026-08-14" {
		t.Errorf("version %q", version)
	}
	if skipped != 7 { // 4 bad URLs + 3 lines of the html tail
		t.Errorf("skipped %d, want 7", skipped)
	}

	var names []string
	for _, e := range entries {
		names = append(names, e.Name)
	}
	if !reflect.DeepEqual(names, []string{"A_000.zip", "ACO_A.ZIP", "plain.st"}) {
		t.Fatalf("names %v", names)
	}
	if !reflect.DeepEqual(entries[0].Games, []string{"Sinbad and the Throne of the Falcon", "Myth"}) || entries[0].Folder != "" {
		t.Errorf("entry %+v", entries[0])
	}
	if entries[2].Games != nil {
		t.Errorf("games of plain.st %v", entries[2].Games)
	}
}

func TestSafeFloppySubfolder(t *testing.T) {
	for folder, want := range map[string]bool{
		"": true, "automation": true, "games/automation": true,
		"..": false, "a/../b": false, "/abs": false, "a//b": false, "a\\b": false, "C:x": false, "a/": false,
	} {
		if got := safeFloppySubfolder(folder); got != want {
			t.Errorf("safeFloppySubfolder(%q) = %v", folder, got)
		}
	}
}

func TestFloppyDownloadRun(t *testing.T) {
	ff := newFakeFiles(t)
	dir := filepath.Join(t.TempDir(), "floppyImages") // created by the run
	base := ff.srv.URL

	ff.files["/A_000.zip"] = zipData(t, map[string][]byte{"A_000.MSA": msaData(1)})
	ff.files["/MULTI.ZIP"] = zipData(t, map[string][]byte{ // other names, dirs, junk, a path escaping the folder
		"disks/Game 1.st": make([]byte, 512*10),
		"Game 2.MSA":      msaData(2),
		"readme.txt":      []byte("hello"),
		"../evil.st":      make([]byte, 512),
		"bad.st":          make([]byte, 100), // not whole sectors
	})
	ff.files["/KEEP.zip"] = zipData(t, map[string][]byte{"keep.msa": msaData(3)})
	ff.files["/plain.st"] = make([]byte, 512*20)
	ff.files["/html.zip"] = []byte("<!DOCTYPE html><html>not a zip</html>")
	ff.files["/index.html"] = []byte("<!DOCTYPE html><html>moved</html>")
	ff.files["/list.csv"] = []byte("2026-08-14\n" +
		base + "/A_000.zip,0x0000,Game\n" +
		base + "/MULTI.ZIP,0x0000,Game 1,Game 2\n" +
		base + "/KEEP.zip,0x0000\n" +
		base + "/plain.st,0x0000\n" +
		base + "/html.zip,0x0000\n" +
		base + "/moved.zip,0x0000\n" +
		base + "/missing.zip,0x0000\n" +
		"<hr><center>nginx</center>\n")

	// an image which is already there (maybe written to by the ST) - never overwritten
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "keep.msa"), msaData(9), 0o644); err != nil {
		t.Fatal(err)
	}

	enabled, free := true, uint64(10<<30)
	d := testDownloader(dir, base+"/list.csv", &enabled, &free)
	d.run()

	st := d.getStatus()
	if st.State != "done" || st.Total != 7 || st.Downloaded != 4 || st.Failed != 3 || st.Present != 0 || st.ListVersion != "2026-08-14" {
		t.Fatalf("status %+v", st)
	}

	want := map[string]int{
		"A_000.zip": 0, "A_000.MSA": 1000, // zip -> its images + an empty zip as the mark
		"MULTI.ZIP": 0, "Game 1.st": 5120, "Game 2.MSA": 1000, "evil.st": 512,
		"KEEP.zip": 0, "keep.msa": 1000,
		"plain.st": 10240,
	}
	if got := dirContent(t, dir); !reflect.DeepEqual(got, want) {
		t.Fatalf("dir content\n got %v\nwant %v", got, want)
	}
	if kept, _ := os.ReadFile(filepath.Join(dir, "keep.msa")); kept[2] != 9 {
		t.Errorf("existing keep.msa was overwritten")
	}
	if _, err := os.Stat(filepath.Join(filepath.Dir(dir), "evil.st")); err == nil {
		t.Errorf("zip entry escaped the folder")
	}

	// second run: everything there or failed for good - only the list is downloaded
	d.run()
	st = d.getStatus()
	if st.Present != 4 || st.Downloaded != 0 || st.Failed != 3 {
		t.Fatalf("second run status %+v", st)
	}
	for _, p := range []string{"/A_000.zip", "/MULTI.ZIP", "/plain.st", "/html.zip", "/moved.zip", "/missing.zip"} {
		if n := ff.hitCount(p); n != 1 {
			t.Errorf("%s downloaded %d times", p, n)
		}
	}
	if n := ff.hitCount("/list.csv"); n != 2 {
		t.Errorf("list downloaded %d times", n)
	}

	// a deleted image in an unpacked zip isn't downloaded again - the zip mark is there
	os.Remove(filepath.Join(dir, "A_000.MSA"))
	d.run()
	if n := ff.hitCount("/A_000.zip"); n != 1 {
		t.Errorf("A_000.zip downloaded again")
	}
}

func TestFloppyDownloadDisabledNoSpaceNoList(t *testing.T) {
	ff := newFakeFiles(t)
	dir := t.TempDir()
	ff.files["/list.csv"] = []byte(ff.srv.URL + "/a.st,0x0000\n" + ff.srv.URL + "/b.st,0x0000\n")
	ff.files["/a.st"] = make([]byte, 512)
	ff.files["/b.st"] = make([]byte, 512)

	enabled, free := false, uint64(10<<30)
	d := testDownloader(dir, ff.srv.URL+"/list.csv", &enabled, &free)

	d.run()
	if st := d.getStatus(); st.State != "disabled" || ff.hitCount("/list.csv") != 0 {
		t.Fatalf("disabled: status %+v, list hits %d", st, ff.hitCount("/list.csv"))
	}

	enabled, free = true, floppyFreeSpaceReserve // would go below the reserve
	d.run()
	if st := d.getStatus(); st.State != "no-space" || ff.hitCount("/list.csv") != 0 {
		t.Fatalf("no space: status %+v", st)
	}

	free = 10 << 30
	calls := 0
	d.diskFree = func(string) (uint64, error) { // enough at the start and for the first file, then not
		calls++
		if calls > 2 {
			return floppyFreeSpaceReserve, nil
		}
		return free, nil
	}
	d.run()
	if st := d.getStatus(); st.State != "no-space" || st.Downloaded != 1 {
		t.Fatalf("space ran out: status %+v", st)
	}
	if got := dirContent(t, dir); !reflect.DeepEqual(got, map[string]int{"a.st": 512}) {
		t.Fatalf("dir content %v", got)
	}

	d.diskFree = func(string) (uint64, error) { return free, nil }
	d.settings = func() generalSettings {
		return generalSettings{FloppyDownloadEnabled: true, FloppyDownloadURL: ff.srv.URL + "/nolist.csv"}
	}
	d.run()
	if st := d.getStatus(); st.State != "error" || !strings.Contains(st.Message, "404") {
		t.Fatalf("no list: status %+v", st)
	}
}

func TestFloppyDownloadStopsWhenDisallowedOrNetworkDown(t *testing.T) {
	ff := newFakeFiles(t)
	dir := t.TempDir()
	var list strings.Builder
	for _, n := range []string{"a", "b", "c", "d", "e", "f", "g"} {
		list.WriteString(ff.srv.URL + "/" + n + ".st,0x0000\n")
		ff.files["/"+n+".st"] = make([]byte, 512)
	}
	ff.files["/list.csv"] = []byte(list.String())

	enabled, free := true, uint64(10<<30)
	d := testDownloader(dir, ff.srv.URL+"/list.csv", &enabled, &free)
	checks := 0
	d.settings = func() generalSettings {
		checks++
		return generalSettings{FloppyDownloadEnabled: checks <= 3, FloppyDownloadURL: ff.srv.URL + "/list.csv"}
	}
	d.run()
	if st := d.getStatus(); st.State != "disabled" || st.Downloaded != 2 {
		t.Fatalf("disallowed during the run: status %+v", st)
	}

	// files on a server which doesn't answer - stops after a few, doesn't remember them as failed for good
	dead := httptest.NewServer(http.NotFoundHandler())
	deadURL := dead.URL
	dead.Close()
	list.Reset()
	for _, n := range []string{"1", "2", "3", "4", "5", "6", "7"} {
		list.WriteString(deadURL + "/" + n + ".st,0x0000\n")
	}
	ff.files["/list2.csv"] = []byte(list.String())
	d = testDownloader(dir, ff.srv.URL+"/list2.csv", &enabled, &free)
	d.run()
	st := d.getStatus()
	if st.State != "error" || st.Checked != floppyMaxNetworkErrors || len(d.failed) != 0 {
		t.Fatalf("network down: status %+v, failed %v", st, d.failed)
	}
}

func TestDiskFreeBytes(t *testing.T) {
	free, err := diskFreeBytes(t.TempDir())
	if err != nil || free == 0 {
		t.Fatalf("diskFreeBytes = %d, %v", free, err)
	}
}

func TestGeneralSettingsHandlers(t *testing.T) {
	t.Setenv("SETTINGS_DIR", t.TempDir())
	t.Setenv("FLOPPY_IMAGES_DIR", "/some/floppyImages")
	s := newServer()

	get := func() map[string]interface{} {
		rec := httptest.NewRecorder()
		s.handleGetGeneralSettings(rec, httptest.NewRequest(http.MethodGet, "/settings/general", nil))
		var resp map[string]interface{}
		if err := json.Unmarshal(rec.Body.Bytes(), &resp); err != nil {
			t.Fatal(err)
		}
		return resp
	}
	put := func(body string) int {
		rec := httptest.NewRecorder()
		s.handlePutGeneralSettings(rec, httptest.NewRequest(http.MethodPut, "/settings/general", strings.NewReader(body)))
		return rec.Code
	}

	resp := get()
	settings := resp["settings"].(map[string]interface{})
	if settings["floppyDownloadEnabled"] != false || settings["floppyDownloadDecided"] != false ||
		settings["floppyDownloadUrl"] != defaultFloppyListURL || resp["floppyImagesDir"] != "/some/floppyImages" {
		t.Fatalf("defaults %v", resp)
	}

	if code := put(`{"floppyDownloadEnabled": true, "floppyDownloadUrl": "file:///etc/passwd"}`); code != http.StatusBadRequest {
		t.Errorf("bad URL accepted: %d", code)
	}
	if code := put(`{"floppyDownloadEnabled": false, "floppyDownloadUrl": ""}`); code != http.StatusNoContent {
		t.Errorf("deny: %d", code)
	}
	settings = get()["settings"].(map[string]interface{})
	if settings["floppyDownloadDecided"] != true || settings["floppyDownloadEnabled"] != false || settings["floppyDownloadUrl"] != defaultFloppyListURL {
		t.Fatalf("after deny %v", settings)
	}

	if code := put(`{"floppyDownloadEnabled": true, "floppyDownloadUrl": " https://example.com/list.csv "}`); code != http.StatusNoContent {
		t.Errorf("allow: %d", code)
	}
	settings = get()["settings"].(map[string]interface{})
	if settings["floppyDownloadEnabled"] != true || settings["floppyDownloadUrl"] != "https://example.com/list.csv" {
		t.Fatalf("after allow %v", settings)
	}
	select { // allowing starts a run right away
	case <-s.downloader.wake:
	default:
		t.Errorf("downloader not triggered")
	}
}
