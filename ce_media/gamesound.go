package main

// Doom's sound effects for the ST: the game (ce_doomgeneric) only says which effect starts / changes / stops on which
// channel; the samples are read here from the same WAD and mixed for each chunk - exactly as many samples as the
// chunk needs, at the ST's rate (DMA or YM). So sound and frames come from one clock, nothing drifts apart.
// Mono (the ST plays mono) - the stereo separation Doom sends is not used.

import (
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"strings"
	"sync"
)

const sfxChannels = 16

// wadFile: the lump directory of a WAD, lumps read when needed
type wadFile struct {
	f     *os.File
	lumps map[string][2]uint32 // name -> file position, size
}

func openWad(path string) (*wadFile, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	var h [12]byte
	if _, err := io.ReadFull(f, h[:]); err != nil || (string(h[:4]) != "IWAD" && string(h[:4]) != "PWAD") {
		f.Close()
		return nil, fmt.Errorf("%s is not a WAD", path)
	}
	n := binary.LittleEndian.Uint32(h[4:])
	dirPos := binary.LittleEndian.Uint32(h[8:])
	if n > 100000 {
		f.Close()
		return nil, fmt.Errorf("%s: %d lumps?", path, n)
	}
	dir := make([]byte, n*16)
	if _, err := f.ReadAt(dir, int64(dirPos)); err != nil {
		f.Close()
		return nil, fmt.Errorf("%s: directory: %v", path, err)
	}
	w := &wadFile{f: f, lumps: map[string][2]uint32{}}
	for i := uint32(0); i < n; i++ {
		e := dir[i*16:]
		name := strings.ToUpper(strings.TrimRight(string(e[8:16]), "\x00"))
		w.lumps[name] = [2]uint32{binary.LittleEndian.Uint32(e), binary.LittleEndian.Uint32(e[4:])} // the last one counts
	}
	return w, nil
}

func (w *wadFile) lump(name string) ([]byte, error) {
	e, ok := w.lumps[strings.ToUpper(name)]
	if !ok {
		return nil, fmt.Errorf("no lump %s", name)
	}
	b := make([]byte, e[1])
	_, err := w.f.ReadAt(b, int64(e[0]))
	return b, err
}

func (w *wadFile) Close() { w.f.Close() }

// a sound effect, decoded: DMX format 3 - rate (W), samples (DW, with 16 padding bytes at each end), 8 bit unsigned
type sfxSound struct {
	pcm  []float32
	rate int
}

func decodeDMX(b []byte) (*sfxSound, error) {
	if len(b) < 8 || binary.LittleEndian.Uint16(b) != 3 {
		return nil, fmt.Errorf("not a DMX sound")
	}
	rate := int(binary.LittleEndian.Uint16(b[2:]))
	n := int(binary.LittleEndian.Uint32(b[4:]))
	if n > len(b)-8 {
		n = len(b) - 8
	}
	data := b[8 : 8+n]
	if len(data) > 32 { // the padding, as Doom's own player skips it
		data = data[16 : len(data)-16]
	}
	s := &sfxSound{pcm: make([]float32, len(data)), rate: rate}
	for i, v := range data {
		s.pcm[i] = (float32(v) - 128) / 128
	}
	if rate <= 0 {
		s.rate = 11025
	}
	return s, nil
}

type sfxVoice struct {
	snd  *sfxSound
	pos  float64 // in samples of the effect
	step float64 // effect samples per output sample
	gain float32
}

type sfxMixer struct {
	mu     sync.Mutex
	wad    *wadFile
	rate   int
	cache  map[string]*sfxSound
	voices [sfxChannels]*sfxVoice
}

func newSfxMixer(wad *wadFile, rate int) *sfxMixer {
	return &sfxMixer{wad: wad, rate: rate, cache: map[string]*sfxSound{}}
}

func volGain(vol byte) float32 {
	if vol > 127 {
		vol = 127
	}
	return float32(vol) / 127
}

func (m *sfxMixer) start(ch int, vol byte, name string) {
	if ch < 0 || ch >= sfxChannels {
		return
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	s, ok := m.cache[name]
	if !ok {
		if b, err := m.wad.lump(name); err == nil {
			s, _ = decodeDMX(b)
		}
		m.cache[name] = s // also a nil - not looked up again
	}
	if s == nil {
		m.voices[ch] = nil
		return
	}
	m.voices[ch] = &sfxVoice{snd: s, step: float64(s.rate) / float64(m.rate), gain: volGain(vol)}
}

func (m *sfxMixer) update(ch int, vol byte) {
	if ch < 0 || ch >= sfxChannels {
		return
	}
	m.mu.Lock()
	if v := m.voices[ch]; v != nil {
		v.gain = volGain(vol)
	}
	m.mu.Unlock()
}

func (m *sfxMixer) stop(ch int) {
	if ch < 0 || ch >= sfxChannels {
		return
	}
	m.mu.Lock()
	m.voices[ch] = nil
	m.mu.Unlock()
}

// mix: the next len(out) samples of all channels; linear interpolation from the effects' 11 kHz
func (m *sfxMixer) mix(out []int16) {
	acc := make([]float32, len(out))
	m.mu.Lock()
	for ch, v := range m.voices {
		if v == nil {
			continue
		}
		pcm := v.snd.pcm
		for i := range acc {
			p := int(v.pos)
			if p >= len(pcm) {
				m.voices[ch] = nil
				break
			}
			s := pcm[p]
			if p+1 < len(pcm) {
				f := float32(v.pos - float64(p))
				s += (pcm[p+1] - s) * f
			}
			acc[i] += s * v.gain
			v.pos += v.step
		}
	}
	m.mu.Unlock()

	for i, a := range acc {
		a *= 0.7 * 32767 // a few effects at once still mostly fit, then clipped
		if a > 32767 {
			a = 32767
		} else if a < -32768 {
			a = -32768
		}
		out[i] = int16(a)
	}
}
