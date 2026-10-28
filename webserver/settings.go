package main

import (
	"encoding/json"
	"fmt"
	"net/http"
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
