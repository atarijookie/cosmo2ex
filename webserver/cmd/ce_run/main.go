// ce_run starts ce_cores and the webserver and stops both when one of them exits - on Windows (and other desktops)
// it does what ce_run.sh + the systemd service do on Linux:
//   - loads .env from its own folder, which is also BIN_DIR (set as env var BIN_DIR, the .env for Windows uses ${BIN_DIR})
//   - creates the dirs from .env and removes PID files left by a crash
//   - makes the runtime copy of the config drive (CONFIG_DRIVE_PATH)
//   - runs both programs in its folder, their output prefixed with [cores] / [web]
//   - Ctrl+C or closing the console window stops both; when one of them exits, the other one is stopped too
//
// Double-click it, or run it from a console. Only one instance can run (lock file in DATA_DIR).
package main

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"regexp"
	"runtime"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/atarijookie/cosmo2ex/webserver/dotenv"
)

// how long the programs get to quit after being told to, before they are killed
const stopTimeout = 10 * time.Second

func main() {
	err := run()
	if err != nil {
		fmt.Fprintln(os.Stderr, "ce_run:", err)
	}

	if ownsConsole() { // started by double-click - the console window closes on exit, let the user read the output first
		fmt.Print("\nPress Enter to close this window...")
		_, _ = fmt.Scanln()
	}

	if err != nil {
		os.Exit(1)
	}
}

func run() error {
	exe, err := os.Executable()
	if err != nil {
		return err
	}
	binDir := filepath.Dir(exe)

	if err := os.Chdir(binDir); err != nil { // both apps load ./.env, so they need to run from BIN_DIR
		return err
	}

	dirs, err := prepare(binDir)
	if err != nil {
		return err
	}

	lock, err := lockInstance(filepath.Join(dirs.data, "ce_run.lock"))
	if err != nil {
		return fmt.Errorf("another ce_run is already running? %v", err)
	}
	defer lock.Close()

	if err := cleanup(dirs, binDir); err != nil {
		return err
	}

	coresName, webName := programNames(runtime.GOOS)
	l := &launcher{
		dir:         binDir,
		cores:       []string{filepath.Join(binDir, coresName)},
		web:         []string{filepath.Join(binDir, webName)},
		out:         os.Stdout,
		stopTimeout: stopTimeout,
	}

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM) // on Windows: Ctrl+C, closing the console window, logoff, shutdown
	return l.run(sig)
}

// programNames returns the file names of ce_cores and the webserver on the given OS
func programNames(goos string) (cores, web string) {
	switch goos {
	case "windows":
		return "ce_cores.exe", "webserver.exe"
	case "linux":
		return "ce_cores.elf", "webserver"
	default:
		return "ce_cores", "webserver"
	}
}

//-------------
// preparing the dirs

type runDirs struct {
	data, pid, configDrive string
}

// prepare loads .env and creates the dirs from it - the programs check on start that they exist
func prepare(binDir string) (runDirs, error) {
	if os.Getenv("BIN_DIR") == "" { // a real env var - the .env can use ${BIN_DIR}, or set its own BIN_DIR
		_ = os.Setenv("BIN_DIR", binDir)
	}
	dotenv.Load(filepath.Join(binDir, ".env"))

	d := runDirs{
		data:        dotenv.Dir("DATA_DIR"),
		pid:         dotenv.Dir("PID_DIR"),
		configDrive: os.Getenv("CONFIG_DRIVE_PATH"),
	}

	for _, key := range []string{"CE_DIR", "DATA_DIR", "LOG_DIR", "PID_DIR", "SETTINGS_DIR"} {
		dir := dotenv.Dir(key)
		if dir == "" {
			return d, fmt.Errorf("%s is not set in .env", key)
		}
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return d, err
		}
	}

	_ = os.Chmod(d.data, 0o700) // owner only - command token (no effect on Windows, there it's in the user's own folder)
	return d, nil
}

// cleanup removes what a crashed run left behind and makes the runtime copy of the config drive. Called only with
// the instance lock held - nothing else is running, so PID files are stale.
func cleanup(d runDirs, binDir string) error {
	pidFiles, _ := filepath.Glob(filepath.Join(d.pid, "*.pid"))
	for _, f := range pidFiles {
		_ = os.Remove(f)
	}

	src := filepath.Join(binDir, "configdrive")
	if _, err := os.Stat(src); err != nil || d.configDrive == "" {
		return nil // nothing to copy, or no config drive in .env (ce_cores reports that)
	}

	if err := checkCopyTarget(src, d.configDrive); err != nil {
		return err
	}
	if err := os.RemoveAll(d.configDrive); err != nil {
		return err
	}
	return copyDir(src, d.configDrive)
}

// checkCopyTarget refuses CONFIG_DRIVE_PATH values which would make the RemoveAll before copying delete something
// it shouldn't - a drive / filesystem root, the source itself, or a dir which contains the source
func checkCopyTarget(src, target string) error {
	absSrc, err := filepath.Abs(src)
	if err != nil {
		return err
	}
	absTarget, err := filepath.Abs(target)
	if err != nil {
		return err
	}

	if filepath.Dir(absTarget) == absTarget {
		return fmt.Errorf("CONFIG_DRIVE_PATH %q is a root dir", target)
	}
	rel, err := filepath.Rel(absTarget, absSrc)
	if err == nil && (rel == "." || (rel != ".." && !strings.HasPrefix(rel, ".."+string(filepath.Separator)))) {
		return fmt.Errorf("CONFIG_DRIVE_PATH %q contains the config drive it should be copied from (%s)", target, src)
	}
	return nil
}

// copyDir copies dir src to dst (which must not exist), files keep their permission bits
func copyDir(src, dst string) error {
	return filepath.Walk(src, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(src, path)
		if err != nil {
			return err
		}
		target := filepath.Join(dst, rel)

		if info.IsDir() {
			return os.MkdirAll(target, 0o755)
		}
		if !info.Mode().IsRegular() { // no symlinks etc. on the config drive
			return nil
		}
		return copyFile(path, target, info.Mode().Perm())
	})
}

func copyFile(src, dst string, perm os.FileMode) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()

	out, err := os.OpenFile(dst, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, perm)
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, in); err != nil {
		out.Close()
		return err
	}
	return out.Close()
}

//-------------
// running the programs

type launcher struct {
	dir         string   // working dir of the programs (BIN_DIR)
	cores, web  []string // command lines
	out         io.Writer
	stopTimeout time.Duration
}

type exited struct {
	name string
	err  error
}

// run starts both programs and waits until they both exit. Returns error when they didn't stop because of a signal.
func (l *launcher) run(sig <-chan os.Signal) error {
	var outMu sync.Mutex // the programs' lines are written whole, not mixed
	coresOut := &prefixWriter{w: l.out, mu: &outMu, prefix: "[cores] ", filter: true}
	webOut := &prefixWriter{w: l.out, mu: &outMu, prefix: "[web]   "}

	cores, err := start("ce_cores", l.cores, l.dir, coresOut)
	if err != nil {
		return err
	}
	web, err := start("webserver", l.web, l.dir, webOut)
	if err != nil {
		interrupt([]*exec.Cmd{cores.cmd}, false)
		wait(l.out, []*proc{cores}, waitAll([]*proc{cores}), l.stopTimeout)
		return err
	}

	done := waitAll([]*proc{cores, web})
	remaining := []*proc{cores, web}

	var result error
	select {
	case <-sig: // the programs got it too when it came from the console, interrupt() does the rest
		fmt.Fprintln(l.out, "stopping")
		interrupt([]*exec.Cmd{cores.cmd, web.cmd}, true)

	case e := <-done:
		other := cores
		if e.name == cores.name {
			other = web
		}
		remaining = []*proc{other}
		result = fmt.Errorf("%s has exited (%s), stopped %s too", e.name, describe(e.err), other.name)
		fmt.Fprintf(l.out, "%s has exited, stopping\n", e.name)
		interrupt([]*exec.Cmd{other.cmd}, false)
	}

	wait(l.out, remaining, done, l.stopTimeout)

	coresOut.Flush()
	webOut.Flush()
	return result
}

// proc is a started program, its output is copied from outRead in the background
type proc struct {
	name      string
	cmd       *exec.Cmd
	outRead   *os.File
	outCopied chan struct{}
}

// start runs the program with stdout + stderr going to out. The output goes through an own pipe, not through
// exec's: Wait() would wait also until everybody closes that one - e.g. a program started by ce_cores, which inherited it.
func start(name string, cmdLine []string, dir string, out io.Writer) (*proc, error) {
	r, w, err := os.Pipe()
	if err != nil {
		return nil, err
	}

	c := exec.Command(cmdLine[0], cmdLine[1:]...)
	c.Dir = dir
	c.Stdout = w
	c.Stderr = w
	err = c.Start()
	w.Close() // the program has its own copy now
	if err != nil {
		r.Close()
		return nil, fmt.Errorf("cannot start %s: %v", filepath.Base(cmdLine[0]), err)
	}

	p := &proc{name: name, cmd: c, outRead: r, outCopied: make(chan struct{})}
	go func() {
		_, _ = io.Copy(out, r)
		close(p.outCopied)
	}()
	return p, nil
}

// waitAll waits for each of the programs in the background, reports them in the order they exit
func waitAll(procs []*proc) <-chan exited {
	done := make(chan exited, len(procs))
	for _, p := range procs {
		go func(p *proc) {
			err := p.cmd.Wait()
			select { // the rest of its output - unless someone else keeps the pipe open
			case <-p.outCopied:
			case <-time.After(2 * time.Second):
			}
			p.outRead.Close()
			done <- exited{p.name, err}
		}(p)
	}
	return done
}

// wait waits until the remaining programs exit - those which don't, get killed after the timeout
func wait(out io.Writer, remaining []*proc, done <-chan exited, timeout time.Duration) {
	timer := time.NewTimer(timeout)
	defer timer.Stop()

	for left := len(remaining); left > 0; {
		select {
		case <-done:
			left--
		case <-timer.C:
			fmt.Fprintln(out, "not stopped in time, killing")
			for _, p := range remaining {
				_ = p.cmd.Process.Kill()
			}
			timer.Reset(timeout) // Wait() returns once they're killed
		}
	}
}

func describe(err error) string {
	var exitErr *exec.ExitError
	if errors.As(err, &exitErr) {
		if exitErr.ExitCode() < 0 { // killed by a signal
			return exitErr.Error()
		}
		return fmt.Sprintf("exit code %d", exitErr.ExitCode())
	}
	if err == nil {
		return "exit code 0"
	}
	return err.Error()
}

//-------------
// output

// clear screen / cursor home escape codes of the ce_cores status output
var escapeCodes = regexp.MustCompile(`\x1b\[[0-9;]*[HJ]`)

// prefixWriter writes whole lines with a prefix; with filter: without escape codes and empty lines (same as ce_run.sh)
type prefixWriter struct {
	w      io.Writer
	mu     *sync.Mutex // shared by the writers of all programs - their lines are written whole, not mixed
	prefix string
	filter bool
	buf    []byte
}

func (p *prefixWriter) Write(data []byte) (int, error) {
	p.mu.Lock()
	defer p.mu.Unlock()

	p.buf = append(p.buf, data...)
	for {
		i := bytes.IndexByte(p.buf, '\n')
		if i < 0 {
			break
		}
		p.writeLine(p.buf[:i])
		p.buf = p.buf[i+1:]
	}
	return len(data), nil
}

// Flush writes the last line, if it didn't end with a new line
func (p *prefixWriter) Flush() {
	p.mu.Lock()
	defer p.mu.Unlock()

	if len(p.buf) > 0 {
		p.writeLine(p.buf)
		p.buf = nil
	}
}

// writeLine - called with mu locked
func (p *prefixWriter) writeLine(line []byte) {
	s := strings.TrimRight(string(line), "\r")
	if p.filter {
		s = escapeCodes.ReplaceAllString(s, "")
		if strings.TrimSpace(s) == "" {
			return
		}
	}
	fmt.Fprintln(p.w, p.prefix+s)
}
