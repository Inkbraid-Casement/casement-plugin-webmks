package main

// The control endpoint for the tests (plain HTTP, JSON answers):
//
//	GET  /info              addresses, certificate fingerprints, the CA, VM ids
//	GET  /stats             logins, tickets, console requests and the last
//	                        console connection's client messages
//	POST /stats/reset       (the last connection keeps going, its counts restart)
//	GET  /sessions          open API sessions of the test users
//	POST /x/root?color=RRGGBB   the X display's background colour
//	POST /x/gradient        a picture of many colours instead (JPEG for Tight)
//	POST /x/cursor?name=watch   the root window's cursor
//	GET  /x/events          X input events since the last reset
//	POST /x/events/reset
//	GET  /x/pointer         where the X pointer is
//	POST /x/pointer?x=10&y=20   moves it, as a guest moving its pointer would
//	POST /x/window          an xterm at the top left, 40x10 characters
//	POST /x/window/close
//	POST /vnc/scale?value=2/3   x11vnc's scaling: a new screen size for clients

import (
	"encoding/json"
	"net/http"
)

func writeJSON(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(v)
}

func fail(w http.ResponseWriter, err error) {
	http.Error(w, err.Error(), http.StatusInternalServerError)
}

func (h *fakeHost) controlMux() *http.ServeMux {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /info", func(w http.ResponseWriter, r *http.Request) { writeJSON(w, h.info()) })
	mux.HandleFunc("GET /stats", func(w http.ResponseWriter, r *http.Request) {
		h.mu.Lock()
		counts := map[string]int{}
		for k, v := range h.counts {
			counts[k] = v
		}
		h.mu.Unlock()
		c := h.console
		c.mu.Lock()
		defer c.mu.Unlock()
		writeJSON(w, map[string]any{
			"counts":          counts,
			"probes":          c.probes,
			"ticket_requests": c.requests,
			"consoles":        c.accepted,
			"last":            c.last,
		})
	})
	mux.HandleFunc("POST /stats/reset", func(w http.ResponseWriter, r *http.Request) {
		h.mu.Lock()
		h.counts = map[string]int{}
		h.mu.Unlock()
		c := h.console
		c.mu.Lock()
		c.probes, c.requests, c.accepted = 0, 0, 0
		// The connection goes on: only what it has counted so far.
		if l := c.last; l != nil {
			l.Keys, l.QemuKeys, l.Pointer, l.Requests, l.FullRequests = nil, nil, nil, 0, 0
		}
		c.mu.Unlock()
		writeJSON(w, map[string]any{})
	})
	mux.HandleFunc("GET /sessions", func(w http.ResponseWriter, r *http.Request) {
		n, err := h.sessions()
		if err != nil {
			fail(w, err)
			return
		}
		writeJSON(w, map[string]any{"sessions": n})
	})
	mux.HandleFunc("POST /x/root", func(w http.ResponseWriter, r *http.Request) {
		if err := h.x.setRoot(r.URL.Query().Get("color")); err != nil {
			fail(w, err)
			return
		}
		writeJSON(w, map[string]any{})
	})
	mux.HandleFunc("POST /x/gradient", func(w http.ResponseWriter, r *http.Request) {
		if err := h.x.setGradient(); err != nil {
			fail(w, err)
			return
		}
		writeJSON(w, map[string]any{})
	})
	mux.HandleFunc("POST /x/cursor", func(w http.ResponseWriter, r *http.Request) {
		if err := h.x.setCursor(r.URL.Query().Get("name")); err != nil {
			fail(w, err)
			return
		}
		writeJSON(w, map[string]any{})
	})
	mux.HandleFunc("GET /x/events", func(w http.ResponseWriter, r *http.Request) { writeJSON(w, h.x.events()) })
	mux.HandleFunc("POST /x/events/reset", func(w http.ResponseWriter, r *http.Request) {
		h.x.resetEvents()
		writeJSON(w, map[string]any{})
	})
	mux.HandleFunc("GET /x/pointer", func(w http.ResponseWriter, r *http.Request) {
		x, y, err := h.x.pointer()
		if err != nil {
			fail(w, err)
			return
		}
		writeJSON(w, map[string]any{"x": x, "y": y})
	})
	mux.HandleFunc("POST /x/pointer", func(w http.ResponseWriter, r *http.Request) {
		if err := h.x.movePointer(r.URL.Query().Get("x"), r.URL.Query().Get("y")); err != nil {
			fail(w, err)
			return
		}
		writeJSON(w, map[string]any{})
	})
	mux.HandleFunc("POST /x/window", func(w http.ResponseWriter, r *http.Request) {
		if err := h.x.openWindow(); err != nil {
			fail(w, err)
			return
		}
		writeJSON(w, map[string]any{})
	})
	mux.HandleFunc("POST /x/window/close", func(w http.ResponseWriter, r *http.Request) {
		h.x.closeWindow()
		writeJSON(w, map[string]any{})
	})
	mux.HandleFunc("POST /vnc/scale", func(w http.ResponseWriter, r *http.Request) {
		if err := h.x.scale(r.URL.Query().Get("value")); err != nil {
			fail(w, err)
			return
		}
		writeJSON(w, map[string]any{})
	})
	return mux
}
