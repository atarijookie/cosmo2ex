package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// IKBD settings: one set for the whole machine, stored right in SETTINGS_DIR with the names and defaults ce_cores reads
// (ce_cores/ikbd/ikbd_general.cpp loadSettings(), ce_cores/ikbd/keybjoys.cpp loadKeys())

func getIKBD(t *testing.T, s *Server, path string) IKBDSettings {
	t.Helper()
	rec := httptest.NewRecorder()
	s.handleGetIKBD(rec, httptest.NewRequest(http.MethodGet, path, nil))
	if rec.Code != http.StatusOK {
		t.Fatalf("GET %s status %d", path, rec.Code)
	}
	var got IKBDSettings
	if err := json.Unmarshal(rec.Body.Bytes(), &got); err != nil {
		t.Fatalf("GET %s: %v", path, err)
	}
	return got
}

func putIKBD(t *testing.T, s *Server, path, body string) int {
	t.Helper()
	rec := httptest.NewRecorder()
	s.handlePutIKBD(rec, httptest.NewRequest(http.MethodPut, path, strings.NewReader(body)))
	return rec.Code
}

func TestIKBDDefaultsMatchCore(t *testing.T) {
	t.Setenv("SETTINGS_DIR", t.TempDir())
	t.Setenv("CMD_UDP_PORT", "") // no ce_cores to tell about the reload
	s := &Server{}

	got := getIKBD(t, s, "/ikbd")
	want := IKBDSettings{
		JoyFirstIs0:           0,
		MouseWheelAsKeys:      1, // ce_cores: MOUSE_WHEEL_AS_KEYS on when not stored
		KeyboardJoy0:          0,
		KeyboardJoy1:          0,
		KeyboardKeysSettings0: "A%S%D%W%LSHIFT",
		KeyboardKeysSettings1: "LEFT%DOWN%RIGHT%UP%RSHIFT",
	}
	if got != want {
		t.Errorf("defaults = %+v, want %+v", got, want)
	}
}

func TestIKBDStoredInSettingsDirNotPerDevice(t *testing.T) {
	dir := t.TempDir()
	t.Setenv("SETTINGS_DIR", dir)
	t.Setenv("CMD_UDP_PORT", "")
	s := &Server{}

	body := `{"joy_first_is_0":1,"mouse_wheel_as_keys":0,"keyboard_joy0":1,"keyboard_joy1":0,` +
		`"keyboard_keys_settings0":"A%S%D%W%LCTRL","keyboard_keys_settings1":"LEFT%DOWN%RIGHT%UP%RCTRL"}`
	if code := putIKBD(t, s, "/ikbd/2C:CF:67:C4:B1:E5", body); code != http.StatusNoContent { // mac of older pages ignored
		t.Fatalf("PUT status %d", code)
	}

	// files ce_cores reads, right in SETTINGS_DIR
	for name, want := range map[string]string{
		"JOY_FIRST_IS_0":          "1",
		"MOUSE_WHEEL_AS_KEYS":     "0",
		"KEYBOARD_JOY0":           "1",
		"KEYBOARD_JOY1":           "0",
		"KEYBOARD_KEYS_SETTINGS0": "A%S%D%W%LCTRL",
		"KEYBOARD_KEYS_SETTINGS1": "LEFT%DOWN%RIGHT%UP%RCTRL",
	} {
		content, err := os.ReadFile(filepath.Join(dir, name))
		if err != nil {
			t.Errorf("%s not stored: %v", name, err)
			continue
		}
		if strings.TrimSpace(string(content)) != want {
			t.Errorf("%s = %q, want %q", name, content, want)
		}
	}
	if _, err := os.Stat(filepath.Join(dir, "2CCF67C4B1E5")); !os.IsNotExist(err) {
		t.Errorf("per device dir created: %v", err)
	}

	got := getIKBD(t, s, "/ikbd")
	if got.JoyFirstIs0 != 1 || got.MouseWheelAsKeys != 0 || got.KeyboardJoy0 != 1 || got.KeyboardKeysSettings0 != "A%S%D%W%LCTRL" {
		t.Errorf("GET after PUT = %+v", got)
	}
}

// values written by ce_cores itself: hotkeys store "1\n" / "0\n", older ce_cores stored the keys as KEYBOARD_KEYS_JOY0 / 1
func TestIKBDReadsValuesWrittenByCore(t *testing.T) {
	dir := t.TempDir()
	t.Setenv("SETTINGS_DIR", dir)
	s := &Server{}

	write := func(name, value string) {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(value), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	write("KEYBOARD_JOY1", "1\n")
	write("MOUSE_WHEEL_AS_KEYS", "0\n")
	write("KEYBOARD_KEYS_JOY0", "J%K%L%I%SPACE")

	got := getIKBD(t, s, "/ikbd")
	if got.KeyboardJoy1 != 1 || got.MouseWheelAsKeys != 0 {
		t.Errorf("hotkey values not read: %+v", got)
	}
	if got.KeyboardKeysSettings0 != "J%K%L%I%SPACE" {
		t.Errorf("old key name not used: %q", got.KeyboardKeysSettings0)
	}

	write("KEYBOARD_KEYS_SETTINGS0", "A%S%D%W%LCTRL") // current name wins over the old one
	if got := getIKBD(t, s, "/ikbd"); got.KeyboardKeysSettings0 != "A%S%D%W%LCTRL" {
		t.Errorf("current key name not preferred: %q", got.KeyboardKeysSettings0)
	}
}
