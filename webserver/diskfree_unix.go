//go:build !windows

package main

import "syscall"

// diskFreeBytes returns the space available to this (non root) user on the filesystem of path
func diskFreeBytes(path string) (uint64, error) {
	var st syscall.Statfs_t
	if err := syscall.Statfs(path, &st); err != nil {
		return 0, err
	}
	return uint64(st.Bavail) * uint64(st.Bsize), nil
}
