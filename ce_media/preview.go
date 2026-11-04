package main

// 'preview': runs a stream like for the ST, but takes the chunks apart here - as the ST would - into PNG files of
// frames (at the monitor's aspect ratio) and a WAV of the sound (YM: the levels the chip would put out). Checks the
// chunks on the way (sizes, times), and says how fast the conversion is and what the ACSI bus would have to carry.

import (
	"encoding/binary"
	"flag"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"time"
)

func previewMain(args []string) int {
	fs := flag.NewFlagSet("preview", flag.ExitOnError)
	machine := fs.String("machine", "ste", "st, ste, tt or falcon")
	vmodeName := fs.String("vmode", "", "st-low, st-high, tt-mid, tt-high or none (default: colour mode of the machine)")
	audioName := fs.String("audio", "", "dma, ym or none (default: what the machine has)")
	start := fs.Float64("start", 0, "start at this second")
	seconds := fs.Float64("seconds", 10, "stop after this many seconds (0 = whole file)")
	every := fs.Int("frames", 0, "write every n-th frame as PNG (0 = about 10 frames)")
	outDir := fs.String("out", "preview_out", "output directory")
	maxSectors := fs.Int("max-sectors", 254, "biggest chunk the ST takes")
	live := fs.Bool("live", false, "low latency stream (1 frame per chunk)")
	halfRate := fs.Bool("half-rate", false, "sound at half the rate (YM 7877 Hz, DMA 12517 Hz)")
	game := fs.Bool("game", false, "the argument is a game on the host (doom), not a file - as CE_DOOM.PRG gets it")
	dec := decoderFlags(fs)
	fs.Parse(args)
	dec.fixup()
	if fs.NArg() != 1 {
		usage()
	}
	file := fs.Arg(0)
	defer dec.waitCleanups(5 * time.Second)
	defer dec.stopWorker()

	req := OpenReq{maxSectors: *maxSectors, startMs: int(*start * 1000), path: file, latencyMs: 1000}
	if *live {
		req.flags |= opfLive
	}
	if *halfRate {
		req.flags |= opfHalfRate
	}
	switch *machine {
	case "st":
		req.machine, req.vmode, req.aout = machST, vmodeSTLow, aoutYM
	case "ste":
		req.machine, req.vmode, req.aout = machSTE, vmodeSTLow, aoutDMA
	case "tt":
		req.machine, req.vmode, req.aout = machTT, vmodeTTMidWin, aoutDMA
	case "falcon":
		req.machine, req.vmode, req.aout = machFalcon, vmodeSTLow, aoutDMA
	default:
		fmt.Fprintf(os.Stderr, "unknown machine %s\n", *machine)
		return 2
	}
	if *vmodeName != "" {
		found := false
		for id, vm := range vmodes {
			if vm.name == *vmodeName {
				req.vmode, found = id, true
			}
		}
		if *vmodeName == "none" {
			req.vmode, found = vmodeNone, true
		}
		if !found {
			fmt.Fprintf(os.Stderr, "unknown video mode %s\n", *vmodeName)
			return 2
		}
	}
	switch *audioName {
	case "":
	case "dma":
		req.aout = aoutDMA
	case "ym":
		req.aout = aoutYM
	case "none":
		req.aout = aoutNone
	default:
		fmt.Fprintf(os.Stderr, "unknown audio %s\n", *audioName)
		return 2
	}

	var l Layout
	var src Source
	var err error
	t0 := time.Now()
	ymTable := dec.ymTable()
	if *game { // a game: no probe, its picture at Doom's 35 fps (or less), its sound effects
		req.flags |= opfLive | opfGame
		if l, err = computeLayout(req, true, req.aout != aoutNone, 35, 1); err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
		g, title, err := openGameSource(dec, file, &l)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
		src = g
		fmt.Printf("game:    %s (%s), started in %v\n", file, title, time.Since(t0).Round(time.Millisecond))
		fmt.Printf("layout:  %s %dx%d, %.3f fps, %d frames/chunk, slot %d sectors\n",
			l.vm.name, l.vm.w, l.vm.h, float64(l.fpsNum)/float64(l.fpsDen), l.F, l.slotSectors)
	} else {
		if err := dec.waitImage(nil, func(m string) { fmt.Println(m) }); err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
		pr, err := dec.probe(file)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
		if l, err = computeLayout(req, pr.hasVideo, pr.hasAudio, pr.fpsNum, pr.fpsDen); err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
		fmt.Printf("file:    %s (%s), %d ms, video %v %dx%d aspect %.3f %d/%d fps, audio %v - probe %v\n", file, pr.title,
			pr.durationMs, pr.hasVideo, pr.width, pr.height, pr.dar, pr.fpsNum, pr.fpsDen, pr.hasAudio, time.Since(t0).Round(time.Millisecond))
		if l.hasVideo() {
			aw, ah := activeSize(l.vm, pr.dar)
			fmt.Printf("layout:  %s %dx%d (picture %dx%d), %.3f fps, %d frames/chunk, slot %d sectors\n",
				l.vm.name, l.vm.w, l.vm.h, aw, ah, float64(l.fpsNum)/float64(l.fpsDen), l.F, l.slotSectors)
		}
		fmt.Printf("         audio format %d at %d Hz, chunk max %d sectors (audio max %d B)\n", l.afmt, l.rate, l.chunkSectors, l.audioMax)

		t0 = time.Now()
		if src, err = openFFmpegSource(dec, file, pr, &l, req.startMs); err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
	}
	defer src.Close()

	os.MkdirAll(*outDir, 0755)
	pv := &previewSink{l: &l, ymTable: ymTable, outDir: *outDir, every: *every, start: time.Now()}
	if pv.every <= 0 {
		pv.every = 1
		if l.hasVideo() && *seconds > 0 {
			pv.every = maxInt(1, int(*seconds*float64(l.fpsNum)/float64(l.fpsDen))/10)
		}
	}
	stop := make(chan struct{})
	pv.limitTicks = uint32(*seconds * float64(l.rate))
	pv.stop = stop

	st := newStream(&l, src, 1, req.startMs, ymTable, pv.sink, stop)
	err = st.run()
	elapsed := time.Since(t0)
	if err != nil && err != errStopped {
		fmt.Fprintln(os.Stderr, "stream:", err)
		return 1
	}

	pv.writeWav()
	contentSec := float64(pv.lastTick) / float64(l.rate)
	fmt.Printf("chunks:  %d (%d frames, %d audio bytes, %.1f kB total), %.2f s of content in %v (%.1fx real time)\n",
		pv.chunks, pv.frames, len(pv.audio), float64(pv.bytes)/1024, contentSec, elapsed.Round(time.Millisecond),
		contentSec/elapsed.Seconds())
	if contentSec > 0 {
		fmt.Printf("ACSI:    %.0f kB/s at real time (in %d B transfers, %.1f transfers/s)\n",
			float64(pv.bytes)/1024/contentSec, pv.maxChunk, float64(pv.chunks)/contentSec)
	}
	for _, e := range pv.errors {
		fmt.Println("ERROR:", e)
	}
	if len(pv.errors) > 0 {
		return 1
	}
	return 0
}

type previewSink struct {
	l          *Layout
	ymTable    []ymEntry
	outDir     string
	every      int
	start      time.Time
	stop       chan struct{}
	limitTicks uint32

	chunks, frames, bytes, maxChunk int
	nextAudio                       uint32
	lastFrameTick, lastTick         uint32
	audio                           []byte
	errors                          []string
}

func (p *previewSink) errorf(f string, a ...interface{}) {
	if len(p.errors) < 20 {
		p.errors = append(p.errors, fmt.Sprintf(f, a...))
	}
}

func (p *previewSink) sink(typ byte, b []byte) error {
	l := p.l
	if typ != msgChunk {
		return nil
	}
	if len(b) > l.chunkSectors*512 {
		p.errorf("chunk %d: %d bytes, more than %d sectors", p.chunks, len(b), l.chunkSectors)
	}
	if len(b)%16 != 0 {
		p.errorf("chunk %d: %d bytes - not a multiple of 16", p.chunks, len(b))
	}
	seq := binary.BigEndian.Uint32(b[chunkSeq:])
	if int(seq) != p.chunks {
		p.errorf("chunk %d has seq %d", p.chunks, seq)
	}
	n := int(b[chunkFrameCount])
	if n > l.F {
		p.errorf("chunk %d: %d frames, layout says at most %d", seq, n, l.F)
	}
	astart := binary.BigEndian.Uint32(b[chunkAudioStart:])
	abytes := int(binary.BigEndian.Uint32(b[chunkAudioBytes:]))
	if l.hasAudio() && abytes > 0 && astart != p.nextAudio {
		p.errorf("chunk %d: audio starts at %d, previous ended at %d", seq, astart, p.nextAudio)
	}
	if abytes%2 != 0 || abytes > l.audioMax {
		p.errorf("chunk %d: %d audio bytes (max %d)", seq, abytes, l.audioMax)
	}
	slot := l.slotSectors * 512
	if chunkHeaderSize+n*slot+abytes > len(b) {
		p.errorf("chunk %d: too short for its content", seq)
		return nil
	}

	for i := 0; i < n; i++ {
		tick := binary.BigEndian.Uint32(b[chunkFrames+i*chunkFrameEntry:])
		if p.frames > 0 && tick <= p.lastFrameTick {
			p.errorf("frame %d: tick %d not after %d", p.frames, tick, p.lastFrameTick)
		}
		p.lastFrameTick = tick
		if tick > p.lastTick {
			p.lastTick = tick
		}
		if p.frames%p.every == 0 {
			pal := make([]uint16, 16)
			for c := range pal {
				pal[c] = binary.BigEndian.Uint16(b[chunkPalettes+i*chunkPaletteSize+c*2:])
			}
			p.writePng(b[chunkHeaderSize+i*slot:chunkHeaderSize+i*slot+l.vm.frameBytes()], pal, p.frames)
		}
		p.frames++
	}

	p.audio = append(p.audio, b[chunkHeaderSize+n*slot:chunkHeaderSize+n*slot+abytes]...)
	p.nextAudio = astart + uint32(abytes)
	if l.hasAudio() && p.nextAudio > p.lastTick {
		p.lastTick = p.nextAudio
	}

	p.chunks++
	p.bytes += len(b)
	if len(b) > p.maxChunk {
		p.maxChunk = len(b)
	}
	if p.limitTicks > 0 && p.lastTick >= p.limitTicks {
		close(p.stop)
		return errStopped
	}
	return nil
}

// paletteRGB: palette register value -> colour, as the ST shows it
func (p *previewSink) paletteRGB(w uint16) color.RGBA {
	ch := func(n uint16) uint8 {
		switch {
		case p.l.machine == machST:
			return uint8((n & 7) * 255 / 7)
		case p.l.vm.id == vmodeSTLow: // STE order
			return uint8((((n << 1) & 0xe) | (n >> 3 & 1)) * 255 / 15)
		default:
			return uint8((n & 15) * 255 / 15)
		}
	}
	return color.RGBA{ch(w >> 8 & 15), ch(w >> 4 & 15), ch(w & 15), 255}
}

func (p *previewSink) writePng(data []byte, pal []uint16, num int) {
	vm := p.l.vm
	const outW, outH = 640, 480 // 4:3 like the monitor
	img := image.NewRGBA(image.Rect(0, 0, outW, outH))
	words := vm.w / 16 * vm.planes
	for oy := 0; oy < outH; oy++ {
		y := oy * vm.h / outH
		for ox := 0; ox < outW; ox++ {
			x := ox * vm.w / outW
			o := y*words*2 + (x/16)*vm.planes*2
			idx := 0
			for pl := 0; pl < vm.planes; pl++ {
				wd := uint16(data[o+pl*2])<<8 | uint16(data[o+pl*2+1])
				idx |= int(wd>>uint(15-x%16)&1) << uint(pl)
			}
			var c color.RGBA
			if vm.planes == 1 {
				c = color.RGBA{255, 255, 255, 255}
				if idx == 1 {
					c = color.RGBA{0, 0, 0, 255}
				}
			} else {
				c = p.paletteRGB(pal[idx])
			}
			img.SetRGBA(ox, oy, c)
		}
	}
	f, err := os.Create(filepath.Join(p.outDir, fmt.Sprintf("frame_%05d.png", num)))
	if err != nil {
		p.errorf("%v", err)
		return
	}
	defer f.Close()
	png.Encode(f, img)
}

func (p *previewSink) writeWav() {
	if !p.l.hasAudio() || len(p.audio) == 0 {
		return
	}
	var pcm []byte
	bits := 16
	if p.l.afmt == afmtDMA { // 8 bit WAV is unsigned
		bits = 8
		pcm = make([]byte, len(p.audio))
		for i, s := range p.audio {
			pcm[i] = s + 128
		}
	} else { // YM: the level the chip puts out
		pcm = make([]byte, len(p.audio)*2)
		for i, s := range p.audio {
			v := int16(p.ymTable[s].level/p.ymTable[255].level*65534 - 32767) // the used range to full scale
			binary.LittleEndian.PutUint16(pcm[i*2:], uint16(v))
		}
	}
	rate := p.l.rate
	h := make([]byte, 44)
	copy(h, "RIFF")
	binary.LittleEndian.PutUint32(h[4:], uint32(36+len(pcm)))
	copy(h[8:], "WAVEfmt ")
	binary.LittleEndian.PutUint32(h[16:], 16)
	binary.LittleEndian.PutUint16(h[20:], 1)
	binary.LittleEndian.PutUint16(h[22:], 1)
	binary.LittleEndian.PutUint32(h[24:], uint32(rate))
	binary.LittleEndian.PutUint32(h[28:], uint32(rate*bits/8))
	binary.LittleEndian.PutUint16(h[32:], uint16(bits/8))
	binary.LittleEndian.PutUint16(h[34:], uint16(bits))
	copy(h[36:], "data")
	binary.LittleEndian.PutUint32(h[40:], uint32(len(pcm)))
	os.WriteFile(filepath.Join(p.outDir, "audio.wav"), append(h, pcm...), 0644)
}
