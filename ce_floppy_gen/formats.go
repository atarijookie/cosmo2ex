package main

import "encoding/binary"

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
// .stx - Pasti image, the format as Hatari reads it (src/floppies/stx.c). Plain sectors: no track image, timings or fuzzy
// bits, each sector with its descriptor. All little endian, only the ID field CRC is big endian.

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

const stxTrackBytes = 6250 // 200 ms of a rotation

func generateSTX(s Spec) []byte {
	image := []byte("RSY\x00")
	image = binary.LittleEndian.AppendUint16(image, 3)            // version
	image = binary.LittleEndian.AppendUint16(image, stxToolAtari) // imaging tool
	image = binary.LittleEndian.AppendUint16(image, 0)            // reserved
	image = append(image, byte(s.Tracks*s.Sides), stxRevision)    // track blocks, revision
	image = binary.LittleEndian.AppendUint32(image, 0)            // reserved

	layout := layoutFor(s.Spt)

	s.forEachTrack(func(track, side int) {
		blockSize := stxBlockSize + s.Spt*stxBlockSize + s.Spt*sectorSize

		image = binary.LittleEndian.AppendUint32(image, uint32(blockSize))
		image = binary.LittleEndian.AppendUint32(image, 0) // fuzzy mask size
		image = binary.LittleEndian.AppendUint16(image, uint16(s.Spt))
		image = binary.LittleEndian.AppendUint16(image, stxTrackFlagSectors)
		image = binary.LittleEndian.AppendUint16(image, stxTrackBytes)
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
			image = append(image, 0, 0) // FDC status, reserved
		}

		image = append(image, s.trackData(track, side)...)
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
