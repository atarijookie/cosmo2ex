package main

import (
	"bytes"
	"context"
	"fmt"
	"image"
	"image/draw"
	"image/png"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/chromedp/cdproto/emulation"
	"github.com/chromedp/cdproto/page"
	"github.com/chromedp/cdproto/runtime"
	"github.com/chromedp/chromedp"
)

// CSS injected into every page. Layers are switched by classes on <html>:
//
//	ce-base   - always: no animations / transitions / caret, so the page holds still between captures
//	ce-noimg  - images, video, canvas, iframes and background images hidden (layout kept: visibility, not display)
//	ce-notext - text drawn transparent (layout kept)
const injectedCSS = `
html.ce-base *, html.ce-base *::before, html.ce-base *::after {
  animation: none !important; transition: none !important; caret-color: transparent !important;
  -webkit-font-smoothing: none !important; scroll-behavior: auto !important;
}
html.ce-base a[href] { text-decoration-line: underline !important; } /* links can't be told apart by colour in mono */
html.ce-noimg img, html.ce-noimg svg, html.ce-noimg video, html.ce-noimg canvas, html.ce-noimg picture,
html.ce-noimg iframe, html.ce-noimg object, html.ce-noimg embed { visibility: hidden !important; }
html.ce-noimg *, html.ce-noimg *::before, html.ce-noimg *::after { background-image: none !important; }
html.ce-notext *, html.ce-notext *::before, html.ce-notext *::after {
  color: transparent !important; -webkit-text-fill-color: transparent !important;
  text-shadow: none !important; text-decoration-color: transparent !important;
}
`

var injectJS = fmt.Sprintf(`(function(){
  const css = %q;
  function add() {
    if (!document.getElementById('ce-style')) {
      const s = document.createElement('style'); s.id = 'ce-style'; s.textContent = css;
      (document.head || document.documentElement).appendChild(s);
    }
    document.documentElement.classList.add('ce-base');
  }
  add(); document.addEventListener('DOMContentLoaded', add);
})();`, injectedCSS)

// scroll through the page so lazy-loaded images load, then back to the top
const lazyScrollJS = `(async (step, maxY) => {
  const wait = ms => new Promise(r => setTimeout(r, ms));
  for (let y = 0; y < Math.min(document.documentElement.scrollHeight, maxY); y += step) { window.scrollTo(0, y); await wait(150); }
  window.scrollTo(0, 0); await wait(300);
  return document.documentElement.scrollHeight;
})(%d, %d)`

// raise every text's font size to at least min CSS pixels
const minFontJS = `((min) => {
  const w = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
  const seen = new Set(); let n, raised = 0;
  while ((n = w.nextNode())) {
    const e = n.parentElement;
    if (!e || seen.has(e) || !n.textContent.trim()) continue;
    seen.add(e);
    if (parseFloat(getComputedStyle(e).fontSize) < min) { e.style.setProperty('font-size', min + 'px', 'important'); raised++; }
  }
  return raised;
})(%d)`

// switch layer classes and wait two frames so the change is painted
const setLayerJS = `(async (noimg, notext) => {
  const c = document.documentElement.classList;
  c.add('ce-base'); c.toggle('ce-noimg', noimg); c.toggle('ce-notext', notext);
  await new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r)));
  return true;
})(%t, %t)`

// Layers of one page capture, all the same size (device pixels)
type Layers struct {
	Text   *image.RGBA // images hidden, text shown
	Plain  *image.RGBA // images hidden, text hidden - Text vs Plain gives the text mask
	Images *image.RGBA // images shown, text hidden
	PageH  int         // captured height in CSS pixels
}

type Browser struct {
	ctx    context.Context
	cancel func()
}

func startBrowser(chromePath string, noAA bool, workDir string) (*Browser, error) {
	opts := append(chromedp.DefaultExecAllocatorOptions[:],
		chromedp.ExecPath(chromePath),
		chromedp.UserDataDir(filepath.Join(workDir, "profile")),
		chromedp.Flag("hide-scrollbars", true),
		chromedp.Flag("disable-lcd-text", true),
		chromedp.Flag("font-render-hinting", "full"),
	)

	if noAA { // fontconfig without anti-aliasing and with full hinting - glyphs come out as crisp 1-bit shapes (Linux)
		conf := filepath.Join(workDir, "fonts.conf")
		err := os.WriteFile(conf, []byte(`<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <include ignore_missing="yes">/etc/fonts/fonts.conf</include>
  <match target="font">
    <edit name="antialias" mode="assign"><bool>false</bool></edit>
    <edit name="hinting" mode="assign"><bool>true</bool></edit>
    <edit name="hintstyle" mode="assign"><const>hintfull</const></edit>
    <edit name="rgba" mode="assign"><const>none</const></edit>
  </match>
</fontconfig>
`), 0644)
		if err != nil {
			return nil, err
		}
		opts = append(opts, chromedp.Env("FONTCONFIG_FILE="+conf))
	}

	allocCtx, allocCancel := chromedp.NewExecAllocator(context.Background(), opts...)
	ctx, ctxCancel := chromedp.NewContext(allocCtx, chromedp.WithErrorf(func(f string, a ...interface{}) {
		msg := fmt.Sprintf(f, a...)
		if strings.Contains(msg, "could not unmarshal event") { // events of newer Chrome the pinned cdproto doesn't know
			return
		}
		fmt.Fprintln(os.Stderr, "chromedp:", msg)
	}))
	if err := chromedp.Run(ctx); err != nil { // starts the browser
		ctxCancel()
		allocCancel()
		return nil, err
	}
	return &Browser{ctx: ctx, cancel: func() { ctxCancel(); allocCancel() }}, nil
}

func (b *Browser) Close() { b.cancel() }

func evalAwait(js string, res interface{}) chromedp.Action {
	return chromedp.Evaluate(js, res, func(p *runtime.EvaluateParams) *runtime.EvaluateParams {
		return p.WithAwaitPromise(true)
	})
}

// capture loads url in a new tab sized for mode and captures up to screens screen heights of it, in three layers
func (b *Browser) capture(url string, m *Mode, screens int, log func(string, ...interface{})) (*Layers, error) {
	tabCtx, tabCancel := chromedp.NewContext(b.ctx)
	defer tabCancel()

	err := chromedp.Run(tabCtx,
		emulation.SetDeviceMetricsOverride(int64(m.RenderW), int64(m.RenderH), m.DPR, false),
		page.SetBypassCSP(true), // else the page's Content-Security-Policy blocks the injected <style>
		chromedp.ActionFunc(func(ctx context.Context) error {
			_, err := page.AddScriptToEvaluateOnNewDocument(injectJS).Do(ctx)
			return err
		}),
	)
	if err != nil {
		return nil, err
	}

	t0 := time.Now()
	navCtx, navCancel := context.WithTimeout(tabCtx, 30*time.Second)
	err = chromedp.Run(navCtx, chromedp.Navigate(url))
	navCancel()
	if err != nil { // a page which never finishes loading is still captured as it is
		log("    navigate: %v (capturing anyway)", err)
	}
	log("    loaded in %d ms", time.Since(t0).Milliseconds())

	maxH := m.RenderH * screens
	var scrollH, raised int
	err = chromedp.Run(tabCtx,
		chromedp.Evaluate(injectJS, nil), // again after load - the new-document script may not have run
		chromedp.Sleep(1500*time.Millisecond),
		evalAwait(fmt.Sprintf(lazyScrollJS, m.RenderH, maxH), &scrollH),
		chromedp.Sleep(700*time.Millisecond),
		chromedp.Evaluate(fmt.Sprintf(minFontJS, m.MinFontPx), &raised),
	)
	if err != nil {
		return nil, err
	}

	var styleState string
	chromedp.Run(tabCtx, chromedp.Evaluate(`(() => { const s = document.getElementById('ce-style');
		return (s ? 'style in ' + s.parentNode.nodeName + ', sheet ' + (s.sheet ? s.sheet.cssRules.length + ' rules' : 'none') : 'NO STYLE') +
		', html class: ' + document.documentElement.className; })()`, &styleState))
	if !strings.HasPrefix(styleState, "style in") {
		log("    WARNING: injected CSS missing: %s", styleState)
	}

	capH := scrollH
	if capH > maxH {
		capH = maxH
	}
	if capH < m.RenderH {
		capH = m.RenderH
	}

	t0 = time.Now()
	layers, err := captureLayers(tabCtx, &page.Viewport{X: 0, Y: 0, Width: float64(m.RenderW), Height: float64(capH), Scale: 1})
	if err != nil {
		return nil, err
	}
	layers.PageH = capH
	log("    %d fonts raised to %d px, page %d px tall, captured %d px, 3 layers in %d ms",
		raised, m.MinFontPx, scrollH, capH, time.Since(t0).Milliseconds())

	return layers, nil
}

// screenshot as PNG: clip nil = the viewport as it is scrolled now, else that part of the page (beyond the viewport too)
func screenshot(ctx context.Context, clip *page.Viewport) ([]byte, error) {
	var buf []byte
	err := chromedp.Run(ctx, chromedp.ActionFunc(func(ctx context.Context) error {
		var err error
		p := page.CaptureScreenshot().WithFormat(page.CaptureScreenshotFormatPng).WithOptimizeForSpeed(true)
		if clip != nil {
			p = p.WithCaptureBeyondViewport(true).WithClip(clip)
		}
		buf, err = p.Do(ctx)
		return err
	}))
	return buf, err
}

// captureLayers takes the three layers of the page (see Layers) and leaves the page drawn normally again
func captureLayers(ctx context.Context, clip *page.Viewport) (*Layers, error) {
	layers := &Layers{}
	shots := []struct {
		noimg, notext bool
		dst           **image.RGBA
	}{
		{true, false, &layers.Text},
		{true, true, &layers.Plain},
		{false, true, &layers.Images},
	}

	for _, s := range shots {
		var ok bool
		if err := chromedp.Run(ctx, evalAwait(fmt.Sprintf(setLayerJS, s.noimg, s.notext), &ok)); err != nil {
			return nil, err
		}
		buf, err := screenshot(ctx, clip)
		if err != nil {
			return nil, err
		}
		img, err := png.Decode(bytes.NewReader(buf))
		if err != nil {
			return nil, err
		}
		*s.dst = toRGBA(img)
	}

	var ok bool
	err := chromedp.Run(ctx, evalAwait(fmt.Sprintf(setLayerJS, false, false), &ok))
	return layers, err
}

// convertLayers turns captured layers into Atari pixels of mode m, w x h: paletted image (index = colour register)
func convertLayers(l *Layers, m *Mode, w, h int) *image.Paletted {
	text := resample(l.Text, w, h)
	plain := resample(l.Plain, w, h)
	imgs := resample(l.Images, w, h)
	mask := textMask(text, plain)
	if m.Planes == 1 {
		return convertMono(text, imgs, mask)
	}
	return convertColor(text, imgs, mask, m.Planes, m.ColorBits)
}

func toRGBA(img image.Image) *image.RGBA {
	if r, ok := img.(*image.RGBA); ok {
		return r
	}
	r := image.NewRGBA(img.Bounds())
	draw.Draw(r, r.Bounds(), img, img.Bounds().Min, draw.Src)
	return r
}
