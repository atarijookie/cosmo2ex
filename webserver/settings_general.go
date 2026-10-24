package main

import (
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
)

// General (not per device) settings - one file per key in SETTINGS_DIR, like the ones of ce_cores (e.g. FLOPPY_IMAGE_0).

const (
	settingFloppyDownloadEnabled = "FLOPPY_DOWNLOAD_ENABLED" // "1" / "0" - missing means the user didn't decide yet
	settingFloppyDownloadURL     = "FLOPPY_DOWNLOAD_URL"

	// TODO: https, and a better place than a private webserver (e.g. a file in the release package)
	defaultFloppyListURL = "http://joo.kie.sk/cosmosex/update/imagelist.csv"
)

type generalSettings struct {
	FloppyDownloadEnabled bool   `json:"floppyDownloadEnabled"`
	FloppyDownloadURL     string `json:"floppyDownloadUrl"`
	FloppyDownloadDecided bool   `json:"floppyDownloadDecided"` // saved once - until then the start page shows a banner
}

func readSetting(key string) (string, bool) {
	content, err := os.ReadFile(filepath.Join(getSettingsDir(), key))
	if err != nil {
		return "", false
	}
	return strings.TrimSpace(string(content)), true
}

func writeSetting(key, value string) error {
	dir := getSettingsDir()
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(dir, key), []byte(value), 0o644)
}

func readGeneralSettings() generalSettings {
	gs := generalSettings{FloppyDownloadURL: defaultFloppyListURL}

	if v, ok := readSetting(settingFloppyDownloadEnabled); ok {
		gs.FloppyDownloadDecided = true
		gs.FloppyDownloadEnabled = v == "1"
	}
	if v, ok := readSetting(settingFloppyDownloadURL); ok && v != "" {
		gs.FloppyDownloadURL = v
	}
	return gs
}

// validFloppyListURL accepts absolute http / https URLs only
func validFloppyListURL(s string) bool {
	u, err := url.Parse(s)
	return err == nil && (u.Scheme == "http" || u.Scheme == "https") && u.Host != ""
}

// floppyImagesDir is where the downloaded floppy images go, and where browsing starts without an image (FLOPPY_IMAGES_DIR
// in .env, next to SETTINGS_DIR). "" if not set - then nothing is downloaded.
func floppyImagesDir() string {
	return os.Getenv("FLOPPY_IMAGES_DIR")
}

// logFloppyImagesDirSpace - on start, create the dir and tell whether the images can be downloaded there
func logFloppyImagesDirSpace() {
	dir := floppyImagesDir()
	if dir == "" {
		log.Printf("FLOPPY_IMAGES_DIR not set in .env - floppy images won't be downloaded")
		return
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		log.Printf("can't create FLOPPY_IMAGES_DIR %s: %v", dir, err)
		return
	}
	free, err := diskFreeBytes(dir)
	if err != nil {
		log.Printf("can't get free space of FLOPPY_IMAGES_DIR %s: %v", dir, err)
		return
	}
	log.Printf("FLOPPY_IMAGES_DIR %s: %d MB free, downloading keeps %d MB free", dir, free>>20, floppyFreeSpaceReserve>>20)
}

func (s *Server) handleGetGeneralSettings(w http.ResponseWriter, r *http.Request) {
	log.Printf("handleGetGeneralSettings %s %s", r.Method, r.URL.Path)

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"settings":        readGeneralSettings(),
		"defaultUrl":      defaultFloppyListURL,
		"floppyImagesDir": floppyImagesDir(),
		"download":        s.downloader.getStatus(),
	})
}

func (s *Server) handlePutGeneralSettings(w http.ResponseWriter, r *http.Request) {
	log.Printf("handlePutGeneralSettings %s %s", r.Method, r.URL.Path)

	var body struct {
		FloppyDownloadEnabled bool   `json:"floppyDownloadEnabled"`
		FloppyDownloadURL     string `json:"floppyDownloadUrl"`
	}
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		http.Error(w, "invalid request body", http.StatusBadRequest)
		return
	}

	listURL := strings.TrimSpace(body.FloppyDownloadURL)
	if listURL == "" {
		listURL = defaultFloppyListURL
	}
	if !validFloppyListURL(listURL) {
		http.Error(w, "the list URL must be an http:// or https:// address", http.StatusBadRequest)
		return
	}

	enabled := "0"
	if body.FloppyDownloadEnabled {
		enabled = "1"
	}
	if err := writeSetting(settingFloppyDownloadURL, listURL); err != nil {
		http.Error(w, fmt.Sprintf("cannot store settings: %v", err), http.StatusInternalServerError)
		return
	}
	if err := writeSetting(settingFloppyDownloadEnabled, enabled); err != nil {
		http.Error(w, fmt.Sprintf("cannot store settings: %v", err), http.StatusInternalServerError)
		return
	}

	if body.FloppyDownloadEnabled { // just allowed (or the URL changed)? don't wait for the next daily run
		s.downloader.trigger()
	}

	w.WriteHeader(http.StatusNoContent)
}
