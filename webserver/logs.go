package main

import (
	"archive/zip"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

// Settings page - download all logs as one zip, wipe all logs.
//
// Log files are '*.log' (current) and '*.log.1' (rotated) in LOG_DIR, written by ce_cores, libdospath and webserver.

// webLogWriter is the writer of webserver.log, set in main() - wiping truncates it through the writer so its size counter is reset too
var webLogWriter *rotatingFileWriter

func isCurrentLog(name string) bool { return strings.HasSuffix(name, ".log") }
func isRotatedLog(name string) bool { return strings.HasSuffix(name, ".log.1") }

// listLogFiles returns sorted names of current and rotated log files in dir
func listLogFiles(dir string) ([]string, error) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil, err
	}

	var names []string
	for _, e := range entries {
		if e.Type().IsRegular() && (isCurrentLog(e.Name()) || isRotatedLog(e.Name())) {
			names = append(names, e.Name())
		}
	}
	sort.Strings(names)
	return names, nil
}

// writeLogsZip writes all log files from dir into a zip stream
func writeLogsZip(dir string, out io.Writer) error {
	names, err := listLogFiles(dir)
	if err != nil {
		return err
	}

	zw := zip.NewWriter(out)

	for _, name := range names {
		if err := addFileToZip(zw, dir, name); err != nil {
			logWarningf("writeLogsZip - skipping %s: %v", name, err) // e.g. rotated away meanwhile - don't fail the whole download
		}
	}
	return zw.Close()
}

func addFileToZip(zw *zip.Writer, dir, name string) error {
	f, err := os.Open(filepath.Join(dir, name))
	if err != nil {
		return err
	}
	defer f.Close()

	info, err := f.Stat()
	if err != nil {
		return err
	}

	hdr, err := zip.FileInfoHeader(info)
	if err != nil {
		return err
	}
	hdr.Method = zip.Deflate

	dst, err := zw.CreateHeader(hdr)
	if err != nil {
		return err
	}
	_, err = io.Copy(dst, f) // log might grow while copying - copy what's there until EOF
	return err
}

// wipeLogs truncates current logs (they're kept open in append mode by their writers, so they continue
// writing to the empty file) and deletes rotated ones. Returns names of wiped files and errors of failed ones.
func wipeLogs(dir string) (wiped []string, failed []string) {
	names, err := listLogFiles(dir)
	if err != nil {
		return nil, []string{err.Error()}
	}

	for _, name := range names {
		path := filepath.Join(dir, name)

		if isRotatedLog(name) {
			err = os.Remove(path)
		} else if webLogWriter != nil && filepath.Clean(path) == filepath.Clean(webLogWriter.path()) {
			err = webLogWriter.truncate()
		} else {
			err = os.Truncate(path, 0)
		}

		if err != nil {
			failed = append(failed, fmt.Sprintf("%s: %v", name, err))
		} else {
			wiped = append(wiped, name)
		}
	}
	return wiped, failed
}

// handleGetLogs sends all logs as a single zip file
func (s *Server) handleGetLogs(w http.ResponseWriter, r *http.Request) {
	logDebugf("handleGetLogs %s %s", r.Method, r.URL.Path)

	dir := envDir("LOG_DIR")
	if _, err := os.Stat(dir); err != nil {
		logErrorf("handleGetLogs - log dir %s: %v", dir, err)
		http.Error(w, "log directory not available", http.StatusInternalServerError)
		return
	}

	fileName := "cosmo2ex_logs_" + time.Now().Format("20060102_150405") + ".zip"
	w.Header().Set("Content-Type", "application/zip")
	w.Header().Set("Content-Disposition", `attachment; filename="`+fileName+`"`)
	w.Header().Set("Cache-Control", "no-store")

	if err := writeLogsZip(dir, w); err != nil { // headers already sent, can only log it
		logErrorf("handleGetLogs - failed to write zip: %v", err)
	}
}

type WipeLogsResponse struct {
	Wiped  []string `json:"wiped"`
	Failed []string `json:"failed,omitempty"`
}

// handleDeleteLogs wipes all logs
func (s *Server) handleDeleteLogs(w http.ResponseWriter, r *http.Request) {
	logDebugf("handleDeleteLogs %s %s", r.Method, r.URL.Path)

	wiped, failed := wipeLogs(envDir("LOG_DIR"))
	if wiped == nil {
		wiped = []string{}
	}

	status := http.StatusOK
	if len(failed) > 0 {
		logErrorf("handleDeleteLogs - failed: %s", strings.Join(failed, "; "))
		status = http.StatusInternalServerError
	}
	logInfof("handleDeleteLogs - logs wiped from web UI: %s", strings.Join(wiped, ", ")) // first line of the new webserver.log
	writeJSON(w, status, WipeLogsResponse{Wiped: wiped, Failed: failed})
}
