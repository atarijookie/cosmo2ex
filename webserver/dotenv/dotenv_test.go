package dotenv

import (
	"os"
	"path/filepath"
	"testing"
)

func TestDefaultDirPerOS(t *testing.T) {
	t.Setenv("HOME", "/Users/atari")
	t.Setenv("LOCALAPPDATA", `C:\Users\atari\AppData\Local`)

	cases := []struct{ goos, key, want string }{
		{"linux", "LOG_DIR", "/var/log/ce"},
		{"linux", "DATA_DIR", "/var/lib/ce"},
		{"linux", "PID_DIR", "/run/ce"},
		{"darwin", "DATA_DIR", filepath.Join("/Users/atari", "Library", "Application Support", "cosmo2ex", "data")},
		{"darwin", "LOG_DIR", filepath.Join("/Users/atari", "Library", "Application Support", "cosmo2ex", "log")},
		{"windows", "PID_DIR", filepath.Join(`C:\Users\atari\AppData\Local`, "cosmo2ex", "run")},
		{"linux", "SETTINGS_DIR", ""},
	}
	for _, c := range cases {
		if got := DefaultDirOn(c.goos, c.key); got != c.want {
			t.Errorf("DefaultDirOn(%q, %q) = %q, want %q", c.goos, c.key, got, c.want)
		}
	}
}

func TestDefaultDirWindowsWithoutLocalAppDataFallsBack(t *testing.T) {
	t.Setenv("LOCALAPPDATA", "")
	if got := DefaultDirOn("windows", "LOG_DIR"); got != "/var/log/ce" {
		t.Errorf("got %q", got)
	}
}

func TestLoadEnvFromDotFileUsesDefaultDirs(t *testing.T) {
	// t.Setenv restores the values after the test; then unset the ones the test needs missing
	for _, key := range []string{"LOG_DIR", "DATA_DIR", "PID_DIR", "CMD_TOKEN_FILE"} {
		t.Setenv(key, "")
		os.Unsetenv(key)
	}
	t.Setenv("PID_DIR", "/from/real/env")

	envFile := filepath.Join(t.TempDir(), ".env")
	content := "LOG_DIR=/from/dotenv/log\nCMD_TOKEN_FILE=${DATA_DIR}/core_cmd.token\n"
	if err := os.WriteFile(envFile, []byte(content), 0o600); err != nil {
		t.Fatal(err)
	}

	Load(envFile)

	check := func(key, want string) {
		if got := os.Getenv(key); got != want {
			t.Errorf("%s = %q, want %q", key, got, want)
		}
	}
	check("LOG_DIR", "/from/dotenv/log")                              // from .env - kept
	check("DATA_DIR", DefaultDir("DATA_DIR"))                         // missing - default
	check("PID_DIR", "/from/real/env")                                // real env var - kept
	check("CMD_TOKEN_FILE", DefaultDir("DATA_DIR")+"/core_cmd.token") // ${DATA_DIR} uses the default
}
