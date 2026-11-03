package main

import (
	"image"
	"image/color"
	"sort"
)

// Conversion of captured layers to an Atari mode: text pixels are mapped hard (threshold / nearest colour), so letters
// stay solid; everything else (images, backgrounds) is dithered - Floyd-Steinberg in mono, ordered (Bayer) in colour,
// so that small page changes don't make the whole dither pattern change.

const textDiffThreshold = 16 // a pixel is text when Text and Plain layers differ by more than this in any channel

// resample box-filters src down (or up) to w x h
func resample(src *image.RGBA, w, h int) *image.RGBA {
	dst := image.NewRGBA(image.Rect(0, 0, w, h))
	sw, sh := src.Bounds().Dx(), src.Bounds().Dy()

	for y := 0; y < h; y++ {
		y0, y1 := y*sh/h, (y+1)*sh/h
		if y1 <= y0 {
			y1 = y0 + 1
		}
		for x := 0; x < w; x++ {
			x0, x1 := x*sw/w, (x+1)*sw/w
			if x1 <= x0 {
				x1 = x0 + 1
			}
			var r, g, b, n int
			for sy := y0; sy < y1 && sy < sh; sy++ {
				o := sy*src.Stride + x0*4
				for sx := x0; sx < x1 && sx < sw; sx++ {
					r += int(src.Pix[o])
					g += int(src.Pix[o+1])
					b += int(src.Pix[o+2])
					o += 4
					n++
				}
			}
			d := y*dst.Stride + x*4
			dst.Pix[d], dst.Pix[d+1], dst.Pix[d+2], dst.Pix[d+3] = uint8(r/n), uint8(g/n), uint8(b/n), 255
		}
	}
	return dst
}

func absDiff(a, b uint8) int {
	if a > b {
		return int(a - b)
	}
	return int(b - a)
}

// textMask marks the pixels which text changes
func textMask(text, plain *image.RGBA) []bool {
	mask := make([]bool, len(text.Pix)/4)
	for i := range mask {
		o := i * 4
		d := absDiff(text.Pix[o], plain.Pix[o])
		if v := absDiff(text.Pix[o+1], plain.Pix[o+1]); v > d {
			d = v
		}
		if v := absDiff(text.Pix[o+2], plain.Pix[o+2]); v > d {
			d = v
		}
		mask[i] = d > textDiffThreshold
	}
	return mask
}

func lum(p []uint8) int {
	return (299*int(p[0]) + 587*int(p[1]) + 114*int(p[2])) / 1000
}

// convertMono: palette index 0 = white, 1 = black (as the ST mono monitor shows colour 0 / 1)
func convertMono(text, images *image.RGBA, mask []bool) *image.Paletted {
	w, h := text.Bounds().Dx(), text.Bounds().Dy()
	out := image.NewPaletted(image.Rect(0, 0, w, h), color.Palette{color.White, color.Black})

	errCur := make([]float64, w+2) // error diffused to this row, offset by 1
	errNext := make([]float64, w+2)

	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			i := y*w + x
			o := i * 4
			if mask[i] { // text - hard threshold, no error carried
				if lum(text.Pix[o:]) < 128 {
					out.Pix[y*out.Stride+x] = 1
				}
				continue
			}

			v := float64(lum(images.Pix[o:]))
			switch { // levels: near-white and near-black areas stay clean instead of getting sparse dots
			case v < 24:
				v = 0
			case v > 232:
				v = 255
			default:
				v = (v - 24) * 255 / 208
			}
			v += errCur[x+1]

			var q float64
			if v >= 128 {
				q = 255
			} else {
				out.Pix[y*out.Stride+x] = 1
			}
			e := v - q
			errCur[x+2] += e * 7 / 16
			errNext[x] += e * 3 / 16
			errNext[x+1] += e * 5 / 16
			errNext[x+2] += e * 1 / 16
		}
		errCur, errNext = errNext, errCur
		for i := range errNext {
			errNext[i] = 0
		}
	}
	return out
}

// hardware palette levels: bits per channel -> 0..255
func hwLevel(c uint8, bits int) uint8 {
	max := (1 << bits) - 1
	l := (int(c)*max + 127) / 255
	return uint8(l * 255 / max)
}

type colEntry struct {
	c     [3]uint8
	count int
}

// medianCut picks n colours from the weighted histogram
func medianCut(entries []colEntry, n int) [][3]uint8 {
	boxes := [][]colEntry{entries}
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
			break // nothing left to split
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

func colDist(a, b [3]int) int {
	dr, dg, db := a[0]-b[0], a[1]-b[1], a[2]-b[2]
	return 3*dr*dr + 4*dg*dg + 2*db*db
}

var bayer8 = [8][8]int{
	{0, 32, 8, 40, 2, 34, 10, 42}, {48, 16, 56, 24, 50, 18, 58, 26},
	{12, 44, 4, 36, 14, 46, 6, 38}, {60, 28, 52, 20, 62, 30, 54, 22},
	{3, 35, 11, 43, 1, 33, 9, 41}, {51, 19, 59, 27, 49, 17, 57, 25},
	{15, 47, 7, 39, 13, 45, 5, 37}, {63, 31, 55, 23, 61, 29, 53, 21},
}

// convertColor: palette of 2^planes colours from the hardware's colour range, text mapped to nearest, rest ordered-dithered
func convertColor(text, images *image.RGBA, mask []bool, planes, bits int) *image.Paletted {
	w, h := text.Bounds().Dx(), text.Bounds().Dy()
	n := 1 << planes

	// histogram of the ideal picture (text from Text layer, the rest from Images layer), in hardware colours
	hist := map[[3]uint8]int{}
	pixel := func(i int) []uint8 {
		if mask[i] {
			return text.Pix[i*4:]
		}
		return images.Pix[i*4:]
	}
	for i := 0; i < w*h; i++ {
		p := pixel(i)
		hist[[3]uint8{hwLevel(p[0], bits), hwLevel(p[1], bits), hwLevel(p[2], bits)}]++
	}
	entries := make([]colEntry, 0, len(hist))
	for c, cnt := range hist {
		entries = append(entries, colEntry{c, cnt})
	}

	// black and white always there, the rest from median cut, snapped to hardware levels
	palCols := [][3]uint8{{255, 255, 255}, {0, 0, 0}}
	seen := map[[3]uint8]bool{{255, 255, 255}: true, {0, 0, 0}: true}
	for _, c := range medianCut(entries, n-2) {
		c = [3]uint8{hwLevel(c[0], bits), hwLevel(c[1], bits), hwLevel(c[2], bits)}
		if !seen[c] {
			seen[c] = true
			palCols = append(palCols, c)
		}
	}
	pal := make(color.Palette, len(palCols))
	pi := make([][3]int, len(palCols))
	for i, c := range palCols {
		pal[i] = color.RGBA{c[0], c[1], c[2], 255}
		pi[i] = [3]int{int(c[0]), int(c[1]), int(c[2])}
	}

	cache := map[[3]int]uint8{}
	nearest := func(c [3]int) (uint8, int) {
		if idx, ok := cache[c]; ok {
			return idx, colDist(c, pi[idx])
		}
		best, bestD := 0, 1<<30
		for i, p := range pi {
			if d := colDist(c, p); d < bestD {
				best, bestD = i, d
			}
		}
		cache[c] = uint8(best)
		return uint8(best), bestD
	}

	spread := map[int]int{4: 80, 16: 48, 256: 24}[n]
	const exactEnough = 9 * 12 * 12 // weighted distance under which a flat area takes the palette colour without dither

	out := image.NewPaletted(image.Rect(0, 0, w, h), pal)
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			i := y*w + x
			p := pixel(i)
			c := [3]int{int(p[0]), int(p[1]), int(p[2])}
			idx, d := nearest(c)
			if !mask[i] && d > exactEnough {
				off := (bayer8[y&7][x&7]*2 - 63) * spread / 128
				idx, _ = nearest([3]int{clamp(c[0] + off), clamp(c[1] + off), clamp(c[2] + off)})
			}
			out.Pix[y*out.Stride+x] = idx
		}
	}
	return out
}

func clamp(v int) int {
	if v < 0 {
		return 0
	}
	if v > 255 {
		return 255
	}
	return v
}

// scaleView enlarges img by integer factors, so the PC preview shows the Atari monitor's aspect ratio
func scaleView(img *image.Paletted, sx, sy int) *image.Paletted {
	b := img.Bounds()
	out := image.NewPaletted(image.Rect(0, 0, b.Dx()*sx, b.Dy()*sy), img.Palette)
	for y := 0; y < out.Rect.Dy(); y++ {
		src := img.Pix[(y/sy)*img.Stride:]
		dst := out.Pix[y*out.Stride:]
		for x := 0; x < out.Rect.Dx(); x++ {
			dst[x] = src[x/sx]
		}
	}
	return out
}
