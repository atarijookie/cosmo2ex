package main

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sync"
)

// getPIDFilePath returns the path to the PID file based on PID_DIR environment variable
func getPIDFilePath() string {
	pidDir := envDir("PID_DIR")
	// Use the executable name as the PID file name
	exePath, err := os.Executable()
	if err != nil {
		// Fallback to a default name if we can't get executable path
		return filepath.Join(pidDir, "webserver.pid")
	}
	exeName := filepath.Base(exePath)
	return filepath.Join(pidDir, exeName+".pid")
}

// writePIDFile writes own PID to pidFilePath, creating the directory if needed. Failures are only logged.
func writePIDFile(pidFilePath string) {
	if err := os.MkdirAll(filepath.Dir(pidFilePath), 0o755); err != nil {
		logErrorf("writePIDFile - failed to create PID directory for %s: %v", pidFilePath, err)
		return
	}

	selfPid := os.Getpid()
	if err := os.WriteFile(pidFilePath, []byte(fmt.Sprintf("%d\n", selfPid)), 0o644); err != nil {
		logErrorf("writePIDFile - failed to write PID file %s: %v", pidFilePath, err)
		return
	}

	logInfof("writePIDFile - pid %d written to %s", selfPid, pidFilePath)
}

// rotatingFileWriter is an io.Writer that writes to a single log file and
// rotates it when the size exceeds maxSize bytes.
type rotatingFileWriter struct {
	mu       sync.Mutex
	dir      string
	baseName string
	file     *os.File
	size     int64
	maxSize  int64
}

func newRotatingFileWriter(dir, baseName string, maxSize int64) (*rotatingFileWriter, error) {
	w := &rotatingFileWriter{
		dir:      dir,
		baseName: baseName,
		maxSize:  maxSize,
	}
	if err := w.openCurrent(); err != nil {
		return nil, err
	}
	return w, nil
}

func (w *rotatingFileWriter) Write(p []byte) (int, error) {
	w.mu.Lock()
	defer w.mu.Unlock()

	if w.file == nil {
		if err := w.openCurrent(); err != nil {
			return 0, err
		}
	}

	// Rotate if this write would exceed maxSize.
	if w.size+int64(len(p)) > w.maxSize {
		if err := w.rotate(); err != nil {
			return 0, err
		}
	}

	n, err := w.file.Write(p)
	w.size += int64(n)
	return n, err
}

func (w *rotatingFileWriter) openCurrent() error {
	path := filepath.Join(w.dir, w.baseName)
	f, err := os.OpenFile(path, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
	if err != nil {
		return err
	}
	info, err := f.Stat()
	if err != nil {
		_ = f.Close()
		return err
	}
	w.file = f
	w.size = info.Size()
	return nil
}

func (w *rotatingFileWriter) rotate() error {
	if w.file != nil {
		_ = w.file.Close()
	}

	currentPath := filepath.Join(w.dir, w.baseName)
	rotatedPath := currentPath + ".1"

	// Keep only two files: base and base.1
	// Remove previous .1 (if any), then move current -> .1
	_ = os.Remove(rotatedPath)
	_ = os.Rename(currentPath, rotatedPath)

	// Open a fresh current file.
	f, err := os.OpenFile(currentPath, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o644)
	if err != nil {
		return err
	}
	w.file = f
	w.size = 0
	return nil
}

// path returns the path of the current log file
func (w *rotatingFileWriter) path() string {
	return filepath.Join(w.dir, w.baseName)
}

// truncate empties the current log file and resets the size counter (used when wiping logs from web UI)
func (w *rotatingFileWriter) truncate() error {
	w.mu.Lock()
	defer w.mu.Unlock()

	if w.file == nil {
		return os.Truncate(w.path(), 0)
	}
	if err := w.file.Truncate(0); err != nil {
		return err
	}
	if _, err := w.file.Seek(0, io.SeekStart); err != nil { // file opened after rotation isn't O_APPEND - would continue at old offset
		return err
	}
	w.size = 0
	return nil
}
