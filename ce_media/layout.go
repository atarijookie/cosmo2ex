package main

import (
	"encoding/binary"
	"fmt"
)

// What a stream looks like for one ST: frame format, audio format, frame rate and how chunks are cut. Chunk k of a
// generation holds frames [k*F, (k+1)*F) and the audio samples [audioStart(k), audioStart(k+1)); all times are in
// clock ticks (the audio sample rate, or 200 Hz without audio) since the start of the generation.

type vmodeInfo struct {
	id        uint8
	name      string
	w, h      int     // frame pixels
	planes    int     // 4 = 16 colours, 1 = mono
	par       float64 // pixel aspect ratio on the monitor (width / height of one pixel)
	defMaxFps int
}

var vmodes = map[uint8]vmodeInfo{
	// ST low / high fill a 4:3 monitor: a pixel is (4/3) / (320/200) = 5/6 as wide as tall
	vmodeSTLow:  {vmodeSTLow, "st-low", 320, 200, 4, 5.0 / 6.0, 20},
	vmodeSTHigh: {vmodeSTHigh, "st-high", 640, 400, 1, 5.0 / 6.0, 20},
	// TT: half of TT mid / TT high (square pixels), in the middle of the screen - full screens would be 150 kB each
	vmodeTTMidWin:  {vmodeTTMidWin, "tt-mid", 320, 240, 4, 1, 20},
	vmodeTTHighWin: {vmodeTTHighWin, "tt-high", 640, 480, 1, 1, 20},
}

func (v vmodeInfo) frameBytes() int { return v.w / 16 * v.planes * 2 * v.h }

const (
	dmaRate    = 25033 // STE / TT / Falcon DMA sound 25 kHz (8 MHz / 320)
	dmaModeReg = 0x82  // mono, 25 kHz
	ymRate     = 15754 // MFP timer A: 2.4576 MHz / 4 / 39 (15753.8 Hz) - the nearest it gets to the 15650 Hz of ST demos
	ymCtrl     = 1     // prescaler 4
	ymData     = 39
	timerRate  = 200 // clock without audio: the 200 Hz system timer

	defaultFramesPerChunk = 3   // 3 ST frames + audio fit in one 127 kB ACSI transfer
	audioOnlyChunkMs      = 500 // audio only: half a second per chunk
	liveAudioOnlyChunkMs  = 40  // low latency audio only
	maxSectorsLimit       = 254
)

type OpenReq struct {
	gen        uint32
	machine    uint8
	vmode      uint8
	aout       uint8
	flags      uint8
	latencyMs  int
	maxFps     int
	maxSectors int
	startMs    int
	path       string
}

func parseOpen(p []byte) (OpenReq, error) {
	if len(p) < 4+openHeaderSize+1 {
		return OpenReq{}, fmt.Errorf("OPEN too short (%d bytes)", len(p))
	}
	h := p[4 : 4+openHeaderSize]
	return OpenReq{
		gen:        binary.BigEndian.Uint32(p),
		machine:    h[openMachine],
		vmode:      h[openVMode],
		aout:       h[openAOut],
		flags:      h[openFlags],
		latencyMs:  int(binary.BigEndian.Uint16(h[openLatency:])),
		maxFps:     int(h[openMaxFps]),
		maxSectors: int(h[openMaxSectors]),
		startMs:    int(binary.BigEndian.Uint32(h[openStartMs:])),
		path:       string(p[4+openHeaderSize:]),
	}, nil
}

type Layout struct {
	vm      vmodeInfo // id vmodeNone = no video
	machine uint8
	afmt    uint8
	rate    int // clock ticks per second
	live    bool

	fpsNum, fpsDen int // frames per second = fpsNum / fpsDen
	F              int // frames per chunk, 0 = audio only

	slotSectors  int
	audioMax     int // most audio bytes in a chunk
	chunkSectors int // most sectors of a chunk
	chunkSamples int // audio only: samples per chunk
}

func (l *Layout) hasVideo() bool { return l.vm.id != vmodeNone }
func (l *Layout) hasAudio() bool { return l.afmt != afmtNone }

// frameTick: clock tick at which frame j of the generation is shown
func (l *Layout) frameTick(j int) uint32 {
	return uint32((int64(j)*int64(l.rate)*int64(l.fpsDen) + int64(l.fpsNum)/2) / int64(l.fpsNum))
}

// audioStart: first sample of chunk k - even, the DMA sound plays words
func (l *Layout) audioStart(k int) int {
	if !l.hasAudio() {
		return 0
	}
	if !l.hasVideo() {
		return k * l.chunkSamples
	}
	s := (int64(k*l.F)*int64(l.rate)*int64(l.fpsDen) + int64(l.fpsNum)/2) / int64(l.fpsNum)
	return int(s &^ 1)
}

func ceilDiv(a, b int) int { return (a + b - 1) / b }

// computeLayout: what this ST gets for this file. srcFpsNum / srcFpsDen = 0 when unknown.
func computeLayout(req OpenReq, hasVideo, hasAudio bool, srcFpsNum, srcFpsDen int) (Layout, error) {
	l := Layout{machine: req.machine, live: req.flags&opfLive != 0}

	maxSectors := req.maxSectors
	if maxSectors <= 0 || maxSectors > maxSectorsLimit {
		maxSectors = maxSectorsLimit
	}

	switch {
	case !hasAudio || req.aout == aoutNone:
		l.afmt, l.rate = afmtNone, timerRate
	case req.aout == aoutDMA:
		l.afmt, l.rate = afmtDMA, dmaRate
	case req.aout == aoutYM:
		l.afmt, l.rate = afmtYM, ymRate
	default:
		return l, fmt.Errorf("unknown audio output %d", req.aout)
	}

	if hasVideo && req.vmode != vmodeNone {
		vm, ok := vmodes[req.vmode]
		if !ok {
			return l, fmt.Errorf("unknown video mode %d", req.vmode)
		}
		l.vm = vm
	}

	if !l.hasVideo() {
		if !l.hasAudio() {
			return l, fmt.Errorf("nothing to play - no video for this screen and no audio")
		}
		ms := audioOnlyChunkMs
		if l.live {
			ms = liveAudioOnlyChunkMs
		}
		l.chunkSamples = (l.rate * ms / 1000) &^ 1
		if max := ((maxSectors - 1) * 512) &^ 1; l.chunkSamples > max {
			l.chunkSamples = max
		}
		l.audioMax = l.chunkSamples
		l.chunkSectors = 1 + ceilDiv(l.audioMax, 512)
		l.fpsNum, l.fpsDen = 1, 1
		return l, nil
	}

	// frame rate: the file's, at most the mode's (or ST's) limit - the fps filter drops frames to get there
	maxFps := l.vm.defMaxFps
	if req.maxFps > 0 && req.maxFps < maxFps {
		maxFps = req.maxFps
	}
	if srcFpsNum > 0 && srcFpsDen > 0 && srcFpsNum <= maxFps*srcFpsDen {
		l.fpsNum, l.fpsDen = srcFpsNum, srcFpsDen
	} else {
		l.fpsNum, l.fpsDen = maxFps, 1
	}

	l.slotSectors = ceilDiv(l.vm.frameBytes(), 512)

	audioFor := func(f int) int { // most audio bytes of a chunk with f frames: the samples of f frames, rounded to even both ends
		if !l.hasAudio() {
			return 0
		}
		return (ceilDiv(f*l.rate*l.fpsDen, l.fpsNum) + 3) &^ 1
	}

	l.F = defaultFramesPerChunk
	if l.live {
		l.F = 1
	}
	for ; l.F > 1; l.F-- { // fewer frames when they don't fit (low frame rate = lots of audio per frame, small maxSectors)
		if 1+l.F*l.slotSectors+ceilDiv(audioFor(l.F), 512) <= maxSectors {
			break
		}
	}
	l.audioMax = audioFor(l.F)
	l.chunkSectors = 1 + l.F*l.slotSectors + ceilDiv(l.audioMax, 512)
	if l.chunkSectors > maxSectors {
		return l, fmt.Errorf("one frame doesn't fit in %d sectors", maxSectors)
	}
	return l, nil
}

// infoBlock: the GET_INFO reply
func infoBlock(state uint8, l *Layout, durationMs int, title, msg string) []byte {
	b := make([]byte, infoSize)
	b[infoState] = state
	if l != nil {
		b[infoVMode] = l.vm.id
		b[infoAFmt] = l.afmt
		b[infoFramesPerChunk] = uint8(l.F)
		if l.hasVideo() {
			binary.BigEndian.PutUint16(b[infoFrameBytes:], uint16(l.vm.frameBytes()))
			binary.BigEndian.PutUint16(b[infoSlotSectors:], uint16(l.slotSectors))
			binary.BigEndian.PutUint16(b[infoFrameW:], uint16(l.vm.w))
			binary.BigEndian.PutUint16(b[infoFrameH:], uint16(l.vm.h))
			b[infoPlanes] = uint8(l.vm.planes)
			binary.BigEndian.PutUint32(b[infoFpsX1000:], uint32(int64(l.fpsNum)*1000/int64(l.fpsDen)))
		}
		binary.BigEndian.PutUint32(b[infoClockRate:], uint32(l.rate))
		b[infoDmaMode] = dmaModeReg
		b[infoYmTimerCtrl] = ymCtrl
		b[infoYmTimerData] = ymData
		binary.BigEndian.PutUint16(b[infoChunkSectors:], uint16(l.chunkSectors))
		binary.BigEndian.PutUint16(b[infoAudioMax:], uint16(l.audioMax))
	}
	binary.BigEndian.PutUint32(b[infoDurationMs:], uint32(durationMs))
	putCString(b[infoTitle:infoTitle+infoTitleSize], title)
	putCString(b[infoMsg:infoMsg+infoMsgSize], msg)
	return b
}

// putCString: zero terminated, cut to fit, non-ASCII replaced (the ST font has its own upper half)
func putCString(dst []byte, s string) {
	n := 0
	for _, r := range s {
		if n >= len(dst)-1 {
			break
		}
		if r < 0x20 || r > 0x7e {
			r = '?'
		}
		dst[n] = byte(r)
		n++
	}
	dst[n] = 0
}
