//go:build !windows

package main

import (
	"os"
	"os/exec"
	"syscall"
)

// interrupt tells the programs to quit with SIGINT, which they handle as a clean shutdown. Also after a signal
// to ce_run - Ctrl+C in a terminal reaches them too, but kill / SIGTERM doesn't.
func interrupt(cmds []*exec.Cmd, fromSignal bool) {
	for _, c := range cmds {
		_ = c.Process.Signal(os.Interrupt)
	}
}

// lockInstance locks the file until the process exits (also when it crashes)
func lockInstance(path string) (*os.File, error) {
	f, err := os.OpenFile(path, os.O_RDWR|os.O_CREATE, 0o600)
	if err != nil {
		return nil, err
	}
	if err := syscall.Flock(int(f.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		f.Close()
		return nil, err
	}
	return f, nil
}

// ownsConsole - only on Windows a console window is opened just for this process
func ownsConsole() bool {
	return false
}
