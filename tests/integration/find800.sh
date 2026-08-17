#!/bin/bash
# Build a gdb script that breaks on every `mov $0x320,%eax` in libcuda,
# logs a short backtrace, and continues.  The first hit during cuCtxCreate
# is the conditional that decided to return 800.
set -e

LIBCUDA="${LIBCUDA:-/usr/local/nvidia-guest/lib/libcuda.so.575.51.03}"

# Find all 800-emission sites (offsets within libcuda).
awk '
/^[0-9a-f]+ <[^>]+>:/ { fname=$2; sub(/:$/,"",fname); next }
/mov.*\$0x320,%eax/ { print $1 ":" fname }
' < <(objdump -d "$LIBCUDA") > /tmp/sites.txt

echo "found $(wc -l < /tmp/sites.txt) sites"

# Build the gdb commands
{
    echo "set pagination off"
    echo "set logging file /tmp/trap800.log"
    echo "set logging overwrite on"
    echo "set logging redirect on"
    echo "set logging enabled on"
    echo "set breakpoint pending on"
    # Add break at libcuda base + offset for each site.
    # We use rbreak by NAME of the enclosing function plus check rax.
    while IFS=: read -r off fname; do
        # Strip trailing colon
        addr_dec=$((16#$off))
        echo "break *((char*)&cuInit + 0x$off - ((char*)&cuInit - (char*)__cuda_libcuda_base))"
    done < /tmp/sites.txt > /dev/null
    # The simpler approach: use rbreak to break on offset within libcuda
    # Actually use `b *0x<libcuda_base>+0x<off>` after libcuda is loaded.

    # Simpler still: break at cuCtxCreate_v2; finish; if ret=800 print bt
    echo "break cuCtxCreate_v2"
    echo "commands"
    echo "  silent"
    echo "  finish"
    echo "end"
    # Hook on finish-stop check rax
    echo "define hook-stop"
    echo "  if \$rax == 0x320"
    echo "    printf \"=== cuCtxCreate returned 800 ===\\n\""
    echo "    bt 20"
    echo "    continue"
    echo "  end"
    echo "end"

    echo "run"
    echo "quit"
} > /tmp/trap.gdb

gdb -q -batch -x /tmp/trap.gdb /tmp/cumemalloc_test 2>&1 | tee /tmp/trap.out
cat /tmp/trap800.log 2>/dev/null
