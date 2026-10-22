// fake-esxi: a vSphere API with VM consoles, for the plugin's end-to-end
// tests.
//
// The API is govmomi's simulator (vcsim) with a vCenter inventory. Its VMs
// answer AcquireTicket("webmks"), which vcsim's don't, with tickets for a
// console endpoint that bridges the WebSocket to a real VNC server, the way
// ESXi does. A plain-HTTP control endpoint lets the tests change the VNC
// server's X display and see what reached it.
//
//	fake-esxi -api 127.0.0.1:10501 -console 127.0.0.2:10502 -control 127.0.0.1:10503 \
//	    -vnc 127.0.0.1:10521 -vnc-qemu 127.0.0.1:10522 -display :21
package main

import (
	"context"
	"crypto/tls"
	"encoding/base64"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/signal"
	"sync"
	"syscall"

	"github.com/vmware/govmomi"
	"github.com/vmware/govmomi/find"
	"github.com/vmware/govmomi/simulator"
	"github.com/vmware/govmomi/vim25/mo"
	"github.com/vmware/govmomi/vim25/types"
)

const noConsoleUser = "noconsole"

type fakeHost struct {
	apiAddr     string
	consoleAddr string
	vnc         string
	vncQemu     string
	display     string
	user        string
	password    string
	apiCert     keyPair
	consoleCert keyPair
	caDER       []byte
	console     *consoleServer
	model       *simulator.Model
	client      *govmomi.Client
	x           *xDisplay

	mu     sync.Mutex
	counts map[string]int
	vmIDs  map[string]string
}

func (h *fakeHost) count(what string) {
	h.mu.Lock()
	h.counts[what]++
	h.mu.Unlock()
}

func main() {
	h := &fakeHost{counts: map[string]int{}, vmIDs: map[string]string{}, console: newConsoleServer()}
	flag.StringVar(&h.apiAddr, "api", "127.0.0.1:10501", "address of the vSphere API (HTTPS)")
	flag.StringVar(&h.consoleAddr, "console", "127.0.0.2:10502", "address of the console host (HTTPS)")
	control := flag.String("control", "127.0.0.1:10503", "address of the control endpoint (HTTP)")
	flag.StringVar(&h.vnc, "vnc", "127.0.0.1:10521", "the VNC server behind the consoles")
	flag.StringVar(&h.vncQemu, "vnc-qemu", "127.0.0.1:10522",
		"the VNC server for the VM called qemu (one that takes QEMU extended key events)")
	flag.StringVar(&h.display, "display", ":21", "the VNC servers' X display")
	flag.StringVar(&h.user, "user", "user", "user name")
	flag.StringVar(&h.password, "password", "pass", "password (also for the user "+noConsoleUser+")")
	trace := flag.String("trace", "", "write the SOAP requests and responses to this file")
	flag.Parse()

	if *trace != "" {
		f, err := os.Create(*trace)
		if err != nil {
			log.Fatal(err)
		}
		simulator.TraceFile = f
		simulator.Trace = true
	}

	ca, caKey, caDER := newCA()
	h.caDER = caDER
	apiHost, _, _ := net.SplitHostPort(h.apiAddr)
	consoleHost, _, _ := net.SplitHostPort(h.consoleAddr)
	h.apiCert = newServerCert("localhost", []string{apiHost}, ca, caKey)
	h.consoleCert = newServerCert("esx01.test", []string{consoleHost}, nil, nil)

	mux := http.NewServeMux()
	mux.HandleFunc("/ticket/", h.console.serveTicket)
	mux.HandleFunc("/{$}", h.console.probe)

	h.model = simulator.VPX()
	h.model.Machine = 7
	h.model.ServiceContent.About.ApiVersion = "8.0.3.0"
	h.model.ServiceContent.About.Version = "8.0.3"
	h.model.ServiceContent.About.FullName = "VMware vCenter Server 8.0.3 (fake-esxi, govmomi simulator)"
	if err := h.model.Create(); err != nil {
		log.Fatal(err)
	}
	h.model.Service.Listen = &url.URL{Host: h.apiAddr, User: url.UserPassword(h.user, h.password)}
	h.model.Service.TLS = &tls.Config{Certificates: []tls.Certificate{h.apiCert.tls}}
	h.model.Service.ServeMux = mux
	sm := h.model.Map().SessionManager()
	sm.ValidLogin = func(req *types.Login) bool {
		ok := (req.UserName == h.user || req.UserName == noConsoleUser || req.UserName == setupUser) &&
			req.Password == h.password
		if req.UserName != setupUser {
			h.count("logins")
			if ok {
				h.count("good_logins")
			}
		}
		return ok
	}
	server := h.model.Service.NewServer()
	defer server.Close()

	for _, e := range h.model.Map().All("VirtualMachine") {
		h.model.Map().Put(&ticketVM{VirtualMachine: e.(*simulator.VirtualMachine), host: h})
	}
	if err := h.setup(); err != nil {
		log.Fatal(err)
	}

	consoleListener, err := tls.Listen("tcp", h.consoleAddr, &tls.Config{Certificates: []tls.Certificate{h.consoleCert.tls}})
	if err != nil {
		log.Fatal(err)
	}
	consoleMux := http.NewServeMux()
	consoleMux.HandleFunc("/ticket/", h.console.serveTicket)
	consoleMux.HandleFunc("/", h.console.probe)
	go func() { log.Fatal(http.Serve(consoleListener, consoleMux)) }()

	h.x = newXDisplay(h.display)
	controlListener, err := net.Listen("tcp", *control)
	if err != nil {
		log.Fatal(err)
	}
	go func() { log.Fatal(http.Serve(controlListener, h.controlMux())) }()

	log.Printf("API https://%s/sdk (SHA-256 %s), console %s (SHA-256 %s), control http://%s",
		h.apiAddr, fingerprint(h.apiCert.der, "sha256"), h.consoleAddr, fingerprint(h.consoleCert.der, "sha256"),
		*control)
	fmt.Println("ready")

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	<-sig
	h.x.close()
}

const setupUser = "fake-esxi-setup"

// setup names the VMs after the test cases, powers one off and gives two
// the same name, through the API like a vSphere administrator would.
func (h *fakeHost) setup() error {
	ctx := context.Background()
	u := &url.URL{Scheme: "https", Host: h.apiAddr, Path: "/sdk", User: url.UserPassword(setupUser, h.password)}
	c, err := govmomi.NewClient(ctx, u, true)
	if err != nil {
		return err
	}
	h.client = c
	finder := find.NewFinder(c.Client)
	dc, err := finder.Datacenter(ctx, "DC0")
	if err != nil {
		return err
	}
	finder.SetDatacenter(dc)
	names := map[string]string{
		"DC0_H0_VM0":     "web-01",
		"DC0_H0_VM1":     "web-02",
		"DC0_H0_VM2":     "esx-direct",
		"DC0_H0_VM3":     "esx-direct-nothumb",
		"DC0_H0_VM4":     "wrong-thumbprint",
		"DC0_H0_VM5":     "no-thumbprint",
		"DC0_H0_VM6":     "refused",
		"DC0_C0_RP0_VM0": "closing",
		"DC0_C0_RP0_VM1": "qemu",
		"DC0_C0_RP0_VM2": "off",
		"DC0_C0_RP0_VM3": "dup",
		"DC0_C0_RP0_VM4": "dup",
		"DC0_C0_RP0_VM5": "web-03",
		// vSphere keeps '%', '/' and '\' in names escaped.
		"DC0_C0_RP0_VM6": "100%25",
	}
	folders, err := dc.Folders(ctx)
	if err != nil {
		return err
	}
	team, err := folders.VmFolder.CreateFolder(ctx, "team")
	if err != nil {
		return err
	}
	for old, name := range names {
		vm, err := finder.VirtualMachine(ctx, old)
		if err != nil {
			return err
		}
		if old == "DC0_C0_RP0_VM4" {
			task, err := team.MoveInto(ctx, []types.ManagedObjectReference{vm.Reference()})
			if err != nil {
				return err
			}
			if err := task.Wait(ctx); err != nil {
				return err
			}
		}
		task, err := vm.Rename(ctx, name)
		if err != nil {
			return err
		}
		if err := task.Wait(ctx); err != nil {
			return fmt.Errorf("renaming %s to %s: %w", old, name, err)
		}
		if name == "off" {
			task, err := vm.PowerOff(ctx)
			if err != nil {
				return err
			}
			if err := task.Wait(ctx); err != nil {
				return err
			}
		}
		key := name
		if old == "DC0_C0_RP0_VM4" {
			key = "team/dup"
		}
		h.vmIDs[key] = vm.Reference().Value
	}
	return nil
}

// sessions counts the API sessions of the test users.
func (h *fakeHost) sessions() (int, error) {
	ctx := context.Background()
	var sm mo.SessionManager
	pc := h.client.PropertyCollector()
	if err := pc.RetrieveOne(ctx, *h.client.ServiceContent.SessionManager, []string{"sessionList"}, &sm); err != nil {
		return 0, err
	}
	n := 0
	for _, s := range sm.SessionList {
		if s.UserName != setupUser {
			n++
		}
	}
	return n, nil
}

func (h *fakeHost) info() map[string]any {
	h.mu.Lock()
	defer h.mu.Unlock()
	ids := map[string]string{}
	for k, v := range h.vmIDs {
		ids[k] = v
	}
	return map[string]any{
		"api":            h.apiAddr,
		"console":        h.consoleAddr,
		"api_sha256":     fingerprint(h.apiCert.der, "sha256"),
		"console_sha256": fingerprint(h.consoleCert.der, "sha256"),
		"ca":             base64.StdEncoding.EncodeToString(h.caDER),
		"vms":            ids,
	}
}
