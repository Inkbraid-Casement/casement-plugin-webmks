package main

// VMs that answer AcquireTicket("webmks"), which vcsim's don't. What the
// ticket says depends on the VM's name, so each test picks its case by the
// VM it opens.

import (
	"encoding/pem"
	"log"
	"net"
	"strconv"
	"time"

	"github.com/vmware/govmomi/simulator"
	"github.com/vmware/govmomi/vim25/methods"
	"github.com/vmware/govmomi/vim25/soap"
	"github.com/vmware/govmomi/vim25/types"
)

type ticketVM struct {
	*simulator.VirtualMachine
	host *fakeHost
}

func (vm *ticketVM) AcquireTicket(ctx *simulator.Context, req *types.AcquireTicket) soap.HasFault {
	h := vm.host
	body := &methods.AcquireTicketBody{}
	h.count("tickets")
	if ctx.Session != nil && ctx.Session.UserName == noConsoleUser {
		self := vm.Self
		body.Fault_ = simulator.Fault("Permission to perform this operation was denied.", &types.NoPermission{
			Object:      &self,
			PrivilegeId: "VirtualMachine.Interact.ConsoleInteract",
		})
		return body
	}
	if req.TicketType != "webmks" {
		body.Fault_ = simulator.Fault("", &types.InvalidArgument{InvalidProperty: "ticketType"})
		return body
	}
	if vm.Runtime.PowerState != types.VirtualMachinePowerStatePoweredOn {
		body.Fault_ = simulator.Fault("The attempted operation cannot be performed in the current state (Powered off).",
			&types.InvalidPowerState{
				RequestedState: types.VirtualMachinePowerStatePoweredOn,
				ExistingState:  vm.Runtime.PowerState,
			})
		return body
	}

	console := h.console
	consoleHost, consolePortText, _ := net.SplitHostPort(h.consoleAddr)
	consolePort, _ := strconv.Atoi(consolePortText)
	_, apiPortText, _ := net.SplitHostPort(h.apiAddr)
	apiPort, _ := strconv.Atoi(apiPortText)
	vnc := h.vnc
	var closeAfter time.Duration
	name := vm.Name
	switch name {
	case "qemu":
		vnc = h.vncQemu
	case "closing":
		closeAfter = 3 * time.Second
	}
	t := types.VirtualMachineTicket{CfgFile: "[datastore1] " + name + "/" + name + ".vmx"}
	consoleCert := []types.VirtualMachineCertThumbprint{{Thumbprint: fingerprint(h.consoleCert.der, "sha256"),
		HashAlgorithm: "sha256"}}
	switch name {
	case "web-02":
		// No URL, no SHA-1: host, port and the SHA-256 list.
		t.Ticket = console.issue(name, vnc, closeAfter)
		t.Host = consoleHost
		t.Port = int32(consolePort)
		t.CertThumbprintList = consoleCert
	case "web-03":
		// vSphere 9: the certificate itself instead of a thumbprint.
		t.Ticket = console.issue(name, vnc, closeAfter)
		t.Host = consoleHost
		t.Port = int32(consolePort)
		t.SslCertificate = string(pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: h.consoleCert.der}))
	case "esx-direct", "esx-direct-nothumb":
		// The console of the host the API runs on: ESXi itself.
		t.Ticket = console.issue(name, vnc, closeAfter)
		t.Port = int32(apiPort)
		if name == "esx-direct" {
			t.SslThumbprint = fingerprint(h.apiCert.der, "sha1")
		}
	case "wrong-thumbprint":
		t.Ticket = console.issue(name, vnc, closeAfter)
		t.Host = consoleHost
		t.Port = int32(consolePort)
		t.SslThumbprint = fingerprint(h.apiCert.der, "sha1")
		t.Url = "wss://" + h.consoleAddr + "/ticket/" + t.Ticket
	case "no-thumbprint":
		t.Ticket = console.issue(name, vnc, closeAfter)
		t.Host = consoleHost
		t.Port = int32(consolePort)
		t.Url = "wss://" + h.consoleAddr + "/ticket/" + t.Ticket
	case "refused":
		// A ticket the console doesn't know.
		t.Ticket = newTicketID()
		t.Host = consoleHost
		t.Port = int32(consolePort)
		t.SslThumbprint = fingerprint(h.consoleCert.der, "sha1")
		t.Url = "wss://" + h.consoleAddr + "/ticket/" + t.Ticket
	default:
		// As vCenter 8 issues them: URL, SHA-1 and the list.
		t.Ticket = console.issue(name, vnc, closeAfter)
		t.Host = consoleHost
		t.Port = int32(consolePort)
		t.SslThumbprint = fingerprint(h.consoleCert.der, "sha1")
		t.CertThumbprintList = consoleCert
		t.Url = "wss://" + h.consoleAddr + "/ticket/" + t.Ticket
	}
	log.Printf("ticket for %s: host %q port %d url %q", name, t.Host, t.Port, t.Url)
	body.Res = &types.AcquireTicketResponse{Returnval: t}
	return body
}
