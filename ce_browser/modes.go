package main

import "fmt"

// Atari video modes the browser renders for. Chromium renders the page at renderW x renderH CSS pixels with device
// scale factor dpr; the captured image is then box-filtered down to w x h Atari pixels. Modes with non-square pixels
// (ST mid: tall, TT low: wide) render at square pixels and get squashed in one direction.
type Mode struct {
	Name        string
	Res         uint8   // SCREENCAST_RES_* code, as CE_BROWS.PRG and ce_cores name the resolution
	W, H        int     // Atari pixels
	Planes      int     // bitplanes: 1 = mono, 2 = 4 colours, 4 = 16 colours, 8 = 256 colours
	ColorBits   int     // bits per R/G/B channel of the hardware palette: ST 3, TT 4 (mono modes: unused)
	RenderW     int     // CSS viewport width
	RenderH     int     // CSS viewport height
	DPR         float64 // device scale factor
	MinFontPx   int     // CSS font sizes below this are raised to it, so text keeps enough rows after downscaling
	ViewSX      int     // integer scale of the PC preview image, so it shows at the monitor's aspect ratio
	ViewSY      int
	Description string
}

var modes = []Mode{
	{Name: "st-low", Res: 0x00, W: 320, H: 200, Planes: 4, ColorBits: 3, RenderW: 320, RenderH: 200, DPR: 1, MinFontPx: 11, ViewSX: 4, ViewSY: 4,
		Description: "ST low 320x200, 16 of 512 colours"},
	{Name: "st-mid", Res: 0x01, W: 640, H: 200, Planes: 2, ColorBits: 3, RenderW: 640, RenderH: 400, DPR: 1, MinFontPx: 16, ViewSX: 2, ViewSY: 4,
		Description: "ST mid 640x200, 4 of 512 colours (rendered 640x400, halved vertically)"},
	{Name: "st-high", Res: 0x02, W: 640, H: 400, Planes: 1, ColorBits: 3, RenderW: 640, RenderH: 400, DPR: 1, MinFontPx: 12, ViewSX: 2, ViewSY: 2,
		Description: "ST high 640x400, mono"},
	{Name: "tt-low", Res: 0x10, W: 320, H: 480, Planes: 8, ColorBits: 4, RenderW: 640, RenderH: 480, DPR: 1, MinFontPx: 14, ViewSX: 4, ViewSY: 2,
		Description: "TT low 320x480, 256 of 4096 colours (rendered 640x480, halved horizontally)"},
	{Name: "tt-mid", Res: 0x11, W: 640, H: 480, Planes: 4, ColorBits: 4, RenderW: 640, RenderH: 480, DPR: 1, MinFontPx: 12, ViewSX: 2, ViewSY: 2,
		Description: "TT mid 640x480, 16 of 4096 colours"},
	{Name: "tt-high", Res: 0x12, W: 1280, H: 960, Planes: 1, ColorBits: 4, RenderW: 1280, RenderH: 960, DPR: 1, MinFontPx: 12, ViewSX: 1, ViewSY: 1,
		Description: "TT high 1280x960, mono"},
}

// zoomed returns the mode with the page zoomed by z: the CSS viewport shrinks by z and the device scale factor grows
// by z, so the Atari gets the same number of pixels, with everything on the page drawn z times bigger.
// The minimum font size is in CSS pixels, so it shrinks by z too (it's about Atari rows, which stay the same).
func (m Mode) zoomed(z float64) Mode {
	if z == 1 {
		return m
	}
	m.RenderW = int(float64(m.RenderW)/z + 0.5)
	m.RenderH = int(float64(m.RenderH)/z + 0.5)
	m.DPR *= z
	m.MinFontPx = int(float64(m.MinFontPx)/z + 0.999)
	m.Name = fmt.Sprintf("%s-z%g", m.Name, z)
	m.Description = fmt.Sprintf("%s, page zoomed %gx (%dx%d CSS pixels)", m.Description, z, m.RenderW, m.RenderH)
	return m
}

func modeByRes(res uint8) *Mode {
	for i := range modes {
		if modes[i].Res == res {
			return &modes[i]
		}
	}
	return nil
}

func modeByName(name string) *Mode {
	for i := range modes {
		if modes[i].Name == name {
			return &modes[i]
		}
	}
	return nil
}
