# VMware console support for Casement

`casement-vmware.exe` is a Casement protocol plugin that opens the console
of a virtual machine on VMware ESXi or vCenter Server in a Casement tab: the
VM's screen, keyboard and mouse, as the vSphere Client's web console shows
them (WebMKS), independent of the guest's own network and remote desktop
services. It is meant for the moments those don't work: installing a guest,
a boot problem, a broken network configuration.

This is an independent project. It is not made or endorsed by Broadcom or
VMware; "VMware" names the software it works with.

## Status

| | |
|---|---|
| vSphere API (SOAP) on ESXi and vCenter: login, VM by inventory path, managed object id or name, power state, `webmks` ticket, logout | done |
| Certificates: Windows' trust or a pinned SHA-256 for the API; the ticket's fingerprints (SHA-1, SHA-256 list, or the vSphere 9 certificate) for the console host | done |
| Console over WebSocket (`wss://…/ticket/…`, subprotocol `binary`), RFB through LibVNCClient: Raw, CopyRect, Hextile, Zlib, ZRLE, Tight with JPEG | done |
| Screen size changes (DesktopSize, ExtendedDesktopSize), one update request in flight | done |
| Keyboard: XT scancodes when the server takes QEMU extended key events, US keysyms otherwise; pointer, wheel; cursor shape and position | done |
| VMware's own pseudo-encodings (cursor, cursor state, LED state, display mode) and messages 127 and 254 | not yet: their layout is unpublished and couldn't be checked against ESXi |
| Reconnecting with a fresh ticket while the API session is still valid | not yet |
| Tested against a real ESXi host or vCenter Server | not yet: against govmomi's simulator and real VNC servers (see [Testing](#testing)) |
| Accepting an unknown certificate with a question instead of the fingerprint field | needs protocol v2 (P2, P3) |
| Relative pointer for guests without VMware Tools or a USB tablet | needs protocol v2 (P5) |
| Typing the clipboard into the VM | needs protocol v2 (P12) |

See [CHANGELOG.md](CHANGELOG.md).

## Installing

Download the zip for the latest release from this repository's
[Releases](https://github.com/Inkbraid/casement-plugin-webmks/releases) and
unpack it so that you get

```
%LOCALAPPDATA%\Casement\plugins\
    vmware\
        plugin.json
        icon.png
        casement-vmware.exe
```

then restart Casement. The plugin's folder is `vmware` (its id), although
the repository is called casement-plugin-webmks. Each release also carries
the matching source code (the tag and GitHub's source archives).

## Connections

New connection → **VMware VM console**:

| Field | Address part | |
|---|---|---|
| User | user | A vSphere user, for example `administrator@vsphere.local`, or `root` on an ESXi host. Required. |
| vCenter or ESXi host | host | The vCenter Server, or the ESXi host itself. Required. |
| Port | port | The API's HTTPS port, 443 unless given. |
| VM name or path | path | Required. See below. |
| Certificate SHA-256 | `thumbprint` | The SHA-256 fingerprint of the host's certificate, for a certificate Windows doesn't trust (ESXi's own, self-signed one). |
| Keyboard | `keys` | `auto` (default): scancodes if the server takes them, keysyms otherwise. `keysyms`: always keysyms of a US layout. |

```
vmware://<user>@<host>[:<port>]/<vm>[?thumbprint=<SHA-256>][&keys=auto|keysyms]
```

The VM is

- an **inventory path** if it contains `/`: `DC1/vm/web-01`, or with
  folders `DC1/vm/Linux/web-01`. On an ESXi host the datacenter is
  `ha-datacenter`: `ha-datacenter/vm/web-01`.
- a **managed object id** if it looks like `vm-123` (vCenter) or is a number
  (ESXi). If no VM has that id, the plugin looks for a VM of that name.
- a **VM name** otherwise. When several VMs have the name, the plugin asks
  for the inventory path.

Names are as the vSphere Client shows them; a VM whose name contains `/`
can only be reached by its id.

Examples:

```
vmware://ann%40corp.local@vcenter.example.com/DC1/vm/web-01
vmware://root@esx01.example.com/web-01?thumbprint=AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89
vmware://administrator%40vsphere.local@vcenter.example.com/vm-1043?keys=keysyms
```

The password comes from Casement (saved, or typed when the plugin reports
that one is needed); the plugin doesn't store it.

What Casement sees as `state`:

| | |
|---|---|
| `connected` | the console's RFB handshake is done |
| `auth_failed` | no password, or the server rejected the user name or password |
| `failed` | with a sentence: the host can't be reached; an untrusted certificate, with its fingerprint to paste into the connection; no VM, several, or one that isn't running; a missing privilege; a ticket the console host refused; a server that issues no console tickets |
| `closed` | the server closed a working console (the VM was powered off, another client took over, the host went into maintenance), or the connection was lost |

## How it works

1. The plugin talks SOAP to `https://<host>/sdk`, as the vSphere Client
   does: `RetrieveServiceContent` (which tells it the API version to use),
   `SessionManager.Login`, then finds the VM: `SearchIndex.FindByInventoryPath`
   for a path, a `ContainerView` of all VMs with `PropertyCollector` for a
   name, or the managed object itself for an id. It reads the VM's power
   state and asks for a console ticket, `VirtualMachine.AcquireTicket("webmks")`.
2. The ticket names the console host: on vCenter, the ESXi host the VM runs
   on (the console connection goes straight to it, not through vCenter); on
   an ESXi host, the host itself. The plugin opens a WebSocket to
   `wss://<host>:<port>/ticket/<ticket>` with the subprotocol `binary`.
3. The WebSocket carries plain RFB (VNC). The ticket is the authentication;
   RFB uses security type None. LibVNCClient handles the handshake and the
   encodings; it reads a loopback socket, and two threads copy between the
   other end and the WebSocket.
4. The picture goes into Casement's surface as 32-bit BGRX, which is what
   the plugin asks the server for. After each `frame_ack` the plugin asks
   for the next incremental update, so one update is in flight and a slow
   tab slows the server down instead of piling up pictures. Screen size
   changes bring a new surface.
5. Keys go as QEMU extended key events with XT scancodes when the server
   announces them (Casement's `0x1xx` keys become `0x80 + xx`, Pause becomes
   `0xC6`), and as X11 keysyms of a US layout otherwise. The pointer is
   absolute; the wheel is buttons 4 and 5. The server's cursor (RichCursor
   or XCursor) becomes Casement's `cursor`, its pointer position updates
   `cursor_pos`.
6. On `disconnect` the plugin closes the WebSocket and ends the API session
   (`SessionManager.Logout`), also after a failure or when the server
   closes the console.

## Things to know

- **Network.** The PC needs HTTPS (443) to the vCenter Server or ESXi host,
  and, with vCenter, to the ESXi host running the VM, under the name vCenter
  knows it by. A console that fails with "Can't find the host esx01…" means
  that name doesn't resolve from the PC.
- **Privileges.** The user needs to be able to log in to the API and the
  privilege *Virtual machine → Interaction → Console interaction*
  (`VirtualMachine.Interact.ConsoleInteract`) on the VM.
- **Free ESXi licence.** The free licence restricts the API; a host that
  refuses the ticket says so ("The server's licence doesn't allow this
  through the API…").
- **Certificates.** For the API, a certificate Windows trusts for the host
  name or address is accepted; anything else only with its SHA-256 in the
  connection. The failure message shows the fingerprint to paste: compare it
  with the one the host shows (on the ESXi console, or `openssl x509
  -fingerprint -sha256` on its certificate) before trusting it. For the
  console host, the plugin accepts the certificate the API vouches for in
  the ticket (its SHA-1, SHA-256 list or, on vSphere 9, the certificate
  itself), one Windows trusts, or, when the console is on the API host
  itself, the certificate already accepted for the API. Nothing secret (the
  password, the session cookie, the ticket) goes to a server whose
  certificate didn't pass.
- **Keyboard layout.** With scancodes the guest's own layout applies. With
  keysyms (servers without QEMU extended key events) the keys are those of a
  US layout; other layouts in the guest give other characters. Whether ESXi
  takes QEMU extended key events is not known yet; the log says which the
  plugin uses (`CASEMENT_VMWARE_LOG=info`).
- **Pointer.** Absolute positioning needs VMware Tools or a USB tablet in
  the guest. Without them (BIOS, some installers) the guest has a relative
  PS/2 mouse and the pointer drifts.
- **Not there:** clipboard (VMware's web console has none either), audio,
  resizing the guest to the window, power operations.
- **Tickets** are single-use and expire after 30 minutes; every connection
  logs in, gets a fresh ticket and logs out again.
- **Logging.** `CASEMENT_VMWARE_LOG=debug` (or `trace`) writes details to
  Casement's log; passwords, session cookies and tickets are never logged.
- **Proxy.** WinHTTP's proxy configuration (`netsh winhttp`) applies; it is
  direct by default.

## Building

The plugin is C++20 with CMake. The build needs LibVNCClient (from
LibVNCServer 0.9.15) with zlib and libjpeg-turbo.

### Windows (MSVC)

Visual Studio 2022 with C++ and CMake, and [vcpkg](https://vcpkg.io) with
`VCPKG_ROOT` set. `vcpkg.json` lists the dependencies at a pinned baseline;
LibVNCServer comes from the overlay port in `vcpkg-ports/`, configured like
the MinGW build. In a Developer Command Prompt:

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake ^
    -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build
ctest --test-dir build -L unit
```

The result is one `build\casement-vmware.exe` without DLLs.

### MinGW-w64 (cross build on Linux)

```
ci/mingw-deps.sh /opt/mingw64 zlib libjpeg_turbo libvncserver
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DCMAKE_PREFIX_PATH=/opt/mingw64
cmake --build build
ctest --test-dir build -L unit
```

`ci/mingw-deps.sh` builds the libraries as static libraries into the prefix,
from sources checked against pinned SHA-256 sums. ctest runs the Windows
programs through Wine.

## Testing

**Unit tests** (`tests/unit`, label `unit`): SOAP request building and
reading replies and faults recorded from vcsim and fake-esxi
(`tests/unit/recorded.h`, made by `tests/unit/record_replies.py`) and in
ESXi's shape; addresses and the VM part's classification, vSphere's name
escaping; fingerprints, hashing, PEM and the certificate check; the console
endpoint a ticket describes; the key tables (US keysyms, XT codes, Shift,
NumLock, Pause); the RFB messages the plugin writes and cursor conversion;
the loopback socket pair.

**End-to-end tests** (`tests/e2e`, label `e2e`) run the real plugin the
way Casement does (`tools/sim`), under Wine on Linux. `ci/e2e.sh` starts:

- **tools/fake-esxi**, which is govmomi's vCenter simulator (vcsim) with
  VMs that answer `AcquireTicket("webmks")`, which vcsim's don't, and a
  console host that bridges the ticket's WebSocket to a real VNC server, as
  ESXi does. The VM's name picks the case: tickets with a URL, SHA-1 and
  SHA-256 list (vCenter 8), with host and port only, with the certificate
  itself (vSphere 9), for the API host itself (ESXi), with a wrong or no
  fingerprint, a ticket the console refuses, a console the server closes.
  A control endpoint lets the tests change the X display and read what
  arrived.
- **x11vnc** and TigerVNC's **X0tigervnc** on an Xvfb display, behind those
  consoles: x11vnc takes keysyms, X0tigervnc takes QEMU extended key events
  with a raw keyboard. `xev` on the root window sees the input arrive.
- **vcsim** and **govc** from govmomi v0.56.0, for the API on its own.

They check: the picture (a solid colour, then a gradient that both servers
send as Tight JPEG), one update request in flight until `frame_ack`, keys as
keysyms and as scancodes (right Ctrl, Pause, Delete) reaching X, the
`keys=keysyms` setting, pointer position and buttons, the wheel, the cursor
shape changing over a window and the guest moving the pointer, a resolution
change (x11vnc scaling) with new surfaces, every kind of ticket and VM
reference, the console certificate check (the ticket never reaches a host
whose certificate doesn't pass), a refused ticket, a server closing the
console (`closed` with its reason), logout on disconnect, also while still
connecting, wrong VMs (powered off, duplicates, missing, not a VM, unknown
id), a user without the console privilege, missing and wrong passwords
(the password never reaches a server whose certificate doesn't pass), a CA
the Windows store trusts, and against vcsim alone: lookups by name, path and
id, failures, and that vcsim issues no console tickets.

Not tested: a real ESXi host or vCenter Server, and so VMware's own RFB
extensions and which encodings and key events ESXi uses; the MSVC build
(CI builds it, it wasn't built here). Under Wine, WinHTTP sends a request
even when the certificate callback closes its handle (on Windows that stops
the request); the plugin therefore writes request bodies only after the
check and checks the console host with an empty request before sending
the ticket, which the tests confirm.

## Protocol v2

These proposals (numbers from the plugin specs) would help:

- **P2 prompts** and **P3 data folder**: ask whether to trust an unknown
  certificate and remember the answer, instead of the fingerprint field.
- **P5 relative pointer**: for guests without VMware Tools or a USB tablet.
- **P12 type text**: type the clipboard into the VM as key events; VM
  consoles have no clipboard.
- **P1 progress text**: show "Logging in…", "Opening the console…" during
  the steps before `connected`.

## Licensing

The plugin is licensed under the GNU General Public License, version 2 or
(at your option) any later version; see [LICENSE](LICENSE). It follows
LibVNCClient's licence.

Third-party code:

| Component | Version | Licence | Use |
|---|---|---|---|
| [LibVNCServer / LibVNCClient](https://github.com/LibVNC/libvncserver) | 0.9.15 | GPL-2.0-or-later | RFB; linked statically (built by `ci/mingw-deps.sh` or the vcpkg overlay port) |
| [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo) | 3.2.0 (MinGW); vcpkg's (MSVC) | IJG, BSD-3-Clause, zlib | JPEG in Tight; linked statically |
| [zlib](https://zlib.net) | 1.3.1 (MinGW); vcpkg's (MSVC) | zlib | linked statically |
| [pugixml](https://github.com/zeux/pugixml) | 1.16 | MIT | SOAP replies; in `third_party/pugixml` with its licence |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | MIT | protocol messages; in `third_party/nlohmann` with its licence |
| [govmomi](https://github.com/vmware/govmomi) | 0.56.0 | Apache-2.0 | tests only: `tools/fake-esxi` and vcsim, not part of the plugin |

This software is based in part on the work of the Independent JPEG Group.

`docs/plugin-protocol.md` is a copy of the
[Casement plugin protocol](https://github.com/Inkbraid/casement-plugin-protocol),
licensed CC BY 4.0.
