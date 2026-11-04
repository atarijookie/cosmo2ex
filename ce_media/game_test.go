package main

import (
	"io"
	"os"
	"testing"
	"time"
)

func TestAtariKeysToDoom(t *testing.T) {
	cases := map[byte]byte{
		0x48: dkUp, 0x50: dkDown, 0x4b: dkLeft, 0x4d: dkRight,
		0x1d: dkFire, 0x39: dkUse, 0x2a: dkShift, 0x38: dkAlt,
		0x01: 27, 0x1c: 13, 0x0f: 9, 0x15: 'y', 0x31: 'n', 0x02: '1', 0x08: '7',
		0x3b: dkF1, 0x44: dkF1 + 9, 0x62: dkF1,
	}
	for sc, want := range cases {
		if got := atariToDoomKey[sc]; got != want {
			t.Errorf("scancode 0x%02x -> 0x%02x, want 0x%02x", sc, got, want)
		}
	}
	if atariToDoomKey[0x61] != 0 { // Undo quits on the ST, never reaches the game
		t.Errorf("Undo is mapped")
	}
}

func TestScaleGameFrame(t *testing.T) {
	src := make([]byte, gameW*gameH*3)
	for i := range src {
		src[i] = 200
	}
	src[0], src[1], src[2] = 1, 2, 3 // top left pixel

	// ST low: 1:1
	dst := make([]byte, 320*200*3)
	scaleGameFrame(dst, 320, 200, src)
	if dst[0] != 1 || dst[len(dst)-1] != 200 {
		t.Errorf("st-low: %v .. %v", dst[:3], dst[len(dst)-3:])
	}

	// TT high window 640 x 480: doubled, 40 black rows above and below
	dst = make([]byte, 640*480*3)
	scaleGameFrame(dst, 640, 480, src)
	row := func(y int) []byte { return dst[y*640*3 : (y+1)*640*3] }
	if row(39)[0] != 0 || row(40)[0] != 1 || row(40)[3] != 1 || row(41)[0] != 1 || row(42)[0] != 200 {
		t.Errorf("tt-high: rows 39..42 start %v %v %v %v", row(39)[:3], row(40)[:6], row(41)[:3], row(42)[:3])
	}
	if row(440)[0] != 0 || row(439)[0] != 200 {
		t.Errorf("tt-high: bottom border wrong")
	}
}

// with the real engine: CE_DOOM_TEST_BIN=../third_party/doomgeneric/ce_doomgeneric CE_DOOM_TEST_WAD=.../freedoom1.wad
func TestGameSourceReal(t *testing.T) {
	bin, wad := os.Getenv("CE_DOOM_TEST_BIN"), os.Getenv("CE_DOOM_TEST_WAD")
	if bin == "" || wad == "" {
		t.Skip("CE_DOOM_TEST_BIN / CE_DOOM_TEST_WAD not set")
	}
	dec := &Decoder{doomBin: bin, doomWad: wad}
	if err := dec.check("doom"); err != nil {
		t.Fatal(err)
	}
	l, err := computeLayout(OpenReq{machine: machST, vmode: vmodeSTLow, aout: aoutNone, flags: opfLive | opfGame}, true, false, 35, 1)
	if err != nil {
		t.Fatal(err)
	}
	src, title, err := openGameSource(dec, "doom", &l)
	if err != nil {
		t.Fatal(err)
	}
	if title != "Freedoom: Phase 1" && title != "Doom" && title != "Doom (shareware)" {
		t.Errorf("title %q", title)
	}

	rgb := make([]byte, l.vm.w*l.vm.h*3)
	t0 := time.Now()
	for i := 0; i < 20; i++ { // 1 s at 20 fps
		if err := src.ReadFrame(rgb); err != nil {
			t.Fatalf("frame %d: %v", i, err)
		}
	}
	if d := time.Since(t0); d < 800*time.Millisecond || d > 2*time.Second {
		t.Errorf("20 frames took %v, want about 1 s", d)
	}
	src.keys([]byte{0x1c, 0x9c}) // Return pressed, released

	src.Close() // the game quits when its stdin closes
	if err := src.ReadFrame(rgb); err != io.EOF {
		t.Errorf("after Close: %v, want EOF", err)
	}
	if _, _, err := openGameSource(dec, "quake", &l); err == nil {
		t.Errorf("unknown game opened")
	}
}

func TestSfxMixer(t *testing.T) {
	// a WAD with one sound: DMX, 11025 Hz, 100 samples + the 2 x 16 padding bytes, a square wave
	snd := make([]byte, 8+132)
	snd[0] = 3
	snd[2], snd[3] = 0x11, 0x2b // 11025
	snd[4] = 132
	for i := 8; i < len(snd); i++ {
		snd[i] = 128
		if i >= 24 && i < 124 {
			snd[i] = 255
			if (i/10)%2 == 0 {
				snd[i] = 1
			}
		}
	}
	dir := make([]byte, 16)
	copy(dir[8:], "DSTEST")
	wad := append([]byte("IWAD\x01\x00\x00\x00"), 0, 0, 0, 0)
	pos := uint32(len(wad))
	wad = append(wad, snd...)
	dirPos := uint32(len(wad))
	dir[0], dir[1] = byte(pos), byte(pos>>8)
	dir[4] = byte(len(snd))
	wad = append(wad, dir...)
	wad[8], wad[9] = byte(dirPos), byte(dirPos>>8)
	path := t.TempDir() + "/t.wad"
	if err := os.WriteFile(path, wad, 0644); err != nil {
		t.Fatal(err)
	}

	w, err := openWad(path)
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()
	m := newSfxMixer(w, 22050) // twice the effect's rate: 100 samples -> 200
	out := make([]int16, 300)
	m.mix(out)
	for _, v := range out {
		if v != 0 {
			t.Fatalf("sound before start")
		}
	}
	m.start(2, 127, "dstest")
	m.start(3, 127, "dsnothere") // unknown: nothing, no crash
	m.mix(out)
	loud := 0
	for i, v := range out {
		if v > 20000 || v < -20000 {
			loud++
		}
		if i >= 202 && v != 0 {
			t.Fatalf("sample %d = %d after the end", i, v)
		}
	}
	if loud < 150 {
		t.Errorf("only %d loud samples", loud)
	}
	if m.voices[2] != nil {
		t.Errorf("the ended effect still on its channel")
	}
	m.start(2, 64, "dstest") // half volume, stopped in the middle
	m.mix(out[:50])
	m.stop(2)
	m.mix(out)
	for _, v := range out {
		if v != 0 {
			t.Fatalf("sound after stop")
		}
	}
}
