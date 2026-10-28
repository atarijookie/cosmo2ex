package main

import (
	"encoding/json"
	"net/http"
)

// HID-related handlers - mouse and keyboard of the web UI go to the input socket of ce_cores.

func (s *Server) handlePostHIDMouse(w http.ResponseWriter, r *http.Request) {
	logDebugf("handlePostHIDMouse %s %s", r.Method, r.URL.Path)
	s.handleHID(w, r, mousePayloadToEvents)
}

func (s *Server) handlePostHIDKeyboard(w http.ResponseWriter, r *http.Request) {
	logDebugf("handlePostHIDKeyboard %s %s", r.Method, r.URL.Path)
	s.handleHID(w, r, keyboardPayloadToEvents)
}

func (s *Server) handleHID(w http.ResponseWriter, r *http.Request, toEvents func(map[string]interface{}) ([]byte, error)) {
	var payload map[string]interface{}
	if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
		http.Error(w, "invalid request body", http.StatusBadRequest)
		return
	}

	events, err := toEvents(payload)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}

	// send to input socket (log error but don't fail request - like when ce_cores is not running)
	if len(events) > 0 {
		if err := s.input.send(events); err != nil {
			logErrorf("handleHID - failed to send to input socket: %v", err)
		}
	}

	w.WriteHeader(http.StatusNoContent)
}
