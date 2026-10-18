package main

import (
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strings"
	"sync"
)

// loadEnvFromDotFile loads simple KEY=VALUE lines from the given .env file
// in the current working directory. It silently ignores missing files
// and malformed lines. Supports variable substitution like ${VAR}.
func loadEnvFromDotFile(path string) {
	data, err := os.ReadFile(path)
	if err != nil {
		// No .env file; only the default dirs.
		setDefaultDirs(nil)
		return
	}

	// First pass: collect all variables into a map
	envVars := make(map[string]string)
	lines := strings.Split(string(data), "\n")
	for _, line := range lines {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		if idx := strings.Index(line, "="); idx != -1 {
			key := strings.TrimSpace(line[:idx])
			val := strings.TrimSpace(line[idx+1:])

			// Optionally strip surrounding quotes.
			if len(val) >= 2 && ((val[0] == '"' && val[len(val)-1] == '"') || (val[0] == '\'' && val[len(val)-1] == '\'')) {
				val = val[1 : len(val)-1]
			}

			if key != "" {
				envVars[key] = val
			}
		}
	}

	// before resolving, so e.g. ${DATA_DIR}/core_cmd.token works even without DATA_DIR in .env
	setDefaultDirs(envVars)

	// Second pass: resolve variable references and set environment variables
	for key, val := range envVars {
		resolved := resolveEnvVar(val, envVars, make(map[string]bool))
		_ = os.Setenv(key, resolved)
	}
}

// resolveEnvVar replaces ${VAR} references in a string with their values.
// It handles nested references and prevents circular dependencies.
func resolveEnvVar(value string, envVars map[string]string, visited map[string]bool) string {
	// Find all ${VAR} patterns
	var result strings.Builder
	i := 0
	for i < len(value) {
		// Look for ${ pattern
		if i < len(value)-1 && value[i] == '$' && value[i+1] == '{' {
			// Find the closing }
			end := strings.Index(value[i+2:], "}")
			if end == -1 {
				// No closing brace, keep as-is
				result.WriteByte(value[i])
				i++
				continue
			}
			end += i + 2 // Adjust for offset

			// Extract variable name
			varName := strings.TrimSpace(value[i+2 : end])

			// Check for circular reference
			if visited[varName] {
				// Circular reference detected, return original
				result.WriteString(value[i : end+1])
				i = end + 1
				continue
			}

			// Look up variable value
			varVal := ""
			if v, ok := envVars[varName]; ok {
				// Recursively resolve nested variables
				visited[varName] = true
				varVal = resolveEnvVar(v, envVars, visited)
				delete(visited, varName)
			} else if v := os.Getenv(varName); v != "" {
				// Check system environment as fallback
				varVal = v
			}

			result.WriteString(varVal)
			i = end + 1
		} else {
			result.WriteByte(value[i])
			i++
		}
	}
	return result.String()
}

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
		log.Printf("writePIDFile - failed to create PID directory for %s: %v", pidFilePath, err)
		return
	}

	selfPid := os.Getpid()
	if err := os.WriteFile(pidFilePath, []byte(fmt.Sprintf("%d\n", selfPid)), 0o644); err != nil {
		log.Printf("writePIDFile - failed to write PID file %s: %v", pidFilePath, err)
		return
	}

	log.Printf("writePIDFile - pid %d written to %s", selfPid, pidFilePath)
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
