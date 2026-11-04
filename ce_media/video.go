package main

// RGB frames -> Atari screen memory. Colour: 16 colours per frame by median cut (colour 0 is always black - the
// borders), mapped with ordered (Bayer) dithering - error diffusion would make still areas shimmer from frame to
// frame. The palette of the previous frame is kept while it's nearly as good, so the dither pattern of still areas
// doesn't change either. Mono: ordered dithering of the brightness.

import (
	"sort"
)

var bayer8 = [8][8]int{
	{0, 32, 8, 40, 2, 34, 10, 42}, {48, 16, 56, 24, 50, 18, 58, 26},
	{12, 44, 4, 36, 14, 46, 6, 38}, {60, 28, 52, 20, 62, 30, 54, 22},
	{3, 35, 11, 43, 1, 33, 9, 41}, {51, 19, 59, 27, 49, 17, 57, 25},
	{15, 47, 7, 39, 13, 45, 5, 37}, {63, 31, 55, 23, 61, 29, 53, 21},
}

type VideoConv struct {
	vm        vmodeInfo
	bits      int // palette bits per channel: ST 3, STE / TT / Falcon 4
	steOrder  bool
	prevPal   [][3]uint8
	lut       []uint8 // nearest palette index for a 5-bit-per-channel colour, 0xff = not known yet
	index     []uint8 // palette index of every pixel
	keepRatio float64 // previous palette stays while its error <= new error * this
}

func newVideoConv(vm vmodeInfo, machine uint8) *VideoConv {
	c := &VideoConv{vm: vm, bits: 4, keepRatio: 1.15}
	if machine == machST {
		c.bits = 3
	}
	c.steOrder = vm.id == vmodeSTLow && machine != machST // ST compatible registers on STE / Falcon: bit 3 is the LSB
	c.lut = make([]uint8, 32768)
	c.index = make([]uint8, vm.w*vm.h)
	return c
}

// Convert: one frame of w x h RGB -> bitplanes and palette words (nil in mono)
func (c *VideoConv) Convert(rgb []byte) ([]byte, []uint16) {
	if c.vm.planes == 1 {
		c.mono(rgb)
		return toBitplanes(c.index, c.vm.w, c.vm.h, 1), nil
	}
	pal := c.palette(rgb)
	c.mapColours(rgb, pal)
	return toBitplanes(c.index, c.vm.w, c.vm.h, c.vm.planes), c.paletteWords(pal)
}

func (c *VideoConv) mono(rgb []byte) {
	w := c.vm.w
	for i := range c.index {
		o := i * 3
		l := (299*int(rgb[o]) + 587*int(rgb[o+1]) + 114*int(rgb[o+2])) / 1000
		x, y := i%w, i/w
		if l*64 < (bayer8[y&7][x&7]*2+1)*128 { // black (pixel 1) where darker than the threshold
			c.index[i] = 1
		} else {
			c.index[i] = 0
		}
	}
}

// hwLevel: 0..255 -> nearest hardware level, back in 0..255
func hwLevel(v uint8, bits int) uint8 {
	max := (1 << bits) - 1
	l := (int(v)*max + 127) / 255
	return uint8(l * 255 / max)
}

type colEntry struct {
	c     [3]uint8
	count int
}

func (c *VideoConv) histogram(rgb []byte) []colEntry {
	shift := uint(8 - c.bits)
	n := 1 << uint(3*c.bits)
	counts := make([]int, n)
	for o := 0; o+2 < len(rgb); o += 3 {
		r, g, b := int(hwLevel(rgb[o], c.bits))>>shift, int(hwLevel(rgb[o+1], c.bits))>>shift, int(hwLevel(rgb[o+2], c.bits))>>shift
		counts[(r<<uint(2*c.bits))|(g<<uint(c.bits))|b]++
	}
	max := (1 << c.bits) - 1
	lv := func(v int) uint8 { return uint8(v * 255 / max) }
	var entries []colEntry
	for i, cnt := range counts {
		if cnt == 0 {
			continue
		}
		r, g, b := i>>uint(2*c.bits), (i>>uint(c.bits))&max, i&max
		entries = append(entries, colEntry{[3]uint8{lv(r), lv(g), lv(b)}, cnt})
	}
	return entries
}

func colDist(a, b [3]int) int {
	dr, dg, db := a[0]-b[0], a[1]-b[1], a[2]-b[2]
	return 3*dr*dr + 4*dg*dg + 2*db*db
}

func toInt3(c [3]uint8) [3]int { return [3]int{int(c[0]), int(c[1]), int(c[2])} }

// palError: how badly the palette shows the histogram (sum of count * distance to the nearest colour)
func palError(entries []colEntry, pal [][3]uint8) int64 {
	var sum int64
	for _, e := range entries {
		ec := toInt3(e.c)
		best := 1 << 30
		for _, p := range pal {
			if d := colDist(ec, toInt3(p)); d < best {
				best = d
			}
		}
		sum += int64(best) * int64(e.count)
	}
	return sum
}

func (c *VideoConv) palette(rgb []byte) [][3]uint8 {
	entries := c.histogram(rgb)
	n := 1 << uint(c.vm.planes)

	others := entries[:0:0] // black is colour 0 anyway
	for _, e := range entries {
		if e.c != [3]uint8{} {
			others = append(others, e)
		}
	}

	pal := [][3]uint8{{0, 0, 0}}
	seen := map[[3]uint8]bool{{0, 0, 0}: true}
	for _, col := range medianCut(others, n-1) {
		col = [3]uint8{hwLevel(col[0], c.bits), hwLevel(col[1], c.bits), hwLevel(col[2], c.bits)}
		if !seen[col] {
			seen[col] = true
			pal = append(pal, col)
		}
	}
	for len(pal) < n { // unused entries - black
		pal = append(pal, [3]uint8{})
	}

	if c.prevPal != nil {
		if float64(palError(entries, c.prevPal)) <= float64(palError(entries, pal))*c.keepRatio {
			return c.prevPal
		}
	}
	c.prevPal = pal
	for i := range c.lut {
		c.lut[i] = 0xff
	}
	return pal
}

func (c *VideoConv) nearest(pal [][3]uint8, col [3]int) uint8 {
	key := (col[0]>>3)<<10 | (col[1]>>3)<<5 | col[2]>>3
	if v := c.lut[key]; v != 0xff {
		return v
	}
	q := [3]int{(col[0]>>3)<<3 | 4, (col[1]>>3)<<3 | 4, (col[2]>>3)<<3 | 4}
	best, bestD := 0, 1<<30
	for i, p := range pal {
		if d := colDist(q, toInt3(p)); d < bestD {
			best, bestD = i, d
		}
	}
	c.lut[key] = uint8(best)
	return uint8(best)
}

func clamp255(v int) int {
	if v < 0 {
		return 0
	}
	if v > 255 {
		return 255
	}
	return v
}

func (c *VideoConv) mapColours(rgb []byte, pal [][3]uint8) {
	const spread = 48 // dither amplitude for 16 colours
	w := c.vm.w
	for i := range c.index {
		o := i * 3
		x, y := i%w, i/w
		off := (bayer8[y&7][x&7]*2 - 63) * spread / 128
		c.index[i] = c.nearest(pal, [3]int{clamp255(int(rgb[o]) + off), clamp255(int(rgb[o+1]) + off), clamp255(int(rgb[o+2]) + off)})
	}
}

func (c *VideoConv) paletteWords(pal [][3]uint8) []uint16 {
	max := (1 << c.bits) - 1
	out := make([]uint16, len(pal))
	for i, p := range pal {
		var w uint16
		for ch := 0; ch < 3; ch++ {
			v := uint16((int(p[ch])*max + 127) / 255)
			if c.steOrder {
				v = (v >> 1) | ((v & 1) << 3)
			}
			w = w<<4 | v
		}
		out[i] = w
	}
	return out
}

// medianCut picks n colours from the weighted histogram
func medianCut(entries []colEntry, n int) [][3]uint8 {
	if len(entries) == 0 || n <= 0 {
		return nil
	}
	boxes := [][]colEntry{append([]colEntry{}, entries...)}
	for len(boxes) < n {
		best, bestScore, bestCh := -1, 0, 0
		for bi, b := range boxes {
			if len(b) < 2 {
				continue
			}
			for ch := 0; ch < 3; ch++ {
				lo, hi, pop := 255, 0, 0
				for _, e := range b {
					v := int(e.c[ch])
					if v < lo {
						lo = v
					}
					if v > hi {
						hi = v
					}
					pop += e.count
				}
				score := (hi - lo) * isqrt(pop)
				if score > bestScore {
					best, bestScore, bestCh = bi, score, ch
				}
			}
		}
		if best < 0 {
			break
		}
		b := boxes[best]
		ch := bestCh
		sort.Slice(b, func(i, j int) bool { return b[i].c[ch] < b[j].c[ch] })
		total := 0
		for _, e := range b {
			total += e.count
		}
		acc, cut := 0, 1
		for i, e := range b {
			acc += e.count
			if acc*2 >= total {
				cut = i + 1
				break
			}
		}
		if cut >= len(b) {
			cut = len(b) - 1
		}
		boxes[best] = b[:cut]
		boxes = append(boxes, b[cut:])
	}

	pal := make([][3]uint8, 0, len(boxes))
	for _, b := range boxes {
		var r, g, bl, pop int
		for _, e := range b {
			r += int(e.c[0]) * e.count
			g += int(e.c[1]) * e.count
			bl += int(e.c[2]) * e.count
			pop += e.count
		}
		if pop > 0 {
			pal = append(pal, [3]uint8{uint8(r / pop), uint8(g / pop), uint8(bl / pop)})
		}
	}
	return pal
}

func isqrt(n int) int {
	r := 0
	for (r+1)*(r+1) <= n {
		r++
	}
	return r
}

// toBitplanes: palette indexes (w multiple of 16) -> Atari screen memory: per 16 pixels one big endian word per
// bitplane (plane 0 first), the leftmost pixel in bit 15
func toBitplanes(index []uint8, w, h, planes int) []byte {
	groups := w / 16
	out := make([]byte, 0, groups*planes*2*h)
	for y := 0; y < h; y++ {
		row := index[y*w : y*w+w]
		for g := 0; g < groups; g++ {
			px := row[g*16 : g*16+16]
			for p := uint(0); p < uint(planes); p++ {
				var word uint16
				for i := 0; i < 16; i++ {
					word |= uint16((px[i]>>p)&1) << uint(15-i)
				}
				out = append(out, byte(word>>8), byte(word))
			}
		}
	}
	return out
}
