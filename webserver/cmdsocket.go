package main

import (
	"fmt"
	"net"
	"os"
	"strings"
)

// writeToCmdSocket sends the given message to ce_cores as a UDP datagram on 127.0.0.1:CMD_UDP_PORT
// and returns the connection, so the caller can read a reply on it (and must close it).
// Every datagram starts with the token from CMD_TOKEN_FILE (written by ce_cores on start): "<token>\n<msg>".
// Both values are expected to be loaded from .env (with ${VAR} expansion handled by loadEnvFromDotFile).
func writeToCmdSocket(msg string) (net.Conn, error) {
	port := os.Getenv("CMD_UDP_PORT")
	if port == "" {
		return nil, fmt.Errorf("CMD_UDP_PORT is not set")
	}

	tokenFile := os.Getenv("CMD_TOKEN_FILE")
	if tokenFile == "" {
		return nil, fmt.Errorf("CMD_TOKEN_FILE is not set")
	}

	// read on every send - ce_cores creates a new token on each start
	token, err := os.ReadFile(tokenFile)
	if err != nil {
		return nil, fmt.Errorf("read command token: %w", err)
	}

	addr := net.JoinHostPort("127.0.0.1", port)
	conn, err := net.Dial("udp", addr)
	if err != nil {
		return nil, fmt.Errorf("dial udp %q: %w", addr, err)
	}

	if _, err := conn.Write([]byte(strings.TrimSpace(string(token)) + "\n" + msg)); err != nil {
		conn.Close()
		return nil, fmt.Errorf("write to udp %q: %w", addr, err)
	}

	return conn, nil
}

// sendToCmdSocket sends the given message to ce_cores, no reply expected.
func sendToCmdSocket(msg string) error {
	conn, err := writeToCmdSocket(msg)
	if err != nil {
		return err
	}
	defer conn.Close()

	logDebugf("cmdsocket: sent to %s: %q", conn.RemoteAddr(), msg)
	return nil
}
