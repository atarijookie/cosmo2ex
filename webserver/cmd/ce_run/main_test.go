package main

import (
	"bytes"
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"testing"
	"time"
)

// TestMain - the test binary is also the fake ce_cores / webserver, see fakeProgram
func TestMain(m *testing.M) {
	if mode := os.Getenv("CE_RUN_FAKE"); mode != "" {
		fakeProgram(mode)
		return
	}
	os.Exit(m.Run())
}

// fakeProgram behaves as one of the programs:
//   - "wait"         prints a line, exits cleanly on interrupt
//   - "exit3"        prints a line and exits with code 3
//   - "ignore"       prints a line, ignores interrupts (has to be killed)
//   - "exit0"        prints a line and exits with code 0
//   - "fail"         prints a line and exits with code 1
//   - "failonce"     like "fail" when the file CE_RUN_MARKER doesn't exist yet (and creates it), then like "wait"
func fakeProgram(mode string) {
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt)

	fmt.Println("\x1b[H\x1b[2Jstarted " + mode) // with the clear-screen codes of ce_cores status output
	fmt.Println("")

	if mode == "failonce" {
		if _, err := os.Stat(os.Getenv("CE_RUN_MARKER")); err != nil {
			_ = os.WriteFile(os.Getenv("CE_RUN_MARKER"), nil, 0o644)
			mode = "fail"
		} else {
			mode = "wait"
		}
	}

	switch mode {
	case "exit0":
		time.Sleep(200 * time.Millisecond)
		os.Exit(0)
	case "fail":
		time.Sleep(100 * time.Millisecond)
		os.Exit(1)
	case "exit3":
		time.Sleep(200 * time.Millisecond)
		os.Exit(3)
	case "wait":
		<-sig
		fmt.Print("got interrupt") // no new line at the end - Flush() writes it
		os.Exit(0)
	case "ignore":
		for {
			<-sig
		}
	}
}

// syncBuffer - written by the output copying goroutines, read by the test
type syncBuffer struct {
	mu sync.Mutex
	b  bytes.Buffer
}

func (s *syncBuffer) Write(p []byte) (int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.b.Write(p)
}

func (s *syncBuffer) String() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.b.String()
}

func fakeLauncher(t *testing.T, coresMode, webMode string, out *syncBuffer) *launcher {
	t.Helper()
	if runtime.GOOS == "windows" {
		t.Skip("interrupting programs uses the console on Windows")
	}
	exe, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}

	marker := filepath.Join(t.TempDir(), "failed-once")
	return &launcher{ // env sets CE_RUN_FAKE just for that program
		dir:           t.TempDir(),
		cores:         []string{"/usr/bin/env", "CE_RUN_FAKE=" + coresMode, "CE_RUN_MARKER=" + marker, exe},
		web:           []string{"/usr/bin/env", "CE_RUN_FAKE=" + webMode, exe},
		out:           out,
		stopTimeout:   2 * time.Second,
		restartDelay:  50 * time.Millisecond,
		maxRestarts:   3,
		restartWindow: time.Minute,
	}
}

func TestRunStopsTheOtherProgramWhenOneExits(t *testing.T) {
	out := &syncBuffer{}
	l := fakeLauncher(t, "wait", "exit3", out)

	err := l.run(make(chan os.Signal))

	if err == nil || !strings.Contains(err.Error(), "webserver has exited (exit code 3)") {
		t.Errorf("error = %v", err)
	}
	got := out.String()
	for _, want := range []string{"[cores] started wait\n", "started exit3\n", "webserver has exited, stopping\n", "[cores] got interrupt\n"} {
		if !strings.Contains(got, want) {
			t.Errorf("output has no %q:\n%s", want, got)
		}
	}
	if strings.Contains(got, "[cores] \x1b") || strings.Contains(got, "[cores] \n") {
		t.Errorf("escape codes / empty lines of ce_cores not filtered:\n%q", got)
	}
	if !strings.Contains(got, "[web]   \x1b[H\x1b[2Jstarted exit3\n[web]   \n") {
		t.Errorf("output of the webserver should be unchanged:\n%q", got)
	}
}

func TestRunStopsBothOnSignal(t *testing.T) {
	out := &syncBuffer{}
	l := fakeLauncher(t, "wait", "wait", out)

	sig := make(chan os.Signal, 1)
	go func() {
		time.Sleep(300 * time.Millisecond)
		sig <- os.Interrupt
	}()

	if err := l.run(sig); err != nil {
		t.Errorf("stopped by signal, but got error %v", err)
	}
	got := out.String()
	if !strings.Contains(got, "[cores] got interrupt") || !strings.Contains(got, "[web]   got interrupt") {
		t.Errorf("both should have been interrupted:\n%s", got)
	}
}

func TestRunKillsProgramWhichDoesNotStop(t *testing.T) {
	out := &syncBuffer{}
	l := fakeLauncher(t, "ignore", "exit3", out)
	l.stopTimeout = 300 * time.Millisecond

	start := time.Now()
	if err := l.run(make(chan os.Signal)); err == nil {
		t.Error("expected error")
	}
	if !strings.Contains(out.String(), "not stopped in time, killing") {
		t.Errorf("output:\n%s", out.String())
	}
	if time.Since(start) > 5*time.Second {
		t.Error("took too long")
	}
}

func TestRunFailsWhenProgramIsMissing(t *testing.T) {
	out := &syncBuffer{}
	l := fakeLauncher(t, "wait", "wait", out)
	l.web = []string{filepath.Join(t.TempDir(), "no-such-webserver")}

	err := l.run(make(chan os.Signal))
	if err == nil || !strings.Contains(err.Error(), "cannot start no-such-webserver") {
		t.Errorf("error = %v", err)
	}
	// the started ce_cores was stopped again - otherwise run() wouldn't have returned
}

func TestPrepareAndCleanup(t *testing.T) {
	bin := t.TempDir()
	ce := filepath.Join(t.TempDir(), "ce")
	for _, key := range []string{"BIN_DIR", "CE_DIR", "DATA_DIR", "LOG_DIR", "PID_DIR", "SETTINGS_DIR", "CONFIG_DRIVE_PATH"} {
		t.Setenv(key, "")
		os.Unsetenv(key)
	}

	env := "CE_DIR=" + ce + "\n" +
		"DATA_DIR=${CE_DIR}/data\nLOG_DIR=${CE_DIR}/log\nPID_DIR=${CE_DIR}/run\nSETTINGS_DIR=${CE_DIR}/settings\n" +
		"CONFIG_DRIVE_PATH=${DATA_DIR}/configdrive\n" +
		"CE_LOGO_FILE=${BIN_DIR}/ce_logo.bin\n"
	write(t, filepath.Join(bin, ".env"), env)
	write(t, filepath.Join(bin, "configdrive", "drivers", "ce_dd.prg"), "driver")

	d, err := prepare(bin)
	if err != nil {
		t.Fatal(err)
	}
	if got := os.Getenv("CE_LOGO_FILE"); got != bin+"/ce_logo.bin" { // BIN_DIR is ce_run's folder
		t.Errorf("CE_LOGO_FILE = %q", got)
	}
	for _, sub := range []string{"data", "log", "run", "settings"} {
		if st, err := os.Stat(filepath.Join(ce, sub)); err != nil || !st.IsDir() {
			t.Errorf("%s not created", sub)
		}
	}

	write(t, filepath.Join(ce, "run", "ce_cores.pid"), "123")            // left by a crash
	write(t, filepath.Join(ce, "data", "configdrive", "OLD.TXT"), "old") // previous copy
	write(t, filepath.Join(ce, "data", "core_cmd.token"), "keep")        // not ours to remove

	if err := cleanup(d, bin); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(ce, "run", "ce_cores.pid")); !os.IsNotExist(err) {
		t.Error("stale PID file not removed")
	}
	if _, err := os.Stat(filepath.Join(ce, "data", "configdrive", "OLD.TXT")); !os.IsNotExist(err) {
		t.Error("old config drive copy not replaced")
	}
	if b, err := os.ReadFile(filepath.Join(ce, "data", "configdrive", "drivers", "ce_dd.prg")); err != nil || string(b) != "driver" {
		t.Errorf("config drive not copied: %q %v", b, err)
	}
	if _, err := os.Stat(filepath.Join(ce, "data", "core_cmd.token")); err != nil {
		t.Error("other files in DATA_DIR must stay")
	}
}

func TestCheckCopyTargetRefusesDangerousTargets(t *testing.T) {
	bin := t.TempDir()
	src := filepath.Join(bin, "configdrive")
	root := string(filepath.Separator)
	if runtime.GOOS == "windows" {
		root = `C:\`
	}

	for _, target := range []string{root, src, bin, filepath.Dir(bin)} {
		if err := checkCopyTarget(src, target); err == nil {
			t.Errorf("target %q should be refused", target)
		}
	}
	for _, target := range []string{filepath.Join(t.TempDir(), "configdrive"), filepath.Join(bin, "configdrive2"), filepath.Join(bin, "..configdrive")} {
		if err := checkCopyTarget(src, target); err != nil {
			t.Errorf("target %q should be fine: %v", target, err)
		}
	}
}

func TestLockInstance(t *testing.T) {
	path := filepath.Join(t.TempDir(), "ce_run.lock")
	first, err := lockInstance(path)
	if err != nil {
		t.Fatal(err)
	}
	if second, err := lockInstance(path); err == nil {
		second.Close()
		t.Error("second lock should fail")
	}
	first.Close()

	again, err := lockInstance(path) // released - e.g. after the first one exited
	if err != nil {
		t.Fatal(err)
	}
	again.Close()
}

func TestProgramNames(t *testing.T) {
	if c, w := programNames("windows"); c != "ce_cores.exe" || w != "webserver.exe" {
		t.Errorf("windows: %s %s", c, w)
	}
	if c, w := programNames("darwin"); c != "ce_cores" || w != "webserver" {
		t.Errorf("darwin: %s %s", c, w)
	}
}

func write(t *testing.T, path, content string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
}

// waitFor waits until the output contains want count times
func waitFor(t *testing.T, out *syncBuffer, want string, count int) {
	t.Helper()
	for i := 0; i < 100; i++ {
		if strings.Count(out.String(), want) >= count {
			return
		}
		time.Sleep(50 * time.Millisecond)
	}
	t.Fatalf("%q not %d times in output:\n%s", want, count, out.String())
}

func TestRunRestartsFailedCores(t *testing.T) {
	out := &syncBuffer{}
	l := fakeLauncher(t, "failonce", "wait", out)

	sig := make(chan os.Signal, 1)
	result := make(chan error, 1)
	go func() { result <- l.run(sig) }()

	waitFor(t, out, "[cores] started failonce", 2) // started, failed, started again
	time.Sleep(200 * time.Millisecond)             // the restarted one installs its signal handler
	sig <- os.Interrupt

	if err := <-result; err != nil {
		t.Errorf("stopped by signal, but got error %v", err)
	}
	got := out.String()
	for _, want := range []string{"ce_cores has failed (exit code 1), restarting it in 50ms\n", "[cores] got interrupt", "[web]   got interrupt"} {
		if !strings.Contains(got, want) {
			t.Errorf("output has no %q:\n%s", want, got)
		}
	}
	if strings.Count(got, "started wait") != 1 { // only the webserver runs in this mode
		t.Errorf("the webserver should keep running, not be restarted:\n%s", got)
	}
}

func TestRunGivesUpWhenCoresFailsTooOften(t *testing.T) {
	out := &syncBuffer{}
	l := fakeLauncher(t, "fail", "wait", out)

	err := l.run(make(chan os.Signal))

	if err == nil || !strings.Contains(err.Error(), "ce_cores keeps failing (last time: exit code 1)") {
		t.Errorf("error = %v", err)
	}
	got := out.String()
	if n := strings.Count(got, "[cores] started fail"); n != 4 { // started + 3 restarts
		t.Errorf("ce_cores started %d times, want 4:\n%s", n, got)
	}
	if !strings.Contains(got, "ce_cores has failed 4 times within 1m0s, stopping") || !strings.Contains(got, "[web]   got interrupt") {
		t.Errorf("output:\n%s", got)
	}
}

func TestRunDoesNotRestartCoresWhichExitedCleanly(t *testing.T) {
	out := &syncBuffer{}
	l := fakeLauncher(t, "exit0", "wait", out)

	err := l.run(make(chan os.Signal))

	if err == nil || !strings.Contains(err.Error(), "ce_cores has exited (exit code 0)") {
		t.Errorf("error = %v", err)
	}
	if strings.Contains(out.String(), "restarting") || !strings.Contains(out.String(), "[web]   got interrupt") {
		t.Errorf("output:\n%s", out.String())
	}
}

func TestMayRestartCountsOnlyRecentRestarts(t *testing.T) {
	l := &launcher{maxRestarts: 2, restartWindow: time.Minute}
	t0 := time.Now()

	if !l.mayRestart(t0) || !l.mayRestart(t0.Add(time.Second)) {
		t.Fatal("first 2 restarts should be allowed")
	}
	if l.mayRestart(t0.Add(2 * time.Second)) {
		t.Error("3rd restart within a minute should not be allowed")
	}
	if !l.mayRestart(t0.Add(61 * time.Second)) { // the 1st one is more than a minute old now
		t.Error("restart should be allowed again after the window")
	}
}
