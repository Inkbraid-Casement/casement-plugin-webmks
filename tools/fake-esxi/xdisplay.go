package main

// The X display behind the VNC servers, through the usual X tools: xsetroot
// and ImageMagick's display for the picture, xsetroot for the cursor, xev on
// the root window for the input that arrives, xdotool for the pointer, an
// xterm for a window with another cursor, x11vnc's remote control for a new
// screen size.

import (
	"bufio"
	"bytes"
	"context"
	"errors"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"log"
	"os"
	"os/exec"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"
)

type xEvent struct {
	Type    string `json:"type"`
	Keycode int    `json:"keycode,omitempty"`
	Keysym  string `json:"keysym,omitempty"`
	Name    string `json:"name,omitempty"`
	Button  int    `json:"button,omitempty"`
	X       int    `json:"x"`
	Y       int    `json:"y"`
	State   string `json:"state,omitempty"`
}

type xDisplay struct {
	display string
	xev     *exec.Cmd
	mu      sync.Mutex
	list    []xEvent
	wmu     sync.Mutex
	window  *exec.Cmd
}

var (
	eventStart = regexp.MustCompile(`^(\w+) event, serial`)
	keyInfo    = regexp.MustCompile(`keycode (\d+) \(keysym (0x[0-9a-f]+), ([^)]*)\)`)
	buttonInfo = regexp.MustCompile(`button (\d+),`)
	rootInfo   = regexp.MustCompile(`root:\((-?\d+),(-?\d+)\)`)
	stateInfo  = regexp.MustCompile(`state (0x[0-9a-f]+)`)
)

func newXDisplay(display string) *xDisplay {
	x := &xDisplay{display: display}
	// Line-buffered, so events arrive as they happen.
	x.xev = exec.Command("stdbuf", "-oL", "xev", "-display", display, "-root", "-event", "keyboard", "-event", "mouse")
	out, err := x.xev.StdoutPipe()
	if err != nil {
		log.Printf("xev: %v", err)
		return x
	}
	x.xev.Stderr = os.Stderr
	if err := x.xev.Start(); err != nil {
		log.Printf("xev: %v", err)
		return x
	}
	go x.read(bufio.NewScanner(out))
	return x
}

// read parses xev's output. xev starts each event with a blank line, so an
// event is complete with its "state" line, which carries the key or button.
func (x *xDisplay) read(s *bufio.Scanner) {
	var cur *xEvent
	for s.Scan() {
		line := s.Text()
		if m := eventStart.FindStringSubmatch(line); m != nil {
			cur = &xEvent{Type: m[1]}
			continue
		}
		if cur == nil {
			continue
		}
		if m := rootInfo.FindStringSubmatch(line); m != nil {
			cur.X, _ = strconv.Atoi(m[1])
			cur.Y, _ = strconv.Atoi(m[2])
		}
		m := stateInfo.FindStringSubmatch(line)
		if m == nil {
			continue
		}
		cur.State = m[1]
		if k := keyInfo.FindStringSubmatch(line); k != nil {
			cur.Keycode, _ = strconv.Atoi(k[1])
			cur.Keysym = k[2]
			cur.Name = k[3]
		}
		if b := buttonInfo.FindStringSubmatch(line); b != nil && strings.HasPrefix(cur.Type, "Button") {
			cur.Button, _ = strconv.Atoi(b[1])
		}
		x.mu.Lock()
		if len(x.list) < 100000 {
			x.list = append(x.list, *cur)
		}
		x.mu.Unlock()
		cur = nil
	}
}

func (x *xDisplay) events() []xEvent {
	x.mu.Lock()
	defer x.mu.Unlock()
	return append([]xEvent{}, x.list...)
}

func (x *xDisplay) resetEvents() {
	x.mu.Lock()
	x.list = nil
	x.mu.Unlock()
}

func (x *xDisplay) run(name string, args ...string) (string, error) {
	cmd := exec.Command(name, args...)
	cmd.Env = append(os.Environ(), "DISPLAY="+x.display)
	out, err := cmd.CombinedOutput()
	if err != nil {
		return "", fmt.Errorf("%s %s: %v: %s", name, strings.Join(args, " "), err, out)
	}
	return string(out), nil
}

var hexColour = regexp.MustCompile(`^[0-9a-fA-F]{6}$`)

func (x *xDisplay) setRoot(colour string) error {
	if !hexColour.MatchString(colour) {
		return fmt.Errorf("colour %q", colour)
	}
	_, err := x.run("xsetroot", "-solid", "#"+colour)
	return err
}

// setGradient fills the root window with a picture of many colours (red
// growing to the right, green downwards), which VNC servers send as JPEG.
func (x *xDisplay) setGradient() error {
	const w, h = 1024, 768
	img := image.NewRGBA(image.Rect(0, 0, w, h))
	for y := 0; y < h; y++ {
		for px := 0; px < w; px++ {
			img.Set(px, y, color.RGBA{R: uint8(px * 255 / (w - 1)), G: uint8(y * 255 / (h - 1)), B: 128, A: 255})
		}
	}
	f, err := os.CreateTemp("", "fake-esxi-*.png")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	if err := png.Encode(f, img); err != nil {
		f.Close()
		return err
	}
	f.Close()
	cmd := exec.Command("display", "-window", "root", f.Name())
	cmd.Env = append(os.Environ(), "DISPLAY="+x.display)
	out, err := cmd.CombinedOutput()
	// ImageMagick 6's display exits with 1 after setting the root window
	// all the same; when something went wrong, it says what.
	var exit *exec.ExitError
	if errors.As(err, &exit) && exit.ExitCode() == 1 && len(bytes.TrimSpace(out)) == 0 {
		err = nil
	}
	if err != nil {
		return fmt.Errorf("display -window root: %v: %s", err, out)
	}
	return nil
}

func (x *xDisplay) setCursor(name string) error {
	_, err := x.run("xsetroot", "-cursor_name", name)
	return err
}

func (x *xDisplay) pointer() (int, int, error) {
	out, err := x.run("xdotool", "getmouselocation", "--shell")
	if err != nil {
		return 0, 0, err
	}
	var px, py int
	for _, line := range strings.Split(out, "\n") {
		if v, ok := strings.CutPrefix(line, "X="); ok {
			px, _ = strconv.Atoi(v)
		}
		if v, ok := strings.CutPrefix(line, "Y="); ok {
			py, _ = strconv.Atoi(v)
		}
	}
	return px, py, nil
}

// openWindow puts an xterm at the top left: a window with a cursor of its
// own, which x11vnc shows when the pointer is over it.
func (x *xDisplay) openWindow() error {
	x.wmu.Lock()
	defer x.wmu.Unlock()
	if x.window != nil {
		return nil
	}
	cmd := exec.Command("xterm", "-display", x.display, "-geometry", "40x10+0+0")
	if err := cmd.Start(); err != nil {
		return err
	}
	x.window = cmd
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	sync := exec.CommandContext(ctx, "xdotool", "search", "--sync", "--onlyvisible", "--pid",
		strconv.Itoa(cmd.Process.Pid))
	sync.Env = append(os.Environ(), "DISPLAY="+x.display)
	if out, err := sync.CombinedOutput(); err != nil {
		return fmt.Errorf("xterm didn't appear: %v: %s", err, out)
	}
	return nil
}

func (x *xDisplay) closeWindow() {
	x.wmu.Lock()
	defer x.wmu.Unlock()
	if x.window != nil {
		_ = x.window.Process.Kill()
		_ = x.window.Wait()
		x.window = nil
	}
}

func (x *xDisplay) movePointer(px, py string) error {
	if _, err := strconv.Atoi(px); err != nil {
		return fmt.Errorf("x %q", px)
	}
	if _, err := strconv.Atoi(py); err != nil {
		return fmt.Errorf("y %q", py)
	}
	_, err := x.run("xdotool", "mousemove", px, py)
	return err
}

func (x *xDisplay) scale(value string) error {
	_, err := x.run("x11vnc", "-display", x.display, "-sync", "-R", "scale:"+value)
	return err
}

func (x *xDisplay) close() {
	x.closeWindow()
	if x.xev != nil && x.xev.Process != nil {
		_ = x.xev.Process.Kill()
	}
}
