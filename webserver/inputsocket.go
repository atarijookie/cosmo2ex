package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"strings"
	"sync"
	"time"
)

// Input socket of ce_cores - keyboard / mouse events of the web UI go there, the same way as events from a desktop app.
// Protocol is described in ce_cores/ikbd/inputsocket.h: hello line with the token from CMD_TOKEN_FILE, "OK\n" answer,
// then 8 byte events (type, index, code uint16, value int32 - big endian).
const (
	inputHelloPrefix = "CEINPUT 1 "
	inputEventSize   = 8

	inputEvKey         = 1
	inputEvMouseMove   = 2
	inputEvMouseButton = 3

	inputRelX = 0 // REL_X
	inputRelY = 1 // REL_Y

	inputKeyCount = 1024 // INPUT_KEY_COUNT in ce_cores
)

// inputEvent encodes one event for the input socket.
func inputEvent(evType, index, code int, value int32) []byte {
	b := make([]byte, inputEventSize)
	b[0] = byte(evType)
	b[1] = byte(index)
	binary.BigEndian.PutUint16(b[2:4], uint16(code))
	binary.BigEndian.PutUint32(b[4:8], uint32(value))
	return b
}

// inputClient keeps one connection to the input socket and reconnects when it breaks (e.g. after ce_cores restart,
// which also creates a new token). Held keys are released by ce_cores when this connection closes.
type inputClient struct {
	mu   sync.Mutex
	conn net.Conn
}

// send writes the events, (re)connecting first if needed.
func (c *inputClient) send(events []byte) error {
	c.mu.Lock()
	defer c.mu.Unlock()

	if c.conn != nil && !connAlive(c.conn) { // ce_cores closed it (e.g. restarted) - writing would succeed, but get lost
		c.conn.Close()
		c.conn = nil
	}

	if c.conn == nil {
		conn, err := dialInputSocket()
		if err != nil {
			return err
		}
		c.conn = conn
	}

	c.conn.SetWriteDeadline(time.Now().Add(time.Second))
	if _, err := c.conn.Write(events); err != nil {
		c.conn.Close()
		c.conn = nil
		return fmt.Errorf("write to input socket: %w", err)
	}
	return nil
}

// connAlive checks that the other side didn't close the connection - ce_cores sends nothing after "OK\n",
// so anything else than a read timeout means the connection is gone.
func connAlive(conn net.Conn) bool {
	conn.SetReadDeadline(time.Now().Add(time.Millisecond))
	var b [1]byte
	_, err := conn.Read(b[:])
	var netErr net.Error
	return errors.As(err, &netErr) && netErr.Timeout()
}

// dialInputSocket connects to 127.0.0.1:INPUT_TCP_PORT and does the hello.
func dialInputSocket() (net.Conn, error) {
	port := os.Getenv("INPUT_TCP_PORT")
	if port == "" {
		return nil, fmt.Errorf("INPUT_TCP_PORT is not set")
	}

	tokenFile := os.Getenv("CMD_TOKEN_FILE")
	if tokenFile == "" {
		return nil, fmt.Errorf("CMD_TOKEN_FILE is not set")
	}

	token, err := os.ReadFile(tokenFile) // read on every connect - ce_cores creates a new token on each start
	if err != nil {
		return nil, fmt.Errorf("read command token: %w", err)
	}

	addr := net.JoinHostPort("127.0.0.1", port)
	conn, err := net.DialTimeout("tcp", addr, time.Second)
	if err != nil {
		return nil, fmt.Errorf("connect to input socket %s: %w", addr, err)
	}

	conn.SetDeadline(time.Now().Add(time.Second))
	if _, err := conn.Write([]byte(inputHelloPrefix + strings.TrimSpace(string(token)) + "\n")); err != nil {
		conn.Close()
		return nil, fmt.Errorf("send hello to input socket: %w", err)
	}

	answer := make([]byte, 3)
	if _, err := io.ReadFull(conn, answer); err != nil || string(answer) != "OK\n" {
		conn.Close()
		return nil, fmt.Errorf("input socket refused hello (wrong token?): %v", err)
	}

	conn.SetDeadline(time.Time{})
	return conn, nil
}

// jsonInt returns integer value of JSON number field key.
func jsonInt(payload map[string]interface{}, key string) (int, bool) {
	f, ok := payload[key].(float64)
	if !ok || f != float64(int(f)) {
		return 0, false
	}
	return int(f), true
}

// jsonUpDown returns 1 for "down", 0 for "up".
func jsonUpDown(payload map[string]interface{}) (int32, bool) {
	switch payload["state"] {
	case "down":
		return 1, true
	case "up":
		return 0, true
	}
	return 0, false
}

// mousePayloadToEvents converts web UI mouse payload to input socket events:
//
//	{"type":"relative", "x": 8, "y": 21}
//	{"type":"buttonleft" / "buttonright", "state":"down" / "up"}
func mousePayloadToEvents(payload map[string]interface{}) ([]byte, error) {
	switch payload["type"] {
	case "relative":
		x, okX := jsonInt(payload, "x")
		y, okY := jsonInt(payload, "y")
		if !okX || !okY {
			return nil, fmt.Errorf("relative mouse needs integer 'x' and 'y'")
		}
		var events []byte
		if x != 0 {
			events = append(events, inputEvent(inputEvMouseMove, 0, inputRelX, int32(x))...)
		}
		if y != 0 {
			events = append(events, inputEvent(inputEvMouseMove, 0, inputRelY, int32(y))...)
		}
		return events, nil

	case "buttonleft", "buttonright":
		state, ok := jsonUpDown(payload)
		if !ok {
			return nil, fmt.Errorf("mouse button needs 'state' down / up")
		}
		button := 0
		if payload["type"] == "buttonright" {
			button = 1
		}
		return inputEvent(inputEvMouseButton, 0, button, state), nil
	}

	return nil, fmt.Errorf("unknown mouse 'type': %v", payload["type"])
}

// keyboardPayloadToEvents converts web UI keyboard payload to input socket event:
//
//	{"type":"pc", "code": 40, "state":"down" / "up"}    - code is Linux key code (see static/js/keylinux.js)
func keyboardPayloadToEvents(payload map[string]interface{}) ([]byte, error) {
	code, ok := jsonInt(payload, "code")
	if !ok || code < 0 || code >= inputKeyCount {
		return nil, fmt.Errorf("keyboard needs integer 'code' 0 .. %d", inputKeyCount-1)
	}

	state, ok := jsonUpDown(payload)
	if !ok {
		return nil, fmt.Errorf("keyboard needs 'state' down / up")
	}

	return inputEvent(inputEvKey, 0, code, state), nil
}
