package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"github.com/go-chi/chi/v5"
)

// fakeCore listens like ce_cores does, checks the token and answers get_frame with the given chunk offsets.
func fakeCore(t *testing.T, frame []byte, chunkSize int, offsets func(n int) []int) {
	t.Helper()
	fakeCoreRequests(t, frame, chunkSize, offsets)
}

// fakeCoreRequests is fakeCore, which also gives the request it got (without token)
func fakeCoreRequests(t *testing.T, frame []byte, chunkSize int, offsets func(n int) []int) <-chan string {
	t.Helper()
	requests := make(chan string, 1)

	ln, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { ln.Close() })

	tokenFile := filepath.Join(t.TempDir(), "core_cmd.token")
	if err := os.WriteFile(tokenFile, []byte("tok"), 0o600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("CMD_UDP_PORT", strconv.Itoa(ln.LocalAddr().(*net.UDPAddr).Port))
	t.Setenv("CMD_TOKEN_FILE", tokenFile)

	go func() {
		buf := make([]byte, 4096)
		n, from, err := ln.ReadFrom(buf)
		if err != nil || !strings.HasPrefix(string(buf[:n]), "tok\n") || !strings.Contains(string(buf[:n]), "get_frame") {
			return // no reply, like ce_cores on a bad request
		}
		requests <- strings.TrimPrefix(string(buf[:n]), "tok\n")

		count := (len(frame) + chunkSize - 1) / chunkSize
		for _, off := range offsets(count) {
			end := off + chunkSize
			if end > len(frame) {
				end = len(frame)
			}
			chunk := make([]byte, screenChunkHeader, screenChunkHeader+end-off)
			binary.BigEndian.PutUint32(chunk[0:4], uint32(off))
			binary.BigEndian.PutUint32(chunk[4:8], uint32(len(frame)))
			binary.BigEndian.PutUint32(chunk[8:12], testFrameSeq)
			ln.WriteTo(append(chunk, frame[off:end]...), from)
		}
	}()
	return requests
}

const testFrameSeq = 0x01020304

func testFrame() []byte {
	return testFrameOfSize(screenFrameMinSize) // ST frame
}

func testFrameOfSize(size int) []byte {
	frame := make([]byte, size)
	for i := range frame {
		frame[i] = byte(i * 7)
	}
	return frame
}

func TestRequestScreenFrameInOrder(t *testing.T) {
	frame := testFrame()
	fakeCore(t, frame, 8000, func(n int) []int {
		offs := []int{}
		for i := 0; i < n; i++ {
			offs = append(offs, i*8000)
		}
		return offs
	})

	got, seq, err := requestScreenFrame(false)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, frame) {
		t.Fatal("reassembled frame differs")
	}
	if seq != testFrameSeq {
		t.Fatalf("frame seq %x, want %x", seq, testFrameSeq)
	}
}

func TestRequestScreenFrameOutOfOrderWithDuplicate(t *testing.T) {
	frame := testFrame()
	fakeCore(t, frame, 8000, func(n int) []int {
		return []int{32000, 8000, 8000, 0, 24000, 16000} // last chunk first, chunk 1 twice
	})

	got, seq, err := requestScreenFrame(false)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, frame) {
		t.Fatal("reassembled frame differs")
	}
	if seq != testFrameSeq {
		t.Fatalf("frame seq %x, want %x", seq, testFrameSeq)
	}
}

func TestRequestScreenFrameLive(t *testing.T) {
	inOrder := func(n int) []int {
		offs := []int{}
		for i := 0; i < n; i++ {
			offs = append(offs, i*8000)
		}
		return offs
	}

	for _, live := range []bool{false, true} {
		requests := fakeCoreRequests(t, testFrame(), 8000, inOrder)
		if _, _, err := requestScreenFrame(live); err != nil {
			t.Fatal(err)
		}
		req := <-requests
		if strings.Contains(req, `"live":1`) != live {
			t.Errorf("live %v: request %q", live, req)
		}
	}
}

func TestRequestScreenFrameTT(t *testing.T) {
	frame := testFrameOfSize(screenFrameMaxSize) // TT frame - offsets above 64 KB
	fakeCore(t, frame, 8000, func(n int) []int {
		offs := []int{}
		for i := n - 1; i >= 0; i-- { // reversed, just to be sure
			offs = append(offs, i*8000)
		}
		return offs
	})

	got, seq, err := requestScreenFrame(false)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, frame) {
		t.Fatal("reassembled frame differs")
	}
	if seq != testFrameSeq {
		t.Fatalf("frame seq %x, want %x", seq, testFrameSeq)
	}
}

func TestRequestScreenFrameMissingChunkTimesOut(t *testing.T) {
	fakeCore(t, testFrame(), 8000, func(n int) []int {
		return []int{0, 8000, 24000, 32000} // 16000 never arrives
	})

	if _, _, err := requestScreenFrame(false); err == nil {
		t.Fatal("expected error for incomplete frame")
	}
}

func TestRequestScreenFrameWrongSizeRejected(t *testing.T) {
	fakeCore(t, make([]byte, 100), 8000, func(n int) []int { return []int{0} })

	if _, _, err := requestScreenFrame(false); err == nil {
		t.Fatal("expected error for frame of wrong size")
	}
}

// screenFps does a GET (body == "") or PUT of /screen/{mac}/fps with SETTINGS_DIR in settingsDir, returns status and fps
func screenFps(t *testing.T, settingsDir, mac, body string) (int, int) {
	t.Helper()
	t.Setenv("SETTINGS_DIR", settingsDir)

	r := chi.NewRouter()
	s := &Server{}
	r.Get("/screen/{mac}/fps", s.handleGetScreenFps)
	r.Put("/screen/{mac}/fps", s.handlePutScreenFps)

	req := httptest.NewRequest(http.MethodGet, "/screen/"+mac+"/fps", nil)
	if body != "" {
		req = httptest.NewRequest(http.MethodPut, "/screen/"+mac+"/fps", strings.NewReader(body))
	}
	w := httptest.NewRecorder()
	r.ServeHTTP(w, req)

	var resp struct {
		Fps int `json:"fps"`
	}
	if w.Code == http.StatusOK {
		if err := json.Unmarshal(w.Body.Bytes(), &resp); err != nil {
			t.Fatal(err)
		}
	}
	return w.Code, resp.Fps
}

func TestScreenFpsDefault(t *testing.T) {
	code, fps := screenFps(t, t.TempDir(), "F412FAD5C554", "")
	if code != http.StatusOK || fps != screencastFpsDefault {
		t.Fatalf("got %d / %d fps, want 200 / %d fps", code, fps, screencastFpsDefault)
	}
}

func TestScreenFpsPutAndGet(t *testing.T) {
	dir := t.TempDir()

	// escaped colons like encodeURIComponent() makes them, lower case - stored in the upper case device dir
	code, fps := screenFps(t, dir, "f4%3A12%3Afa%3Ad5%3Ac5%3A54", `{"fps":12}`)
	if code != http.StatusOK || fps != 12 {
		t.Fatalf("put: got %d / %d fps, want 200 / 12 fps", code, fps)
	}

	content, err := os.ReadFile(filepath.Join(dir, "F412FAD5C554", screencastFpsKey))
	if err != nil || strings.TrimSpace(string(content)) != "12" {
		t.Fatalf("setting file: %q, %v", content, err)
	}

	if code, fps := screenFps(t, dir, "F412FAD5C554", ""); code != http.StatusOK || fps != 12 {
		t.Fatalf("get: got %d / %d fps, want 200 / 12 fps", code, fps)
	}
}

func TestScreenFpsClamped(t *testing.T) {
	dir := t.TempDir()
	cases := []struct {
		body string
		want int
	}{
		{`{"fps":0}`, screencastFpsMin},
		{`{"fps":-5}`, screencastFpsMin},
		{`{"fps":26}`, screencastFpsMax},
		{`{"fps":1000}`, screencastFpsMax},
	}
	for _, c := range cases {
		if code, fps := screenFps(t, dir, "F412FAD5C554", c.body); code != http.StatusOK || fps != c.want {
			t.Errorf("%s: got %d / %d fps, want 200 / %d fps", c.body, code, fps, c.want)
		}
	}

	// junk in the setting file - default
	if err := os.WriteFile(filepath.Join(dir, "F412FAD5C554", screencastFpsKey), []byte("abc"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, fps := screenFps(t, dir, "F412FAD5C554", ""); fps != screencastFpsDefault {
		t.Errorf("junk file: got %d fps, want %d", fps, screencastFpsDefault)
	}
}

func TestScreenFpsInvalid(t *testing.T) {
	dir := t.TempDir()
	if code, _ := screenFps(t, dir, "F412FAD5C554", `{}`); code != http.StatusBadRequest {
		t.Errorf("missing fps: got %d, want 400", code)
	}
	if code, _ := screenFps(t, dir, "..", `{"fps":5}`); code != http.StatusBadRequest {
		t.Errorf("invalid mac: got %d, want 400", code)
	}
}
