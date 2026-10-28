package main

import (
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"strings"

	"github.com/go-chi/chi/v5"
)

// Device-related handlers.

// isValidMACFormat checks if a string is a valid MAC address format (12 hex characters)
func isValidMACFormat(s string) bool {
	matched, _ := regexp.MatchString(`^[0-9a-fA-F]{12}$`, s)
	return matched
}

// formatMACWithColons converts "aabbccddee01" to "aa:bb:cc:dd:ee:01"
func formatMACWithColons(mac string) string {
	if len(mac) != 12 {
		return mac
	}
	return mac[0:2] + ":" + mac[2:4] + ":" + mac[4:6] + ":" + mac[6:8] + ":" + mac[8:10] + ":" + mac[10:12]
}

// connectedMACs returns MACs (12 hex chars, upper case, like the device dirs in SETTINGS_DIR) of the devices
// which have at least one HDD / FDD / IKBD connection to ce_cores right now. ce_cores writes them to
// CONNECTED_DEVICES_FILE, one per line; no file (e.g. ce_cores not running) = nothing is connected.
func connectedMACs() map[string]bool {
	macs := make(map[string]bool)

	path := os.Getenv("CONNECTED_DEVICES_FILE")
	if path == "" {
		return macs
	}

	content, err := os.ReadFile(path)
	if err != nil {
		return macs
	}

	for _, line := range strings.Split(string(content), "\n") {
		mac := strings.ToUpper(strings.TrimSpace(line))
		if isValidMACFormat(mac) {
			macs[mac] = true
		}
	}
	return macs
}

func (s *Server) handleListDevices(w http.ResponseWriter, r *http.Request) {
	logDebugf("handleListDevices %s %s", r.Method, r.URL.Path)

	// Get SETTINGS_DIR from environment, default to current directory if not set
	settingsDir := os.Getenv("SETTINGS_DIR")
	if settingsDir == "" {
		settingsDir = "."
	}

	// Read directory entries
	entries, err := os.ReadDir(settingsDir)
	if err != nil {
		logErrorf("handleListDevices - error reading directory %s: %v", settingsDir, err)
		http.Error(w, "cannot read settings directory", http.StatusInternalServerError)
		return
	}

	connected := connectedMACs()

	// Filter directories that look like MAC addresses (12 hex characters)
	devices := make([]Device, 0)
	for _, entry := range entries {
		if !entry.IsDir() {
			continue
		}

		folderName := entry.Name()

		if(folderName == "000000000000") {
			continue;
		}

		if isValidMACFormat(folderName) {
			// Format MAC address with colons
			macFormatted := formatMACWithColons(folderName)

			// Read device_name file if it exists
			deviceNameFile := filepath.Join(settingsDir, folderName, "device_name")
			deviceName := folderName // Default to MAC without colons
			if content, err := os.ReadFile(deviceNameFile); err == nil {
				deviceName = strings.TrimSpace(string(content))
				// If file exists but is empty, use MAC without colons
				if deviceName == "" {
					deviceName = folderName
				}
			}

			devices = append(devices, Device{
				MAC:       macFormatted,
				Name:      deviceName,
				Connected: connected[strings.ToUpper(folderName)],
			})
		}
	}

	// Handle connected filter if requested
	connectedOnly := strings.EqualFold(r.URL.Query().Get("connected"), "true")
	if connectedOnly {
		filtered := make([]Device, 0)
		for _, d := range devices {
			if d.Connected {
				filtered = append(filtered, d)
			}
		}
		devices = filtered
	}

	writeJSON(w, http.StatusOK, devices)
}

func (s *Server) handleGetDeviceName(w http.ResponseWriter, r *http.Request) {
	logDebugf("handleGetDeviceName %s %s", r.Method, r.URL.Path)

	mac := chi.URLParam(r, "mac")
	// Strip colons from MAC for file path (MAC in URL might have colons)
	macWithoutColons := stripMACColons(mac)

	// Get SETTINGS_DIR from environment, default to current directory if not set
	settingsDir := os.Getenv("SETTINGS_DIR")
	if settingsDir == "" {
		settingsDir = "."
	}

	// Build path to device_name file: SETTINGS_DIR/{mac}/device_name
	deviceDir := filepath.Join(settingsDir, macWithoutColons)
	deviceNameFile := filepath.Join(deviceDir, "device_name")

	// Read device name from file
	content, err := os.ReadFile(deviceNameFile)
	if err != nil {
		// If file doesn't exist, return mac without colons
		w.Header().Set("Content-Type", "text/plain")
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte(macWithoutColons))
		return
	}

	// Return the device name as plain text
	name := strings.TrimSpace(string(content))
	w.Header().Set("Content-Type", "text/plain")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write([]byte(name))
}

func (s *Server) handlePutDeviceName(w http.ResponseWriter, r *http.Request) {
	logDebugf("handlePutDeviceName %s %s", r.Method, r.URL.Path)

	mac := chi.URLParam(r, "mac")
	// Strip colons from MAC for file path (MAC in URL might have colons)
	macWithoutColons := stripMACColons(mac)

	// Read the device name from request body
	body, err := io.ReadAll(r.Body)
	if err != nil {
		http.Error(w, "invalid request body", http.StatusBadRequest)
		return
	}
	deviceName := strings.TrimSpace(string(body))

	// Get SETTINGS_DIR from environment, default to current directory if not set
	settingsDir := os.Getenv("SETTINGS_DIR")
	if settingsDir == "" {
		settingsDir = "."
	}

	// Build path to device_name file: SETTINGS_DIR/{mac}/device_name
	deviceDir := filepath.Join(settingsDir, macWithoutColons)
	deviceNameFile := filepath.Join(deviceDir, "device_name")

	// Create device directory if it doesn't exist
	if err := os.MkdirAll(deviceDir, 0o755); err != nil {
		logErrorf("handlePutDeviceName - error creating device directory %s: %v", deviceDir, err)
		http.Error(w, "cannot create device directory", http.StatusInternalServerError)
		return
	}

	// Write device name to file
	if err := os.WriteFile(deviceNameFile, []byte(deviceName), 0o644); err != nil {
		logErrorf("handlePutDeviceName - error writing file %s: %v", deviceNameFile, err)
		http.Error(w, "cannot write device_name", http.StatusInternalServerError)
		return
	}

	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) handleGetDeviceFeatures(w http.ResponseWriter, r *http.Request) {
	logDebugf("handleGetDeviceFeatures %s %s", r.Method, r.URL.Path)

	mac := chi.URLParam(r, "mac")
	// Strip colons from MAC for file path (MAC in URL might have colons)
	macWithoutColons := stripMACColons(mac)

	// Get SETTINGS_DIR from environment, default to current directory if not set
	settingsDir := os.Getenv("SETTINGS_DIR")
	if settingsDir == "" {
		settingsDir = "."
	}

	// Build path to features file: SETTINGS_DIR/{mac}/features
	deviceDir := filepath.Join(settingsDir, macWithoutColons)
	featuresFile := filepath.Join(deviceDir, "features")

	// Read features file
	content, err := os.ReadFile(featuresFile)
	if err != nil || len(strings.TrimSpace(string(content))) == 0 {
		// File doesn't exist or is empty - return all features as true
		response := map[string]bool{
			"acsi": true,
			"scsi": true,
			"fdd":  true,
			"ikbd": true,
		}
		writeJSON(w, http.StatusOK, response)
		return
	}

	// Parse content to check for feature letters (case-insensitive)
	contentStr := strings.ToUpper(strings.TrimSpace(string(content)))

	response := map[string]bool{
		"acsi": strings.Contains(contentStr, "A"),
		"scsi": strings.Contains(contentStr, "S"),
		"fdd":  strings.Contains(contentStr, "F"),
		"ikbd": strings.Contains(contentStr, "I"),
	}

	writeJSON(w, http.StatusOK, response)
}

// handleDeleteDevice removes the whole settings dir of the device (SETTINGS_DIR/{mac}), but only when the device is not
// connected right now - ce_cores would just create the dir again while it's using the device.
func (s *Server) handleDeleteDevice(w http.ResponseWriter, r *http.Request) {
	logDebugf("handleDeleteDevice %s %s", r.Method, r.URL.Path)

	// chi gives the param still escaped when the path had escaped chars, e.g. "F4%3A12%3A..." from encodeURIComponent()
	mac, err := url.PathUnescape(chi.URLParam(r, "mac"))
	if err != nil {
		http.Error(w, "invalid MAC address", http.StatusBadRequest)
		return
	}

	mac = strings.ToUpper(stripMACColons(mac))
	if !isValidMACFormat(mac) || mac == "000000000000" { // only device dirs, never anything else in SETTINGS_DIR
		http.Error(w, "invalid MAC address", http.StatusBadRequest)
		return
	}

	if connectedMACs()[mac] {
		logWarningf("handleDeleteDevice - device %s is connected, not deleting", mac)
		http.Error(w, "device is connected, disconnect it first", http.StatusConflict)
		return
	}

	settingsDir := os.Getenv("SETTINGS_DIR")
	if settingsDir == "" {
		settingsDir = "."
	}

	// find the dir case insensitive - MAC in URL and dir name might differ in case
	entries, err := os.ReadDir(settingsDir)
	if err != nil {
		logErrorf("handleDeleteDevice - error reading directory %s: %v", settingsDir, err)
		http.Error(w, "cannot read settings directory", http.StatusInternalServerError)
		return
	}

	for _, entry := range entries {
		if !entry.IsDir() || !strings.EqualFold(entry.Name(), mac) {
			continue
		}

		deviceDir := filepath.Join(settingsDir, entry.Name())
		if err := os.RemoveAll(deviceDir); err != nil {
			logErrorf("handleDeleteDevice - error removing %s: %v", deviceDir, err)
			http.Error(w, "cannot delete device settings", http.StatusInternalServerError)
			return
		}

		logInfof("handleDeleteDevice - deleted settings of device %s (%s)", mac, deviceDir)
		w.WriteHeader(http.StatusNoContent)
		return
	}

	http.Error(w, "device not found", http.StatusNotFound)
}
