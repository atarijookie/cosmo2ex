package main

// 'serve' - the browser for CE_BROWS.PRG. ce_cores starts it with -connect 127.0.0.1:<port>; it keeps one Chromium
// tab, watches it for changes and sends each changed page area converted to the Atari mode as a frame. Commands from
// the ST (mode, URL, back / forward, mouse, keys, scroll) come from ce_cores. Messages: link.go.

import (
	"bytes"
	"context"
	"crypto/sha256"
	"flag"
	"fmt"
	"log"
	"math"
	"net/url"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/chromedp/cdproto/emulation"
	"github.com/chromedp/cdproto/input"
	"github.com/chromedp/cdproto/page"
	"github.com/chromedp/chromedp"
)

const (
	checkFastMs = 250  // how often the page is checked for changes after input / while loading
	checkSlowMs = 1500 // ... and otherwise
	fastForMs   = 2500 // how long after input / load the checks stay fast
	pokeDelayMs = 120  // first check after input - give the page a moment to react
)

type server struct {
	link    *Link
	tab     context.Context
	home    string
	search  string
	verbose bool
	events  chan string
	msgs    chan linkMsg

	mode       *Mode // nil until SET_MODE
	zoomQ      uint8 // zoom in quarters
	pageW      int   // page area in Atari pixels
	pageH      int
	cssW       int // page area in CSS pixels
	cssH       int
	seq        uint32
	lastSent   []byte // bitplanes of the last sent frame
	lastHash   [32]byte
	haveHash   bool
	loading    bool
	navigated  bool // something was loaded (else the first check after SET_MODE goes to the home page)
	fastUntil  time.Time
	nextCheck  time.Time
	lastStatus Status
	haveStatus bool
	phase      string // what is going on, for CE_BROWS.PRG's status line: opening, rendering, ...
	errMsg     string // error of the last navigation - shown instead of the phase until the next one
	firstFrame bool   // the first frame of this page / mode is still to come
	committed  bool   // the navigation got as far as the new page (until then the old page is still shown)

	strip         *scrollStrip // converted strip of the page for fast scrolling (scrollstrip.go), nil = none
	lastScrollAt  time.Time
	stripGen      int              // bumped when the page changed - a strip captured before is thrown away
	stripBuilding bool             // a strip is being captured in the background
	stripReady    chan stripResult // ... and comes here
	captureMu     sync.Mutex       // one layer capture at a time - they switch classes on the page
	stripCancel   func()           // cancels the background strip capture
}

func serveMain(args []string) {
	fs := flag.NewFlagSet("serve", flag.ExitOnError)
	connect := fs.String("connect", "", "ce_cores link address, host:port (given by ce_cores when it starts ce_browser)")
	chromePath := fs.String("chrome", os.Getenv("CHROME_PATH"), "path to chrome / chrome-headless-shell (default: $CHROME_PATH, then Chrome for Testing in user cache dir)")
	noAA := fs.Bool("noaa", true, "turn font anti-aliasing off through fontconfig (Linux only)")
	home := fs.String("home", "https://en.wikipedia.org/wiki/Atari_ST", "home page")
	search := fs.String("search", "https://www.bing.com/search?q=%s", "search URL for text which isn't an address, %s = the text")
	verbose := fs.Bool("v", os.Getenv("CE_BROWSER_VERBOSE") != "", "log every frame and command (or set CE_BROWSER_VERBOSE)")
	fs.Parse(args)

	log.SetPrefix("ce_browser: ")
	log.SetFlags(log.Ltime | log.Lmicroseconds)

	if *connect == "" {
		log.Fatal("serve needs -connect host:port")
	}
	if *chromePath == "" {
		*chromePath = chromeDefaultPath()
	}
	if _, err := os.Stat(*chromePath); err != nil {
		log.Fatalf("chrome not found at %q - set CHROME_PATH or -chrome", *chromePath)
	}

	link, err := dialLink(*connect)
	if err != nil {
		log.Fatalf("can't connect to ce_cores at %s: %v", *connect, err)
	}
	defer link.Close()

	// profile kept between runs - cookies and logins stay
	cache, _ := os.UserCacheDir()
	workDir := filepath.Join(cache, "ce_browser", "serve")
	if err := os.MkdirAll(workDir, 0755); err != nil {
		log.Fatal(err)
	}

	b, err := startBrowser(*chromePath, *noAA, workDir)
	if err != nil {
		log.Fatalf("starting chrome: %v", err)
	}
	defer b.Close()

	s := &server{link: link, home: *home, search: *search, verbose: *verbose,
		events: make(chan string, 64), msgs: make(chan linkMsg, 64), stripReady: make(chan stripResult, 1)}

	tab, tabCancel := chromedp.NewContext(b.ctx)
	defer tabCancel()
	s.tab = tab
	chromedp.ListenTarget(tab, func(ev interface{}) { // must not block - only passes the event on
		var name string
		switch ev.(type) {
		case *page.EventFrameStartedLoading:
			name = "start"
		case *page.EventLoadEventFired:
			name = "load"
		case *page.EventFrameStoppedLoading:
			name = "stop"
		case *page.EventFrameNavigated:
			if e := ev.(*page.EventFrameNavigated); e.Frame != nil && e.Frame.ParentID == "" { // main frame only
				name = "navigated"
			}
		}
		if name != "" {
			select {
			case s.events <- name:
			default:
			}
		}
	})
	err = chromedp.Run(tab,
		page.SetBypassCSP(true), // else the page's Content-Security-Policy blocks the injected <style>
		chromedp.ActionFunc(func(ctx context.Context) error {
			_, err := page.AddScriptToEvaluateOnNewDocument(injectJS).Do(ctx)
			return err
		}),
		chromedp.Navigate("about:blank"),
	)
	if err != nil {
		log.Fatalf("setting up the tab: %v", err)
	}

	go func() { // reader - the connection closing ends ce_browser
		for {
			m, err := link.read()
			if err != nil {
				log.Printf("link to ce_cores closed: %v", err)
				close(s.msgs)
				return
			}
			s.msgs <- m
		}
	}()

	sigs := make(chan os.Signal, 1)
	signal.Notify(sigs, syscall.SIGTERM, syscall.SIGINT)

	log.Printf("connected to ce_cores at %s", *connect)
	s.run(sigs)
}

func (s *server) run(sigs chan os.Signal) {
	ticker := time.NewTicker(50 * time.Millisecond)
	defer ticker.Stop()

	for {
		select {
		case m, ok := <-s.msgs:
			if !ok {
				return
			}
			s.handle(m)
		case ev := <-s.events:
			switch ev {
			case "start":
				s.loading = true
			case "navigated":
				s.committed = true
			case "load":
				s.dropStrip() // late content came in
				s.loading = false
				s.reportLoaded()
				s.loadDone()
			case "stop":
				s.loading = false
				s.loadDone()
			}
			s.poke()
		case <-ticker.C:
			if s.mode != nil && !time.Now().Before(s.nextCheck) {
				s.check()
			}
		case sig := <-sigs:
			log.Printf("%v - quitting", sig)
			return
		}
	}
}

func (s *server) logv(f string, a ...interface{}) {
	if s.verbose {
		log.Printf(f, a...)
	}
}

// poke - something happened (input, navigation): check the page soon and often for a while
func (s *server) poke() {
	now := time.Now()
	s.fastUntil = now.Add(fastForMs * time.Millisecond)
	next := now.Add(pokeDelayMs * time.Millisecond)
	if s.nextCheck.After(next) || s.nextCheck.Before(now) {
		s.nextCheck = next
	}
}

func (s *server) run1(what string, actions ...chromedp.Action) {
	if err := chromedp.Run(s.tab, actions...); err != nil {
		log.Printf("%s: %v", what, err)
		if s.tab.Err() != nil { // the tab / browser is gone - ce_cores starts a new ce_browser
			log.Fatal("browser gone, quitting")
		}
	}
}

//--------------------------------------------------------
// commands from ce_cores

func (s *server) handle(m linkMsg) {
	p := m.payload
	switch m.typ {
	case msgHello:
		s.link.sendHelloReply()
	case msgSetMode:
		if len(p) >= 7 {
			s.setMode(p[0], p[1], int(p[2])<<8|int(p[3]), int(p[4])<<8|int(p[5]), int(p[6]))
		}
	case msgNavigate:
		s.navigate(atariToUTF8(p))
	case msgNavAction:
		if len(p) >= 1 {
			s.navAction(p[0])
		}
	case msgMouse:
		if len(p) >= 6 {
			s.mouse(p[0], int(p[1])<<8|int(p[2]), int(p[3])<<8|int(p[4]), p[5])
		}
	case msgKey:
		if len(p) >= 3 {
			s.key(p[0], p[1], p[2])
		}
	case msgScroll:
		if len(p) >= 4 {
			s.scroll(int8(p[0]), int8(p[1]), int8(p[2]), int8(p[3]))
		}
	default:
		log.Printf("unknown message 0x%02x ignored", m.typ)
	}
}

func (s *server) setMode(res, zoomQ uint8, w, h, planes int) {
	m := modeByRes(res)
	if m == nil || w != m.W || h > m.H || h < 16 || planes != m.Planes || zoomQ < 2 {
		log.Printf("bad mode: res 0x%02x, %dx%d, %d planes, zoom %d/4", res, w, h, planes, zoomQ)
		return
	}

	z := float64(zoomQ) / 4
	sx := float64(m.RenderW) / float64(m.W) // CSS pixels per Atari pixel at zoom 1 (2 across in TT low, 2 down in ST mid)
	sy := float64(m.RenderH) / float64(m.H)
	s.mode, s.zoomQ, s.pageW, s.pageH = m, zoomQ, w, h
	s.cssW = int(math.Round(float64(w) * sx / z))
	s.cssH = int(math.Round(float64(h) * sy / z))

	s.run1("set viewport", emulation.SetDeviceMetricsOverride(int64(s.cssW), int64(s.cssH), m.DPR*z, false))
	log.Printf("mode %s, page %dx%d, zoom %gx -> viewport %dx%d CSS pixels", m.Name, w, h, z, s.cssW, s.cssH)

	s.dropStrip()
	s.lastSent = nil // full frame for the new mode
	s.haveHash = false
	s.firstFrame = true
	s.poke() // the first check goes to the home page if no URL came by then

}

// toURL - what the user typed: an address (with or without scheme), else a search
func (s *server) toURL(text string) string {
	t := strings.TrimSpace(text)
	switch {
	case t == "":
		return s.home
	case strings.Contains(t, "://") || strings.HasPrefix(t, "about:"):
		return t
	case !strings.ContainsAny(t, " \t") && (strings.Contains(t, ".") || strings.HasPrefix(t, "localhost")):
		return "https://" + t
	}
	return fmt.Sprintf(s.search, url.QueryEscape(t))
}

func (s *server) navigate(text string) {
	u := s.toURL(text)
	log.Printf("navigate: %s", u)
	s.navigated = true
	s.loading = true
	s.committed = false
	s.firstFrame = true
	s.errMsg = ""
	s.dropStrip()
	host := u
	if pu, err := url.Parse(u); err == nil && pu.Host != "" {
		host = pu.Host
	}
	s.setPhase("Opening " + host + "...")

	s.run1("navigate", chromedp.ActionFunc(func(ctx context.Context) error {
		_, _, errText, err := page.Navigate(u).Do(ctx) // returns when the navigation started, not when loaded
		if err == nil && errText != "" {
			s.errMsg = "Can't open " + host + ": " + errText
			s.committed = true // Chrome shows its error page - let it be seen
			s.updateStatus()
		}
		return err
	}))
	s.poke()
}

// setPhase changes what the status line says and sends it right away
func (s *server) setPhase(p string) {
	s.phase = p
	s.updateStatus()
}

// loadDone - the page finished loading (or was stopped)
func (s *server) loadDone() {
	s.committed = true // also when the navigation never got to a new page (a download, ...) - show what's there
	if !s.firstFrame {
		s.setPhase("")
	}
}

// frameShown - the Atari has (or is getting) the current picture of the page
func (s *server) frameShown() {
	if !s.firstFrame {
		return
	}
	s.firstFrame = false
	if s.loading {
		s.setPhase("Loading rest of page...")
	} else {
		s.setPhase("")
	}
}

// reportLoaded - a page finished loading: if it's a real page (not Chrome's error page), it goes to the URL history
func (s *server) reportLoaded() {
	var href string
	if err := chromedp.Run(s.tab, chromedp.Evaluate("location.href", &href)); err != nil {
		return
	}
	if strings.HasPrefix(href, "http://") || strings.HasPrefix(href, "https://") {
		s.logv("loaded: %s", href)
		s.link.sendHistoryAdd(href)
	}
}

func (s *server) navAction(a uint8) {
	s.dropStrip()
	switch a {
	case navBack, navForward:
		s.run1("history", chromedp.ActionFunc(func(ctx context.Context) error {
			cur, entries, err := page.GetNavigationHistory().Do(ctx)
			if err != nil {
				return err
			}
			i := int(cur) - 1
			if a == navForward {
				i = int(cur) + 1
			}
			if i < 0 || i >= len(entries) {
				return nil
			}
			return page.NavigateToHistoryEntry(entries[i].ID).Do(ctx)
		}))
	case navReload:
		s.run1("reload", page.Reload())
	case navStop:
		s.run1("stop", page.StopLoading())
	case navHome:
		s.navigate(s.home)
		return
	}
	s.poke()
}

// Atari page area pixels -> CSS pixels of the viewport
func (s *server) toCSS(x, y int) (float64, float64) {
	return float64(x) * float64(s.cssW) / float64(s.pageW), float64(y) * float64(s.cssH) / float64(s.pageH)
}

func (s *server) mouse(ev uint8, x, y int, buttons uint8) {
	if s.mode == nil {
		return
	}
	cx, cy := s.toCSS(x, y)
	var held int64 // CDP: 1 left, 2 right
	if buttons&1 != 0 {
		held |= 1
	}
	if buttons&2 != 0 {
		held |= 2
	}
	btn := input.Left
	if buttons&1 == 0 && buttons&2 != 0 {
		btn = input.Right
	}

	switch ev {
	case mouseMove:
		s.run1("mouse", input.DispatchMouseEvent(input.MouseMoved, cx, cy).WithButtons(held))
	case mouseDown:
		s.dropStrip() // a click may change the page
		s.run1("mouse", input.DispatchMouseEvent(input.MousePressed, cx, cy).WithButton(btn).WithButtons(held).WithClickCount(1))
	case mouseUp:
		s.dropStrip()
		s.run1("mouse", input.DispatchMouseEvent(input.MouseReleased, cx, cy).WithButton(btn).WithButtons(held).WithClickCount(1))
	}
	s.poke()
}

type keyDef struct {
	key, code string
	vk        int64
	text      string
}

// non-printing keys by Atari scancode
var atariKeys = map[uint8]keyDef{
	0x01: {"Escape", "Escape", 27, ""},
	0x0e: {"Backspace", "Backspace", 8, ""},
	0x0f: {"Tab", "Tab", 9, ""},
	0x1c: {"Enter", "Enter", 13, "\r"},
	0x72: {"Enter", "NumpadEnter", 13, "\r"},
	0x53: {"Delete", "Delete", 46, ""},
	0x52: {"Insert", "Insert", 45, ""},
	0x47: {"Home", "Home", 36, ""},
	0x48: {"ArrowUp", "ArrowUp", 38, ""},
	0x50: {"ArrowDown", "ArrowDown", 40, ""},
	0x4b: {"ArrowLeft", "ArrowLeft", 37, ""},
	0x4d: {"ArrowRight", "ArrowRight", 39, ""},
	0x3b: {"F1", "F1", 112, ""}, 0x3c: {"F2", "F2", 113, ""}, 0x3d: {"F3", "F3", 114, ""}, 0x3e: {"F4", "F4", 115, ""},
	0x3f: {"F5", "F5", 116, ""}, 0x40: {"F6", "F6", 117, ""}, 0x41: {"F7", "F7", 118, ""}, 0x42: {"F8", "F8", 119, ""},
	0x43: {"F9", "F9", 120, ""}, 0x44: {"F10", "F10", 121, ""},
}

func (s *server) key(scancode, ascii, shift uint8) {
	s.dropStrip()           // typing, Enter, ... may change the page
	var mods input.Modifier // Kbshift(): bit 0/1 shift, 2 control, 3 alternate
	if shift&3 != 0 {
		mods |= input.ModifierShift
	}
	if shift&4 != 0 {
		mods |= input.ModifierCtrl
	}
	if shift&8 != 0 {
		mods |= input.ModifierAlt
	}

	k, special := atariKeys[scancode]
	if !special {
		if ascii < 32 || ascii == 127 {
			s.logv("key: scancode 0x%02x ascii %d not mapped", scancode, ascii)
			return
		}
		ch := atariToUTF8([]byte{ascii})
		k = keyDef{key: ch, vk: int64(strings.ToUpper(ch)[0])}
		if mods&(input.ModifierCtrl|input.ModifierAlt) == 0 { // a typed character, not a shortcut
			k.text = ch
		}
	}

	down := input.DispatchKeyEvent(input.KeyDown).WithKey(k.key).WithWindowsVirtualKeyCode(k.vk).WithModifiers(mods)
	if k.code != "" {
		down = down.WithCode(k.code)
	}
	if k.text != "" {
		down = down.WithText(k.text)
	}
	up := input.DispatchKeyEvent(input.KeyUp).WithKey(k.key).WithWindowsVirtualKeyCode(k.vk).WithModifiers(mods)
	s.run1("key", down, up)
	s.poke()
}

// scroll by lines / columns (40 CSS pixels) and pages (90 % of the viewport), negative = up / left
func (s *server) scroll(lines, pages, cols, hpages int8) {
	if cols == 0 && hpages == 0 && s.scrollFast(lines, pages) {
		s.poke() // checks resume when the scrolling stops
		return
	}
	s.dropStrip()
	js := fmt.Sprintf("window.scrollBy(%d * 40 + %d * Math.round(window.innerWidth * 0.9), %d * 40 + %d * Math.round(window.innerHeight * 0.9))",
		cols, hpages, lines, pages)
	s.run1("scroll", chromedp.Evaluate(js, nil))
	s.poke()
}

//--------------------------------------------------------
// watching the page

// check - has the page changed? If so, render it and send the frame. Also sends the status when it changed.
func (s *server) check() {
	if !s.navigated { // nothing loaded yet and no URL came with the mode - home page
		s.navigate(s.home)
		return
	}
	if s.scrolling() { // strip frames are going out - the full render comes when the scrolling stops
		s.nextCheck = s.lastScrollAt.Add(scrollSettleMs * time.Millisecond)
		return
	}

	now := time.Now()
	fast := s.loading || now.Before(s.fastUntil)
	if fast {
		s.nextCheck = now.Add(checkFastMs * time.Millisecond)
		var raised int
		minFont := int(math.Ceil(float64(s.mode.MinFontPx) * 4 / float64(s.zoomQ)))
		s.run1("inject", chromedp.Evaluate(injectJS, nil), chromedp.Evaluate(fmt.Sprintf(minFontJS, minFont), &raised))
	} else {
		s.nextCheck = now.Add(checkSlowMs * time.Millisecond)
	}

	s.updateStatus()

	shot, err := screenshot(s.tab, nil)
	if err != nil {
		log.Printf("screenshot: %v", err)
		return
	}
	if s.firstFrame && !s.committed { // still the old page on the screen - not worth sending
		return
	}
	h := sha256.Sum256(shot)
	if s.haveHash && h == s.lastHash {
		return
	}
	s.lastHash, s.haveHash = h, true
	s.render()
}

func (s *server) render() {
	if s.firstFrame {
		s.setPhase("Rendering page...")
	}
	t0 := time.Now()
	ctx, cancel := context.WithTimeout(s.tab, 8*time.Second) // a page which never paints must not hang ce_browser
	s.captureMu.Lock()                                       // a background strip capture may be running
	layers, err := captureLayers(ctx, nil)
	s.captureMu.Unlock()
	cancel()
	if err != nil {
		log.Printf("capture: %v", err)
		return
	}
	t1 := time.Now()

	img := convertLayers(layers, s.mode, s.pageW, s.pageH)
	data := toBitplanes(img, s.mode.Planes)
	if s.lastSent != nil && bytes.Equal(data, s.lastSent) {
		s.logv("render: same pixels as frame %d, not sent", s.seq)
		s.frameShown()
		return
	}

	s.seq++
	f := &Frame{seq: s.seq, res: s.mode.Res, zoom: s.zoomQ, width: s.pageW, height: s.pageH, planes: s.mode.Planes,
		palette: paletteWords(img.Palette, s.mode.Planes, s.mode.ColorBits), data: data}
	if err := s.link.sendFrame(f); err != nil {
		log.Printf("send frame: %v", err)
		return
	}
	s.lastSent = data
	s.frameShown()
	s.logv("frame %d: capture %d ms, convert %d ms, %d bytes", s.seq, t1.Sub(t0).Milliseconds(),
		time.Since(t1).Milliseconds(), len(data))
}

func (s *server) updateStatus() {
	st := Status{caretX: 0xffff, caretY: 0xffff, progress: 100}
	if s.loading {
		st.flags |= stfLoading
		st.progress = 50
	}

	err := chromedp.Run(s.tab, chromedp.ActionFunc(func(ctx context.Context) error {
		cur, entries, err := page.GetNavigationHistory().Do(ctx)
		if err != nil || int(cur) >= len(entries) {
			return err
		}
		st.url = entries[cur].URL
		st.title = entries[cur].Title
		if cur > 0 {
			st.flags |= stfCanBack
		}
		if int(cur) < len(entries)-1 {
			st.flags |= stfCanForward
		}
		return nil
	}))
	if err != nil {
		log.Printf("history: %v", err)
		return
	}

	st.message = s.phase
	if s.errMsg != "" {
		st.message = s.errMsg
	}
	st.url = toAtariASCII(st.url) // the ST shows only ASCII sensibly - see ascii.go
	st.title = toAtariASCII(st.title)
	st.message = toAtariASCII(st.message)

	if s.haveStatus && st == s.lastStatus {
		return
	}
	if err := s.link.sendStatus(st); err == nil {
		s.lastStatus, s.haveStatus = st, true
	}
}

//--------------------------------------------------------

// Atari ST character set 0x80..0xaf (the rest above 0x7f is rarely typed: Hebrew, Greek, maths) -> Unicode
var atariHigh = []rune("ÇüéâäàåçêëèïîìÄÅÉæÆôöòûùÿÖÜ¢£¥ßƒáíóúñÑªº¿⌐¬½¼¡«»ãõØøœŒÀÃÕ¨´†¶©®™")

func atariToUTF8(b []byte) string {
	var sb strings.Builder
	for _, c := range b {
		switch {
		case c == 0:
			return sb.String()
		case c < 0x80:
			sb.WriteByte(c)
		case int(c-0x80) < len(atariHigh):
			sb.WriteRune(atariHigh[c-0x80])
		default:
			sb.WriteRune('?')
		}
	}
	return sb.String()
}
