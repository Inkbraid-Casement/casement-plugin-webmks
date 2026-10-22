package main

// The console endpoint: GET /ticket/<ticket> upgraded to a WebSocket with
// the subprotocol "binary", carrying RFB to and from a real VNC server, the
// way ESXi serves a VM's console.

import (
	"crypto/rand"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"io"
	"log"
	"net"
	"net/http"
	"slices"
	"strings"
	"sync"
	"time"
)

type ticket struct {
	vm   string
	vnc  string
	used bool
	// The console closes after this long (0: never).
	closeAfter time.Duration
}

type keyEvent struct {
	Keysym  uint32 `json:"keysym"`
	Keycode uint32 `json:"keycode,omitempty"`
	Down    bool   `json:"down"`
}

type pointerEvent struct {
	Buttons uint8  `json:"buttons"`
	X       uint16 `json:"x"`
	Y       uint16 `json:"y"`
}

// rfbStats is what the client sent on the last console connection, read
// from the RFB stream as it passes through.
type rfbStats struct {
	Version        string         `json:"version"`
	Encodings      []int32        `json:"encodings"`
	PixelFormat    []byte         `json:"pixel_format"`
	Keys           []keyEvent     `json:"keys"`
	QemuKeys       []keyEvent     `json:"qemu_keys"`
	Pointer        []pointerEvent `json:"pointer"`
	Requests       int            `json:"update_requests"`
	FullRequests   int            `json:"full_update_requests"`
	Unparsed       string         `json:"unparsed,omitempty"`
	Subprotocols   []string       `json:"subprotocols"`
	Path           string         `json:"path"`
	ClientClosed   bool           `json:"client_closed"`
	ConnectedAt    time.Time      `json:"connected_at"`
	DisconnectedAt time.Time      `json:"disconnected_at"`
}

type consoleServer struct {
	mu       sync.Mutex
	tickets  map[string]*ticket
	last     *rfbStats
	probes   int
	requests int // GET /ticket/… however they end
	accepted int
}

func newConsoleServer() *consoleServer {
	return &consoleServer{tickets: map[string]*ticket{}}
}

func newTicketID() string {
	b := make([]byte, 16)
	if _, err := rand.Read(b); err != nil {
		panic(err)
	}
	return hex.EncodeToString(b)
}

// issue registers a ticket for the VM's console.
func (s *consoleServer) issue(vm, vnc string, closeAfter time.Duration) string {
	id := newTicketID()
	s.mu.Lock()
	s.tickets[id] = &ticket{vm: vm, vnc: vnc, closeAfter: closeAfter}
	s.mu.Unlock()
	return id
}

func (s *consoleServer) take(id string) *ticket {
	s.mu.Lock()
	defer s.mu.Unlock()
	t := s.tickets[id]
	if t == nil || t.used {
		return nil
	}
	t.used = true
	return t
}

// probe answers anything that isn't a ticket, like ESXi's welcome page.
func (s *consoleServer) probe(w http.ResponseWriter, r *http.Request) {
	s.mu.Lock()
	s.probes++
	s.mu.Unlock()
	w.Header().Set("Content-Type", "text/plain")
	w.WriteHeader(http.StatusOK)
	if r.Method != http.MethodHead {
		io.WriteString(w, "fake ESXi\n")
	}
}

func (s *consoleServer) serveTicket(w http.ResponseWriter, r *http.Request) {
	s.mu.Lock()
	s.requests++
	s.mu.Unlock()
	id := strings.TrimPrefix(r.URL.Path, "/ticket/")
	t := s.take(id)
	if t == nil {
		log.Printf("console: unknown or used ticket %q", id)
		http.Error(w, "Not Found", http.StatusNotFound)
		return
	}
	protocols := offeredProtocols(r)
	if !slices.Contains(protocols, "binary") {
		log.Printf("console: subprotocols %v", protocols)
		http.Error(w, "the console speaks the binary subprotocol", http.StatusBadRequest)
		return
	}
	vnc, err := net.Dial("tcp", t.vnc)
	if err != nil {
		log.Printf("console: %v", err)
		http.Error(w, "the VM's console isn't available", http.StatusServiceUnavailable)
		return
	}
	ws, err := upgrade(w, r, "binary")
	if err != nil {
		log.Printf("console: %v", err)
		http.Error(w, err.Error(), http.StatusBadRequest)
		vnc.Close()
		return
	}
	stats := &rfbStats{Subprotocols: protocols, Path: r.URL.Path, ConnectedAt: time.Now()}
	s.mu.Lock()
	s.accepted++
	s.last = stats
	s.mu.Unlock()
	log.Printf("console: %s connected (%s)", t.vm, t.vnc)
	go s.bridge(ws, vnc, stats, t)
}

func (s *consoleServer) bridge(ws *wsConn, vnc net.Conn, stats *rfbStats, t *ticket) {
	var once sync.Once
	done := make(chan struct{})
	finish := func() { once.Do(func() { close(done) }) }
	// Server to client: each read as one binary message.
	go func() {
		defer finish()
		buf := make([]byte, 64*1024)
		for {
			n, err := vnc.Read(buf)
			if n > 0 {
				if werr := ws.WriteMessage(buf[:n]); werr != nil {
					return
				}
			}
			if err != nil {
				return
			}
		}
	}()
	// Client to server, read as RFB on the way.
	go func() {
		defer finish()
		parser := &rfbParser{stats: stats, lock: &s.mu}
		buf := make([]byte, 64*1024)
		for {
			n, err := ws.Read(buf)
			if n > 0 {
				parser.feed(buf[:n])
				if _, werr := vnc.Write(buf[:n]); werr != nil {
					return
				}
			}
			if err != nil {
				if errors.Is(err, errClosed) {
					s.mu.Lock()
					stats.ClientClosed = true
					s.mu.Unlock()
				}
				return
			}
		}
	}()
	var timer <-chan time.Time
	if t.closeAfter > 0 {
		timer = time.After(t.closeAfter)
	}
	select {
	case <-done:
		ws.conn.Close()
	case <-timer:
		log.Printf("console: closing %s", t.vm)
		ws.Close(1000, "The virtual machine was powered off")
	}
	vnc.Close()
	s.mu.Lock()
	stats.DisconnectedAt = time.Now()
	s.mu.Unlock()
	log.Printf("console: %s disconnected", t.vm)
}

// rfbParser follows the client's side of an RFB 3.x stream.
type rfbParser struct {
	stats *rfbStats
	lock  *sync.Mutex
	buf   []byte
	phase int // 0 version, 1 security type, 2 ClientInit, 3 messages, 4 lost
	minor int
}

func (p *rfbParser) feed(data []byte) {
	p.lock.Lock()
	defer p.lock.Unlock()
	p.buf = append(p.buf, data...)
	for p.step() {
	}
}

// step consumes one complete item; false when more bytes are needed.
func (p *rfbParser) step() bool {
	b := p.buf
	need := func(n int) bool { return len(b) >= n }
	consume := func(n int) bool { p.buf = p.buf[n:]; return true }
	switch p.phase {
	case 0:
		if !need(12) {
			return false
		}
		p.stats.Version = strings.TrimSpace(string(b[:12]))
		p.minor = int(b[10]-'0')*10 + int(b[11]-'0')
		p.phase = 1
		if p.minor < 7 {
			p.phase = 2
		}
		return consume(12)
	case 1:
		if !need(1) {
			return false
		}
		p.phase = 2
		return consume(1)
	case 2:
		if !need(1) {
			return false
		}
		p.phase = 3
		return consume(1)
	case 3:
		if !need(1) {
			return false
		}
		switch b[0] {
		case 0: // SetPixelFormat
			if !need(20) {
				return false
			}
			p.stats.PixelFormat = append([]byte(nil), b[4:20]...)
			return consume(20)
		case 2: // SetEncodings
			if !need(4) {
				return false
			}
			n := int(binary.BigEndian.Uint16(b[2:4]))
			if !need(4 + 4*n) {
				return false
			}
			p.stats.Encodings = p.stats.Encodings[:0]
			for i := 0; i < n; i++ {
				p.stats.Encodings = append(p.stats.Encodings, int32(binary.BigEndian.Uint32(b[4+4*i:])))
			}
			return consume(4 + 4*n)
		case 3: // FramebufferUpdateRequest
			if !need(10) {
				return false
			}
			p.stats.Requests++
			if b[1] == 0 {
				p.stats.FullRequests++
			}
			return consume(10)
		case 4: // KeyEvent
			if !need(8) {
				return false
			}
			p.stats.Keys = append(p.stats.Keys, keyEvent{Keysym: binary.BigEndian.Uint32(b[4:8]), Down: b[1] != 0})
			return consume(8)
		case 5: // PointerEvent
			if !need(6) {
				return false
			}
			p.stats.Pointer = append(p.stats.Pointer, pointerEvent{
				Buttons: b[1], X: binary.BigEndian.Uint16(b[2:4]), Y: binary.BigEndian.Uint16(b[4:6])})
			return consume(6)
		case 6: // ClientCutText
			if !need(8) {
				return false
			}
			n := int(int32(binary.BigEndian.Uint32(b[4:8])))
			if n < 0 {
				n = -n
			}
			if !need(8 + n) {
				return false
			}
			return consume(8 + n)
		case 255: // QEMU
			if !need(2) {
				return false
			}
			if b[1] != 0 {
				break
			}
			if !need(12) {
				return false
			}
			p.stats.QemuKeys = append(p.stats.QemuKeys, keyEvent{
				Down:    binary.BigEndian.Uint16(b[2:4]) != 0,
				Keysym:  binary.BigEndian.Uint32(b[4:8]),
				Keycode: binary.BigEndian.Uint32(b[8:12])})
			return consume(12)
		}
		p.stats.Unparsed = hex.EncodeToString(b[:min(len(b), 16)])
		p.phase = 4
		return false
	}
	return false
}
