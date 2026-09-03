#!/usr/bin/env bash
# nvkvm-report.sh -- collect the facts a bug report needs, in one pass.
#
#   ./scripts/nvkvm-report.sh > nvkvm-report.txt
#
# Run it on the HOST for a host-side problem, and inside the GUEST for a
# guest-side one. Ideally both -- a lot of nvkvm questions are only answerable
# by comparing the two. It detects which side it is on.
#
# It reads; it never changes anything and never needs root (a few sections say
# more when run as root, and say so when they do not).
#
# READ THE OUTPUT BEFORE POSTING IT. It contains your kernel version, GPU
# model, driver version and paths under your home directory. It deliberately
# does not dump the environment, only named NVKVM_* variables.

REPORT_VERSION=1
export LC_ALL=C

have() { command -v "$1" >/dev/null 2>&1; }

# Bounded so a wedged nvidia-smi cannot hang the whole report.
run() {
	local label="$1"; shift
	printf '\n$ %s\n' "$label"
	if have timeout; then
		timeout 15 "$@" 2>&1 | sed 's/^/    /'
	else
		"$@" 2>&1 | sed 's/^/    /'
	fi
	local rc=${PIPESTATUS[0]}
	[ "$rc" -eq 124 ] && printf '    (timed out after 15s -- that is itself a finding)\n'
	[ "$rc" -ne 0 ] && [ "$rc" -ne 124 ] && printf '    (exit %d)\n' "$rc"
	return 0
}

sec() { printf '\n\n===== %s =====\n' "$1"; }
note() { printf '  %s\n' "$1"; }

# ---------------------------------------------------------------- which side
SIDE="host"
if [ -e /sys/bus/virtio/drivers/nvkvm-guest ] || lsmod 2>/dev/null | grep -q '^nvkvm_guest' \
   || [ -e /sys/module/nvkvm_guest ]; then
	SIDE="guest"
fi

printf '===== nvkvm report v%d =====\n' "$REPORT_VERSION"
printf '  generated : %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
printf '  side      : %s (auto-detected)\n' "$SIDE"
printf '  user      : uid=%s root=%s\n' "$(id -u)" "$([ "$(id -u)" -eq 0 ] && echo yes || echo no)"

# ------------------------------------------------------------------- system
sec "system"
if [ -r /etc/os-release ]; then
	note "distro : $(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-unknown}")"
fi
note "kernel : $(uname -s) $(uname -r) $(uname -m)"
if have lscpu; then
	note "cpu    : $(lscpu | sed -n 's/^Model name: *//p' | head -1)"
	# MAXPHYADDR: a 39-bit host needs the adaptive GPA window. Worth knowing
	# before anything else when the guest will not start at all.
	note "addr   : $(lscpu | sed -n 's/^Address sizes: *//p' | head -1)"
fi
note "memory : $(awk '/MemTotal/{printf "%.1f GiB", $2/1048576}' /proc/meminfo 2>/dev/null)"
have systemd-detect-virt && note "virt   : $(systemd-detect-virt 2>/dev/null) (none = bare metal)"

# ------------------------------------------------------------------- kvm
sec "kvm"
if [ -e /dev/kvm ]; then
	note "/dev/kvm : present, $(stat -c '%A %U:%G' /dev/kvm 2>/dev/null)"
	[ -r /dev/kvm ] && [ -w /dev/kvm ] && note "access   : this user can open it" \
		|| note "access   : this user CANNOT open it -- add yourself to the kvm group"
else
	note "/dev/kvm : ABSENT -- no hardware virtualisation available here"
fi
[ -r /sys/module/kvm_intel/parameters/nested ] && \
	note "nested   : intel=$(cat /sys/module/kvm_intel/parameters/nested 2>/dev/null)"
[ -r /sys/module/kvm_amd/parameters/nested ] && \
	note "nested   : amd=$(cat /sys/module/kvm_amd/parameters/nested 2>/dev/null)"

# ------------------------------------------------------------------- nvidia
sec "nvidia driver"
if [ -r /proc/driver/nvidia/version ]; then
	sed 's/^/  /' /proc/driver/nvidia/version
else
	note "/proc/driver/nvidia/version absent -- the NVIDIA driver is not loaded here"
fi
have nvidia-smi && run "nvidia-smi -L" nvidia-smi -L
have nvidia-smi && run "nvidia-smi --query-gpu=name,driver_version,pstate,persistence_mode,power.draw,power.limit,memory.used,memory.total --format=csv" \
	nvidia-smi --query-gpu=name,driver_version,pstate,persistence_mode,power.draw,power.limit,memory.used,memory.total --format=csv

if [ "$SIDE" = host ]; then
	# Runtime power management. Relevant to laptops: a mapping revoked around a
	# PM transition is the known cause of a KVM_RUN EFAULT (see patches/0010).
	for p in /proc/driver/nvidia/gpus/*/power; do
		[ -r "$p" ] || continue
		printf '\n  %s\n' "$p"
		sed 's/^/    /' "$p"
	done
	for d in /sys/bus/pci/drivers/nvidia/[0-9a-f]*:*; do
		[ -e "$d" ] || continue
		note "runtime_status : $(basename "$d") = $(cat "$d/power/runtime_status" 2>/dev/null)"
	done
	# nvkvm never binds vfio-pci. If something did, that is the bug.
	if [ -d /sys/bus/pci/drivers/vfio-pci ]; then
		v=$(ls /sys/bus/pci/drivers/vfio-pci 2>/dev/null | grep -c ':' )
		note "vfio-pci : bound to $v device(s) -- nvkvm does not use vfio-pci; a bound GPU here is a misconfiguration"
	fi
fi

# ------------------------------------------------------------------- nvkvm
sec "nvkvm build"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." 2>/dev/null && pwd)"
if [ -n "$here" ] && [ -d "$here/.git" ] && have git; then
	note "repo     : $here"
	note "revision : $(git -C "$here" describe --tags --always --dirty 2>/dev/null)"
	note "branch   : $(git -C "$here" rev-parse --abbrev-ref HEAD 2>/dev/null)"
fi
if [ -r "$here/src/common/nvkvm_proto.h" ]; then
	note "proto    : $(sed -n 's/^#define NVKVM_PROTO_VERSION *//p' "$here/src/common/nvkvm_proto.h" | head -1) (source tree)"
fi
for q in /opt/qemu-nvkvm/bin/qemu-system-x86_64 /opt/qemu-nvkvm/qemu-system-x86_64 \
         "$here/qemu-nvkvm/build/qemu-system-x86_64"; do
	[ -x "$q" ] || continue
	note "qemu     : $q"
	run "$q --version" "$q" --version
	# The forwarder device only exists in a patched build. If this is empty,
	# QEMU is stock and nothing else in the report matters.
	printf '\n$ %s -device help | grep nvgpu\n' "$q"
	("$q" -device help 2>/dev/null | grep -i nvgpu | sed 's/^/    /') || true
	break
done
for s in /usr/lib/nvkvm/nvkvm_stub "$here/src/stub/nvkvm_stub"; do
	[ -x "$s" ] && note "stub     : $s"
done
have docker && run "docker images (nvkvm)" \
	docker images --format '{{.Repository}}:{{.Tag}} {{.ID}} {{.CreatedSince}}' --filter reference='*nvkvm*'

# --------------------------------------------------------------- guest side
if [ "$SIDE" = guest ]; then
	sec "guest module"
	lsmod 2>/dev/null | awk 'NR==1 || /nvkvm/' | sed 's/^/  /'
	if have modinfo; then
		run "modinfo nvkvm-guest" modinfo nvkvm-guest
	fi
	note "nvidia nodes : $(ls /dev/nvidia* 2>/dev/null | tr '\n' ' ')"
	note "drm nodes    : $(ls /dev/dri/ 2>/dev/null | tr '\n' ' ')"
	if [ -e /dev/dri/renderD128 ]; then
		note "renderD128   : $(stat -c '%A %U:%G' /dev/dri/renderD128) -- your user needs to be in that group"
	fi
fi

# ------------------------------------------------------------------ display
sec "display"
note "session  : XDG_SESSION_TYPE=${XDG_SESSION_TYPE:-unset} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-unset} DISPLAY=${DISPLAY:-unset}"
if have glxinfo; then
	gl=$(glxinfo -B 2>/dev/null)
	if [ -n "$gl" ]; then
		note "GL vendor: $(printf '%s\n' "$gl" | sed -n 's/^OpenGL vendor string: *//p')"
		note "GL render: $(printf '%s\n' "$gl" | sed -n 's/^OpenGL renderer string: *//p')"
		note "(a Mesa/llvmpipe renderer on the host means the present path must be readback, not native)"
	else
		note "glxinfo present but produced nothing (no display reachable from this shell)"
	fi
else
	note "glxinfo not installed -- install mesa-utils if the question is about the present path"
fi

sec "nvkvm environment"
found=0
for v in NVKVM_PRESENT_MODE NVKVM_ISOLATE_MODE NVKVM_ISOLATE_NO_HARDEN NVKVM_ISOLATE_UID_BASE \
         NVKVM_STUB_PATH NVKVM_DEBUG NVKVM_STUB_DEBUG NVKVM_CTRL_AUDIT NVKVM_RING_DISABLE \
         NVKVM_FORCE_HOST_BITS NVKVM_PRESENT_SYNC NVKVM_BAR_WINDOW NVKVM_IMAGE_TAG; do
	if [ -n "${!v}" ]; then note "$v=${!v}"; found=1; fi
done
[ "$found" -eq 0 ] && note "(none set -- all defaults)"

# --------------------------------------------------------------------- logs
sec "kernel log (nvkvm / nvidia)"
if dmesg >/dev/null 2>&1; then
	klog=$(dmesg 2>/dev/null | grep -iE 'nvkvm|NVRM|nvidia-uvm|Failed to auto-unmap' | tail -40)
	if [ -n "$klog" ]; then
		printf '%s\n' "$klog" | sed 's/^/  /'
	else
		note "(no nvkvm/NVRM lines in the kernel log)"
	fi
else
	note "dmesg not readable as this user -- re-run with sudo, or paste 'journalctl -k' output"
fi

sec "QEMU stderr -- PLEASE INCLUDE THIS"
cat <<'EOT'
  QEMU's stderr is where nvkvm reports the things no other log will show, and
  it is not collected automatically. If you ran the compose stack:

      docker compose logs vmm | tail -200

  If you launched QEMU by hand, re-run it with stderr captured:

      qemu-system-x86_64 ... 2>&1 | tee /tmp/nvkvm-qemu.log

  Lines that are worth reporting even when everything appears to work:

      nvkvm: KVM_RUN EFAULT -- mapping not resolvable yet, retrying up to ...
      nvkvm: KVM_RUN EFAULT cleared after N ms
      nvkvm: protocol version mismatch: host=N guest=M
      DENY / EACCES lines from the ctrl allowlist

  The first two mean a GPU mapping briefly went away and nvkvm waited it out
  instead of killing your VM. That is working as intended, but how often it
  happens and on what hardware is genuinely unknown -- reporting it helps.
EOT

sec "end of report"
note "Review this before posting it publicly."
