package main

import (
	"os"
	"path/filepath"
	"runtime"
)

// defaultDirKeys are the dirs which get a per-OS default when neither .env nor a real env var sets them.
var defaultDirKeys = []string{"LOG_DIR", "DATA_DIR", "PID_DIR"}

// defaultDir returns the per-OS default of LOG_DIR / DATA_DIR / PID_DIR, "" for other keys - must match
// Utils::defaultDir() in ce_cores, both programs use the same dirs.
//   - Linux:   system dirs, as the .env shipped for Linux / docker has them
//   - macOS:   ~/Library/Application Support/cosmo2ex/...
//   - Windows: %LOCALAPPDATA%\cosmo2ex\...
//
// Desktop users run it as regular user, without access to /var and /run.
func defaultDir(key string) string {
	return defaultDirOn(runtime.GOOS, key)
}

func defaultDirOn(goos, key string) string {
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

// envDir returns the dir from env var key (loaded from .env), or its per-OS default if it's not set.
func envDir(key string) string {
	if dir := os.Getenv(key); dir != "" {
		return dir
	}
	return defaultDir(key)
}

// setDefaultDirs sets env vars of default dirs which are neither in .env (envVars) nor real env vars,
// so ${DATA_DIR} etc. in other .env values resolve to the default too.
func setDefaultDirs(envVars map[string]string) {
	for _, key := range defaultDirKeys {
		if _, inDotEnv := envVars[key]; inDotEnv || os.Getenv(key) != "" {
			continue
		}
		_ = os.Setenv(key, defaultDir(key))
	}
}
