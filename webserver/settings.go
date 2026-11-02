package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// Settings page handlers - runtime settings of ce_cores and webserver.

const coreReplyTimeout = 500 * time.Millisecond

type LogLevelResponse struct {
	Webserver int    `json:"webserver"`
	Core      *int   `json:"core"`                // nil when ce_cores didn't reply (not running?)
	CoreError string `json:"coreError,omitempty"` // why the core log level is unknown
}

type LogLevelRequest struct {
	LogLevel *int `json:"loglevel"`
}

// requestCoreLogLevel asks ce_cores for its current log level over the command socket
func requestCoreLogLevel() (int, error) {
	conn, err := writeToCmdSocket(`{"module":"all","action":"get_loglevel"}`)
	if err != nil {
		return 0, err
	}
	defer conn.Close()

	if err := conn.SetReadDeadline(time.Now().Add(coreReplyTimeout)); err != nil {
		return 0, err
	}

	buf := make([]byte, 1024)
	n, err := conn.Read(buf)
	if err != nil {
		return 0, fmt.Errorf("read reply: %w", err)
	}

	var reply struct {
		LogLevel *int `json:"loglevel"`
	}
	if err := json.Unmarshal(buf[:n], &reply); err != nil {
		return 0, fmt.Errorf("parse reply %q: %w", string(buf[:n]), err)
	}
	if reply.LogLevel == nil {
		return 0, fmt.Errorf("reply %q has no loglevel", string(buf[:n]))
	}
	return *reply.LogLevel, nil
}

func currentLogLevels() LogLevelResponse {
	resp := LogLevelResponse{Webserver: getLogLevel()}

	if level, err := requestCoreLogLevel(); err == nil {
		resp.Core = &level
	} else {
		logWarningf("currentLogLevels - failed to get core log level: %v", err)
		resp.CoreError = err.Error()
	}
	return resp
}

func (s *Server) handleGetLogLevel(w http.ResponseWriter, r *http.Request) {
	logDebugf("handleGetLogLevel %s %s", r.Method, r.URL.Path)
	writeJSON(w, http.StatusOK, currentLogLevels())
}

// handlePutLogLevel sets the same log level to webserver and ce_cores, replies with the levels after the change
func (s *Server) handlePutLogLevel(w http.ResponseWriter, r *http.Request) {
	logDebugf("handlePutLogLevel %s %s", r.Method, r.URL.Path)

	var req LogLevelRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.LogLevel == nil {
		logWarningf("handlePutLogLevel - invalid request body")
		http.Error(w, "invalid request body", http.StatusBadRequest)
		return
	}

	level := clampLogLevel(*req.LogLevel)
	setLogLevel(level)

	cmd := map[string]interface{}{"module": "all", "action": "set_loglevel", "loglevel": level}
	if cmdJSON, err := json.Marshal(cmd); err == nil {
		if err := sendToCmdSocket(string(cmdJSON)); err != nil {
			logErrorf("handlePutLogLevel - failed to send cmd socket: %v", err)
		}
	}

	// ce_cores handles commands one by one, so this get comes after the set above
	writeJSON(w, http.StatusOK, currentLogLevels())
}

//-------------------------------------------------------------------------------------------------
// SLM laser printer paper size - global setting, ce_cores reads it on each printer command (hdd/slm/slmprinter.cpp)

const slmPaperFileName = "SLM_PAPER_SIZE" // same as SLM_PAPER_SETTING in ce_cores
const slmPaperDefault = "letter"          // same as SLM_PAPER_DEFAULT in ce_cores

var slmPaperSizes = []string{"letter", "legal", "a4", "b5"}

type SlmSettings struct {
	Paper string `json:"paper"`
}

func slmPaperFilePath() string {
	settingsDir := os.Getenv("SETTINGS_DIR")
	if settingsDir == "" {
		settingsDir = "."
	}
	return filepath.Join(settingsDir, slmPaperFileName)
}

func isValidSlmPaper(paper string) bool {
	for _, p := range slmPaperSizes {
		if p == paper {
			return true
		}
	}
	return false
}

// loadSlmPaper returns the stored paper size, default if not stored or not valid
func loadSlmPaper() string {
	content, err := os.ReadFile(slmPaperFilePath())
	if err != nil {
		return slmPaperDefault
	}

	paper := strings.TrimSpace(string(content))
	if !isValidSlmPaper(paper) {
		return slmPaperDefault
	}
	return paper
}

func (s *Server) handleGetSlm(w http.ResponseWriter, r *http.Request) {
	logDebugf("handleGetSlm %s %s", r.Method, r.URL.Path)
	writeJSON(w, http.StatusOK, SlmSettings{Paper: loadSlmPaper()})
}

func (s *Server) handlePutSlm(w http.ResponseWriter, r *http.Request) {
	logDebugf("handlePutSlm %s %s", r.Method, r.URL.Path)

	var req SlmSettings
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || !isValidSlmPaper(req.Paper) {
		logWarningf("handlePutSlm - invalid request body")
		http.Error(w, "paper must be one of: "+strings.Join(slmPaperSizes, ", "), http.StatusBadRequest)
		return
	}

	// via temp file + rename - ce_cores might read it just now
	path := slmPaperFilePath()
	tmpPath := path + ".tmp"
	if err := os.WriteFile(tmpPath, []byte(req.Paper), 0o644); err != nil {
		logErrorf("handlePutSlm - error writing file %s: %v", tmpPath, err)
		http.Error(w, "cannot write setting", http.StatusInternalServerError)
		return
	}
	if err := os.Rename(tmpPath, path); err != nil {
		logErrorf("handlePutSlm - error renaming %s: %v", tmpPath, err)
		http.Error(w, "cannot write setting", http.StatusInternalServerError)
		return
	}

	writeJSON(w, http.StatusOK, SlmSettings{Paper: req.Paper})
}
