package main

// ffmpeg / ffprobe - installed on the host (-ffmpeg / -ffprobe), or in a docker image (-docker-image), so nothing has
// to be installed besides docker. Video and audio are decoded by two ffmpeg processes started at the same position:
// one pipe each works the same on Linux, macOS and Windows (a 2nd output pipe of one process wouldn't on Windows).

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
)

type Decoder struct {
	ffmpeg      string
	ffprobe     string
	dockerImage string // when set, ffmpeg and ffprobe run in this image: docker run ... <image>
	docker      string // docker binary
	img         *imageState
	ymTablePath string         // YM table from a file instead of the built one
	cleanups    sync.WaitGroup // stopped decoders / containers still ending
	wmu         sync.Mutex
	worker      *worker // docker: the container ffmpeg runs in (dockerworker.go)
	doomBin     string  // games (game.go): engine and WAD - default: found by findDoom
	doomWad     string
}

//--------------------------------------------------------------------------------------------------------------------
// probe

type Probe struct {
	durationMs int
	title      string

	hasVideo       bool
	videoIndex     int
	width, height  int
	dar            float64 // display aspect ratio of the picture
	fpsNum, fpsDen int

	hasAudio   bool
	audioIndex int
}

type ffprobeOut struct {
	Streams []struct {
		Index        int               `json:"index"`
		CodecType    string            `json:"codec_type"`
		Width        int               `json:"width"`
		Height       int               `json:"height"`
		SAR          string            `json:"sample_aspect_ratio"`
		DAR          string            `json:"display_aspect_ratio"`
		AvgFrameRate string            `json:"avg_frame_rate"`
		RFrameRate   string            `json:"r_frame_rate"`
		Disposition  map[string]int    `json:"disposition"`
		Tags         map[string]string `json:"tags"`
	} `json:"streams"`
	Format struct {
		Duration string            `json:"duration"`
		Tags     map[string]string `json:"tags"`
	} `json:"format"`
}

func parseRatio(s string) (int, int) {
	parts := strings.Split(s, "/")
	if len(parts) != 2 {
		parts = strings.Split(s, ":")
	}
	if len(parts) != 2 {
		return 0, 0
	}
	a, err1 := strconv.Atoi(parts[0])
	b, err2 := strconv.Atoi(parts[1])
	if err1 != nil || err2 != nil || a <= 0 || b <= 0 {
		return 0, 0
	}
	return a, b
}

func (d *Decoder) probe(file string) (*Probe, error) {
	cmd, err := d.command("ffprobe", file, []string{"-v", "error", "-print_format", "json", "-show_format", "-show_streams", "{input}"}, false)
	if err != nil {
		return nil, err
	}

	var stdout, stderr bytes.Buffer
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	if err := cmd.Run(); err != nil {
		msg := lastLine(stderr.String())
		if msg == "" {
			msg = err.Error()
		}
		return nil, fmt.Errorf("ffprobe: %s", msg)
	}
	return parseProbe(stdout.Bytes(), filepath.Base(file))
}

func parseProbe(js []byte, fileName string) (*Probe, error) {
	var out ffprobeOut
	if err := json.Unmarshal(js, &out); err != nil {
		return nil, fmt.Errorf("ffprobe output: %v", err)
	}

	p := &Probe{title: fileName}
	if t := out.Format.Tags["title"]; t != "" {
		p.title = t
	}
	if dur, err := strconv.ParseFloat(out.Format.Duration, 64); err == nil && dur > 0 {
		p.durationMs = int(dur * 1000)
	}

	for _, s := range out.Streams {
		switch s.CodecType {
		case "video":
			if p.hasVideo || s.Disposition["attached_pic"] != 0 || s.Width <= 0 || s.Height <= 0 { // cover art of a song isn't a video
				continue
			}
			p.hasVideo, p.videoIndex, p.width, p.height = true, s.Index, s.Width, s.Height
			p.dar = float64(s.Width) / float64(s.Height)
			if a, b := parseRatio(s.DAR); a > 0 {
				p.dar = float64(a) / float64(b)
			} else if a, b := parseRatio(s.SAR); a > 0 {
				p.dar = float64(s.Width*a) / float64(s.Height*b)
			}
			p.fpsNum, p.fpsDen = parseRatio(s.AvgFrameRate)
			if p.fpsNum == 0 {
				p.fpsNum, p.fpsDen = parseRatio(s.RFrameRate)
			}
		case "audio":
			if !p.hasAudio {
				p.hasAudio, p.audioIndex = true, s.Index
			}
		}
	}
	if !p.hasVideo && !p.hasAudio {
		return nil, fmt.Errorf("no video or audio in the file")
	}
	return p, nil
}

func lastLine(s string) string {
	lines := strings.Split(strings.TrimSpace(s), "\n")
	return strings.TrimSpace(lines[len(lines)-1])
}

//--------------------------------------------------------------------------------------------------------------------
// decoding processes

type proc struct {
	d        *Decoder
	cmd      *exec.Cmd
	out      io.ReadCloser
	stdin    io.WriteCloser // docker: closing it ends the decoder in the container
	stderr   *tailBuffer
	stopOnce sync.Once
}

// tailBuffer keeps the last few kB of the stderr of ffmpeg - for the error message
type tailBuffer struct {
	mu  sync.Mutex
	buf []byte
}

func (t *tailBuffer) Write(p []byte) (int, error) {
	t.mu.Lock()
	t.buf = append(t.buf, p...)
	if len(t.buf) > 4096 {
		t.buf = t.buf[len(t.buf)-4096:]
	}
	t.mu.Unlock()
	return len(p), nil
}

func (t *tailBuffer) String() string {
	t.mu.Lock()
	defer t.mu.Unlock()
	return string(t.buf)
}

func (d *Decoder) start(file string, args []string) (*proc, error) {
	cmd, err := d.command("ffmpeg", file, args, true)
	if err != nil {
		return nil, err
	}
	p := &proc{d: d, cmd: cmd, stderr: &tailBuffer{}}
	cmd.Stderr = p.stderr
	out, err := cmd.StdoutPipe()
	if err != nil {
		return nil, err
	}
	p.out = out
	if d.dockerImage != "" {
		if p.stdin, err = cmd.StdinPipe(); err != nil {
			return nil, err
		}
	}
	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("can't start %s: %v", cmd.Path, err)
	}
	return p, nil
}

// errorText: why it ended (when it ended without data)
func (p *proc) errorText() string {
	if s := lastLine(p.stderr.String()); s != "" {
		return s
	}
	return "decoder ended"
}

// stop ends the decoder right away: native ffmpeg is killed; in docker, its stdin closes (killing the docker client
// wouldn't stop it in the container). Waiting for it goes on in the background.
func (p *proc) stop() {
	p.stopOnce.Do(func() {
		if p.stdin != nil {
			p.stdin.Close()
		} else if p.cmd.Process != nil {
			p.cmd.Process.Kill()
		}
		p.out.Close()
		p.d.cleanups.Add(1)
		go func() {
			defer p.d.cleanups.Done()
			done := make(chan struct{})
			go func() { p.cmd.Wait(); close(done) }()
			select {
			case <-done:
			case <-time.After(5 * time.Second):
				if p.cmd.Process != nil {
					p.cmd.Process.Kill()
				}
			}
		}()
	})
}

// waitCleanups: before exiting - the containers of stopped decoders are gone (or the timeout passed)
func (d *Decoder) waitCleanups(timeout time.Duration) {
	done := make(chan struct{})
	go func() { d.cleanups.Wait(); close(done) }()
	select {
	case <-done:
	case <-time.After(timeout):
	}
}

// videoArgs: frames of exactly w x h RGB, the picture scaled to aw x ah (aspect kept) in the middle, black around
func videoArgs(pr *Probe, startMs int, fpsNum, fpsDen, w, h, aw, ah int) []string {
	vf := fmt.Sprintf("fps=%d/%d,scale=%d:%d:flags=area,setsar=1,pad=%d:%d:(ow-iw)/2:(oh-ih)/2:color=black,format=rgb24",
		fpsNum, fpsDen, aw, ah, w, h)
	return append(seekArgs(startMs), "-i", "{input}", "-map", fmt.Sprintf("0:%d", pr.videoIndex),
		"-an", "-sn", "-dn", "-vf", vf, "-f", "rawvideo", "-pix_fmt", "rgb24", "-")
}

// audioArgs: mono signed 16 bit little endian at the ST's rate
func audioArgs(pr *Probe, startMs int, rate int) []string {
	return append(seekArgs(startMs), "-i", "{input}", "-map", fmt.Sprintf("0:%d", pr.audioIndex),
		"-vn", "-sn", "-dn", "-ac", "1", "-ar", strconv.Itoa(rate), "-f", "s16le", "-")
}

func seekArgs(startMs int) []string {
	a := []string{"-hide_banner", "-nostdin", "-loglevel", "error"}
	if startMs > 0 {
		a = append(a, "-ss", fmt.Sprintf("%d.%03d", startMs/1000, startMs%1000))
	}
	return a
}

// activeSize: size of the picture on the frame - as big as fits, with the file's aspect ratio as the monitor shows it
func activeSize(vm vmodeInfo, dar float64) (int, int) {
	if dar <= 0 {
		dar = 4.0 / 3.0
	}
	frameAspect := float64(vm.w) * vm.par / float64(vm.h)
	aw, ah := vm.w, vm.h
	if dar > frameAspect { // wider - bars top and bottom
		ah = int(float64(vm.w)*vm.par/dar + 0.5)
	} else {
		aw = int(float64(vm.h)*dar/vm.par + 0.5)
	}
	aw, ah = aw&^1, ah&^1
	if aw < 2 {
		aw = 2
	}
	if ah < 2 {
		ah = 2
	}
	return aw, ah
}
