package main

// Messages between ce_cores (BrowserLink) and ce_browser - see ../ce_cores/hdd/browser/browserlink_protocol.h,
// keep in sync. Each message: type (B), payload length (DW), payload; multi byte values big endian.

import (
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"sync"
)

const (
	linkVersion    = 1
	linkHeaderSize = 5
	linkMaxPayload = 2 * 1024 * 1024

	// ce_cores -> ce_browser
	msgHello     = 0x01
	msgSetMode   = 0x02
	msgNavigate  = 0x03
	msgNavAction = 0x04
	msgMouse     = 0x05
	msgKey       = 0x06
	msgScroll    = 0x07

	// ce_browser -> ce_cores
	msgHelloReply = 0x81
	msgFrame      = 0x82
	msgStatus     = 0x83
	msgHistoryAdd = 0x84
)

// BRW_* values from ../ce_cores/hdd/browser/browser_commands.h
const (
	navBack    = 0
	navForward = 1
	navReload  = 2
	navStop    = 3
	navHome    = 4

	mouseMove = 0
	mouseDown = 1
	mouseUp   = 2

	stfLoading    = 0x02
	stfCanBack    = 0x04
	stfCanForward = 0x08
)

type linkMsg struct {
	typ     byte
	payload []byte
}

type Link struct {
	conn    net.Conn
	writeMu sync.Mutex
}

func dialLink(addr string) (*Link, error) {
	c, err := net.Dial("tcp", addr)
	if err != nil {
		return nil, err
	}
	return &Link{conn: c}, nil
}

func (l *Link) Close() { l.conn.Close() }

// read returns the next message, or an error when the connection is gone
func (l *Link) read() (linkMsg, error) {
	var hdr [linkHeaderSize]byte
	if _, err := io.ReadFull(l.conn, hdr[:]); err != nil {
		return linkMsg{}, err
	}
	n := binary.BigEndian.Uint32(hdr[1:])
	if n > linkMaxPayload {
		return linkMsg{}, fmt.Errorf("message 0x%02x with length %d", hdr[0], n)
	}
	p := make([]byte, n)
	if _, err := io.ReadFull(l.conn, p); err != nil {
		return linkMsg{}, err
	}
	return linkMsg{hdr[0], p}, nil
}

func (l *Link) write(typ byte, payload []byte) error {
	buf := make([]byte, linkHeaderSize+len(payload))
	buf[0] = typ
	binary.BigEndian.PutUint32(buf[1:], uint32(len(payload)))
	copy(buf[linkHeaderSize:], payload)

	l.writeMu.Lock()
	defer l.writeMu.Unlock()
	_, err := l.conn.Write(buf)
	return err
}

func (l *Link) sendHistoryAdd(url string) error {
	return l.write(msgHistoryAdd, []byte(url))
}

func (l *Link) sendHelloReply() error {
	return l.write(msgHelloReply, []byte{0, linkVersion})
}

// Frame - one converted page area, as sent to ce_cores
type Frame struct {
	seq     uint32
	res     uint8
	zoom    uint8 // quarters
	width   int
	height  int
	planes  int
	palette []uint16
	data    []byte
}

func (l *Link) sendFrame(f *Frame) error {
	p := make([]byte, 14, 14+len(f.palette)*2+len(f.data))
	binary.BigEndian.PutUint32(p[0:], f.seq)
	p[4] = f.res
	p[5] = f.zoom
	binary.BigEndian.PutUint16(p[6:], uint16(f.width))
	binary.BigEndian.PutUint16(p[8:], uint16(f.height))
	p[10] = byte(f.planes)
	binary.BigEndian.PutUint16(p[12:], uint16(len(f.palette)))
	for _, c := range f.palette {
		p = append(p, byte(c>>8), byte(c))
	}
	p = append(p, f.data...)
	return l.write(msgFrame, p)
}

// Status - what the toolbar of CE_BROWS.PRG shows
type Status struct {
	flags    uint8
	progress uint8
	caretX   uint16
	caretY   uint16
	url      string
	title    string
	message  string
}

func appendLenString(p []byte, s string) []byte {
	if len(s) > 0xffff {
		s = s[:0xffff]
	}
	p = append(p, byte(len(s)>>8), byte(len(s)))
	return append(p, s...)
}

func (l *Link) sendStatus(s Status) error {
	p := []byte{s.flags, s.progress, byte(s.caretX >> 8), byte(s.caretX), byte(s.caretY >> 8), byte(s.caretY)}
	p = appendLenString(p, s.url)
	p = appendLenString(p, s.title)
	p = appendLenString(p, s.message)
	return l.write(msgStatus, p)
}
