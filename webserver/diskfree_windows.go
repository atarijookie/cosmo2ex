package main

import (
	"syscall"
	"unsafe"
)

var getDiskFreeSpaceExW = syscall.NewLazyDLL("kernel32.dll").NewProc("GetDiskFreeSpaceExW")

// diskFreeBytes returns the space available to this user on the drive of path
func diskFreeBytes(path string) (uint64, error) {
	p, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return 0, err
	}

	var freeToCaller, total, totalFree uint64
	r, _, callErr := getDiskFreeSpaceExW.Call(uintptr(unsafe.Pointer(p)),
		uintptr(unsafe.Pointer(&freeToCaller)), uintptr(unsafe.Pointer(&total)), uintptr(unsafe.Pointer(&totalFree)))
	if r == 0 {
		return 0, callErr
	}
	return freeToCaller, nil
}
