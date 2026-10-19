package main

import (
	"os"
	"os/exec"
	"syscall"
	"unsafe"
)

var (
	kernel32                 = syscall.NewLazyDLL("kernel32.dll")
	generateConsoleCtrlEvent = kernel32.NewProc("GenerateConsoleCtrlEvent")
	getConsoleProcessList    = kernel32.NewProc("GetConsoleProcessList")
)

// interrupt tells the programs to quit. Windows has no signals to send to another process, but both programs share
// ce_run's console, and a Ctrl+C event on it is what they handle as a clean shutdown (Cygwin turns it into SIGINT
// for ce_cores). After a signal to ce_run they already got the same console event, so nothing to do then.
// The event reaches ce_run too - it's already stopping, so that changes nothing. Whoever doesn't quit gets killed
// after the timeout.
func interrupt(cmds []*exec.Cmd, fromSignal bool) {
	if fromSignal {
		return
	}
	const ctrlCEvent = 0
	_, _, _ = generateConsoleCtrlEvent.Call(ctrlCEvent, 0) // 0: all processes on this console
}

// lockInstance opens the file without sharing - nobody else can open it until this process exits (also when it crashes)
func lockInstance(path string) (*os.File, error) {
	name, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return nil, err
	}
	h, err := syscall.CreateFile(name, syscall.GENERIC_READ|syscall.GENERIC_WRITE, 0, nil, syscall.OPEN_ALWAYS,
		syscall.FILE_ATTRIBUTE_NORMAL, 0)
	if err != nil {
		return nil, err
	}
	return os.NewFile(uintptr(h), path), nil
}

// ownsConsole tells if ce_run is the only process on its console - then the console window was opened for it
// (double-click in Explorer) and closes when it exits
func ownsConsole() bool {
	var pids [2]uint32
	n, _, _ := getConsoleProcessList.Call(uintptr(unsafe.Pointer(&pids[0])), uintptr(len(pids)))
	return n == 1
}
