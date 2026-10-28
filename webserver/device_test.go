package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"

	"github.com/go-chi/chi/v5"
)

// listDevices sets up SETTINGS_DIR with dirs of these devices and CONNECTED_DEVICES_FILE with this content
// (nil = no file), then returns the devices from /devices?query
func listDevices(t *testing.T, deviceDirs []string, connectedFile *string, query string) []Device {
	t.Helper()

	settingsDir := t.TempDir()
	t.Setenv("SETTINGS_DIR", settingsDir)
	for _, dir := range deviceDirs {
		if err := os.Mkdir(filepath.Join(settingsDir, dir), 0o755); err != nil {
			t.Fatal(err)
		}
	}

	connectedPath := filepath.Join(t.TempDir(), "connected_devices")
	t.Setenv("CONNECTED_DEVICES_FILE", connectedPath)
	if connectedFile != nil {
		if err := os.WriteFile(connectedPath, []byte(*connectedFile), 0o644); err != nil {
			t.Fatal(err)
		}
	}

	s := &Server{}
	req := httptest.NewRequest(http.MethodGet, "/devices"+query, nil)
	w := httptest.NewRecorder()
	s.handleListDevices(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status %d, want 200", w.Code)
	}

	var devices []Device
	if err := json.Unmarshal(w.Body.Bytes(), &devices); err != nil {
		t.Fatal(err)
	}
	return devices
}

func connectedByMAC(devices []Device) map[string]bool {
	m := make(map[string]bool)
	for _, d := range devices {
		m[d.MAC] = d.Connected
	}
	return m
}

func TestListDevicesConnectedFromFile(t *testing.T) {
	content := "2CCF67C4B1E5\nf412fad5c554\n\nnot-a-mac\n" // lower case and junk lines are fine too
	devices := listDevices(t, []string{"2CCF67C4B1E5", "CA4E3EA40030", "F412FAD5C554", "000000000000"}, &content, "")

	got := connectedByMAC(devices)
	want := map[string]bool{"2C:CF:67:C4:B1:E5": true, "CA:4E:3E:A4:00:30": false, "F4:12:FA:D5:C5:54": true}

	if len(got) != len(want) {
		t.Fatalf("got %v, want %v", got, want)
	}
	for mac, connected := range want {
		if got[mac] != connected {
			t.Errorf("%s connected = %v, want %v", mac, got[mac], connected)
		}
	}
}

func TestListDevicesNoConnectedFile(t *testing.T) {
	devices := listDevices(t, []string{"2CCF67C4B1E5"}, nil, "") // ce_cores not running

	if len(devices) != 1 || devices[0].Connected {
		t.Fatalf("got %+v, want one not connected device", devices)
	}
}

func TestListDevicesConnectedOnlyFilter(t *testing.T) {
	content := "CA4E3EA40030\n"
	devices := listDevices(t, []string{"2CCF67C4B1E5", "CA4E3EA40030"}, &content, "?connected=true")

	if len(devices) != 1 || devices[0].MAC != "CA:4E:3E:A4:00:30" || !devices[0].Connected {
		t.Fatalf("got %+v, want just CA:4E:3E:A4:00:30", devices)
	}
}

// deleteDevice sets up SETTINGS_DIR with these device dirs and CONNECTED_DEVICES_FILE with this content,
// calls DELETE /device/{mac} and returns the status code and the settings dir
func deleteDevice(t *testing.T, deviceDirs []string, connected string, mac string) (int, string) {
	t.Helper()

	settingsDir := t.TempDir()
	t.Setenv("SETTINGS_DIR", settingsDir)
	for _, dir := range deviceDirs {
		if err := os.MkdirAll(filepath.Join(settingsDir, dir, "floppy"), 0o755); err != nil {
			t.Fatal(err)
		}
	}

	connectedPath := filepath.Join(t.TempDir(), "connected_devices")
	t.Setenv("CONNECTED_DEVICES_FILE", connectedPath)
	if err := os.WriteFile(connectedPath, []byte(connected), 0o644); err != nil {
		t.Fatal(err)
	}

	r := chi.NewRouter()
	s := &Server{}
	r.Delete("/device/{mac}", s.handleDeleteDevice)

	req := httptest.NewRequest(http.MethodDelete, "/device/"+mac, nil)
	w := httptest.NewRecorder()
	r.ServeHTTP(w, req)
	return w.Code, settingsDir
}

func dirExists(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.IsDir()
}

func TestDeleteDeviceNotConnected(t *testing.T) {
	code, dir := deleteDevice(t, []string{"AABBCCDDEE01", "AABBCCDDEE02"}, "AABBCCDDEE02\n", "aa:bb:cc:dd:ee:01")
	if code != http.StatusNoContent {
		t.Fatalf("status %d, want 204", code)
	}
	if dirExists(filepath.Join(dir, "AABBCCDDEE01")) {
		t.Fatal("device dir still exists")
	}
	if !dirExists(filepath.Join(dir, "AABBCCDDEE02")) {
		t.Fatal("other device dir was deleted")
	}
}

func TestDeleteDeviceEscapedMAC(t *testing.T) {
	// page sends MAC from /devices through encodeURIComponent(), so colons come as %3A
	code, dir := deleteDevice(t, []string{"F412FAD5C554"}, "", "F4%3A12%3AFA%3AD5%3AC5%3A54")
	if code != http.StatusNoContent {
		t.Fatalf("status %d, want 204", code)
	}
	if dirExists(filepath.Join(dir, "F412FAD5C554")) {
		t.Fatal("device dir still exists")
	}
}

func TestDeleteDeviceConnected(t *testing.T) {
	code, dir := deleteDevice(t, []string{"AABBCCDDEE01"}, "AABBCCDDEE01\n", "AABBCCDDEE01")
	if code != http.StatusConflict {
		t.Fatalf("status %d, want 409", code)
	}
	if !dirExists(filepath.Join(dir, "AABBCCDDEE01")) {
		t.Fatal("connected device dir was deleted")
	}
}

func TestDeleteDeviceInvalidMAC(t *testing.T) {
	for _, mac := range []string{"..", "000000000000", "AABBCCDDEE0G", "tls"} {
		if code, _ := deleteDevice(t, []string{"AABBCCDDEE01"}, "", mac); code != http.StatusBadRequest {
			t.Errorf("mac %q: status %d, want 400", mac, code)
		}
	}
}

func TestDeleteDeviceNotFound(t *testing.T) {
	if code, _ := deleteDevice(t, []string{"AABBCCDDEE01"}, "", "AABBCCDDEE09"); code != http.StatusNotFound {
		t.Fatalf("status %d, want 404", code)
	}
}
