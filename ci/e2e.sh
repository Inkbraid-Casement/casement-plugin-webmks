#!/usr/bin/env bash
# Starts the servers the end-to-end tests talk to and runs the tests:
#
#   ci/e2e.sh <build directory>
#
#   - Xvfb on display :21, with x11vnc (keysyms) and TigerVNC's X0tigervnc
#     (QEMU extended key events, raw keyboard) sharing it;
#   - tools/fake-esxi: govmomi's simulator with consoles in front of them;
#   - vcsim and govc from govmomi, built from the pinned tag, for the API
#     on its own.
#
# Everything listens on 127.0.0.1 (and 127.0.0.2) ports 10500-10599. Needs
# Go, git, Xvfb, x11vnc, X0tigervnc, xev, xsetroot, xdotool, xterm,
# ImageMagick's display and openssl. E2E_WORK keeps the govmomi build
# between runs.
set -euo pipefail

BUILD=$(realpath "$1")
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${E2E_WORK:-$(mktemp -d)}
mkdir -p "$WORK/bin" "$WORK/logs"
WORK=$(realpath "$WORK")

DISPLAY_NUMBER=21
GOVMOMI_TAG=v0.56.0
GOVMOMI_COMMIT=81608f9b9725d64e4ed447b9a338c9f03dbf8840
API=127.0.0.1:10501
CONSOLE=127.0.0.2:10502
CONTROL=127.0.0.1:10503
VCSIM=127.0.0.1:10510
X11VNC_PORT=10521
TIGERVNC_PORT=10522

pids=()
cleanup() {
    for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
    for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
}
trap cleanup EXIT

# Waits until something listens on host:port.
wait_port() {
    local host=${1%:*} port=${1##*:}
    for _ in $(seq 100); do
        if (exec 3<>"/dev/tcp/$host/$port") 2>/dev/null; then return 0; fi
        sleep 0.1
    done
    echo "nothing listens on $1" >&2
    return 1
}

log() { echo "e2e: $*" >&2; }

# The X display and the two VNC servers on it.
if [ -e "/tmp/.X$DISPLAY_NUMBER-lock" ]; then
    echo "display :$DISPLAY_NUMBER is in use" >&2
    exit 1
fi
Xvfb ":$DISPLAY_NUMBER" -screen 0 1024x768x24 -nolisten tcp >"$WORK/logs/xvfb.log" 2>&1 &
pids+=($!)
for _ in $(seq 100); do [ -e "/tmp/.X11-unix/X$DISPLAY_NUMBER" ] && break; sleep 0.1; done
export DISPLAY=":$DISPLAY_NUMBER"
# Without XFIXES x11vnc picks the cursor from the window under the pointer,
# so the shape changes when the pointer moves onto a window.
x11vnc -display "$DISPLAY" -rfbport "$X11VNC_PORT" -localhost -nopw -forever -shared -noxfixes \
    >"$WORK/logs/x11vnc.log" 2>&1 &
pids+=($!)
X0tigervnc -display "$DISPLAY" -rfbport "$TIGERVNC_PORT" -localhost=1 -SecurityTypes None -RawKeyboard=1 \
    >"$WORK/logs/x0tigervnc.log" 2>&1 &
pids+=($!)
wait_port "127.0.0.1:$X11VNC_PORT"
wait_port "127.0.0.1:$TIGERVNC_PORT"

# fake-esxi.
log "building fake-esxi"
(cd "$ROOT/tools/fake-esxi" && go build -o "$WORK/bin/fake-esxi" .)
"$WORK/bin/fake-esxi" -api "$API" -console "$CONSOLE" -control "$CONTROL" \
    -vnc "127.0.0.1:$X11VNC_PORT" -vnc-qemu "127.0.0.1:$TIGERVNC_PORT" -display "$DISPLAY" \
    >"$WORK/logs/fake-esxi.log" 2>&1 &
pids+=($!)
wait_port "$CONTROL"

# vcsim and govc from the pinned govmomi tag: they are Go modules that
# can't be installed with `go install`.
if [ ! -x "$WORK/bin/vcsim" ] || [ ! -x "$WORK/bin/govc" ]; then
    log "building vcsim and govc $GOVMOMI_TAG"
    rm -rf "$WORK/govmomi"
    git -c advice.detachedHead=false clone -q --depth 1 --branch "$GOVMOMI_TAG" https://github.com/vmware/govmomi \
        "$WORK/govmomi"
    if [ "$(git -C "$WORK/govmomi" rev-parse HEAD)" != "$GOVMOMI_COMMIT" ]; then
        echo "govmomi $GOVMOMI_TAG isn't $GOVMOMI_COMMIT" >&2
        exit 1
    fi
    go -C "$WORK/govmomi/vcsim" build -o "$WORK/bin/vcsim" .
    go -C "$WORK/govmomi/govc" build -o "$WORK/bin/govc" .
fi
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$WORK/vcsim.key" -out "$WORK/vcsim.pem" -days 2 \
    -subj /CN=127.0.0.1 -addext subjectAltName=IP:127.0.0.1 2>/dev/null
VCSIM_SHA256=$(openssl x509 -in "$WORK/vcsim.pem" -noout -fingerprint -sha256 | cut -d= -f2)
"$WORK/bin/vcsim" -l "$VCSIM" -username user -password pass -tlscert "$WORK/vcsim.pem" -tlskey "$WORK/vcsim.key" \
    >"$WORK/logs/vcsim.log" 2>&1 &
pids+=($!)
wait_port "$VCSIM"
# One VM off, and a second VM with an existing name in another folder.
export GOVC_URL="https://user:pass@$VCSIM/sdk" GOVC_INSECURE=1
govc=("$WORK/bin/govc")
"${govc[@]}" vm.power -off DC0_H0_VM1 >/dev/null
"${govc[@]}" folder.create /DC0/vm/team
"${govc[@]}" object.mv /DC0/vm/DC0_C0_RP0_VM0 /DC0/vm/team
"${govc[@]}" object.rename /DC0/vm/team/DC0_C0_RP0_VM0 DC0_C0_RP0_VM1
VCSIM_VM_ID=$("${govc[@]}" ls -i /DC0/vm/DC0_H0_VM0 | cut -d: -f2)

export CASEMENT_E2E=1
export CASEMENT_E2E_FAKE_ESXI=$CONTROL
export CASEMENT_E2E_VCSIM=$VCSIM
export CASEMENT_E2E_VCSIM_SHA256=$VCSIM_SHA256
export CASEMENT_E2E_VCSIM_VM_ID=$VCSIM_VM_ID
export WINEDEBUG=${WINEDEBUG:--all}
log "running the end-to-end tests"
status=0
ctest --test-dir "$BUILD" --output-on-failure -L e2e || status=$?
if [ "$status" != 0 ]; then
    for f in "$WORK"/logs/*.log; do
        echo "== $f"
        tail -n 40 "$f"
    done
fi
exit "$status"
