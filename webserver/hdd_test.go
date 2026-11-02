package main

import "testing"

// ACSI_DEVTYPE_{i} values accepted from the ACSI IDs page - same as DEVTYPE_* in ce_cores/misc/global.h
func TestIsValidDevType(t *testing.T) {
	valid := map[int]bool{0: true, 1: false, 2: true, 3: true, 4: true, 5: false, -1: false}

	for devType, want := range valid {
		if got := isValidDevType(devType); got != want {
			t.Errorf("isValidDevType(%d) = %v, want %v", devType, got, want)
		}
	}
}
