#!/usr/bin/env bash
#
# run_pin_ladder.sh — drive pin_size_probe one size per process.
#
# Why one process per size: nvkvm_cpu_pages_migrate_range() dedupes by start
# GVA and never forgets a migrated range until the fd closes.  A probe that
# unmaps and remaps in a loop recycles VAs, so the *second* registration at a
# recycled address returns 0 without migrating anything and the results turn
# non-deterministic.  A fresh process has an empty cpu_pages list.
#
#   bash run_pin_ladder.sh [--shared] [probe_binary]
#
# Prints one "M one ..." line per size, plus the kernel's own DIAG counters if
# dmesg is readable.
set -u

SHARED=""
PROBE="${PROBE:-/tmp/pin_size_probe}"
for a in "$@"; do
    case "$a" in
        --shared) SHARED="--shared" ;;
        *) PROBE="$a" ;;
    esac
done

SIZES="1048576 2097152 2097153 4194304 8388608 16777216 16777217 17825792 33554432 67108864 83886080 134217728 268435456 536870912 1073741824 2147483648"

echo "== pin ladder ($( [ -n "$SHARED" ] && echo MAP_SHARED || echo MAP_PRIVATE )) =="
for n in $SIZES; do
    "$PROBE" --one "$n" $SHARED 2>&1 | grep '^M one' || echo "M one CRASH bytes=$n"
done
