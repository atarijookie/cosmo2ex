package main

import (
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

// fakeLogLevelCore answers get_loglevel and remembers set_loglevel, like ce_cores command socket does
func fakeLogLevelCore(t *testing.T, level int) {
	ln, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { ln.Close() })

	tokenFile := filepath.Join(t.TempDir(), "core_cmd.token")
	if err := os.WriteFile(tokenFile, []byte("abc123\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("CMD_UDP_PORT", strconv.Itoa(ln.LocalAddr().(*net.UDPAddr).Port))
	t.Setenv("CMD_TOKEN_FILE", tokenFile)

	go func() {
		buf := make([]byte, 1024)
		for {
			n, from, err := ln.ReadFrom(buf)
			if err != nil {
				return
			}
			msg := string(buf[:n])
			if strings.Contains(msg, `"set_loglevel"`) {
				i := strings.Index(msg, `"loglevel":`)
				level, _ = strconv.Atoi(msg[i+len(`"loglevel":`) : i+len(`"loglevel":`)+1])
			} else if strings.Contains(msg, `"get_loglevel"`) {
				ln.WriteTo([]byte(`{"loglevel":`+strconv.Itoa(level)+`}`), from)
			}
		}
	}()
}

func TestRequestCoreLogLevel(t *testing.T) {
	fakeLogLevelCore(t, 2)

	level, err := requestCoreLogLevel()
	if err != nil {
		t.Fatal(err)
	}
	if level != 2 {
		t.Fatalf("got %d, want 2", level)
	}
}

func TestRequestCoreLogLevelNoCore(t *testing.T) {
	ln, err := net.ListenPacket("udp", "127.0.0.1:0") // bound, but never replies
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()

	tokenFile := filepath.Join(t.TempDir(), "core_cmd.token")
	os.WriteFile(tokenFile, []byte("abc123\n"), 0o600)
	t.Setenv("CMD_UDP_PORT", strconv.Itoa(ln.LocalAddr().(*net.UDPAddr).Port))
	t.Setenv("CMD_TOKEN_FILE", tokenFile)

	start := time.Now()
	if _, err := requestCoreLogLevel(); err == nil {
		t.Fatal("expected error when core doesn't reply")
	}
	if time.Since(start) > 2*coreReplyTimeout {
		t.Fatalf("took too long: %v", time.Since(start))
	}
}

func TestHandlePutLogLevelSetsBoth(t *testing.T) {
	fakeLogLevelCore(t, 1)
	defer setLogLevel(getLogLevel())
	setLogLevel(logError)

	s := &Server{}
	req := httptest.NewRequest(http.MethodPut, "/settings/loglevel", strings.NewReader(`{"loglevel":4}`))
	rec := httptest.NewRecorder()
	s.handlePutLogLevel(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("status %d", rec.Code)
	}
	if getLogLevel() != logDebug {
		t.Fatalf("webserver log level %d, want %d", getLogLevel(), logDebug)
	}
	if body := rec.Body.String(); !strings.Contains(body, `"webserver":4`) || !strings.Contains(body, `"core":4`) {
		t.Fatalf("unexpected body: %s", body)
	}
}

func TestHandlePutLogLevelInvalidBody(t *testing.T) {
	s := &Server{}
	req := httptest.NewRequest(http.MethodPut, "/settings/loglevel", strings.NewReader(`{}`))
	rec := httptest.NewRecorder()
	s.handlePutLogLevel(rec, req)

	if rec.Code != http.StatusBadRequest {
		t.Fatalf("status %d, want 400", rec.Code)
	}
}

func TestClampLogLevel(t *testing.T) {
	for in, want := range map[int]int{-1: logOff, 0: logOff, 3: logInfo, 4: logDebug, 9: logDebug} {
		if got := clampLogLevel(in); got != want {
			t.Errorf("clampLogLevel(%d) = %d, want %d", in, got, want)
		}
	}
}
