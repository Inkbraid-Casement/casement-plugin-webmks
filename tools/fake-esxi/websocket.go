package main

// The server side of RFC 6455, as much as an ESXi console endpoint needs:
// the upgrade with a subprotocol, binary messages in both directions, ping
// and close.

import (
	"bufio"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"strings"
	"sync"
)

const websocketGUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

const (
	opContinuation = 0x0
	opText         = 0x1
	opBinary       = 0x2
	opClose        = 0x8
	opPing         = 0x9
	opPong         = 0xa
)

// wsConn is an upgraded connection. Reads happen on one goroutine; writes
// are serialised.
type wsConn struct {
	conn  net.Conn
	r     *bufio.Reader
	wlock sync.Mutex
	// Payload left of the current data frame.
	remaining uint64
	mask      [4]byte
	masked    bool
	maskPos   int
}

// offeredProtocols lists the subprotocols in Sec-WebSocket-Protocol.
func offeredProtocols(r *http.Request) []string {
	var out []string
	for _, h := range r.Header.Values("Sec-WebSocket-Protocol") {
		for _, p := range strings.Split(h, ",") {
			if p = strings.TrimSpace(p); p != "" {
				out = append(out, p)
			}
		}
	}
	return out
}

// upgrade answers the handshake, choosing `protocol`.
func upgrade(w http.ResponseWriter, r *http.Request, protocol string) (*wsConn, error) {
	if !strings.EqualFold(r.Header.Get("Upgrade"), "websocket") ||
		!strings.Contains(strings.ToLower(r.Header.Get("Connection")), "upgrade") {
		return nil, errors.New("not a WebSocket upgrade")
	}
	if r.Header.Get("Sec-WebSocket-Version") != "13" {
		return nil, fmt.Errorf("WebSocket version %q", r.Header.Get("Sec-WebSocket-Version"))
	}
	key := r.Header.Get("Sec-WebSocket-Key")
	if key == "" {
		return nil, errors.New("no Sec-WebSocket-Key")
	}
	hj, ok := w.(http.Hijacker)
	if !ok {
		return nil, errors.New("can't hijack the connection")
	}
	conn, rw, err := hj.Hijack()
	if err != nil {
		return nil, err
	}
	sum := sha1.Sum([]byte(key + websocketGUID))
	accept := base64.StdEncoding.EncodeToString(sum[:])
	reply := "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" +
		"Sec-WebSocket-Accept: " + accept + "\r\n"
	if protocol != "" {
		reply += "Sec-WebSocket-Protocol: " + protocol + "\r\n"
	}
	reply += "\r\n"
	if _, err := rw.WriteString(reply); err != nil {
		conn.Close()
		return nil, err
	}
	if err := rw.Flush(); err != nil {
		conn.Close()
		return nil, err
	}
	return &wsConn{conn: conn, r: rw.Reader}, nil
}

func (c *wsConn) writeFrame(op byte, payload []byte) error {
	c.wlock.Lock()
	defer c.wlock.Unlock()
	head := []byte{0x80 | op}
	switch n := len(payload); {
	case n < 126:
		head = append(head, byte(n))
	case n < 65536:
		head = append(head, 126, byte(n>>8), byte(n))
	default:
		head = append(head, 127)
		head = binary.BigEndian.AppendUint64(head, uint64(n))
	}
	if _, err := c.conn.Write(append(head, payload...)); err != nil {
		return err
	}
	return nil
}

// WriteMessage sends one binary message.
func (c *wsConn) WriteMessage(payload []byte) error { return c.writeFrame(opBinary, payload) }

// Close sends a close frame with a status and reason and closes the
// connection.
func (c *wsConn) Close(status uint16, reason string) {
	payload := binary.BigEndian.AppendUint16(nil, status)
	payload = append(payload, reason...)
	_ = c.writeFrame(opClose, payload)
	c.conn.Close()
}

// errClosed is returned when the client sent a close frame.
var errClosed = errors.New("closed by the client")

// Read returns data from binary (and continuation) frames, answering pings
// on the way.
func (c *wsConn) Read(p []byte) (int, error) {
	for c.remaining == 0 {
		if err := c.nextFrame(); err != nil {
			return 0, err
		}
	}
	n := uint64(len(p))
	if n > c.remaining {
		n = c.remaining
	}
	got, err := io.ReadFull(c.r, p[:n])
	if c.masked {
		for i := 0; i < got; i++ {
			p[i] ^= c.mask[c.maskPos&3]
			c.maskPos++
		}
	}
	c.remaining -= uint64(got)
	return got, err
}

// nextFrame reads frame headers until a data frame with payload starts.
func (c *wsConn) nextFrame() error {
	var head [2]byte
	if _, err := io.ReadFull(c.r, head[:]); err != nil {
		return err
	}
	op := head[0] & 0x0f
	masked := head[1]&0x80 != 0
	length := uint64(head[1] & 0x7f)
	switch length {
	case 126:
		var b [2]byte
		if _, err := io.ReadFull(c.r, b[:]); err != nil {
			return err
		}
		length = uint64(binary.BigEndian.Uint16(b[:]))
	case 127:
		var b [8]byte
		if _, err := io.ReadFull(c.r, b[:]); err != nil {
			return err
		}
		length = binary.BigEndian.Uint64(b[:])
	}
	if !masked {
		return errors.New("an unmasked frame from the client")
	}
	if _, err := io.ReadFull(c.r, c.mask[:]); err != nil {
		return err
	}
	c.masked = true
	c.maskPos = 0
	switch op {
	case opBinary, opContinuation:
		c.remaining = length
		return nil
	case opText:
		return errors.New("a text frame on a binary console")
	case opPing, opPong, opClose:
		if length > 125 {
			return errors.New("a control frame over 125 bytes")
		}
		payload := make([]byte, length)
		if _, err := io.ReadFull(c.r, payload); err != nil {
			return err
		}
		for i := range payload {
			payload[i] ^= c.mask[i&3]
		}
		switch op {
		case opPing:
			return c.writeFrame(opPong, payload)
		case opClose:
			_ = c.writeFrame(opClose, payload)
			return errClosed
		}
		return nil
	default:
		return fmt.Errorf("frame opcode %d", op)
	}
}
