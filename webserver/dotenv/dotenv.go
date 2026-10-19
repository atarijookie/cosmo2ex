// Package dotenv loads the .env file shared by ce_cores and the webserver (and the ce_run launcher) into env vars,
// and knows the per-OS default dirs.
package dotenv

import (
	"os"
	"path/filepath"
	"runtime"
	"strings"
)

// Load loads simple KEY=VALUE lines from the given .env file
// into env vars. It silently ignores missing files
// and malformed lines. Supports variable substitution like ${VAR}.
func Load(path string) {
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

// defaultDirKeys are the dirs which get a per-OS default when neither .env nor a real env var sets them.
var defaultDirKeys = []string{"LOG_DIR", "DATA_DIR", "PID_DIR"}

// DefaultDir returns the per-OS default of LOG_DIR / DATA_DIR / PID_DIR, "" for other keys - must match
// Utils::defaultDir() in ce_cores, both programs use the same dirs.
//   - Linux:   system dirs, as the .env shipped for Linux / docker has them
//   - macOS:   ~/Library/Application Support/cosmo2ex/...
//   - Windows: %LOCALAPPDATA%\cosmo2ex\...
//
// Desktop users run it as regular user, without access to /var and /run.
func DefaultDir(key string) string {
	return DefaultDirOn(runtime.GOOS, key)
}

func DefaultDirOn(goos, key string) string {
	sub := map[string]string{"LOG_DIR": "log", "DATA_DIR": "data", "PID_DIR": "run"}[key]
	if sub == "" {
		return ""
	}

	switch goos {
	case "darwin":
		if home, err := os.UserHomeDir(); err == nil && home != "" {
			return filepath.Join(home, "Library", "Application Support", "cosmo2ex", sub)
		}
	case "windows":
		if localAppData := os.Getenv("LOCALAPPDATA"); localAppData != "" {
			return filepath.Join(localAppData, "cosmo2ex", sub)
		}
	}

	// Linux - and fallback if the per-user dir can't be found on other OS
	return map[string]string{"LOG_DIR": "/var/log/ce", "DATA_DIR": "/var/lib/ce", "PID_DIR": "/run/ce"}[key]
}

// Dir returns the dir from env var key (loaded from .env), or its per-OS default if it's not set.
func Dir(key string) string {
	if dir := os.Getenv(key); dir != "" {
		return dir
	}
	return DefaultDir(key)
}

// setDefaultDirs sets env vars of default dirs which are neither in .env (envVars) nor real env vars,
// so ${DATA_DIR} etc. in other .env values resolve to the default too.
func setDefaultDirs(envVars map[string]string) {
	for _, key := range defaultDirKeys {
		if _, inDotEnv := envVars[key]; inDotEnv || os.Getenv(key) != "" {
			continue
		}
		_ = os.Setenv(key, DefaultDir(key))
	}
}
