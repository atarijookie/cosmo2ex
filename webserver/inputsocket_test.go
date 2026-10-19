package main

import (
	"bufio"
	"bytes"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"
)

// fakeInputSocket accepts connections like ce_cores, checks the hello and passes received events to the events channel.
type fakeInputSocket struct {
	ln        net.Listener
	tokenFile string
	events    chan []byte   // one event per item
	conns     chan net.Conn // accepted (after valid hello)
	hellos    chan string
}

func newFakeInputSocket(t *testing.T, token string) *fakeInputSocket {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	f := &fakeInputSocket{ln: ln, events: make(chan []byte, 100), conns: make(chan net.Conn, 10), hellos: make(chan string, 10)}
	f.tokenFile = filepath.Join(t.TempDir(), "core_cmd.token")
	f.setToken(t, token)
	t.Setenv("INPUT_TCP_PORT", strconv.Itoa(ln.Addr().(*net.TCPAddr).Port))
	t.Setenv("CMD_TOKEN_FILE", f.tokenFile)
	t.Cleanup(func() { ln.Close() })
	go f.serve()
	return f
}

func (f *fakeInputSocket) setToken(t *testing.T, token string) {
	if err := os.WriteFile(f.tokenFile, []byte(token+"\n"), 0o600); err != nil {
		t.Fatal(err)
	}
}

func (f *fakeInputSocket) serve() {
	for {
		conn, err := f.ln.Accept()
		if err != nil {
			return
		}
		go func() {
			r := bufio.NewReader(conn)
			line, err := r.ReadString('\n')
			if err != nil {
				conn.Close()
				return
			}
			f.hellos <- line
			token, _ := os.ReadFile(f.tokenFile)
			if line != inputHelloPrefix+strings.TrimSpace(string(token))+"\n" {
				conn.Close()
				return
			}
			conn.Write([]byte("OK\n"))
			f.conns <- conn
			for {
				ev := make([]byte, inputEventSize)
				if _, err := io.ReadFull(r, ev); err != nil {
					return
				}
				f.events <- ev
			}
		}()
	}
}

func (f *fakeInputSocket) nextEvent(t *testing.T) []byte {
	t.Helper()
	select {
	case ev := <-f.events:
		return ev
	case <-time.After(2 * time.Second):
		t.Fatal("no event received")
		return nil
	}
}

func postHID(s *Server, handler func(http.ResponseWriter, *http.Request), body string) int {
	rec := httptest.NewRecorder()
	handler(rec, httptest.NewRequest("POST", "/hid", strings.NewReader(body)))
	return rec.Code
}

func TestHIDKeyboardGoesToInputSocket(t *testing.T) {
	f := newFakeInputSocket(t, "tok1")
	s := &Server{input: &inputClient{}}

	if code := postHID(s, s.handlePostHIDKeyboard, `{"type":"pc","code":30,"state":"down"}`); code != http.StatusNoContent {
		t.Fatalf("status %d", code)
	}
	if got, want := f.nextEvent(t), inputEvent(inputEvKey, 0, 30, 1); !bytes.Equal(got, want) {
		t.Fatalf("got % x, want % x", got, want)
	}
	postHID(s, s.handlePostHIDKeyboard, `{"type":"pc","code":30,"state":"up"}`)
	if got, want := f.nextEvent(t), inputEvent(inputEvKey, 0, 30, 0); !bytes.Equal(got, want) {
		t.Fatalf("got % x, want % x", got, want)
	}
	if hello := <-f.hellos; hello != "CEINPUT 1 tok1\n" {
		t.Fatalf("hello %q", hello)
	}
	if len(f.hellos) != 0 {
		t.Fatal("expected one connection for both events")
	}
}

func TestHIDMouseGoesToInputSocket(t *testing.T) {
	f := newFakeInputSocket(t, "tok")
	s := &Server{input: &inputClient{}}

	postHID(s, s.handlePostHIDMouse, `{"type":"relative","x":-8,"y":21}`)
	postHID(s, s.handlePostHIDMouse, `{"type":"relative","x":0,"y":3}`) // zero X is not sent
	postHID(s, s.handlePostHIDMouse, `{"type":"buttonright","state":"down"}`)
	postHID(s, s.handlePostHIDMouse, `{"type":"buttonleft","state":"up"}`)

	want := [][]byte{
		inputEvent(inputEvMouseMove, 0, inputRelX, -8),
		inputEvent(inputEvMouseMove, 0, inputRelY, 21),
		inputEvent(inputEvMouseMove, 0, inputRelY, 3),
		inputEvent(inputEvMouseButton, 0, 1, 1),
		inputEvent(inputEvMouseButton, 0, 0, 0),
	}
	for i, w := range want {
		if got := f.nextEvent(t); !bytes.Equal(got, w) {
			t.Fatalf("event %d: got % x, want % x", i, got, w)
		}
	}
}

func TestHIDInvalidPayloadIsRejected(t *testing.T) {
	f := newFakeInputSocket(t, "tok")
	s := &Server{input: &inputClient{}}

	bad := []struct {
		handler func(http.ResponseWriter, *http.Request)
		body    string
	}{
		{s.handlePostHIDKeyboard, `{"type":"pc","code":30,"state":"pressed"}`},
		{s.handlePostHIDKeyboard, `{"type":"pc","code":30.5,"state":"down"}`},
		{s.handlePostHIDKeyboard, `{"type":"pc","code":5000,"state":"down"}`},
		{s.handlePostHIDMouse, `{"type":"absolute","x":1,"y":2}`},
		{s.handlePostHIDMouse, `{"type":"relative","x":"1","y":2}`},
		{s.handlePostHIDMouse, `{"type":"buttonmiddle","state":"down"}`},
		{s.handlePostHIDMouse, `not json`},
	}
	for _, b := range bad {
		if code := postHID(s, b.handler, b.body); code != http.StatusBadRequest {
			t.Errorf("%s: status %d, want 400", b.body, code)
		}
	}
	select {
	case ev := <-f.events:
		t.Fatalf("unexpected event % x", ev)
	case <-time.After(100 * time.Millisecond):
	}
}

func TestInputClientReconnectsAfterCoreRestart(t *testing.T) {
	f := newFakeInputSocket(t, "old")
	s := &Server{input: &inputClient{}}

	postHID(s, s.handlePostHIDKeyboard, `{"type":"pc","code":1,"state":"down"}`)
	f.nextEvent(t)
	conn := <-f.conns

	f.setToken(t, "new") // ce_cores restarted: new token, old connection closed
	conn.Close()
	time.Sleep(50 * time.Millisecond)

	postHID(s, s.handlePostHIDKeyboard, `{"type":"pc","code":2,"state":"down"}`)
	if got, want := f.nextEvent(t), inputEvent(inputEvKey, 0, 2, 1); !bytes.Equal(got, want) {
		t.Fatalf("event after restart: got % x, want % x", got, want)
	}
	<-f.hellos
	if hello := <-f.hellos; hello != "CEINPUT 1 new\n" {
		t.Fatalf("second hello %q", hello)
	}
}

func TestHIDWithoutCoreStillAnswers(t *testing.T) {
	t.Setenv("INPUT_TCP_PORT", "1") // nothing listens there
	t.Setenv("CMD_TOKEN_FILE", filepath.Join(t.TempDir(), "missing"))
	s := &Server{input: &inputClient{}}

	if code := postHID(s, s.handlePostHIDKeyboard, `{"type":"pc","code":30,"state":"down"}`); code != http.StatusNoContent {
		t.Fatalf("status %d", code)
	}
}
