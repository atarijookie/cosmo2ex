package main

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"
)

// Screen-related handlers.

// Screencast frame from ce_cores: resolution (1) + palette (32) + screen data (32000).
// It's requested by 'get_frame' on the command socket and comes back as chunk datagrams of
// offset (2 bytes, big endian) + frame size (2 bytes, big endian) + data - see SCREENCAST_CHUNK_* in ce_cores.
const (
	screenFrameSize    = 1 + 32 + 32000
	screenChunkHeader  = 4
	screenFrameTimeout = 500 * time.Millisecond
)

func requestScreenFrame() ([]byte, error) {
	conn, err := writeToCmdSocket(`{"module":"screencast","action":"get_frame"}`)
	if err != nil {
		return nil, err
	}
	defer conn.Close()

	if err := conn.SetReadDeadline(time.Now().Add(screenFrameTimeout)); err != nil {
		return nil, err
	}

	return readScreenFrame(conn)
}

// readScreenFrame reassembles the frame from chunk datagrams read from conn, until all bytes arrived.
func readScreenFrame(conn net.Conn) ([]byte, error) {
	frame := make([]byte, screenFrameSize)
	gotOffsets := make(map[int]bool)
	received := 0
	buf := make([]byte, 64*1024)

	for received < screenFrameSize {
		n, err := conn.Read(buf)
		if err != nil {
			return nil, fmt.Errorf("read frame chunk (got %d of %d bytes): %w", received, screenFrameSize, err)
		}

		if n < screenChunkHeader {
			return nil, fmt.Errorf("frame chunk too short: %d bytes", n)
		}

		offset := int(binary.BigEndian.Uint16(buf[0:2]))
		total := int(binary.BigEndian.Uint16(buf[2:4]))
		data := buf[screenChunkHeader:n]

		if total != screenFrameSize || offset+len(data) > screenFrameSize {
			return nil, fmt.Errorf("invalid frame chunk: offset %d, size %d, total %d", offset, len(data), total)
		}

		if gotOffsets[offset] { // duplicate - don't count it twice
			continue
		}
		gotOffsets[offset] = true

		copy(frame[offset:], data)
		received += len(data)
	}

	return frame, nil
}

func (s *Server) handleGetScreen(w http.ResponseWriter, r *http.Request) {
	logDebugf("handleGetScreen %s %s", r.Method, r.URL.Path)

	data, err := requestScreenFrame()
	if err != nil {
		logErrorf("handleGetScreen - failed to get frame from core: %v", err)
		http.Error(w, "screen frame not available", http.StatusServiceUnavailable)
		return
	}

	// Set response headers
	w.Header().Set("Content-Type", "application/binary")
	w.Header().Set("Cache-Control", "no-cache, no-store")
	w.Header().Set("Pragma", "no-cache")
	w.Header().Set("Content-Length", fmt.Sprintf("%d", len(data)))

	// Write the data
	w.WriteHeader(http.StatusOK)
	if _, err := w.Write(data); err != nil {
		logErrorf("handleGetScreen - failed to write response: %v", err)
	}
}

func (s *Server) handlePostScreenScreenshot(w http.ResponseWriter, r *http.Request) {
	logDebugf("handlePostScreenScreenshot %s %s", r.Method, r.URL.Path)

	// Send screenshot command to core (log error but don't fail the request)
	cmd := map[string]string{"module": "screencast", "action": "do_screenshot"}
	cmdJSON, err := json.Marshal(cmd)
	if err != nil {
		logErrorf("handlePostScreenScreenshot - error marshaling command: %v", err)
	} else {
		if err := sendToCmdSocket(string(cmdJSON)); err != nil {
			logErrorf("handlePostScreenScreenshot - failed to send cmd socket: %v", err)
		}
	}

	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) handlePostScreenVBL(w http.ResponseWriter, r *http.Request) {
	logDebugf("handlePostScreenVBL %s %s", r.Method, r.URL.Path)

	mac := chi.URLParam(r, "mac")
	macWithoutColons := stripMACColons(mac)

	// Parse JSON payload to get boolean value
	var payload struct {
		Value bool `json:"value"`
	}
	if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
		http.Error(w, "invalid request body", http.StatusBadRequest)
		return
	}

	// Get SETTINGS_DIR from environment, default to current directory if not set
	settingsDir := getSettingsDir()

	// Build path to VBL settings file: SETTINGS_DIR/{mac}/vbl
	deviceDir := filepath.Join(settingsDir, macWithoutColons)
	vblFile := filepath.Join(deviceDir, "vbl")

	// Create device directory if it doesn't exist
	if err := os.MkdirAll(deviceDir, 0o755); err != nil {
		logErrorf("handlePostScreenVBL - error creating device directory %s: %v", deviceDir, err)
		http.Error(w, "cannot create device directory", http.StatusInternalServerError)
		return
	}

	// Convert boolean to 0 or 1
	vblValue := "0"
	if payload.Value {
		vblValue = "1"
	}

	// Write VBL value to file (0 or 1)
	if err := os.WriteFile(vblFile, []byte(vblValue), 0o644); err != nil {
		logErrorf("handlePostScreenVBL - error writing file %s: %v", vblFile, err)
		http.Error(w, "cannot write vbl", http.StatusInternalServerError)
		return
	}

	// Send VBL command to core (log error but don't fail the request)
	vblInt := 0
	if payload.Value {
		vblInt = 1
	}
	cmd := map[string]interface{}{"module": "screencast", "action": "vbl", "value": vblInt}
	cmdJSON, err := json.Marshal(cmd)
	if err != nil {
		logErrorf("handlePostScreenVBL - error marshaling command: %v", err)
	} else {
		if err := sendToCmdSocket(string(cmdJSON)); err != nil {
			logErrorf("handlePostScreenVBL - failed to send cmd socket: %v", err)
		}
	}

	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) handleGetScreenVBL(w http.ResponseWriter, r *http.Request) {
	logDebugf("handleGetScreenVBL %s %s", r.Method, r.URL.Path)

	mac := chi.URLParam(r, "mac")
	macWithoutColons := stripMACColons(mac)

	// Get SETTINGS_DIR from environment, default to current directory if not set
	settingsDir := getSettingsDir()

	// Build path to VBL settings file: SETTINGS_DIR/{mac}/vbl
	deviceDir := filepath.Join(settingsDir, macWithoutColons)
	vblFile := filepath.Join(deviceDir, "vbl")

	// Read VBL settings from file if it exists
	content, err := os.ReadFile(vblFile)
	if err != nil {
		// File doesn't exist, return false
		response := map[string]bool{
			"vbl": false,
		}
		writeJSON(w, http.StatusOK, response)
		return
	}

	// Parse 0 or 1 and convert to boolean
	vblStr := strings.TrimSpace(string(content))
	vblBool := false
	if vblStr == "1" {
		vblBool = true
	} else if vblStr == "0" {
		vblBool = false
	} else {
		// If file contains something else, try to parse as integer
		if val, err := strconv.Atoi(vblStr); err == nil && val != 0 {
			vblBool = true
		}
	}

	// Return VBL as boolean
	response := map[string]bool{
		"vbl": vblBool,
	}
	writeJSON(w, http.StatusOK, response)
}
