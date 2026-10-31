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

// .env.local next to the real .env (path may be a symlink to it) overrides keys, and the override is used in other values
func TestLoadEnvLocalOverridesThroughSymlink(t *testing.T) {
	for _, key := range []string{"BIN_DIR", "CONFIG_DRIVE_DIR", "CE_DD_PRG_FILE", "LOG_LEVEL"} {
		t.Setenv(key, "")
		os.Unsetenv(key)
	}

	root := t.TempDir()
	env := "BIN_DIR=/opt/ce/bin\n" +
		"CONFIG_DRIVE_DIR=${BIN_DIR}/configdrive\n" +
		"CE_DD_PRG_FILE=${CONFIG_DRIVE_DIR}/drivers/ce_dd.prg\n" +
		"LOG_LEVEL=3\n"
	if err := os.WriteFile(filepath.Join(root, ".env"), []byte(env), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, ".env.local"), []byte("CONFIG_DRIVE_DIR=/src/ce_cores/configdrive\n"), 0o644); err != nil {
		t.Fatal(err)
	}

	sub := filepath.Join(root, "webserver")
	if err := os.Mkdir(sub, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink("../.env", filepath.Join(sub, ".env")); err != nil {
		t.Fatal(err)
	}

	Load(filepath.Join(sub, ".env"))

	if got := os.Getenv("CONFIG_DRIVE_DIR"); got != "/src/ce_cores/configdrive" {
		t.Errorf("CONFIG_DRIVE_DIR = %q", got)
	}
	if got := os.Getenv("CE_DD_PRG_FILE"); got != "/src/ce_cores/configdrive/drivers/ce_dd.prg" {
		t.Errorf("CE_DD_PRG_FILE = %q", got)
	}
	if got := os.Getenv("LOG_LEVEL"); got != "3" {
		t.Errorf("LOG_LEVEL = %q, not overridden so it should stay", got)
	}
}
