package main

// 'serve': connected to ce_cores, plays what the ST opens - one stream at a time, OPEN / SEEK replace it.

import (
	"encoding/binary"
	"fmt"
	"log"
	"os"
	"os/signal"
	"syscall"
	"time"
)

type streamCtl struct {
	stop chan struct{}
	done chan struct{}
	src  Source
}

type server struct {
	link *Link
	dec  *Decoder

	cur     *streamCtl
	req     OpenReq
	probe   *Probe
	layout  *Layout
	ymTable []ymEntry
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
				log.Printf("ce_cores link version %d", binary.BigEndian.Uint16(m.payload))
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
			if len(m.payload) < 8 || s.layout == nil {
				continue
			}
			s.stopStream()
			s.startStream(binary.BigEndian.Uint32(m.payload), int(binary.BigEndian.Uint32(m.payload[4:])), false)
		case msgInput:
			// files don't take keys - a game source would get them here
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
	ctl := &streamCtl{stop: make(chan struct{}), done: make(chan struct{})}
	s.cur = ctl
	req := s.req

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
		ctl.src = src
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
		t0 = time.Now()
		if err := st.run(); err != nil && err != errStopped {
			log.Printf("stream: %v", err)
		} else if err == nil {
			log.Printf("stream generation %d ended (sent in %v)", gen, time.Since(t0).Round(time.Millisecond))
		}
	}()
}

func maxInt(a, b int) int {
	if a > b {
		return a
	}
	return b
}
