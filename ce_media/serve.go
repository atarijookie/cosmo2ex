package main

// 'serve': connected to ce_cores, plays what the ST opens - one stream at a time, OPEN / SEEK replace it.

import (
	"encoding/binary"
	"fmt"
	"log"
	"os"
	"os/signal"
	"sync"
	"syscall"
	"time"
)

type streamCtl struct {
	stop chan struct{}
	done chan struct{}
	mu   sync.Mutex // src is set by the stream's goroutine, read by the message loop (keys)
	src  Source
	gen  uint32
	// live streams: one token per chunk that may be on the way to the ST - a chunk takes one, msgTaken gives it
	// back. So at most liveInFlight chunks wait anywhere between here and the ST (the TCP buffers could hold
	// seconds of them), and each one is made from what's newest when the ST is ready for it. nil = not used.
	credits chan struct{}
}

const liveInFlight = 2

// waitCredit: before each chunk of a live stream - a token, or the stream is stopped. nil when not used.
func (ctl *streamCtl) waitCredit() func() error {
	if ctl.credits == nil {
		return nil
	}
	return func() error {
		select {
		case <-ctl.credits:
			return nil
		case <-ctl.stop:
			return errStopped
		}
	}
}

type server struct {
	link *Link
	dec  *Decoder

	cur         *streamCtl
	peerVersion int // of ce_cores: from 2 on it says when the ST took a live chunk (msgTaken)
	req         OpenReq
	probe       *Probe
	layout      *Layout
	ymTable     []ymEntry
}

func serve(addr string, dec *Decoder) error {
	link, err := dialLink(addr)
	if err != nil {
		return err
	}
	defer link.Close()

	s := &server{link: link, dec: dec, ymTable: dec.ymTable()}

	// stopped by ce_cores (SIGTERM) or the link closing: the decoders go, and their containers with them -
	// a container left behind would decode the rest of the file for nothing
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
	go func() {
		<-sig
		log.Printf("stopping")
		link.Close() // the read loop ends, serve() stops the stream and waits for the containers
	}()
	defer dec.waitCleanups(5 * time.Second)
	defer dec.stopWorker() // after the stream is stopped (defers run backwards)
	dec.ensureImage()      // ffmpeg in docker: pull the image now if it's not there, not when the 1st file is opened
	if err := link.send(msgHelloReply, []byte{0, linkVersion}); err != nil {
		return err
	}
	defer s.stopStream()

	for {
		m, err := link.read()
		if err != nil {
			return fmt.Errorf("link: %v", err)
		}
		switch m.typ {
		case msgHello:
			if len(m.payload) >= 2 {
				s.peerVersion = int(binary.BigEndian.Uint16(m.payload))
				log.Printf("ce_cores link version %d", s.peerVersion)
			}
		case msgTaken:
			if s.cur != nil && s.cur.credits != nil && len(m.payload) >= 4 && binary.BigEndian.Uint32(m.payload) == s.cur.gen {
				select {
				case s.cur.credits <- struct{}{}:
				default:
				}
			}
		case msgOpen:
			s.stopStream()
			req, err := parseOpen(m.payload)
			if err != nil {
				log.Printf("bad OPEN: %v", err)
				s.sendInfoError(err.Error())
				continue
			}
			s.req = req
			s.probe, s.layout = nil, nil
			s.startStream(req.gen, req.startMs, true)
		case msgSeek:
			if len(m.payload) < 8 || s.layout == nil || s.req.flags&opfGame != 0 { // a game can't seek
				continue
			}
			s.stopStream()
			s.startStream(binary.BigEndian.Uint32(m.payload), int(binary.BigEndian.Uint32(m.payload[4:])), false)
		case msgInput:
			if s.cur != nil {
				s.cur.mu.Lock()
				g, ok := s.cur.src.(*gameSource)
				s.cur.mu.Unlock()
				if ok {
					g.keys(m.payload)
				}
			}
		case msgClose:
			s.stopStream()
			s.probe, s.layout = nil, nil
		default:
			log.Printf("unknown message 0x%02x ignored", m.typ)
		}
	}
}

func (s *server) sendInfoError(msg string) {
	log.Printf("error: %s", msg)
	s.link.send(msgInfo, infoBlock(stateError, nil, 0, "", msg))
}

func (s *server) stopStream() {
	if s.cur == nil {
		return
	}
	close(s.cur.stop)
	<-s.cur.done
	s.cur = nil
}

// startStream: probe (first time) and decode from startMs. The goroutine sends INFO when it's the OPEN.
func (s *server) startStream(gen uint32, startMs int, sendInfo bool) {
	ctl := &streamCtl{stop: make(chan struct{}), done: make(chan struct{}), gen: gen}
	if req := s.req; req.flags&opfLive != 0 && s.peerVersion >= 2 {
		ctl.credits = make(chan struct{}, liveInFlight)
		for i := 0; i < liveInFlight; i++ {
			ctl.credits <- struct{}{}
		}
	}
	s.cur = ctl
	req := s.req

	if req.flags&opfGame != 0 {
		go func() {
			defer close(ctl.done)
			s.runGame(ctl, gen, req)
		}()
		return
	}

	go func() {
		defer close(ctl.done)

		err := s.dec.waitImage(ctl.stop, func(msg string) { // the ST shows how the download goes
			s.link.send(msgInfo, infoBlock(stateStarting, nil, 0, "", msg))
		})
		if err != nil {
			if err != errStopped {
				s.sendInfoError(err.Error())
			}
			return
		}

		if s.probe == nil {
			t0 := time.Now()
			pr, err := s.dec.probe(req.path)
			if err != nil {
				s.sendInfoError(err.Error())
				return
			}
			l, err := computeLayout(req, pr.hasVideo, pr.hasAudio, pr.fpsNum, pr.fpsDen)
			if err != nil {
				s.sendInfoError(err.Error())
				return
			}
			s.probe, s.layout = pr, &l
			log.Printf("%s: %dx%d %.3f fps, audio %v, %d ms - %s, %d frames/chunk, %d sectors/chunk, clock %d Hz (probe %v)",
				req.path, pr.width, pr.height, float64(pr.fpsNum)/float64(maxInt(pr.fpsDen, 1)), pr.hasAudio, pr.durationMs,
				l.vm.name, l.F, l.chunkSectors, l.rate, time.Since(t0).Round(time.Millisecond))
		}
		pr, l := s.probe, s.layout

		t0 := time.Now()
		src, err := openFFmpegSource(s.dec, req.path, pr, l, startMs)
		log.Printf("decoding from %d ms, first frame after %v", startMs, time.Since(t0).Round(time.Millisecond))
		if err != nil {
			s.sendInfoError(err.Error())
			return
		}
		ctl.mu.Lock()
		ctl.src = src
		ctl.mu.Unlock()
		defer src.Close()

		select { // stopped while starting - the ST moved on
		case <-ctl.stop:
			return
		default:
		}

		if sendInfo {
			if l.afmt == afmtYM {
				s.link.send(msgYmTable, ymTableBytes(s.ymTable))
			}
			s.link.send(msgInfo, infoBlock(stateReady, l, pr.durationMs, pr.title, ""))
		}

		// a stop closes the source, so a blocked read returns
		go func() { <-ctl.stop; src.Close() }()

		st := newStream(l, src, gen, startMs, s.ymTable, s.link.send, ctl.stop)
		st.waitReady = ctl.waitCredit()
		t0 = time.Now()
		if err := st.run(); err != nil && err != errStopped {
			log.Printf("stream: %v", err)
		} else if err == nil {
			log.Printf("stream generation %d ended (sent in %v)", gen, time.Since(t0).Round(time.Millisecond))
		}
	}()
}

// runGame: a game on the host (OPEN with MED_OPF_GAME) - no probe, no seek; it runs until the ST closes it or the
// game ends (quit from its menu)
func (s *server) runGame(ctl *streamCtl, gen uint32, req OpenReq) {
	if req.vmode == vmodeNone {
		s.sendInfoError("a game needs a screen")
		return
	}
	l, err := computeLayout(req, true, req.aout != aoutNone, 35, 1) // Doom's world moves 35 times a second
	if err != nil {
		s.sendInfoError(err.Error())
		return
	}
	s.layout = &l

	s.link.send(msgInfo, infoBlock(stateStarting, nil, 0, "", "Starting "+req.path+"..."))
	src, title, err := openGameSource(s.dec, req.path, &l)
	if err != nil {
		s.sendInfoError(err.Error())
		return
	}
	ctl.mu.Lock()
	ctl.src = src
	ctl.mu.Unlock()
	defer src.Close()

	select {
	case <-ctl.stop:
		return
	default:
	}
	log.Printf("game %s: %s, %d/%d fps, %d sectors/chunk", req.path, l.vm.name, l.fpsNum, l.fpsDen, l.chunkSectors)
	if l.afmt == afmtYM { // before READY - the ST asks for the table as soon as it sees READY
		s.link.send(msgYmTable, ymTableBytes(s.ymTable))
	}
	s.link.send(msgInfo, infoBlock(stateReady, &l, 0, title, ""))

	go func() { <-ctl.stop; src.Close() }()
	st := newStream(&l, src, gen, 0, s.ymTable, s.link.send, ctl.stop)
	st.waitReady = ctl.waitCredit() // the frame is taken when the ST is ready for it - as fresh as it gets
	if err := st.run(); err != nil && err != errStopped {
		log.Printf("game stream: %v", err)
	} else if err == nil {
		log.Printf("game %s ended", req.path)
	}
}

func maxInt(a, b int) int {
	if a > b {
		return a
	}
	return b
}
