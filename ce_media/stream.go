package main

// A stream: raw frames and PCM from a Source -> chunks as the ST gets them (media_commands.h, MED_CHUNK_*).
// The Source is ffmpeg for files; anything else that makes RGB frames and PCM (a game running on the host) can be one.

import (
	"encoding/binary"
	"errors"
	"io"
)

// Source of raw media: frames of exactly layout w x h RGB at the layout's frame rate, mono signed 16 bit PCM at the
// layout's clock rate. Both return io.EOF at the end; ReadAudio fills as much as there is.
type Source interface {
	ReadFrame(rgb []byte) error
	ReadAudio(pcm []int16) (int, error)
	Close()
}

var errStopped = errors.New("stream stopped")

type Stream struct {
	l          *Layout
	src        Source
	gen        uint32
	genStartMs int
	video      *VideoConv
	audio      AudioConv
	sink       func(typ byte, payload []byte) error
	stop       <-chan struct{}
	waitReady  func() error // live streams: before each chunk, until the ST is ready for another one (nil: never)
}

func newStream(l *Layout, src Source, gen uint32, genStartMs int, ymTable []ymEntry,
	sink func(byte, []byte) error, stop <-chan struct{}) *Stream {
	s := &Stream{l: l, src: src, gen: gen, genStartMs: genStartMs, sink: sink, stop: stop}
	if l.hasVideo() {
		s.video = newVideoConv(l.vm, l.machine)
	}
	switch l.afmt {
	case afmtDMA:
		s.audio = newDMAConv(l.rate)
	case afmtYM:
		s.audio = newYMConv(ymTable, l.rate)
	}
	return s
}

func (s *Stream) stopped() bool {
	select {
	case <-s.stop:
		return true
	default:
		return false
	}
}

// run sends chunks until the source ends (the last chunk has MED_CHF_END) or the stream is stopped
func (s *Stream) run() error {
	l := s.l
	var rgb []byte
	if l.hasVideo() {
		rgb = make([]byte, l.vm.w*l.vm.h*3)
	}
	videoEOF := !l.hasVideo()
	audioEOF := !l.hasAudio()

	for k := 0; ; k++ {
		if s.stopped() {
			return errStopped
		}
		if s.waitReady != nil {
			if err := s.waitReady(); err != nil {
				return err
			}
		}

		var frames [][]byte
		var pals [][]uint16
		for len(frames) < l.F && !videoEOF {
			if err := s.src.ReadFrame(rgb); err != nil {
				if err != io.EOF {
					return err
				}
				videoEOF = true
				break
			}
			bp, pal := s.video.Convert(rgb)
			frames = append(frames, bp)
			pals = append(pals, pal)
		}

		var audio []byte
		realAudio := 0
		if l.hasAudio() {
			pcm := make([]int16, l.audioStart(k+1)-l.audioStart(k))
			if !audioEOF {
				n, err := s.src.ReadAudio(pcm)
				realAudio = n
				if err != nil {
					if err != io.EOF {
						return err
					}
					audioEOF = true // the rest of pcm stays silence
				}
			}
			if !(audioEOF && realAudio == 0 && videoEOF) {
				audio = s.audio.Convert(pcm)
			}
		}

		end := videoEOF && audioEOF
		if err := s.sink(msgChunk, s.chunk(k, frames, pals, audio, end)); err != nil {
			return err
		}
		if end {
			return nil
		}
	}
}

func (s *Stream) chunk(k int, frames [][]byte, pals [][]uint16, audio []byte, end bool) []byte {
	l := s.l
	slot := l.slotSectors * 512
	size := chunkHeaderSize + len(frames)*slot + len(audio)
	size = (size + 15) &^ 15 // the DMA moves 16 B at a time
	b := make([]byte, size)

	binary.BigEndian.PutUint32(b[chunkGeneration:], s.gen)
	binary.BigEndian.PutUint32(b[chunkSeq:], uint32(k))
	if end {
		b[chunkFlags] |= chfEnd
	}
	b[chunkFrameCount] = uint8(len(frames))
	binary.BigEndian.PutUint32(b[chunkAudioStart:], uint32(l.audioStart(k)))
	binary.BigEndian.PutUint32(b[chunkAudioBytes:], uint32(len(audio)))
	binary.BigEndian.PutUint32(b[chunkGenStartMs:], uint32(s.genStartMs))

	for i := range frames {
		binary.BigEndian.PutUint32(b[chunkFrames+i*chunkFrameEntry:], l.frameTick(k*l.F+i))
		for c, w := range pals[i] {
			if c < chunkPaletteSize/2 {
				binary.BigEndian.PutUint16(b[chunkPalettes+i*chunkPaletteSize+c*2:], w)
			}
		}
		copy(b[chunkHeaderSize+i*slot:], frames[i])
	}
	copy(b[chunkHeaderSize+len(frames)*slot:], audio)
	return b
}

//--------------------------------------------------------------------------------------------------------------------
// ffmpeg as the source

type ffmpegSource struct {
	video, audio *proc
	first        []byte // the 1st frame, read at open to see that decoding works
	abuf         []byte
}

func openFFmpegSource(dec *Decoder, file string, pr *Probe, l *Layout, startMs int) (*ffmpegSource, error) {
	s := &ffmpegSource{}
	var err error

	if l.hasVideo() {
		aw, ah := activeSize(l.vm, pr.dar)
		s.video, err = dec.start(file, videoArgs(pr, startMs, l.fpsNum, l.fpsDen, l.vm.w, l.vm.h, aw, ah))
		if err != nil {
			return nil, err
		}
	}
	if l.hasAudio() {
		s.audio, err = dec.start(file, audioArgs(pr, startMs, l.rate))
		if err != nil {
			s.Close()
			return nil, err
		}
	}

	if s.video != nil { // decoding works when the 1st frame comes (else: unsupported codec, broken file, ...)
		s.first = make([]byte, l.vm.w*l.vm.h*3)
		if _, err := io.ReadFull(s.video.out, s.first); err != nil {
			msg := s.video.errorText()
			s.Close()
			return nil, errors.New(msg)
		}
	}
	return s, nil
}

func (s *ffmpegSource) ReadFrame(rgb []byte) error {
	if s.first != nil {
		copy(rgb, s.first)
		s.first = nil
		return nil
	}
	if s.video == nil {
		return io.EOF
	}
	if _, err := io.ReadFull(s.video.out, rgb); err != nil {
		return io.EOF // the end, or ffmpeg died - either way no more frames
	}
	return nil
}

func (s *ffmpegSource) ReadAudio(pcm []int16) (int, error) {
	if s.audio == nil {
		return 0, io.EOF
	}
	if cap(s.abuf) < len(pcm)*2 {
		s.abuf = make([]byte, len(pcm)*2)
	}
	buf := s.abuf[:len(pcm)*2]
	n, err := io.ReadFull(s.audio.out, buf)
	for i := 0; i < n/2; i++ {
		pcm[i] = int16(binary.LittleEndian.Uint16(buf[i*2:]))
	}
	if err != nil {
		return n / 2, io.EOF
	}
	return n / 2, nil
}

func (s *ffmpegSource) Close() {
	if s.video != nil {
		s.video.stop()
	}
	if s.audio != nil {
		s.audio.stop()
	}
}
