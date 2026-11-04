package main

// Connection to ce_cores (MediaLink) - see ../ce_cores/hdd/media/medialink_protocol.h. Each message: type (B),
// payload length (DW), payload; multi byte values big endian.

import (
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"sync"
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

// send writes one message; it blocks while ce_cores has enough chunks (it stops reading) - that's the flow control
func (l *Link) send(typ byte, payload []byte) error {
	hdr := make([]byte, linkHeaderSize)
	hdr[0] = typ
	binary.BigEndian.PutUint32(hdr[1:], uint32(len(payload)))

	l.writeMu.Lock()
	defer l.writeMu.Unlock()
	if _, err := l.conn.Write(hdr); err != nil {
		return err
	}
	_, err := l.conn.Write(payload)
	return err
}
