package main

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"
)

// Screen-related handlers.

// Screencast frame from ce_cores: resolution (1) + palette (512, room for 256 TT colors) + screen data, which is
// 32000 bytes for ST and 153600 for TT resolutions - see SCREENCAST_* in ce_cores screencastacsicommand.h.
// It's requested by 'get_frame' on the command socket and comes back as chunk datagrams of
// offset (4 bytes, big endian) + frame size (4 bytes, big endian) + frame sequence number (4 bytes, big endian) + data.
const (
	screenFrameMinSize = 1 + 512 + 32000
	screenFrameMaxSize = 1 + 512 + 153600
	screenChunkHeader  = 12
	screenFrameTimeout = 500 * time.Millisecond
)

// Screencast frames per second, stored as SCREENCAST_FPS in the device settings dir. ce_cores reads it on every
// screencast config request from CE_DD.PRG (every 3 seconds) and sends it as VBL frame skip, so a change applies while running.
// Limits and default must match SCREENCAST_FPS_* in ce_cores screencastacsicommand.h.
const (
	screencastFpsKey     = "SCREENCAST_FPS"
	screencastFpsMin     = 1
	screencastFpsMax     = 25
	screencastFpsDefault = 3
)

// requestScreenFrame gets the latest frame from ce_cores. With live set, the frame is for a running screencast, which
// keeps ST sending frames (it stops a few seconds after the last live request).
func requestScreenFrame(live bool) ([]byte, uint32, error) {
	request := `{"module":"screencast","action":"get_frame"}`
	if live {
		request = `{"module":"screencast","action":"get_frame","live":1}`
	}

	conn, err := writeToCmdSocket(request)
	if err != nil {
		return nil, 0, err
	}
	defer conn.Close()

	if err := conn.SetReadDeadline(time.Now().Add(screenFrameTimeout)); err != nil {
		return nil, 0, err
	}

	return readScreenFrame(conn)
}

// readScreenFrame reassembles the frame from chunk datagrams read from conn, until all bytes arrived.
// Returns also the frame sequence number - it changes with each new frame from ST.
func readScreenFrame(conn net.Conn) ([]byte, uint32, error) {
	var frame []byte // allocated by the first chunk, all chunks must tell the same frame size and sequence number
	var seq uint32
	gotOffsets := make(map[int]bool)
	received := 0
	buf := make([]byte, 64*1024)

	for frame == nil || received < len(frame) {
		n, err := conn.Read(buf)
		if err != nil {
			return nil, 0, fmt.Errorf("read frame chunk (got %d of %d bytes): %w", received, len(frame), err)
		}

		if n < screenChunkHeader {
			return nil, 0, fmt.Errorf("frame chunk too short: %d bytes", n)
		}

		offset := int(binary.BigEndian.Uint32(buf[0:4]))
		total := int(binary.BigEndian.Uint32(buf[4:8]))
		chunkSeq := binary.BigEndian.Uint32(buf[8:12])
		data := buf[screenChunkHeader:n]

		if frame == nil {
			seq = chunkSeq
			if total < screenFrameMinSize || total > screenFrameMaxSize {
				return nil, 0, fmt.Errorf("invalid frame size %d", total)
			}
			frame = make([]byte, total)
		}

		if total != len(frame) || chunkSeq != seq || offset+len(data) > len(frame) {
			return nil, 0, fmt.Errorf("invalid frame chunk: offset %d, size %d, total %d", offset, len(data), total)
		}

		if gotOffsets[offset] { // duplicate - don't count it twice
			continue
		}
		gotOffsets[offset] = true

		copy(frame[offset:], data)
		received += len(data)
	}

	return frame, seq, nil
}

func (s *Server) handleGetScreen(w http.ResponseWriter, r *http.Request) {
	logDebugf("handleGetScreen %s %s", r.Method, r.URL.Path)

	data, seq, err := requestScreenFrame(r.URL.Query().Get("live") == "1")
	if err != nil {
		logErrorf("handleGetScreen - failed to get frame from core: %v", err)
		http.Error(w, "screen frame not available", http.StatusServiceUnavailable)
		return
	}

	// Set response headers
	w.Header().Set("Content-Type", "application/binary")
	w.Header().Set("X-Frame-Seq", strconv.FormatUint(uint64(seq), 10)) // changes with each new frame from ST
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

// handlePostScreenStop stops the screencast right away, without waiting for the live frames timeout
func (s *Server) handlePostScreenStop(w http.ResponseWriter, r *http.Request) {
	logDebugf("handlePostScreenStop %s %s", r.Method, r.URL.Path)

	if err := sendToCmdSocket(`{"module":"screencast","action":"screencast_stop"}`); err != nil {
		logErrorf("handlePostScreenStop - failed to send cmd socket: %v", err)
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

// screencastDeviceDir returns the settings dir of the device in the {mac} URL param, or "" when the MAC isn't valid
func screencastDeviceDir(r *http.Request) string {
	// chi gives the param still escaped when the path had escaped chars, e.g. "F4%3A12%3A..." from encodeURIComponent()
	mac, err := url.PathUnescape(chi.URLParam(r, "mac"))
	if err != nil {
		return ""
	}

	mac = strings.ToUpper(stripMACColons(mac))
	if !isValidMACFormat(mac) {
		return ""
	}
	return filepath.Join(getSettingsDir(), mac)
}

func readScreencastFps(deviceDir string) int {
	content, err := os.ReadFile(filepath.Join(deviceDir, screencastFpsKey))
	if err != nil {
		return screencastFpsDefault
	}
	fps, err := strconv.Atoi(strings.TrimSpace(string(content)))
	if err != nil {
		return screencastFpsDefault
	}
	return clampScreencastFps(fps)
}

func clampScreencastFps(fps int) int {
	if fps < screencastFpsMin {
		return screencastFpsMin
	}
	if fps > screencastFpsMax {
		return screencastFpsMax
	}
	return fps
}

func (s *Server) handleGetScreenFps(w http.ResponseWriter, r *http.Request) {
	logDebugf("handleGetScreenFps %s %s", r.Method, r.URL.Path)

	deviceDir := screencastDeviceDir(r)
	if deviceDir == "" {
		http.Error(w, "invalid MAC address", http.StatusBadRequest)
		return
	}

	writeJSON(w, http.StatusOK, map[string]int{"fps": readScreencastFps(deviceDir)})
}

// handlePutScreenFps stores the screencast fps (clamped to limits), replies with the stored value
func (s *Server) handlePutScreenFps(w http.ResponseWriter, r *http.Request) {
	logDebugf("handlePutScreenFps %s %s", r.Method, r.URL.Path)

	deviceDir := screencastDeviceDir(r)
	if deviceDir == "" {
		http.Error(w, "invalid MAC address", http.StatusBadRequest)
		return
	}

	var payload struct {
		Fps *int `json:"fps"`
	}
	if err := json.NewDecoder(r.Body).Decode(&payload); err != nil || payload.Fps == nil {
		http.Error(w, "invalid request body", http.StatusBadRequest)
		return
	}
	fps := clampScreencastFps(*payload.Fps)

	if err := os.MkdirAll(deviceDir, 0o755); err != nil {
		logErrorf("handlePutScreenFps - error creating device directory %s: %v", deviceDir, err)
		http.Error(w, "cannot create device directory", http.StatusInternalServerError)
		return
	}

	// atomic - ce_cores reads it from another process at any time
	if err := writeFileAtomic(filepath.Join(deviceDir, screencastFpsKey), []byte(strconv.Itoa(fps)+"\n"), 0o644); err != nil {
		logErrorf("handlePutScreenFps - error writing setting: %v", err)
		http.Error(w, "cannot write setting", http.StatusInternalServerError)
		return
	}

	writeJSON(w, http.StatusOK, map[string]int{"fps": fps})
}
