package main

// ffmpeg in docker without a container per decoder: creating and removing containers took seconds (a seek had to
// wait for 2 removed and 2 created ones), so there's one worker container for the directory of the file. It mounts
// that directory read-only as /media, has no network, and runs just 'cat' on its stdin - ffprobe and ffmpeg start in
// it with 'docker exec' (~0.2 s).
//
// Nothing is killed from the outside - killing a docker client doesn't stop what runs in the container. Instead the
// stdin of every docker client is a pipe from ce_media: closing it ends what runs in the container (the worker's
// 'cat', the decoder's wrapper which then kills its ffmpeg). When ce_media dies, the system closes those pipes, so
// the containers go away with it too.

import (
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"sync/atomic"
	"time"
)

var containerSeq int64

type worker struct {
	name   string
	dir    string
	cmd    *exec.Cmd
	stdin  io.WriteCloser
	stderr *tailBuffer
	exited chan struct{}
}

// decoderWrapper: runs ffmpeg in the background, gives up its own stdout (so the output ends when ffmpeg ends), and
// kills ffmpeg when stdin closes
const decoderWrapper = `ffmpeg "$@" & p=$!; exec >/dev/null; cat >/dev/null; kill -9 $p 2>/dev/null`

// workerFor: the worker container for this directory - the running one, or a new one (the old one stops)
func (d *Decoder) workerFor(dir string) (*worker, error) {
	d.wmu.Lock()
	defer d.wmu.Unlock()

	if w := d.worker; w != nil {
		select {
		case <-w.exited:
			d.worker = nil
		default:
			if w.dir == dir {
				return w, nil
			}
			d.stopWorkerLocked()
		}
	}

	w := &worker{
		name:   fmt.Sprintf("ce_media_%d_%d", os.Getpid(), atomic.AddInt64(&containerSeq, 1)),
		dir:    dir,
		stderr: &tailBuffer{},
		exited: make(chan struct{}),
	}
	w.cmd = exec.Command(d.docker, "run", "-i", "--rm", "--init", "--name", w.name, "--network", "none",
		"-v", dir+":/media:ro", "--entrypoint", "sh", d.dockerImage, "-c", "cat >/dev/null")
	w.cmd.Stderr = w.stderr
	var err error
	if w.stdin, err = w.cmd.StdinPipe(); err != nil {
		return nil, err
	}
	if err := w.cmd.Start(); err != nil {
		return nil, fmt.Errorf("can't start %s: %v", d.docker, err)
	}
	go func() { w.cmd.Wait(); close(w.exited) }()

	for i := 0; i < 100; i++ { // running when an exec works
		if exec.Command(d.docker, "exec", w.name, "true").Run() == nil {
			d.worker = w
			return w, nil
		}
		select {
		case <-w.exited:
			msg := lastLine(w.stderr.String())
			if msg == "" {
				msg = "the ffmpeg container ended"
			}
			return nil, fmt.Errorf("docker: %s", msg)
		case <-time.After(100 * time.Millisecond):
		}
	}
	w.stdin.Close()
	d.removeContainer(w.name)
	return nil, fmt.Errorf("docker: the ffmpeg container didn't start")
}

func (d *Decoder) stopWorkerLocked() {
	w := d.worker
	if w == nil {
		return
	}
	d.worker = nil
	w.stdin.Close() // its 'cat' ends, the container ends and is removed (--rm)
	d.cleanups.Add(1)
	go func() {
		defer d.cleanups.Done()
		select {
		case <-w.exited:
		case <-time.After(5 * time.Second): // didn't end - make sure
			d.removeContainer(w.name)
		}
	}()
}

// stopWorker: when ce_media ends
func (d *Decoder) stopWorker() {
	d.wmu.Lock()
	d.stopWorkerLocked()
	d.wmu.Unlock()
}

func (d *Decoder) removeContainer(name string) {
	exec.Command(d.docker, "rm", "-f", name).Run()
}

// command: the tool with args, the input file is the "{input}" arg. In docker: exec in the worker container of the
// file's directory; a decoder (stream = true) runs in decoderWrapper, which ends it when its stdin closes.
func (d *Decoder) command(tool string, file string, args []string, stream bool) (*exec.Cmd, error) {
	input := file
	var prefix []string
	bin := d.ffmpeg
	if tool == "ffprobe" {
		bin = d.ffprobe
	}

	if d.dockerImage != "" {
		w, err := d.workerFor(filepath.Dir(file))
		if err != nil {
			return nil, err
		}
		bin = d.docker
		input = "/media/" + filepath.Base(file)
		prefix = []string{"exec", "-i", w.name}
		if stream {
			prefix = append(prefix, "sh", "-c", decoderWrapper, "sh")
		} else {
			prefix = append(prefix, tool)
		}
	}

	full := append([]string{}, prefix...)
	for _, a := range args {
		if a == "{input}" {
			a = input
		}
		full = append(full, a)
	}
	return exec.Command(bin, full...), nil
}
