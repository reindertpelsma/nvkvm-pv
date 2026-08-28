#!/bin/bash
BI=/sys/kernel/debug/dma_buf/bufinfo
ts=$(date +%s); up=$(cut -d. -f1 /proc/uptime)
dcount=$(awk '/^[0-9]+\t/{n++} END{print n+0}' $BI 2>/dev/null)
dbytes=$(awk '/^[0-9]+\t/{s+=$1} END{print s+0}' $BI 2>/dev/null)
qpid=$(pgrep -f qemu-system-x86_64 | head -1)
gpid=$(pgrep -x gnome-shell | head -1)
cnt(){ if [ -n "$1" ]; then ls -l /proc/$1/fd 2>/dev/null | grep -c "/dev/nvidia"; else echo 0; fi; }
qfd=$(cnt "$qpid"); gfd=$(cnt "$gpid")
nstub=0; sfd=0
for p in /proc/[0-9]*; do
  if grep -q nvkvm_stu "$p/comm" 2>/dev/null; then
    nstub=$((nstub+1)); n=$(ls -l $p/fd 2>/dev/null | grep -c "/dev/nvidia"); sfd=$((sfd+n))
  fi
done
gmaps=0; qmaps=0
[ -n "$gpid" ] && gmaps=$(grep -c nvidia /proc/$gpid/maps 2>/dev/null)
[ -n "$qpid" ] && qmaps=$(grep -c nvidia /proc/$qpid/maps 2>/dev/null)
vaerr=$(dmesg 2>/dev/null | grep -c "alloc VA space")
smi=$(nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits 2>/dev/null)
echo "$ts up=$up dmabuf_n=$dcount dmabuf_MB=$((dbytes/1048576)) qemu_fd=$qfd qemu_maps=$qmaps gshell_fd=$gfd gshell_maps=$gmaps stubs=$nstub stub_fd=$sfd vaerr=$vaerr vram=$smi"
