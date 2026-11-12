package main

import (
	"encoding/json"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

const (
	defaultKeys0 = "A%S%D%W%LSHIFT"
	defaultKeys1 = "LEFT%DOWN%RIGHT%UP%RSHIFT"
)

type IKBDSettings struct {
	JoyFirstIs0           int    `json:"joy_first_is_0"`
	MouseWheelAsKeys      int    `json:"mouse_wheel_as_keys"`
	KeyboardJoy0          int    `json:"keyboard_joy0"`
	KeyboardJoy1          int    `json:"keyboard_joy1"`
	KeyboardKeysSettings0 string `json:"keyboard_keys_settings0"`
	KeyboardKeysSettings1 string `json:"keyboard_keys_settings1"`
}

// IKBD settings are one set for the whole machine, stored right in SETTINGS_DIR - ce_cores has one IKBD emulation, and the
// keyboard / mouse / joysticks of this machine and of the web UI go to all connected STs. The names and defaults must
// match what ce_cores reads (ce_cores/ikbd/ikbd_general.cpp loadSettings(), ce_cores/ikbd/keybjoys.cpp loadKeys()).
// The {mac} in the /ikbd/{mac} route is ignored, it is accepted so older pages keep working.
func ikbdSettingsDir() string {
	settingsDir := os.Getenv("SETTINGS_DIR")
	if settingsDir == "" {
		settingsDir = "."
	}
	return settingsDir
}

func (s *Server) handleGetIKBD(w http.ResponseWriter, r *http.Request) {
	logDebugf("handleGetIKBD %s %s", r.Method, r.URL.Path)

	dir := ikbdSettingsDir()

	resp := IKBDSettings{
		JoyFirstIs0:      readSettingIntDefault(dir, "JOY_FIRST_IS_0", 0),
		MouseWheelAsKeys: readSettingIntDefault(dir, "MOUSE_WHEEL_AS_KEYS", 1), // on in ce_cores when not stored
		KeyboardJoy0:     readSettingIntDefault(dir, "KEYBOARD_JOY0", 0),
		KeyboardJoy1:     readSettingIntDefault(dir, "KEYBOARD_JOY1", 0),
		// older ce_cores stored the keys as KEYBOARD_KEYS_JOY0 / 1 - used when the current name is not stored yet (like ce_cores does)
		KeyboardKeysSettings0: readKeySettingsWithDefault(dir, "KEYBOARD_KEYS_SETTINGS0", readKeySettingsWithDefault(dir, "KEYBOARD_KEYS_JOY0", defaultKeys0)),
		KeyboardKeysSettings1: readKeySettingsWithDefault(dir, "KEYBOARD_KEYS_SETTINGS1", readKeySettingsWithDefault(dir, "KEYBOARD_KEYS_JOY1", defaultKeys1)),
	}

	writeJSON(w, http.StatusOK, resp)
}

func (s *Server) handlePutIKBD(w http.ResponseWriter, r *http.Request) {
	logDebugf("handlePutIKBD %s %s", r.Method, r.URL.Path)

	var payload IKBDSettings
	if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
		http.Error(w, "invalid request body", http.StatusBadRequest)
		return
	}

	if !isZeroOrOne(payload.JoyFirstIs0) ||
		!isZeroOrOne(payload.MouseWheelAsKeys) ||
		!isZeroOrOne(payload.KeyboardJoy0) ||
		!isZeroOrOne(payload.KeyboardJoy1) {
		http.Error(w, "numeric fields must be 0 or 1", http.StatusBadRequest)
		return
	}

	deviceDir := ikbdSettingsDir()

	if err := os.MkdirAll(deviceDir, 0o755); err != nil {
		logErrorf("handlePutIKBD - error creating directory %s: %v", deviceDir, err)
		http.Error(w, "cannot create device directory", http.StatusInternalServerError)
		return
	}

	if err := writeSettingInt(deviceDir, "JOY_FIRST_IS_0", payload.JoyFirstIs0); err != nil {
		http.Error(w, "cannot write setting", http.StatusInternalServerError)
		return
	}
	if err := writeSettingInt(deviceDir, "MOUSE_WHEEL_AS_KEYS", payload.MouseWheelAsKeys); err != nil {
		http.Error(w, "cannot write setting", http.StatusInternalServerError)
		return
	}
	if err := writeSettingInt(deviceDir, "KEYBOARD_JOY0", payload.KeyboardJoy0); err != nil {
		http.Error(w, "cannot write setting", http.StatusInternalServerError)
		return
	}
	if err := writeSettingInt(deviceDir, "KEYBOARD_JOY1", payload.KeyboardJoy1); err != nil {
		http.Error(w, "cannot write setting", http.StatusInternalServerError)
		return
	}
	if err := writeSettingString(deviceDir, "KEYBOARD_KEYS_SETTINGS0", payload.KeyboardKeysSettings0); err != nil {
		http.Error(w, "cannot write setting", http.StatusInternalServerError)
		return
	}
	if err := writeSettingString(deviceDir, "KEYBOARD_KEYS_SETTINGS1", payload.KeyboardKeysSettings1); err != nil {
		http.Error(w, "cannot write setting", http.StatusInternalServerError)
		return
	}

	// Send reload command to core
	cmd := map[string]string{"module": "ikbd", "action": "reload"}
	if cmdJSON, err := json.Marshal(cmd); err == nil {
		if err := sendToCmdSocket(string(cmdJSON)); err != nil {
			logErrorf("handlePutIKBD - failed to send cmd socket: %v", err)
		}
	}

	w.WriteHeader(http.StatusNoContent)
}

func readSettingString(deviceDir, fileName string) string {
	content, err := os.ReadFile(filepath.Join(deviceDir, fileName))
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(content))
}

func readKeySettingsWithDefault(deviceDir, fileName, def string) string {
	content, err := os.ReadFile(filepath.Join(deviceDir, fileName))
	if err != nil {
		return def
	}
	val := strings.TrimSpace(string(content))
	if val == "" {
		return def
	}
	return val
}

func writeSettingInt(deviceDir, fileName string, value int) error {
	return os.WriteFile(filepath.Join(deviceDir, fileName), []byte(strconv.Itoa(value)), 0o644)
}

func writeSettingString(deviceDir, fileName, value string) error {
	return os.WriteFile(filepath.Join(deviceDir, fileName), []byte(value), 0o644)
}

func isZeroOrOne(v int) bool {
	return v == 0 || v == 1
}
