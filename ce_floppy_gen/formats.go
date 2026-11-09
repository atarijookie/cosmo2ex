package main

import (
	"bytes"
	"encoding/binary"
)

//----------------------------------------------------------------------------
// .st - just the sectors

func generateST(s Spec) []byte {
	image := make([]byte, 0, s.Tracks*s.Sides*s.Spt*sectorSize)
	s.forEachTrack(func(track, side int) {
		image = append(image, s.trackData(track, side)...)
	})
	return image
}

//----------------------------------------------------------------------------
// .msa - big endian header, then each track with its length, compressed if it helps

const msaMarker = 0xE5

func generateMSA(s Spec) []byte {
	image := []byte{}
	for _, word := range []int{0x0E0F, s.Spt, s.Sides - 1, 0, s.Tracks - 1} {
		image = binary.BigEndian.AppendUint16(image, uint16(word))
	}

	s.forEachTrack(func(track, side int) {
		data := s.trackData(track, side)
		packed := msaCompress(data)

		if len(packed) >= len(data) { // length equal to the track size means not compressed
			packed = data
		}

		image = binary.BigEndian.AppendUint16(image, uint16(len(packed)))
		image = append(image, packed...)
	})
	return image
}

// A run is: marker, value, 16-bit count. The marker byte itself can be stored only like that.
func msaCompress(data []byte) []byte {
	out := []byte{}

	for pos := 0; pos < len(data); {
		run := 1
		for pos+run < len(data) && data[pos+run] == data[pos] {
			run++
		}

		if run >= 5 || data[pos] == msaMarker { // a run takes 4 bytes
			out = append(out, msaMarker, data[pos])
			out = binary.BigEndian.AppendUint16(out, uint16(run))
		} else {
			for i := 0; i < run; i++ {
				out = append(out, data[pos])
			}
		}
		pos += run
	}
	return out
}

//----------------------------------------------------------------------------
// .stx - Pasti image, the format as Hatari reads it (src/floppies/stx.c). No track image, each sector with its descriptor,
// timing and fuzzy data only when asked for. All little endian, except of the ID field CRC and the timing data.

const (
	stxBlockSize        = 16
	stxTrackFlagSectors = 0x01 // the track has sector descriptors
	stxToolAtari        = 0x01
	stxRevision         = 2
	stxReadTime         = 32 * sectorSize // us, standard speed
)

// Track layout as ce_cores encodes .st and .msa images, so the sectors are at the same places in all formats.
// Bytes: gap at the track start, and in each sector: gap, sync 3x, ID mark, ID field 6 B, gap 22 + 12, sync 3x, data mark,
// data, CRC 2 B, gap.
type stxLayout struct {
	trackStartGap, gapBeforeSync, gapAfterSector int
}

func layoutFor(spt int) stxLayout {
	if spt > 10 { // 11 sectors fit into a track only with short gaps
		return stxLayout{10, 3, 2}
	}
	return stxLayout{60, 12, 40}
}

func (l stxLayout) sectorBytes() int {
	return l.gapBeforeSync + 3 + 1 + 6 + 22 + 12 + 3 + 1 + sectorSize + 2 + l.gapAfterSector
}

const (
	stxTrackBytes     = 6250 // 200 ms of a rotation
	stxLongTrackBytes = 6500 // 4 % more, about what the mastered long tracks have

	stxSectorVariable = 0x01
	stxSectorCrcError = 0x08
	stxSectorFuzzy    = 0x80
)

// The same as the old Pasti images use for Macrodos: 2 B for each 16 bytes of the sector, +- 4.7 % around the standard.
func stxTimingData() []byte {
	data := []byte{}
	for block := 0; block < sectorSize/16; block++ {
		value := byte(0x7f)
		switch block / 8 {
		case 1:
			value = 0x85
		case 2:
			value = 0x79
		}
		data = append(data, 0, value)
	}
	return data
}

func generateSTX(s Spec) []byte {
	image := []byte("RSY\x00")
	image = binary.LittleEndian.AppendUint16(image, 3)            // version
	image = binary.LittleEndian.AppendUint16(image, stxToolAtari) // imaging tool
	image = binary.LittleEndian.AppendUint16(image, 0)            // reserved
	image = append(image, byte(s.Tracks*s.Sides), stxRevision)    // track blocks, revision
	image = binary.LittleEndian.AppendUint32(image, 0)            // reserved

	layout := layoutFor(s.Spt)

	s.forEachTrack(func(track, side int) {
		fuzzy := []byte{}
		if s.Fuzzy { // the upper half of the sector is unstable: 0 in the mask, low nibble of each byte
			fuzzy = bytes.Repeat([]byte{0xff}, sectorSize)
			for i := sectorSize / 2; i < sectorSize; i++ {
				fuzzy[i] = 0xf0
			}
		}

		timing := []byte{} // 4 B: flags, size, then the data of all the variable sectors
		if s.Timing {
			timing = binary.LittleEndian.AppendUint16(timing, 5)
			timing = binary.LittleEndian.AppendUint16(timing, uint16(4+s.Spt*sectorSize/16*2))
			for sector := 1; sector <= s.Spt; sector++ {
				timing = append(timing, stxTimingData()...)
			}
		}

		trackBytes := stxTrackBytes
		if s.Long {
			trackBytes = stxLongTrackBytes
		}

		blockSize := stxBlockSize + s.Spt*stxBlockSize + len(fuzzy) + s.Spt*sectorSize + len(timing)

		image = binary.LittleEndian.AppendUint32(image, uint32(blockSize))
		image = binary.LittleEndian.AppendUint32(image, uint32(len(fuzzy)))
		image = binary.LittleEndian.AppendUint16(image, uint16(s.Spt))
		image = binary.LittleEndian.AppendUint16(image, stxTrackFlagSectors)
		image = binary.LittleEndian.AppendUint16(image, uint16(trackBytes))
		image = append(image, byte(track)|byte(side)<<7, 0) // track number + side, record type

		for sector := 1; sector <= s.Spt; sector++ {
			// position in bits, just after the ID mark
			idMark := layout.trackStartGap + (sector-1)*layout.sectorBytes() + layout.gapBeforeSync + 3 + 1

			image = binary.LittleEndian.AppendUint32(image, uint32((sector-1)*sectorSize)) // data offset
			image = binary.LittleEndian.AppendUint16(image, uint16(idMark*8))
			image = binary.LittleEndian.AppendUint16(image, stxReadTime)

			id := []byte{byte(track), byte(side), byte(sector), 2} // 2: 512 B
			image = append(image, id...)
			image = binary.BigEndian.AppendUint16(image, idFieldCrc(id))

			status := byte(0)
			if s.Timing {
				status |= stxSectorVariable
			}
			if s.Fuzzy && sector == s.Spt {
				status |= stxSectorFuzzy | stxSectorCrcError
			}
			image = append(image, status, 0) // FDC status, reserved
		}

		image = append(image, fuzzy...)
		image = append(image, s.trackData(track, side)...)
		image = append(image, timing...)
	})
	return image
}

// CRC of the ID field as on the floppy: over the 3x A1 sync, the ID mark and the ID field.
func idFieldCrc(id []byte) uint16 {
	crc := uint16(0xffff)
	for _, val := range append([]byte{0xa1, 0xa1, 0xa1, 0xfe}, id...) {
		crc ^= uint16(val) << 8
		for bit := 0; bit < 8; bit++ {
			if crc&0x8000 != 0 {
				crc = crc<<1 ^ 0x1021
			} else {
				crc <<= 1
			}
		}
	}
	return crc
}
