package main

import (
	"encoding/binary"
	"io"
	"math"
	"os"
	"testing"
)

func TestLayoutST(t *testing.T) {
	l, err := computeLayout(OpenReq{machine: machST, vmode: vmodeSTLow, aout: aoutYM, maxSectors: 254}, true, true, 25, 1)
	if err != nil {
		t.Fatal(err)
	}
	if l.F != 3 || l.slotSectors != 63 || l.rate != ymRate || l.fpsNum != 20 || l.fpsDen != 1 {
		t.Fatalf("unexpected layout %+v", l)
	}
	if l.chunkSectors > 254 {
		t.Fatalf("chunk %d sectors", l.chunkSectors)
	}
	// audio of consecutive chunks joins up, all even, never more than audioMax
	for k := 0; k < 1000; k++ {
		a, b := l.audioStart(k), l.audioStart(k+1)
		if a%2 != 0 || b-a > l.audioMax || b < a {
			t.Fatalf("chunk %d audio %d..%d (max %d)", k, a, b, l.audioMax)
		}
	}
	if l.frameTick(20) != ymRate {
		t.Fatalf("frame 20 at tick %d, want %d", l.frameTick(20), ymRate)
	}
}

func TestLayoutTTAndLowFps(t *testing.T) {
	l, err := computeLayout(OpenReq{machine: machTT, vmode: vmodeTTMidWin, aout: aoutDMA, maxSectors: 254}, true, true, 30000, 1001)
	if err != nil {
		t.Fatal(err)
	}
	if l.slotSectors != 75 || l.F != 3 || l.chunkSectors > 254 {
		t.Fatalf("TT layout %+v", l)
	}

	// 2 fps: 3 frames would need 37 kB of audio - fewer frames per chunk
	l, err = computeLayout(OpenReq{machine: machTT, vmode: vmodeTTMidWin, aout: aoutDMA, maxSectors: 254}, true, true, 2, 1)
	if err != nil {
		t.Fatal(err)
	}
	if l.fpsNum != 2 || l.chunkSectors > 254 || l.F < 1 || l.F >= 3 {
		t.Fatalf("low fps layout %+v", l)
	}

	// small transfers
	l, err = computeLayout(OpenReq{machine: machSTE, vmode: vmodeSTLow, aout: aoutDMA, maxSectors: 100}, true, true, 25, 1)
	if err != nil || l.F != 1 || l.chunkSectors > 100 {
		t.Fatalf("100 sectors: %+v %v", l, err)
	}

	// live: 1 frame per chunk
	l, _ = computeLayout(OpenReq{machine: machSTE, vmode: vmodeSTLow, aout: aoutDMA, flags: opfLive}, true, true, 25, 1)
	if l.F != 1 {
		t.Fatalf("live F %d", l.F)
	}
}

func TestLayoutAudioOnly(t *testing.T) {
	l, err := computeLayout(OpenReq{machine: machSTE, vmode: vmodeNone, aout: aoutDMA, maxSectors: 254}, true, true, 25, 1)
	if err != nil {
		t.Fatal(err)
	}
	if l.hasVideo() || l.F != 0 || l.chunkSamples != (dmaRate/2)&^1 || l.chunkSectors > 254 {
		t.Fatalf("audio only %+v", l)
	}
	if _, err := computeLayout(OpenReq{machine: machST, vmode: vmodeNone, aout: aoutNone}, true, true, 25, 1); err == nil {
		t.Fatal("nothing to play should fail")
	}
	// file without audio: clock from the 200 Hz timer
	l, _ = computeLayout(OpenReq{machine: machSTE, vmode: vmodeSTLow, aout: aoutDMA}, true, false, 25, 1)
	if l.afmt != afmtNone || l.rate != timerRate || l.audioMax != 0 {
		t.Fatalf("no audio %+v", l)
	}
}

func TestActiveSize(t *testing.T) {
	cases := []struct {
		vm     uint8
		dar    float64
		aw, ah int
	}{
		{vmodeSTLow, 16.0 / 9, 320, 150},
		{vmodeSTLow, 4.0 / 3, 320, 200},
		{vmodeTTMidWin, 16.0 / 9, 320, 180},
		{vmodeTTMidWin, 4.0 / 3, 320, 240},
		{vmodeSTHigh, 1, 480, 400},
	}
	for _, c := range cases {
		aw, ah := activeSize(vmodes[c.vm], c.dar)
		if aw != c.aw || ah != c.ah {
			t.Errorf("%s %.3f: %dx%d, want %dx%d", vmodes[c.vm].name, c.dar, aw, ah, c.aw, c.ah)
		}
	}
}

// snr of a sine through a converter and back
func sineSNR(t *testing.T, conv AudioConv, back func(b byte) float64, rate int) float64 {
	n := rate // 1 s
	pcm := make([]int16, n)
	for i := range pcm {
		pcm[i] = int16(16000 * math.Sin(2*math.Pi*440*float64(i)/float64(rate)))
	}
	out := conv.Convert(pcm)
	if len(out) != n {
		t.Fatalf("%d bytes for %d samples", len(out), n)
	}
	// skip the fade in, fit gain + offset by least squares, measure the rest as noise
	start := rate / 10
	var sx, sy, sxx, sxy float64
	m := float64(n - start)
	for i := start; i < n; i++ {
		x, y := float64(pcm[i]), back(out[i])
		sx, sy, sxx, sxy = sx+x, sy+y, sxx+x*x, sxy+x*y
	}
	a := (m*sxy - sx*sy) / (m*sxx - sx*sx)
	b := (sy - a*sx) / m
	var sig, noise float64
	for i := start; i < n; i++ {
		x, y := float64(pcm[i]), back(out[i])
		e := y - (a*x + b)
		sig += a * x * a * x
		noise += e * e
	}
	return 10 * math.Log10(sig/noise)
}

func TestAudioDMA(t *testing.T) {
	snr := sineSNR(t, newDMAConv(dmaRate), func(b byte) float64 { return float64(int8(b)) }, dmaRate)
	if snr < 35 {
		t.Fatalf("DMA SNR %.1f dB", snr)
	}
	t.Logf("DMA 8 bit SNR %.1f dB", snr)
}

func TestAudioYM(t *testing.T) {
	table := buildYmTable()
	if len(table) != 256 {
		t.Fatal("table size")
	}
	for i := 1; i < len(table); i++ {
		if table[i].level < table[i-1].level {
			t.Fatalf("table not sorted at %d", i)
		}
	}
	if table[0].level != 0 || table[255].level < ymMaxLevel*0.98 || table[255].level > ymMaxLevel*1.02 {
		t.Fatalf("table range %f..%f", table[0].level, table[255].level)
	}
	distinct := map[float64]bool{}
	for _, e := range table {
		distinct[e.level] = true
	}
	if len(distinct) < 210 { // the used range has dense, even steps
		t.Fatalf("only %d distinct levels", len(distinct))
	}
	snr := sineSNR(t, newYMConv(table, ymRate), func(b byte) float64 { return table[b].level }, ymRate)
	if snr < 34 {
		t.Fatalf("YM SNR %.1f dB", snr)
	}
	t.Logf("YM SNR %.1f dB", snr)

	tb := ymTableBytes(table)
	if len(tb) != ymTableSize {
		t.Fatal("table bytes")
	}
	// entry 255: three longwords for registers 8, 9, 10 at the volumes of the entry
	for ch := 0; ch < 3; ch++ {
		v := binary.BigEndian.Uint32(tb[255*ymTableEntry+ch*4:])
		if v>>24 != uint32(8+ch) || v&0xff00ff != 0 || uint8(v>>8) != table[255].vol[ch] {
			t.Fatalf("entry 255 ch %d = %08x", ch, v)
		}
	}
}

func TestVideoConv(t *testing.T) {
	vm := vmodes[vmodeSTLow]
	rgb := make([]byte, vm.w*vm.h*3)
	for y := 0; y < vm.h; y++ { // black border rows, a red / blue gradient in the middle
		for x := 0; x < vm.w; x++ {
			if y < 20 || y >= 180 {
				continue
			}
			o := (y*vm.w + x) * 3
			rgb[o] = uint8(x * 255 / vm.w)
			rgb[o+2] = uint8(255 - x*255/vm.w)
		}
	}

	c := newVideoConv(vm, machSTE)
	bp, pal := c.Convert(rgb)
	if len(bp) != vm.frameBytes() || len(pal) != 16 {
		t.Fatalf("%d bytes, %d colours", len(bp), len(pal))
	}
	if pal[0] != 0 {
		t.Fatalf("colour 0 = %03x, want black", pal[0])
	}
	for i := 0; i < 20*vm.w/16*4*2; i++ { // border rows: colour 0 everywhere
		if bp[i] != 0 {
			t.Fatalf("border byte %d = %02x", i, bp[i])
		}
	}

	// same picture again: the palette is kept
	_, pal2 := c.Convert(rgb)
	for i := range pal {
		if pal[i] != pal2[i] {
			t.Fatal("palette changed for the same picture")
		}
	}

	// STE order: level 15 = 0xf, level 1 = 0x8
	if w := (&VideoConv{bits: 4, steOrder: true}).paletteWords([][3]uint8{{255, 17, 0}}); w[0] != 0xf80 {
		t.Fatalf("STE palette word %03x", w[0])
	}
	if w := (&VideoConv{bits: 3}).paletteWords([][3]uint8{{255, 0, 255}}); w[0] != 0x707 {
		t.Fatalf("ST palette word %03x", w[0])
	}
}

func TestBitplanes(t *testing.T) {
	idx := make([]uint8, 16)
	idx[0] = 1  // plane 0, bit 15
	idx[15] = 2 // plane 1, bit 0
	b := toBitplanes(idx, 16, 1, 2)
	if len(b) != 4 || b[0] != 0x80 || b[1] != 0 || b[2] != 0 || b[3] != 1 {
		t.Fatalf("bitplanes % x", b)
	}
}

// fakeSource: n frames of grey, audio of a given length - like a game would give
type fakeSource struct {
	frames, samples int
}

func (f *fakeSource) ReadFrame(rgb []byte) error {
	if f.frames == 0 {
		return io.EOF
	}
	f.frames--
	for i := range rgb {
		rgb[i] = 128
	}
	return nil
}

func (f *fakeSource) ReadAudio(pcm []int16) (int, error) {
	n := len(pcm)
	if n > f.samples {
		n = f.samples
	}
	f.samples -= n
	if n < len(pcm) {
		return n, io.EOF
	}
	return n, nil
}

func (f *fakeSource) Close() {}

func runFake(t *testing.T, l *Layout, src Source) [][]byte {
	var chunks [][]byte
	st := newStream(l, src, 7, 1500, buildYmTable(), func(typ byte, p []byte) error {
		if typ != msgChunk {
			t.Fatalf("message %02x", typ)
		}
		chunks = append(chunks, p)
		return nil
	}, make(chan struct{}))
	if err := st.run(); err != nil {
		t.Fatal(err)
	}
	return chunks
}

func TestStreamChunks(t *testing.T) {
	l, _ := computeLayout(OpenReq{machine: machSTE, vmode: vmodeSTLow, aout: aoutDMA, maxSectors: 254}, true, true, 20, 1)

	// 10 frames (0.5 s) of video, 1 s of audio: 4 chunks with frames (3+3+3+1), then audio only chunks, the last one END
	chunks := runFake(t, &l, &fakeSource{frames: 10, samples: dmaRate})
	total, frames := 0, 0
	for i, c := range chunks {
		if binary.BigEndian.Uint32(c[chunkGeneration:]) != 7 || binary.BigEndian.Uint32(c[chunkSeq:]) != uint32(i) ||
			binary.BigEndian.Uint32(c[chunkGenStartMs:]) != 1500 {
			t.Fatalf("chunk %d header % x", i, c[:24])
		}
		end := c[chunkFlags]&chfEnd != 0
		if end != (i == len(chunks)-1) {
			t.Fatalf("chunk %d end flag %v", i, end)
		}
		n := int(c[chunkFrameCount])
		frames += n
		ab := int(binary.BigEndian.Uint32(c[chunkAudioBytes:]))
		if int(binary.BigEndian.Uint32(c[chunkAudioStart:])) != total {
			t.Fatalf("chunk %d audio start %d, want %d", i, binary.BigEndian.Uint32(c[chunkAudioStart:]), total)
		}
		total += ab
		if len(c) < chunkHeaderSize+n*l.slotSectors*512+ab || len(c) > l.chunkSectors*512 {
			t.Fatalf("chunk %d size %d", i, len(c))
		}
	}
	if frames != 10 || total < dmaRate {
		t.Fatalf("%d frames, %d samples", frames, total)
	}

	// video longer than audio: silence fills up, ends with the video (the end is seen at the next read - an empty
	// END chunk follows the 3 full ones)
	chunks = runFake(t, &l, &fakeSource{frames: 9, samples: 100})
	if len(chunks) != 4 || binary.BigEndian.Uint32(chunks[2][chunkAudioBytes:]) == 0 || chunks[2][chunkFlags]&chfEnd != 0 ||
		chunks[3][chunkFlags]&chfEnd == 0 || chunks[3][chunkFrameCount] != 0 || binary.BigEndian.Uint32(chunks[3][chunkAudioBytes:]) != 0 {
		t.Fatalf("%d chunks", len(chunks))
	}
}

func TestPullProgress(t *testing.T) {
	s := &imageState{layers: map[string]bool{}}
	lines := []string{"1.36: Pulling from library/busybox", "034d6572bf28: Pulling fs layer", "aaaa: Waiting",
		"034d6572bf28: Download complete", "5108bc06b83a: Download complete", "034d6572bf28: Pull complete",
		"Digest: sha256:73aa", "Status: Downloaded newer image for busybox:1.36"}
	for _, l := range lines {
		if id, st, ok := parsePullLine(l); ok {
			switch st {
			case "Pulling fs layer", "Waiting":
				if _, seen := s.layers[id]; !seen {
					s.layers[id] = false
				}
			case "Pull complete", "Already exists":
				s.layers[id] = true
			}
		}
	}
	if got := s.progressText(); got != "Downloading ffmpeg image 1/2" {
		t.Fatalf("progress %q", got)
	}
	if _, _, ok := parsePullLine("Digest: sha256:73aa"); ok {
		t.Fatal("Digest line taken as a layer")
	}
}

func TestYmMixAndLoadTable(t *testing.T) {
	// the loudest channel dominates: one channel one step up beats two more mid ones
	if ymMix(14, 3, 2) <= ymMix(13, 11, 9) || ymMix(15, 8, 7) <= ymMix(14, 12, 12) {
		t.Fatal("mix model doesn't let the loudest channel dominate")
	}
	if m := ymMix(15, 15, 15); m < 0.999 || m > 1.001 {
		t.Fatalf("full output %f", m)
	}

	// a table file in the ST's format comes back with the same volumes, index = level
	built := buildYmTable()
	path := t.TempDir() + "/vol.tab"
	if err := os.WriteFile(path, ymTableBytes(built), 0644); err != nil {
		t.Fatal(err)
	}
	loaded, err := loadYmTable(path)
	if err != nil {
		t.Fatal(err)
	}
	for i := range loaded {
		if loaded[i].vol != built[i].vol || loaded[i].level != float64(i)/255 {
			t.Fatalf("entry %d: %v %f", i, loaded[i].vol, loaded[i].level)
		}
	}
	bad := ymTableBytes(built)
	bad[5*ymTableEntry] = 7 // not a volume register
	os.WriteFile(path, bad, 0644)
	if _, err := loadYmTable(path); err == nil {
		t.Fatal("a broken table was taken")
	}
}
