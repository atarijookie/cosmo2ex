package main

import (
	"image"
	"image/color"
)

// toBitplanes converts a paletted image (width multiple of 16) to the Atari screen memory format: for every 16
// pixels of a row, one big endian word per bitplane (plane 0 first), the leftmost pixel in bit 15.
func toBitplanes(img *image.Paletted, planes int) []byte {
	w, h := img.Rect.Dx(), img.Rect.Dy()
	groups := w / 16
	out := make([]byte, 0, groups*planes*2*h)

	for y := 0; y < h; y++ {
		row := img.Pix[y*img.Stride : y*img.Stride+w]
		for g := 0; g < groups; g++ {
			px := row[g*16 : g*16+16]
			for p := 0; p < planes; p++ {
				var word uint16
				for i := 0; i < 16; i++ {
					if (px[i]>>uint(p))&1 != 0 {
						word |= 0x8000 >> uint(i)
					}
				}
				out = append(out, byte(word>>8), byte(word))
			}
		}
	}
	return out
}

// paletteWords converts the palette to the values of the palette registers: 0x0RGB with bits per channel (ST 3, TT 4),
// padded to 2^planes entries. Mono modes get {white, black} - colour 0 is the background.
func paletteWords(pal color.Palette, planes, bits int) []uint16 {
	if planes == 1 {
		if bits == 0 {
			bits = 3
		}
		max := uint16(1<<uint(bits)) - 1
		return []uint16{max<<8 | max<<4 | max, 0}
	}

	n := 1 << uint(planes)
	max := (1 << uint(bits)) - 1
	out := make([]uint16, n)
	for i := 0; i < n && i < len(pal); i++ {
		r, g, b, _ := pal[i].RGBA()
		lv := func(v uint32) uint16 { return uint16((int(v>>8)*max + 127) / 255) }
		out[i] = lv(r)<<8 | lv(g)<<4 | lv(b)
	}
	return out
}
