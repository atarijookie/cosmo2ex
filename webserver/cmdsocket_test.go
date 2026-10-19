package main

import (
	"net"
	"os"
	"path/filepath"
	"strconv"
	"testing"
	"time"
)

func TestSendToCmdSocketPrefixesToken(t *testing.T) {
	ln, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()

	tokenFile := filepath.Join(t.TempDir(), "core_cmd.token")
	if err := os.WriteFile(tokenFile, []byte("abc123\n"), 0o600); err != nil {
		t.Fatal(err)
	}

	t.Setenv("CMD_UDP_PORT", strconv.Itoa(ln.LocalAddr().(*net.UDPAddr).Port))
	t.Setenv("CMD_TOKEN_FILE", tokenFile)

	if err := sendToCmdSocket(`{"module":"all","action":"generate_status"}`); err != nil {
		t.Fatal(err)
	}

	buf := make([]byte, 1024)
	ln.SetReadDeadline(time.Now().Add(2 * time.Second))
	n, _, err := ln.ReadFrom(buf)
	if err != nil {
		t.Fatal(err)
	}

	want := "abc123\n" + `{"module":"all","action":"generate_status"}`
	if got := string(buf[:n]); got != want {
		t.Fatalf("got %q, want %q", got, want)
	}
}

func TestSendToCmdSocketMissingToken(t *testing.T) {
	t.Setenv("CMD_UDP_PORT", "7310")
	t.Setenv("CMD_TOKEN_FILE", filepath.Join(t.TempDir(), "missing"))

	if err := sendToCmdSocket("{}"); err == nil {
		t.Fatal("expected error when token file is missing")
	}
}
