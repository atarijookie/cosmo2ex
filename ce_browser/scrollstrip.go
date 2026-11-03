package main

// Fast vertical scrolling. A normal frame takes 3 screenshots (the layers) and a conversion - 200..400 ms - too slow
// for scrolling with the mouse at the edge. So at the first scroll a strip of the page a few screens tall is captured
// and converted once; each scroll step after that just cuts the right rows out of it - milliseconds. Before the view
// reaches the end of the strip, the next one is captured in the background, so the scrolling doesn't stop for it.
// When the scrolling stops, the normal check renders the real view again (sticky headers, things which changed).

import (
	"context"
	"fmt"
	"image"
	"log"
	"math"
	"time"

	"github.com/chromedp/cdproto/page"
	"github.com/chromedp/chromedp"
)

const (
	stripScreens   = 6               // how many screens tall a strip is
	stripPrefetch  = 3               // start the next strip when less than this many screens are left ahead
	stripMaxAge    = 5 * time.Second // older strips are captured anew - the page may have changed
	scrollSettleMs = 350             // no scroll for this long = scrolling stopped, normal checks again
	stripTimeout   = 5 * time.Second // a capture which doesn't finish by then (page navigated away, ...) is given up
)

type scrollStrip struct {
	img     *image.Paletted // Atari pixels, page width wide
	palette []uint16
	x       float64 // window.scrollX it was captured at (CSS pixels)
	top     float64 // document Y of its first row (CSS pixels)
	bottom  float64 // document Y below its last row
	built   time.Time
}

type stripResult struct {
	strip *scrollStrip // nil if the capture failed
	gen   int
}

func (st *scrollStrip) covers(x, y, h float64) bool {
	return st != nil && st.x == x && y >= st.top && y+h <= st.bottom && time.Since(st.built) <= stripMaxAge
}

// dropStrip - the page may look different now (input, navigation, mode change); a strip being captured is of no use too
func (s *server) dropStrip() {
	s.strip = nil
	s.stripGen++
	if s.stripCancel != nil { // a background capture can't finish on a changed page - it would wait for paints forever
		s.stripCancel()
		s.stripCancel = nil
	}
}

// stripTop - where a strip for the view at y starts: mostly ahead in the direction of scrolling
func (s *server) stripTop(y, docH float64, up bool) (top, h float64) {
	cssH := float64(s.cssH)
	top = y - 0.5*cssH
	if up {
		top = y - (stripScreens-1.5)*cssH
	}
	h = stripScreens * cssH
	top = math.Max(0, math.Min(top, docH-h))
	h = math.Min(h, docH-top)
	return top, h
}

// buildStrip captures and converts a strip - in the background (goroutine) too, so only what it's given is used
func (s *server) buildStrip(ctx context.Context, x, top, h float64, mode *Mode, pageW, pageH, cssW, cssH int) *scrollStrip {
	t0 := time.Now()
	s.captureMu.Lock() // the layers are switched by classes on the page - one capture at a time
	layers, err := captureLayers(ctx, &page.Viewport{X: x, Y: top, Width: float64(cssW), Height: h, Scale: 1})
	s.captureMu.Unlock()
	if err != nil {
		log.Printf("strip capture: %v", err)
		return nil
	}
	rows := int(math.Round(h * float64(pageH) / float64(cssH)))
	img := convertLayers(layers, mode, pageW, rows)
	if s.verbose {
		log.Printf("strip: %d..%d CSS px, %d rows, %d ms", int(top), int(top+h), rows, time.Since(t0).Milliseconds())
	}
	return &scrollStrip{img: img, palette: paletteWords(img.Palette, mode.Planes, mode.ColorBits),
		x: x, top: top, bottom: top + h, built: time.Now()}
}

// takeStrip - a background strip finished: use it if it's still for this page (wait = until it's done)
func (s *server) takeStrip(wait bool) {
	if !s.stripBuilding {
		return
	}
	var r stripResult
	if wait {
		r = <-s.stripReady
	} else {
		select {
		case r = <-s.stripReady:
		default:
			return
		}
	}
	s.stripBuilding = false
	if r.gen == s.stripGen && r.strip != nil {
		s.strip = r.strip
	}
}

// scrollFast scrolls vertically and sends the new view cut from the strip; false if it couldn't (then the normal
// render does it)
func (s *server) scrollFast(lines, pages int8) bool {
	if s.mode == nil || s.firstFrame {
		return false
	}

	js := fmt.Sprintf(`(() => { window.scrollBy(0, %d * 40 + %d * Math.round(window.innerHeight * 0.9));
		return [window.scrollX, window.scrollY, document.documentElement.scrollHeight]; })()`, lines, pages)
	var pos []float64
	if err := chromedp.Run(s.tab, chromedp.Evaluate(js, &pos)); err != nil || len(pos) != 3 {
		log.Printf("scroll: %v", err)
		return false
	}
	x, y, docH := pos[0], pos[1], pos[2]
	cssH := float64(s.cssH)
	up := lines < 0 || pages < 0

	s.takeStrip(false)
	if !s.strip.covers(x, y, cssH) && s.stripBuilding { // the next strip is on its way - wait for it rather than capture again
		s.takeStrip(true)
	}
	if !s.strip.covers(x, y, cssH) {
		top, h := s.stripTop(y, docH, up)
		if h < cssH || y < top || y+cssH > top+h {
			return false // short page or odd numbers - the normal way
		}
		ctx, cancel := context.WithTimeout(s.tab, stripTimeout)
		s.strip = s.buildStrip(ctx, x, top, h, s.mode, s.pageW, s.pageH, s.cssW, s.cssH)
		cancel()
		if s.strip == nil {
			return false
		}
	}
	st := s.strip

	// little left ahead: capture the next strip in the background
	left := st.bottom - (y + cssH)
	if up {
		left = y - st.top
	}
	atEnd := (!up && st.bottom >= docH) || (up && st.top <= 0)
	if left < stripPrefetch*cssH && !atEnd && !s.stripBuilding {
		top, h := s.stripTop(y, docH, up)
		if h >= cssH {
			s.stripBuilding = true
			gen, mode, pw, ph, cw, ch := s.stripGen, s.mode, s.pageW, s.pageH, s.cssW, s.cssH
			ctx, cancel := context.WithTimeout(s.tab, stripTimeout)
			s.stripCancel = cancel
			go func() {
				defer cancel()
				s.stripReady <- stripResult{s.buildStrip(ctx, x, top, h, mode, pw, ph, cw, ch), gen}
			}()
		}
	}

	// the rows of the view
	off := int(math.Round((y - st.top) * float64(s.pageH) / cssH))
	if off+s.pageH > st.img.Rect.Dy() {
		off = st.img.Rect.Dy() - s.pageH
	}
	view := image.NewPaletted(image.Rect(0, 0, s.pageW, s.pageH), st.img.Palette)
	copy(view.Pix, st.img.Pix[off*st.img.Stride:(off+s.pageH)*st.img.Stride])

	s.seq++
	data := toBitplanes(view, s.mode.Planes)
	f := &Frame{seq: s.seq, res: s.mode.Res, zoom: s.zoomQ, width: s.pageW, height: s.pageH, planes: s.mode.Planes,
		palette: st.palette, data: data}
	if err := s.link.sendFrame(f); err != nil {
		log.Printf("send frame: %v", err)
		return false
	}
	s.lastSent = data
	s.haveHash = false // after the scrolling the normal check renders the real view
	s.lastScrollAt = time.Now()
	s.logv("frame %d: scrolled to %d, from the strip", s.seq, int(y))
	return true
}

// scrolling - the normal checks wait, they'd take the time the strip frames need
func (s *server) scrolling() bool {
	return time.Since(s.lastScrollAt) < scrollSettleMs*time.Millisecond
}
