#!/usr/bin/env python3
# Read QEMU's g_mapva table live out of /proc/<pid>/mem.
# g_mapva is the host-side record of every RM_MAP_MEMORY the guest made that
# has not been retired by a matching RM_UNMAP_MEMORY.  It is a fixed 8192-entry
# array: once it fills, nvkvm_mapva_record() silently drops new records and
# later unmaps are issued with a zero VA, orphaning the BAR1 mapping.
# Occupancy is therefore a LEADING indicator -- it saturates before dmesg says
# anything.
import re, struct, subprocess, sys

ENT = 40          # int,u32,u32,u32,u32,pad,u64,u64
NMAX = 8192

def sym(binpath, name):
    out = subprocess.check_output(['nm', binpath], text=True, stderr=subprocess.DEVNULL)
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[2] == name:
            return int(p[0], 16)
    raise SystemExit(f'symbol {name} not found')

pid = sys.argv[1] if len(sys.argv) > 1 else subprocess.check_output(
    ['pgrep','-f','qemu-system-x86_64'], text=True).split()[0]
binp = f'/proc/{pid}/root/opt/qemu-nvkvm/bin/qemu-system-x86_64'

# PIE? find load base of the qemu binary in the process map
base = 0
etype = subprocess.check_output(['file','-L',binp], text=True)
if 'pie' in etype.lower() or 'shared object' in etype.lower():
    with open(f'/proc/{pid}/maps') as f:
        for line in f:
            if 'qemu-system-x86_64' in line:
                base = int(line.split('-')[0], 16); break

a_tab = base + sym(binp, 'g_mapva')
a_seq = base + sym(binp, 'g_mapva_seq')

with open(f'/proc/{pid}/mem','rb', buffering=0) as m:
    m.seek(a_seq); seq = struct.unpack('<Q', m.read(8))[0]
    m.seek(a_tab); buf = m.read(ENT * NMAX)

used = 0
isos = {}
for i in range(NMAX):
    in_use, iso = struct.unpack_from('<iI', buf, i*ENT)
    if in_use:
        used += 1
        isos[iso] = isos.get(iso, 0) + 1
top = sorted(isos.items(), key=lambda kv: -kv[1])[:5]
print(f'mapva_used={used}/{NMAX} mapva_seq_total={seq} distinct_isolates={len(isos)} top={top}')
