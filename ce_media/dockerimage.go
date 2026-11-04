package main

// ffmpeg in docker: the image has to be there before the first file can be opened. 'docker run' would pull it on its
// own, but silently, inside the first ffprobe - a 1 GB image takes longer than the ST waits. So ce_media checks for
// the image when it starts and pulls it in the background; a file opened meanwhile waits, and the ST is told how
// far the download is.

import (
	"bufio"
	"fmt"
	"log"
	"os/exec"
	"strings"
	"sync"
	"time"
)

const (
	imgUnknown = iota
	imgChecking
	imgPulling
	imgReady
	imgFailed
)

type imageState struct {
	mu      sync.Mutex
	state   int
	layers  map[string]bool // layer id -> done
	err     string
	changed chan struct{} // closed and replaced on every change
}

func (d *Decoder) imageInit() {
	d.img = &imageState{changed: make(chan struct{})}
}

func (s *imageState) set(f func()) {
	s.mu.Lock()
	f()
	close(s.changed)
	s.changed = make(chan struct{})
	s.mu.Unlock()
}

// ensureImage starts the check / pull in the background, unless it's ready or already going
func (d *Decoder) ensureImage() {
	if d.dockerImage == "" {
		return
	}
	s := d.img
	s.mu.Lock()
	if s.state == imgChecking || s.state == imgPulling || s.state == imgReady {
		s.mu.Unlock()
		return
	}
	s.state = imgChecking
	s.mu.Unlock()

	go d.checkAndPull()
}

func (d *Decoder) checkAndPull() {
	s := d.img
	if exec.Command(d.docker, "image", "inspect", "--format", "{{.Id}}", d.dockerImage).Run() == nil {
		s.set(func() { s.state = imgReady })
		return
	}

	log.Printf("docker image %s not here, pulling it", d.dockerImage)
	s.set(func() { s.state, s.layers, s.err = imgPulling, map[string]bool{}, "" })

	cmd := exec.Command(d.docker, "pull", d.dockerImage)
	out, err := cmd.StdoutPipe()
	tail := &tailBuffer{}
	cmd.Stderr = tail
	if err == nil {
		err = cmd.Start()
	}
	if err != nil {
		s.set(func() { s.state, s.err = imgFailed, fmt.Sprintf("can't run %s: %v", d.docker, err) })
		return
	}

	sc := bufio.NewScanner(out)
	for sc.Scan() {
		if id, status, ok := parsePullLine(sc.Text()); ok {
			s.set(func() {
				switch status {
				case "Pulling fs layer", "Waiting":
					if _, seen := s.layers[id]; !seen {
						s.layers[id] = false
					}
				case "Pull complete", "Already exists":
					s.layers[id] = true
				}
			})
		}
	}

	if err := cmd.Wait(); err != nil {
		msg := lastLine(tail.String())
		if msg == "" {
			msg = err.Error()
		}
		log.Printf("docker pull %s failed: %s", d.dockerImage, msg)
		s.set(func() { s.state, s.err = imgFailed, "docker pull failed: "+msg })
		return
	}
	log.Printf("docker image %s pulled", d.dockerImage)
	s.set(func() { s.state = imgReady })
}

// parsePullLine: "034d6572bf28: Pull complete" -> id, status
func parsePullLine(line string) (string, string, bool) {
	i := strings.Index(line, ": ")
	if i <= 0 {
		return "", "", false
	}
	id := line[:i]
	for _, c := range id {
		if !strings.ContainsRune("0123456789abcdef", c) {
			return "", "", false
		}
	}
	return id, strings.TrimSpace(line[i+2:]), true
}

// progressText: what the ST shows while the image is being pulled
func (s *imageState) progressText() string {
	done, total := 0, len(s.layers)
	for _, d := range s.layers {
		if d {
			done++
		}
	}
	if total == 0 {
		return "Downloading ffmpeg image..."
	}
	return fmt.Sprintf("Downloading ffmpeg image %d/%d", done, total)
}

// waitImage blocks until the image is there (nil), the pull failed (error) or stop closes (errStopped); progress
// gets the text for the ST whenever it changes, but at least every few seconds (so it's seen the host is alive)
func (d *Decoder) waitImage(stop <-chan struct{}, progress func(string)) error {
	if d.dockerImage == "" {
		return nil
	}
	d.ensureImage() // a failed pull is tried again on the next file

	last := ""
	for {
		s := d.img
		s.mu.Lock()
		state, errText, changed := s.state, s.err, s.changed
		text := ""
		if state == imgChecking {
			text = "Checking ffmpeg image..."
		} else if state == imgPulling {
			text = s.progressText()
		}
		s.mu.Unlock()

		switch state {
		case imgReady:
			return nil
		case imgFailed:
			return fmt.Errorf("%s", errText)
		}
		if text != last {
			progress(text)
			last = text
		}

		select {
		case <-stop:
			return errStopped
		case <-changed:
		case <-time.After(5 * time.Second):
		}
	}
}
