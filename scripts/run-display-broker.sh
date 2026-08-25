#!/usr/bin/env bash
# Build and run the display broker in the current host desktop session.
# It intentionally stays outside Docker: it owns the window, input focus and
# optional keyboard grab. The VMM receives only this unix socket.
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BROKER="${NVKVM_BROKER_BIN:-$ROOT/src/broker/nvkvm-display-broker}"
SOCKET="${NVKVM_BROKER_SOCKET:-/run/nvkvm/steamos.sock}"
BACKEND="${NVKVM_BROKER_BACKEND:-wayland}"
SIZE="${NVKVM_BROKER_SIZE:-1280x800}"
GROUP="${NVKVM_BROKER_GROUP:-$(id -gn)}"

if [ ! -x "$BROKER" ]; then
    echo "== building the host display broker ==" >&2
    make -C "$ROOT/src/broker" nvkvm-display-broker
fi

dir="$(dirname "$SOCKET")"
if [ ! -d "$dir" ] || [ ! -w "$dir" ]; then
    echo "ERROR: $dir must exist and be writable by $(id -un)." >&2
    echo "Create it once with:" >&2
    echo "  sudo install -d -m 0770 -o $(id -un) -g $GROUP $dir" >&2
    exit 1
fi

echo "== host display broker ==" >&2
echo "socket: $SOCKET (0660, group $GROUP)" >&2
echo "backend: $BACKEND, initial size: $SIZE, persistent across VM restarts" >&2
echo "CTRL+ALT+G toggles grab; focus loss always releases it." >&2
exec "$BROKER" \
    --socket "$SOCKET" \
    --socket-mode 0660 \
    --socket-group "$GROUP" \
    --allow-user root \
    --backend "$BACKEND" \
    --size "$SIZE" \
    --title "SteamOS — nvkvm" \
    --persist \
    "$@"
