package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
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
