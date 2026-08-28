#!/usr/bin/env bash
#
# ogkm_ctrl_audit.sh — re-derive tests/abi_parity/ogkm_gamescope_ctrl_audit.tsv.
#
# Answers one question per (command, driver tag): could an UNPRIVILEGED user
# issue this RM control on bare metal?  The authority is the NVOC exported-method
# table's `accessRight` field, which is a one-limb RS_ACCESS mask (0x2 =
# RS_ACCESS_NICE = capable(CAP_SYS_NICE)), NOT the RMCTRL_FLAGS word -- every
# command audited here carries RMCTRL_FLAGS_NON_PRIVILEGED and four of them are
# nonetheless privileged.  Reading the flags alone is the trap this script
# exists to avoid.
#
# It also records what RS_ACCESS_NICE resolves to on each tag (the primitive
# moved from os_allow_priority_override() to os_check_access() at 565.57.01,
# with no change in meaning) and the layout of NV2080_CTRL_FIFO_GET_INFO_PARAMS
# (inline table vs NvP64 list -- the difference between "needs no marshalling"
# and "leaks a guest VA").
#
# Usage:  tools/ogkm_ctrl_audit.sh [tag-list-file] > out.tsv
# Default tag list: every tag `git ls-remote` returns for majors 515..610.
#
# Fetches only the handful of files it needs per tag (via codeload tarball with
# --wildcards, plus raw.githubusercontent for two small headers), so a full
# 216-tag sweep is minutes and a few hundred MB of transfer, not a 20 GB clone.
set -uo pipefail

REPO=https://github.com/NVIDIA/open-gpu-kernel-modules
CMDS="20801109 20801115 a06f0111 a06f0109 a06c0110 a06c0107 a06c0101 a06c0103 a06c0105 a06f0103 a06f0104"
JOBS="${JOBS:-12}"

taglist="${1:-}"
if [ -z "$taglist" ]; then
	taglist=$(mktemp)
	git ls-remote --tags "$REPO" \
	  | sed -n 's#.*refs/tags/\([0-9].*\)\^{}#\1#p' \
	  | awk -F. '$1>=515 && $1<=610' | sort -u > "$taglist"
fi

probe_one() {
	tag="$1"; cmds="$2"
	work=$(mktemp -d)
	if ! curl -sSL --retry 3 --max-time 300 \
	     "$REPO/archive/refs/tags/${tag}.tar.gz" \
	   | tar xz -C "$work" --wildcards \
	     '*/src/nvidia/generated/g_subdevice_nvoc.c' \
	     '*/src/nvidia/generated/g_kernel_channel_nvoc.c' \
	     '*/src/nvidia/generated/g_kernel_channel_group_api_nvoc.c' \
	     '*/src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080fifo.h' \
	     '*/kernel-open/nvidia/os-interface.c' 2>/dev/null; then
		echo -e "${tag}\tFETCH_FAILED"; rm -rf "$work"; return
	fi
	root=$(find "$work" -maxdepth 1 -mindepth 1 -type d | head -1)
	TAG="$tag" CMDS="$cmds" ROOT="$root" python3 - <<'PY'
import re,os,sys
root=os.environ['ROOT']; tag=os.environ['TAG']
pat=re.compile(r'/\*flags=\*/\s*(0x[0-9a-fA-F]+)u.*?'
               r'/\*accessRight=\*/\s*(0x[0-9a-fA-F]+)u.*?'
               r'/\*methodId=\*/\s*(0x[0-9a-fA-F]+)u', re.S)
db={}
for f in ("g_subdevice_nvoc.c","g_kernel_channel_nvoc.c",
          "g_kernel_channel_group_api_nvoc.c"):
    p=os.path.join(root,"src/nvidia/generated",f)
    if os.path.exists(p):
        for m in pat.finditer(open(p,encoding='utf8',errors='replace').read()):
            fl,ar,mid=m.groups(); db[int(mid,16)]=(fl,ar)
cells=[]
for c in os.environ['CMDS'].split():
    e=db.get(int(c,16))
    cells.append(e[1] if e else 'ABSENT')
for c in os.environ['CMDS'].split():
    e=db.get(int(c,16))
    cells.append(e[0] if e else 'ABSENT')
# what RS_ACCESS_NICE costs on this tag
oi=os.path.join(root,"kernel-open/nvidia/os-interface.c"); nice='NONE'
if os.path.exists(oi):
    t=open(oi,encoding='utf8',errors='replace').read()
    m=re.search(r'os_allow_priority_override\s*\([^)]*\)\s*\{(.*?)\n\}',t,re.S)
    if m and 'CAP_SYS_NICE' in m.group(1):
        nice='os_allow_priority_override->capable(CAP_SYS_NICE)'
    else:
        m=re.search(r'os_check_access\s*\([^)]*\)\s*\{(.*?)\n\}',t,re.S)
        if m:
            n=re.search(r'RS_ACCESS_NICE:(.*?)(?=case\s|default:)',m.group(1),re.S)
            if n and 'CAP_SYS_NICE' in n.group(1):
                nice='os_check_access(NICE)->capable(CAP_SYS_NICE)'
cells.append(nice)
# FIFO_GET_INFO param shape: inline table (no guest VA) or NvP64 list?
fh=os.path.join(root,"src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080fifo.h")
shape='ABSENT'
if os.path.exists(fh):
    t=open(fh,encoding='utf8',errors='replace').read()
    m=re.search(r'typedef struct NV2080_CTRL_FIFO_GET_INFO_PARAMS\s*\{(.*?)\}',t,re.S)
    if m:
        body=re.sub(r'/\*.*?\*/','',m.group(1),flags=re.S)
        mx=re.search(r'NV2080_CTRL_FIFO_GET_INFO_MAX_ENTRIES\s+\((\d+)\)',t)
        shape=('POINTER' if 'NvP64' in body else
               'INLINE_ARRAY' if 'fifoInfoTbl[' in body else 'OTHER')
        shape+=';max=%s'%(mx.group(1) if mx else '?')
cells.append(shape)
print(tag+"\t"+"\t".join(cells))
PY
	rm -rf "$work"
}
export -f probe_one
export REPO

{
	printf 'tag'
	for c in $CMDS; do printf '\tar_%s' "$c"; done
	for c in $CMDS; do printf '\tfl_%s' "$c"; done
	printf '\tnice_primitive\tfifo_get_info_params\n'
	xargs -P "$JOBS" -I{} bash -c 'probe_one "$@"' _ {} "$CMDS" < "$taglist" | sort -V
}
