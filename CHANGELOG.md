# Changelog

## 0.1.0 (2026-10-23)

- `vmware://` connections to VM consoles on ESXi and vCenter Server through
  the vSphere Web Services API (SOAP): login, VM by inventory path, managed
  object id or name, power state check, `webmks` ticket, logout when the tab
  ends or fails.
- Certificates: Windows' trust or a pinned SHA-256 for the API, with the
  fingerprint in the failure message; the ticket's SHA-1, SHA-256 list or
  certificate (vSphere 9) for the console host. Passwords, session cookies
  and tickets only go to servers that passed.
- The console as a WebSocket with the `binary` subprotocol, decoded by
  LibVNCClient 0.9.15: Raw, CopyRect, Hextile, Zlib, ZRLE and Tight with
  JPEG; screen size changes; one update request in flight.
- Keys as XT scancodes when the server takes QEMU extended key events, as
  US-layout keysyms otherwise (`keys=keysyms` forces them); pointer, wheel,
  cursor shape and position.
- `failed` with a sentence for unreachable hosts, untrusted certificates,
  missing, duplicate or stopped VMs, missing privileges, refused tickets and
  servers without console tickets; `closed` with the server's reason.
- Tests: unit tests, and end-to-end tests under Wine against govmomi's
  simulator with consoles in front of x11vnc and TigerVNC (tools/fake-esxi)
  and against plain vcsim.
