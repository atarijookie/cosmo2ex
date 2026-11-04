package main

// Games running on the host as a Source - for now Doom: ce_doomgeneric (third_party/doomgeneric, our port of
// doomgeneric) with a WAD (Freedoom by default). Its stdout: 320 x 200 RGB frames as fast as it draws, and sound
// effect events (gamesound.go mixes them); its stdin: keys (2 bytes: pressed, Doom key code). ce_media keeps the
// newest frame and hands one to the stream at the layout's frame rate, with the sound of that time - the game runs
// at its own speed, the ST gets what's newest.

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"time"
)

const (
	gameW = 320
	gameH = 200
)

// where the WAD is looked for when -doom-wad isn't given: next to ce_media (tools/ of the macOS / Windows folder,
// ../tools of /opt/ce/bin on Linux, set up by ce_setup_tools), then the system's (Debian: freedoom package, image)
var wadNames = []string{"doom.wad", "doom1.wad", "freedoom1.wad"} // a bought Doom first, when it's there

func exeDir() string {
	exe, err := os.Executable()
	if err != nil {
		return "."
	}
	if r, err := filepath.EvalSymlinks(exe); err == nil {
		exe = r
	}
	return filepath.Dir(exe)
}

func fileExists(p string) bool {
	st, err := os.Stat(p)
	return err == nil && !st.IsDir()
}

// findDoom: the engine and the WAD it will play, or why not
func (d *Decoder) findDoom() (bin, wad string, err error) {
	dir := exeDir()

	bin = d.doomBin
	if bin == "" {
		bin = filepath.Join(dir, "ce_doomgeneric")
		if runtime.GOOS == "windows" {
			bin += ".exe"
		}
	}
	if !fileExists(bin) {
		return "", "", fmt.Errorf("Doom engine not found: %s", bin)
	}

	if d.doomWad != "" {
		if !fileExists(d.doomWad) {
			return "", "", fmt.Errorf("Doom WAD not found: %s", d.doomWad)
		}
		return bin, d.doomWad, nil
	}
	dirs := []string{filepath.Join(dir, "tools"), filepath.Join(dir, "..", "tools"),
		"/usr/share/games/doom", "/usr/local/share/games/doom"}
	for _, dd := range dirs {
		for _, n := range wadNames {
			if p := filepath.Join(dd, n); fileExists(p) {
				return bin, filepath.Clean(p), nil
			}
		}
	}
	return "", "", errors.New("no Doom WAD (freedoom1.wad) - run ce_setup_tools on the host")
}

// gameTitle: what the ST shows as the title
func gameTitle(wad string) string {
	switch strings.ToLower(filepath.Base(wad)) {
	case "freedoom1.wad":
		return "Freedoom: Phase 1"
	case "doom1.wad":
		return "Doom (shareware)"
	default:
		return "Doom"
	}
}

//--------------------------------------------------------------------------------------------------------------------

type gameSource struct {
	cmd   *exec.Cmd
	stdin io.WriteCloser

	mu     sync.Mutex
	latest []byte // newest frame, 320 x 200 RGB
	ended  chan struct{}
	errBuf tailBuffer // the game's last messages - for the error text when it doesn't start

	w, h     int // layout frame size
	frameDur time.Duration
	t0       time.Time
	n        int

	sfx *sfxMixer // nil without sound
	wad *wadFile

	closeOnce sync.Once
	keyMu     sync.Mutex
}

func openGameSource(dec *Decoder, name string, l *Layout) (*gameSource, string, error) {
	if strings.ToLower(name) != "doom" {
		return nil, "", fmt.Errorf("unknown game %q", name)
	}
	bin, wad, err := dec.findDoom()
	if err != nil {
		return nil, "", err
	}
	if a, err := filepath.Abs(bin); err == nil { // it runs in its own dir
		bin = a
	}
	if a, err := filepath.Abs(wad); err == nil {
		wad = a
	}

	// config and saved games: in the user's cache dir, they stay between games
	work := filepath.Join(os.TempDir(), "ce_media_doom")
	if c, err := os.UserCacheDir(); err == nil {
		work = filepath.Join(c, "ce_media", "doom")
	}
	os.MkdirAll(work, 0755)

	s := &gameSource{latest: make([]byte, gameW*gameH*3), ended: make(chan struct{}), w: l.vm.w, h: l.vm.h}
	if l.hasAudio() { // the sound effects come from the same WAD
		if s.wad, err = openWad(wad); err != nil {
			return nil, "", err
		}
		s.sfx = newSfxMixer(s.wad, l.rate)
	}
	s.frameDur = time.Duration(int64(time.Second) * int64(l.fpsDen) / int64(l.fpsNum))
	s.cmd = exec.Command(bin, "-iwad", wad)
	s.cmd.Dir = work // default.cfg and saved games go here
	s.cmd.Stderr = &s.errBuf
	out, err := s.cmd.StdoutPipe()
	if err != nil {
		return nil, "", err
	}
	if s.stdin, err = s.cmd.StdinPipe(); err != nil {
		return nil, "", err
	}
	if err := s.cmd.Start(); err != nil {
		return nil, "", fmt.Errorf("can't start %s: %v", bin, err)
	}
	log.Printf("doom: %s -iwad %s (pid %d)", bin, wad, s.cmd.Process.Pid)

	first := make(chan error, 1)
	go func() { // reader: keeps the newest frame, passes the sound events to the mixer
		got := false
		err := s.readRecords(bufio.NewReaderSize(out, 256*1024), func() {
			if !got {
				got = true
				first <- nil
			}
		})
		if !got {
			if err == nil {
				err = io.EOF
			}
			first <- err
		}
		close(s.ended)
	}()

	select {
	case err := <-first:
		if err != nil {
			s.Close()
			return nil, "", fmt.Errorf("Doom didn't start: %s", s.errBuf.lastLine())
		}
	case <-time.After(15 * time.Second):
		s.Close()
		return nil, "", errors.New("Doom didn't start (no picture in 15 s)")
	}
	s.t0 = time.Now()
	return s, gameTitle(wad), nil
}

// ReadFrame: the newest frame, at the layout's frame rate; io.EOF when the game ended (quit from its menu)
func (s *gameSource) ReadFrame(rgb []byte) error {
	due := s.t0.Add(time.Duration(s.n) * s.frameDur)
	if d := time.Until(due); d > 0 {
		select {
		case <-s.ended:
			return io.EOF
		case <-time.After(d):
		}
	} else if d < -time.Second { // way behind (the stream was blocked) - don't hurry to catch up
		s.t0 = time.Now()
		s.n = 0
	}
	select {
	case <-s.ended:
		return io.EOF
	default:
	}
	s.n++

	s.mu.Lock()
	scaleGameFrame(rgb, s.w, s.h, s.latest)
	s.mu.Unlock()
	return nil
}

// readRecords: what the game writes (see doomgeneric_ce.c), until it ends
func (s *gameSource) readRecords(r *bufio.Reader, gotFrame func()) error {
	buf := make([]byte, gameW*gameH*3)
	var hdr [4]byte
	for {
		t, err := r.ReadByte()
		if err != nil {
			return err
		}
		switch t {
		case 'F':
			if _, err := io.ReadFull(r, buf); err != nil {
				return err
			}
			s.mu.Lock()
			copy(s.latest, buf)
			s.mu.Unlock()
			gotFrame()
		case 'S': // channel, volume, separation, name length, name
			if _, err := io.ReadFull(r, hdr[:4]); err != nil {
				return err
			}
			name := make([]byte, hdr[3])
			if _, err := io.ReadFull(r, name); err != nil {
				return err
			}
			if s.sfx != nil {
				s.sfx.start(int(hdr[0]), hdr[1], string(name))
			}
		case 'U': // channel, volume, separation
			if _, err := io.ReadFull(r, hdr[:3]); err != nil {
				return err
			}
			if s.sfx != nil {
				s.sfx.update(int(hdr[0]), hdr[1])
			}
		case 'X': // channel
			if _, err := io.ReadFull(r, hdr[:1]); err != nil {
				return err
			}
			if s.sfx != nil {
				s.sfx.stop(int(hdr[0]))
			}
		default:
			return fmt.Errorf("game: unknown record 0x%02x", t)
		}
	}
}

// ReadAudio: the sound effects playing now, mixed - always all of pcm (silence when nothing plays)
func (s *gameSource) ReadAudio(pcm []int16) (int, error) {
	if s.sfx == nil {
		return 0, io.EOF
	}
	select {
	case <-s.ended:
		return 0, io.EOF
	default:
	}
	s.sfx.mix(pcm)
	return len(pcm), nil
}

// keys: IKBD scancodes of the ST (bit 7 = released)
func (s *gameSource) keys(scancodes []byte) {
	s.keyMu.Lock()
	defer s.keyMu.Unlock()
	var ev []byte
	for _, sc := range scancodes {
		k := atariToDoomKey[sc&0x7f]
		if k == 0 {
			continue
		}
		pressed := byte(1)
		if sc&0x80 != 0 {
			pressed = 0
		}
		ev = append(ev, pressed, k)
	}
	if len(ev) > 0 {
		s.stdin.Write(ev)
	}
}

// Close: stdin closed - the game quits; killed when it doesn't
func (s *gameSource) Close() {
	s.closeOnce.Do(func() {
		s.keyMu.Lock()
		s.stdin.Close()
		s.keyMu.Unlock()
		done := make(chan struct{})
		go func() { s.cmd.Wait(); close(done) }()
		select {
		case <-done:
		case <-time.After(2 * time.Second):
			s.cmd.Process.Kill()
			<-done
		}
		if s.wad != nil {
			s.wad.Close()
		}
		log.Printf("doom: ended")
	})
}

// scaleGameFrame: 320 x 200 into w x h - the biggest whole multiple, in the middle, black around
func scaleGameFrame(dst []byte, w, h int, src []byte) {
	for i := range dst {
		dst[i] = 0
	}
	f := w / gameW
	if h/gameH < f {
		f = h / gameH
	}
	if f < 1 { // smaller than the game (not a mode we have) - cut
		f = 1
	}
	ox, oy := (w-gameW*f)/2, (h-gameH*f)/2
	for y := 0; y < gameH*f && oy+y < h; y++ {
		if oy+y < 0 {
			continue
		}
		srow := src[(y/f)*gameW*3:]
		drow := dst[((oy+y)*w)*3:]
		for x := 0; x < gameW*f && ox+x < w; x++ {
			if ox+x < 0 {
				continue
			}
			copy(drow[(ox+x)*3:(ox+x)*3+3], srow[(x/f)*3:(x/f)*3+3])
		}
	}
}

// lastLine: the last line the program wrote (tailBuffer: ffmpeg.go)
func (t *tailBuffer) lastLine() string {
	lines := strings.Split(strings.TrimSpace(t.String()), "\n")
	for i := len(lines) - 1; i >= 0; i-- {
		if l := strings.TrimSpace(lines[i]); l != "" {
			if len(l) > 120 {
				l = l[:120]
			}
			return l
		}
	}
	return "no message"
}

//--------------------------------------------------------------------------------------------------------------------
// ST keyboard -> Doom keys (doomkeys.h). The ST's scancodes are the PC's (set 1) for most keys.

const (
	dkRight  = 0xae
	dkLeft   = 0xac
	dkUp     = 0xad
	dkDown   = 0xaf
	dkStrafL = 0xa0
	dkStrafR = 0xa1
	dkUse    = 0xa2
	dkFire   = 0xa3
	dkShift  = 0x80 + 0x36
	dkAlt    = 0x80 + 0x38
	dkF1     = 0x80 + 0x3b
	dkHome   = 0x80 + 0x47
	dkIns    = 0x80 + 0x52
	dkDel    = 0x80 + 0x53
)

var atariToDoomKey = func() [128]byte {
	var m [128]byte
	m[0x01] = 27 // Esc - menu
	for i, c := range "1234567890-=" {
		m[0x02+i] = byte(c)
	}
	m[0x0e] = 0x7f // Backspace
	m[0x0f] = 9    // Tab - map
	for i, c := range "qwertyuiop[]" {
		m[0x10+i] = byte(c)
	}
	m[0x1c] = 13     // Return
	m[0x1d] = dkFire // Control - fire
	for i, c := range "asdfghjkl;'`" {
		m[0x1e+i] = byte(c)
	}
	m[0x2a] = dkShift // left Shift - run
	m[0x2b] = '\\'
	for i, c := range "zxcvbnm" {
		m[0x2c+i] = byte(c)
	}
	m[0x33] = dkStrafL // , .  - strafe, as in the original Doom
	m[0x34] = dkStrafR
	m[0x35] = '/'
	m[0x36] = dkShift // right Shift
	m[0x38] = dkAlt   // Alternate - strafe with the arrows
	m[0x39] = dkUse   // Space - open doors, switches
	for i := 0; i < 10; i++ {
		m[0x3b+i] = byte(dkF1 + i) // F1 .. F10
	}
	m[0x47] = dkHome // Clr Home
	m[0x48] = dkUp
	m[0x4b] = dkLeft
	m[0x4d] = dkRight
	m[0x50] = dkDown
	m[0x52] = dkIns // Insert
	m[0x53] = dkDel // Delete
	m[0x62] = dkF1  // Help - Doom's help screen
	m[0x4a] = '-'   // keypad - (zoom the map out)
	m[0x4e] = '='   // keypad + (in)
	m[0x68] = dkUp  // keypad 8 4 6 2
	m[0x6a] = dkLeft
	m[0x6c] = dkRight
	m[0x6e] = dkDown
	m[0x72] = 13 // keypad Enter
	return m
}()
