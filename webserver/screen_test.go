package main

import (
	"bytes"
	"encoding/binary"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

// fakeCore listens like ce_cores does, checks the token and answers get_frame with the given chunk offsets.
func fakeCore(t *testing.T, frame []byte, chunkSize int, offsets func(n int) []int) {
	t.Helper()

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

		count := (len(frame) + chunkSize - 1) / chunkSize
		for _, off := range offsets(count) {
			end := off + chunkSize
			if end > len(frame) {
				end = len(frame)
			}
			chunk := make([]byte, screenChunkHeader, screenChunkHeader+end-off)
			binary.BigEndian.PutUint16(chunk[0:2], uint16(off))
			binary.BigEndian.PutUint16(chunk[2:4], uint16(len(frame)))
			ln.WriteTo(append(chunk, frame[off:end]...), from)
		}
	}()
}

func testFrame() []byte {
	frame := make([]byte, screenFrameSize)
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

	got, err := requestScreenFrame()
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, frame) {
		t.Fatal("reassembled frame differs")
	}
}

func TestRequestScreenFrameOutOfOrderWithDuplicate(t *testing.T) {
	frame := testFrame()
	fakeCore(t, frame, 8000, func(n int) []int {
		return []int{32000, 8000, 8000, 0, 24000, 16000} // last chunk first, chunk 1 twice
	})

	got, err := requestScreenFrame()
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, frame) {
		t.Fatal("reassembled frame differs")
	}
}

func TestRequestScreenFrameMissingChunkTimesOut(t *testing.T) {
	fakeCore(t, testFrame(), 8000, func(n int) []int {
		return []int{0, 8000, 24000, 32000} // 16000 never arrives
	})

	if _, err := requestScreenFrame(); err == nil {
		t.Fatal("expected error for incomplete frame")
	}
}

func TestRequestScreenFrameWrongSizeRejected(t *testing.T) {
	fakeCore(t, make([]byte, 100), 8000, func(n int) []int { return []int{0} })

	if _, err := requestScreenFrame(); err == nil {
		t.Fatal("expected error for frame of wrong size")
	}
}
