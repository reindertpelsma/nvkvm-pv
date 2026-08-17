#!/usr/bin/env bash
#
# measure_pin_transient.sh — show that the memory the migration itself needs
# does not scale with the registration size.
#
# Run INSIDE the guest.  Registers a buffer of the given size and samples
# /proc/meminfo every 200 ms while the registration runs.
#
# What to look for:
#
#   The buffer itself is anonymous guest memory that exists BEFORE the call, so
#   it is not what we are measuring.  What we are measuring is whether the
#   migration adds a second copy of the whole range on top of it.
#
#   Under strict per-chunk migration each chunk's pinned pages are released as
#   soon as that chunk is remapped onto its memfd, so guest MemFree should CLIMB
#   steadily during the registration — by roughly the buffer size, in chunk-sized
#   steps — rather than staying flat until the very end.  A climb that tracks
#   progress is the observable signature of incremental unpinning.
#
#   The kernel's own DIAG line reports the authoritative number:
#     "dup_peak=<bytes>" — the largest amount of data that existed in BOTH the
#     pinned guest pages and a memfd at any single instant.  That is the
#     quantity the old 16 MiB cap was protecting, and it should read 2097152
#     (one chunk) for every size.
#
#   bash measure_pin_transient.sh 1073741824
set -u
N="${1:-1073741824}"
PROBE="${PROBE:-/tmp/pin_size_probe}"

echo "== registering $N bytes ($((N / 1024 / 1024)) MiB) =="
sudo dmesg -C 2>/dev/null || true

read_free() { awk '/^MemFree:/{print $2}' /proc/meminfo; }

BEFORE=$(read_free)
( while :; do echo "  sample MemFree_kB=$(read_free)"; sleep 0.2; done ) &
SAMPLER=$!
trap 'kill $SAMPLER 2>/dev/null' EXIT

"$PROBE" --one "$N" | grep -E '^M (one|pattern_bytes_wrong|vmas_overlapping)'

kill $SAMPLER 2>/dev/null
AFTER=$(read_free)

echo "MemFree_before_kB=$BEFORE"
echo "MemFree_after_kB=$AFTER"
echo "delta_kB=$((AFTER - BEFORE))"
echo "== kernel DIAG =="
sudo dmesg | grep -E 'DIAG: migrate_range' | tail -3
