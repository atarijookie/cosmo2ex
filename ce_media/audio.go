package main

// 16 bit PCM (mono, already at the ST's rate - ffmpeg resamples) -> what the ST plays.
//   DMA (STE / TT / Falcon): signed 8 bit, with error feedback so quiet parts don't just fall to steps.
//   YM (plain ST): the ST's timer A interrupt takes one byte per sample and writes the three volume registers from
//   the YM table entry of that byte. The table holds 256 combinations of the three channel volumes, sorted by their
//   summed output level; here every sample gets the entry nearest to it (plus error feedback). All of that is done
//   here - the ST only looks up the table.

import (
	"encoding/binary"
	"fmt"
	"log"
	"math"
	"os"
	"sort"
)

type AudioConv interface {
	Convert(pcm []int16) []byte
}

const fadeInSamples = 0.02 // seconds - the start of a generation fades in from silence (YM: from level 0, no click)

//--------------------------------------------------------------------------------------------------------------------

type dmaConv struct {
	err     float64
	fade    int // samples left of the fade in
	fadeLen int
}

func newDMAConv(rate int) *dmaConv {
	n := int(float64(rate) * fadeInSamples)
	return &dmaConv{fade: n, fadeLen: n}
}

func (c *dmaConv) Convert(pcm []int16) []byte {
	out := make([]byte, len(pcm))
	for i, s := range pcm {
		v := float64(s) / 256
		if c.fade > 0 {
			v *= float64(c.fadeLen-c.fade) / float64(c.fadeLen)
			c.fade--
		}
		want := v + c.err
		q := math.Floor(want + 0.5)
		if q > 127 {
			q = 127
		}
		if q < -128 {
			q = -128
		}
		c.err = want - q
		if c.err > 1 || c.err < -1 { // clipped - don't carry it
			c.err = 0
		}
		out[i] = byte(int8(q))
	}
	return out
}

//--------------------------------------------------------------------------------------------------------------------

// ymVolLevel: output of one channel at volume v (0..15) - about 3 dB per step, 0 is off
func ymVolLevel(v int) float64 {
	if v == 0 {
		return 0
	}
	return math.Pow(2, float64(v-15)/2)
}

type ymEntry struct {
	vol   [3]uint8
	level float64 // 0..1 (sum of the three channels / 3)
}

// How the ST mixes the three channels: the chip's outputs are tied together, so the loudest channel dominates and
// the others add much less than their own level - roughly a 'soft maximum' (sum of level^p)^(1/p). p was fitted to
// the order of the volume combinations in the table which ST demos used (Wizzcat / Delta Force): a plain sum puts a
// third more of its entries in the wrong order. A plain sum here made most table entries put out another level than
// the converter thought - heard as noise and distortion.
const ymMixP = 1.5

// ymMix: output of three channels at these volumes, 0..1
func ymMix(a, b, c int) float64 {
	sum := 0.0
	for _, v := range []int{a, b, c} {
		sum += math.Pow(ymVolLevel(v), ymMixP)
	}
	return math.Pow(sum, 1/ymMixP) / math.Pow(3, 1/ymMixP)
}

// ymMaxLevel: the top of the range the samples use. Near the chip's maximum there are only a few combinations (the
// volume steps are 3 dB each), so a waveform reaching there would get coarse steps at its peaks.
const ymMaxLevel = 0.4

// buildYmTable: 256 volume combinations, as evenly spread over 0..ymMaxLevel as there are, sorted by level
func buildYmTable() []ymEntry {
	return buildYmTableUpTo(ymMaxLevel)
}

func buildYmTableUpTo(maxLevel float64) []ymEntry {
	var all []ymEntry
	for a := 0; a < 16; a++ {
		for b := 0; b <= a; b++ {
			for c := 0; c <= b; c++ {
				all = append(all, ymEntry{[3]uint8{uint8(a), uint8(b), uint8(c)},
					ymMix(a, b, c)})
			}
		}
	}
	sort.Slice(all, func(i, j int) bool { return all[i].level < all[j].level })

	table := make([]ymEntry, 256)
	for i := range table {
		target := float64(i) / 255 * maxLevel
		k := sort.Search(len(all), func(j int) bool { return all[j].level >= target })
		if k == len(all) || (k > 0 && target-all[k-1].level < all[k].level-target) {
			k--
		}
		table[i] = all[k]
	}
	sort.SliceStable(table, func(i, j int) bool { return table[i].level < table[j].level })
	return table
}

// ymTableBytes: the GET_YM_TABLE reply - per entry 3 longwords for 0xffff8800 and the level
func ymTableBytes(table []ymEntry) []byte {
	out := make([]byte, ymTableSize)
	top := table[len(table)-1].level
	for i, e := range table {
		o := i * ymTableEntry
		for ch := 0; ch < 3; ch++ {
			binary.BigEndian.PutUint32(out[o+ch*4:], uint32(8+ch)<<24|uint32(e.vol[ch])<<8)
		}
		l := e.level / top // 0..65535 over the used range - the ST takes the entry nearest 32768 as silence
		if l > 1 {
			l = 1
		}
		binary.BigEndian.PutUint32(out[o+12:], uint32(l*65535+0.5))
	}
	return out
}

type ymConv struct {
	table    []ymEntry
	maxLevel float64
	err      float64
	fade     int
	fadeLen  int
}

func newYMConv(table []ymEntry, rate int) *ymConv {
	n := int(float64(rate) * fadeInSamples)
	return &ymConv{table: table, maxLevel: table[len(table)-1].level, fade: n, fadeLen: n}
}

func (c *ymConv) nearest(v float64) int {
	t := c.table
	k := sort.Search(len(t), func(j int) bool { return t[j].level >= v })
	if k == len(t) || (k > 0 && v-t[k-1].level < t[k].level-v) {
		k--
	}
	return k
}

func (c *ymConv) Convert(pcm []int16) []byte {
	out := make([]byte, len(pcm))
	for i, s := range pcm {
		v := (float64(s) + 32768) / 65536 * c.maxLevel // silence in the middle of the used range
		if c.fade > 0 {
			v *= float64(c.fadeLen-c.fade) / float64(c.fadeLen)
			c.fade--
		}
		want := v + c.err
		k := c.nearest(want)
		c.err = want - c.table[k].level
		if c.err > 0.02 || c.err < -0.02 { // clipped at an end - don't let the error run away
			c.err = 0
		}
		out[i] = byte(k)
	}
	return out
}

// loadYmTable: a table from a file, in the format the ST gets (256 x 16 B: three longwords for 0xffff8800, the rest
// ignored) - e.g. one of the tables ST demos used. Its entries are taken as evenly spaced levels, as such tables are
// made: index = output.
func loadYmTable(path string) ([]ymEntry, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	if len(b) < ymTableSize {
		return nil, fmt.Errorf("YM table %s: %d bytes, want %d", path, len(b), ymTableSize)
	}
	table := make([]ymEntry, 256)
	for i := range table {
		for ch := 0; ch < 3; ch++ {
			v := binary.BigEndian.Uint32(b[i*ymTableEntry+ch*4:])
			if v>>24 != uint32(8+ch) || (v>>8)&0xff > 15 {
				return nil, fmt.Errorf("YM table %s: entry %d isn't volume registers 8, 9, 10", path, i)
			}
			table[i].vol[ch] = uint8(v >> 8)
		}
		table[i].level = float64(i) / 255
	}
	return table, nil
}

// ymTable: the table from -ym-table, or the built one when there's none (or it can't be read)
func (d *Decoder) ymTable() []ymEntry {
	if d.ymTablePath != "" {
		t, err := loadYmTable(d.ymTablePath)
		if err == nil {
			log.Printf("YM table from %s", d.ymTablePath)
			return t
		}
		log.Printf("%v - using the built table", err)
	}
	return buildYmTable()
}
