#!/usr/bin/env bash
# Run inside the SteamOS service: docker compose exec steamos nvkvm-steamos-ssh
set -euo pipefail
STATE_DIR="${NVKVM_STEAMOS_STATE_DIR:-/var/lib/nvkvm-steamos}"
KEY="${NVKVM_STEAMOS_SSH_IDENTITY:-$STATE_DIR/ssh/id_ed25519}"
PORT="${NVKVM_STEAMOS_SSH_PORT:-15022}"
SSH_USER="${NVKVM_STEAMOS_SSH_USER:-root}"
[ -r "$KEY" ] || {
    echo "No container-managed private key at $KEY." >&2
    echo "The data volume contains an externally supplied authorized_keys; pass" >&2
    echo "NVKVM_STEAMOS_SSH_IDENTITY=/path/to/key if that key is mounted here." >&2
    exit 1
}
exec ssh -i "$KEY" -p "$PORT" \
    -o StrictHostKeyChecking=accept-new \
    -o UserKnownHostsFile="$STATE_DIR/ssh/known_hosts" \
    "$SSH_USER@127.0.0.1" "$@"
