package main

import (
	"bytes"
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
)

func allSpecs() []Spec {
	specs := []Spec{}
	for _, spt := range []int{8, 9, 10, 11} {
		for _, sides := range []int{1, 2} {
			for _, data := range []string{"trsise", "random"} {
				specs = append(specs, Spec{Tracks: 80, Sides: sides, Spt: spt, Data: data, Seed: 7})
			}
		}
	}
	return specs
}

func (s Spec) wholeImage() []byte {
	return generateST(s)
}

func TestTrSiSeIsWhatTestFloppyExpects(t *testing.T) {
	s := Spec{Data: "trsise"}
	data := s.sectorData(79, 1, 9)

	if len(data) != 512 || data[0] != 79 || data[1] != 1 || data[2] != 9 {
		t.Fatalf("bad start of sector: % x", data[:3])
	}
	for i := 3; i < 512; i++ { // as generateWriteData() in test_ce/test_floppy/main.c
		if data[i] != byte(i) {
			t.Fatalf("byte %d is %d", i, data[i])
		}
	}
}

func TestRandomDataIsReproducibleAndDiffers(t *testing.T) {
	a := Spec{Data: "random", Seed: 1}
	if !bytes.Equal(a.sectorData(5, 1, 3), a.sectorData(5, 1, 3)) {
		t.Fatal("not reproducible")
	}
	if bytes.Equal(a.sectorData(5, 1, 3), a.sectorData(5, 1, 4)) || bytes.Equal(a.sectorData(5, 1, 3), a.sectorData(5, 0, 3)) || bytes.Equal(a.sectorData(5, 1, 3), a.sectorData(6, 1, 3)) {
		t.Fatal("sectors have the same data")
	}
	b := Spec{Data: "random", Seed: 2}
	if bytes.Equal(a.sectorData(5, 1, 3), b.sectorData(5, 1, 3)) {
		t.Fatal("seed doesn't change the data")
	}

	// fixed value: the same data on any machine and Go version
	if got := a.sectorData(5, 1, 3)[:4]; !bytes.Equal(got, randomDataStart) {
		t.Fatalf("random data changed: % x", got)
	}
}

var randomDataStart = []byte{0xc4, 0x29, 0x99, 0x33}

func TestStSizeAndOrder(t *testing.T) {
	s := Spec{Tracks: 80, Sides: 2, Spt: 9, Data: "trsise"}
	image := generateST(s)

	if len(image) != 737280 {
		t.Fatalf("size %d", len(image))
	}

	offset := (40*2 + 1) * 9 * 512 // track 40, side 1
	offset += 4 * 512              // sector 5
	if !bytes.Equal(image[offset:offset+3], []byte{40, 1, 5}) {
		t.Fatalf("got % x", image[offset:offset+3])
	}
}

// the way MSA_UnCompress in ce_cores/fdd/msa.cpp decodes it
func msaDecode(t *testing.T, file []byte) (image []byte, spt, sides, tracks int) {
	t.Helper()

	if binary.BigEndian.Uint16(file) != 0x0E0F {
		t.Fatal("bad MSA ID")
	}
	spt = int(binary.BigEndian.Uint16(file[2:]))
	sides = int(binary.BigEndian.Uint16(file[4:])) + 1
	start := int(binary.BigEndian.Uint16(file[6:]))
	tracks = int(binary.BigEndian.Uint16(file[8:])) + 1
	if start != 0 {
		t.Fatalf("start track %d", start)
	}

	p := file[10:]
	trackSize := spt * 512

	for i := 0; i < tracks*sides; i++ {
		length := int(binary.BigEndian.Uint16(p))
		if length > trackSize || length > len(p)-2 {
			t.Fatalf("track item %d: length %d", i, length)
		}
		data := p[2 : 2+length]
		p = p[2+length:]

		if length == trackSize {
			image = append(image, data...)
			continue
		}

		done := 0
		for done < trackSize {
			if len(data) == 0 {
				t.Fatalf("track item %d: data too short", i)
			}
			if data[0] != 0xE5 {
				image = append(image, data[0])
				data = data[1:]
				done++
				continue
			}
			if len(data) < 4 {
				t.Fatalf("track item %d: run cut off", i)
			}
			count := int(binary.BigEndian.Uint16(data[2:]))
			if done+count > trackSize {
				t.Fatalf("track item %d: run too long", i)
			}
			image = append(image, bytes.Repeat([]byte{data[1]}, count)...)
			done += count
			data = data[4:]
		}
		if len(data) != 0 {
			t.Fatalf("track item %d: %d bytes left", i, len(data))
		}
	}

	if len(p) != 0 {
		t.Fatalf("%d bytes after the last track", len(p))
	}
	return
}

func TestMsaDecodesToTheSameSectors(t *testing.T) {
	for _, s := range allSpecs() {
		image, spt, sides, tracks := msaDecode(t, generateMSA(s))

		if spt != s.Spt || sides != s.Sides || tracks != s.Tracks {
			t.Fatalf("%+v: got geometry %d %d %d", s, spt, sides, tracks)
		}
		if !bytes.Equal(image, s.wholeImage()) {
			t.Fatalf("%+v: different data", s)
		}
	}
}

func TestMsaCompress(t *testing.T) {
	for _, data := range [][]byte{
		{1, 2, 3},
		bytes.Repeat([]byte{9}, 5000),
		bytes.Repeat([]byte{0xE5}, 7),
		{0xE5},
		{1, 0xE5, 2, 2, 2, 2, 3, 3, 3, 3, 3, 0xE5, 0xE5},
	} {
		packed := msaCompress(data)
		got := []byte{}
		for len(packed) > 0 {
			if packed[0] == 0xE5 {
				got = append(got, bytes.Repeat([]byte{packed[1]}, int(binary.BigEndian.Uint16(packed[2:])))...)
				packed = packed[4:]
			} else {
				got = append(got, packed[0])
				packed = packed[1:]
			}
		}
		if !bytes.Equal(got, data) {
			t.Fatalf("% x: got % x", data, got)
		}
	}

	if len(msaCompress(bytes.Repeat([]byte{9}, 5000))) != 4 {
		t.Fatal("run not compressed")
	}
}

func TestStxStructure(t *testing.T) {
	for _, s := range allSpecs() {
		file := generateSTX(s)

		if string(file[:4]) != "RSY\x00" || binary.LittleEndian.Uint16(file[4:]) != 3 || int(file[10]) != s.Tracks*s.Sides || file[11] != 2 {
			t.Fatalf("%+v: bad header % x", s, file[:16])
		}

		p := file[16:]
		s.forEachTrack(func(track, side int) {
			blockSize := int(binary.LittleEndian.Uint32(p))
			if binary.LittleEndian.Uint32(p[4:]) != 0 || int(binary.LittleEndian.Uint16(p[8:])) != s.Spt || binary.LittleEndian.Uint16(p[10:]) != 1 {
				t.Fatalf("%+v: bad track header % x", s, p[:16])
			}
			if int(p[14]) != track|side<<7 {
				t.Fatalf("%+v: track number %x", s, p[14])
			}

			descriptors := p[16:]
			data := descriptors[s.Spt*16:]
			lastBit := 0

			for sector := 1; sector <= s.Spt; sector++ {
				d := descriptors[(sector-1)*16:]
				offset := binary.LittleEndian.Uint32(d)
				bit := int(binary.LittleEndian.Uint16(d[4:]))

				if bit <= lastBit || bit >= 6250*8 {
					t.Fatalf("%+v: sector %d at bit %d", s, sector, bit)
				}
				lastBit = bit

				if int(d[8]) != track || int(d[9]) != side || int(d[10]) != sector || d[11] != 2 || d[14] != 0 {
					t.Fatalf("%+v: bad ID % x", s, d[:16])
				}
				if binary.BigEndian.Uint16(d[12:]) != idFieldCrc(d[8:12]) {
					t.Fatalf("%+v: bad ID CRC", s)
				}
				if !bytes.Equal(data[offset:offset+512], s.sectorData(track, side, sector)) {
					t.Fatalf("%+v: bad data of sector %d", s, sector)
				}
			}

			if blockSize != 16+s.Spt*16+s.Spt*512 {
				t.Fatalf("%+v: block size %d", s, blockSize)
			}
			p = p[blockSize:]
		})

		if len(p) != 0 {
			t.Fatalf("%+v: %d bytes after the last track", s, len(p))
		}
	}
}

func TestStxProtectionTricks(t *testing.T) {
	s := Spec{Tracks: 2, Sides: 1, Spt: 9, Data: "trsise", Timing: true, Fuzzy: true, Long: true}
	file, err := generate("stx", s)
	if err != nil {
		t.Fatal(err)
	}

	p := file[16:]
	if fuzzySize := binary.LittleEndian.Uint32(p[4:]); fuzzySize != 512 {
		t.Fatalf("fuzzy size %d", fuzzySize)
	}
	if size := binary.LittleEndian.Uint16(p[12:]); size != 6500 {
		t.Fatalf("track size %d", size)
	}

	descriptors := p[16:]
	if descriptors[14] != 0x01 || descriptors[8*16+14] != 0x89 {
		t.Fatalf("status % x % x", descriptors[14], descriptors[8*16+14])
	}

	fuzzy := descriptors[9*16:]
	if fuzzy[0] != 0xff || fuzzy[255] != 0xff || fuzzy[256] != 0xf0 || fuzzy[511] != 0xf0 {
		t.Fatal("bad fuzzy mask")
	}

	data := fuzzy[512:]
	if !bytes.Equal(data[8*512:8*512+512], s.sectorData(0, 0, 9)) {
		t.Fatal("bad sector data")
	}

	timing := data[9*512:]
	if binary.LittleEndian.Uint16(timing) != 5 || binary.LittleEndian.Uint16(timing[2:]) != 4+9*64 {
		t.Fatalf("bad timing header % x", timing[:4])
	}
	if timing[4] != 0 || timing[5] != 0x7f || timing[4+16] != 0 || timing[5+16] != 0x85 || timing[5+32] != 0x79 {
		t.Fatalf("bad timing data % x", timing[4:40])
	}

	blockSize := int(binary.LittleEndian.Uint32(p))
	if blockSize != 16+9*16+512+9*512+4+9*64 {
		t.Fatalf("block size %d", blockSize)
	}
	if len(file) != 16+2*blockSize {
		t.Fatalf("file size %d", len(file))
	}

	for _, format := range []string{"st", "msa"} {
		if _, err := generate(format, s); err == nil {
			t.Errorf("%s accepted the tricks", format)
		}
	}

	if name := s.fileName("stx"); name != "2t_9spt_1s_trsise_timing_fuzzy_long.stx" {
		t.Errorf("name %s", name)
	}
}

func TestIdFieldCrcOfKnownValue(t *testing.T) {
	// track 0, side 0, sector 1, 512 B - CRC of this ID field on every standard ST floppy
	if crc := idFieldCrc([]byte{0, 0, 1, 2}); crc != 0xCA6F {
		t.Fatalf("got %04x", crc)
	}
}

func TestLayoutFitsTrack(t *testing.T) {
	for spt := 8; spt <= 11; spt++ {
		l := layoutFor(spt)
		if size := l.trackStartGap + spt*l.sectorBytes(); size > stxTrackBytes {
			t.Fatalf("%d sectors: %d bytes", spt, size)
		}
	}
}

func TestBadParametersAreRefused(t *testing.T) {
	good := Spec{Tracks: 80, Sides: 2, Spt: 9, Data: "trsise"}

	bad := []Spec{good, good, good, good, good, good, good}
	bad[0].Tracks = 0
	bad[1].Tracks = 86
	bad[2].Sides = 3
	bad[3].Spt = 7
	bad[4].Spt = 12
	bad[5].Data = "x"

	for _, s := range bad[:6] {
		if _, err := generate("st", s); err == nil {
			t.Fatalf("%+v accepted", s)
		}
	}
	if _, err := generate("img", good); err == nil {
		t.Fatal("format img accepted")
	}
}

func TestCommandLine(t *testing.T) {
	dir := t.TempDir()

	out := filepath.Join(dir, "a.msa")
	if err := run([]string{"-format", "msa", "-spt", "10", "-sides", "1", "-data", "random", "-tracks", "82", "-seed", "5", "-o", out}); err != nil {
		t.Fatal(err)
	}
	file, err := os.ReadFile(out)
	if err != nil {
		t.Fatal(err)
	}
	_, spt, sides, tracks := msaDecode(t, file)
	if spt != 10 || sides != 1 || tracks != 82 {
		t.Fatalf("got %d %d %d", spt, sides, tracks)
	}

	if err := run([]string{"-all", filepath.Join(dir, "all")}); err != nil {
		t.Fatal(err)
	}
	names, err := filepath.Glob(filepath.Join(dir, "all", "*"))
	if err != nil {
		t.Fatal(err)
	}
	if len(names) != 3*3*2*2 {
		t.Fatalf("%d images", len(names))
	}

	if err := run([]string{"-spt", "7", "-o", filepath.Join(dir, "x")}); err == nil {
		t.Fatal("bad parameter accepted")
	}
	if err := run([]string{"stray"}); err == nil {
		t.Fatal("stray argument accepted")
	}
}
