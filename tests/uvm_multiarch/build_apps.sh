#!/usr/bin/env bash
#
# tests/uvm_multiarch/build_apps.sh — build the real managed-memory
# applications ON THE BOX (the KVM host side), into the 9p-shared repo tree so
# the guest can just run them.
#
# WHY ON THE BOX AND NOT IN THE GUEST.  The guest is a minimal cloud image with
# no CUDA toolkit and only user-mode NAT for networking; installing a 4 GB
# toolkit through that, four times, is the expensive way to get the same
# binaries.  The box has the toolkit's natural home and a real network link.
# The binaries are dynamically linked against the CUDA *runtime* libraries,
# which are copied alongside them — but NEVER against libcuda, which must come
# from the guest's own staged host-driver userspace.  Shipping a box libcuda
# into the guest would have the guest talk to a driver stub instead of nvkvm,
# which is the one way to make this whole exercise measure nothing.
#
# Applications chosen because they ACTUALLY ALLOCATE MANAGED MEMORY:
#   conjugateGradientUM   cuBLAS + cuSPARSE over cudaMallocManaged, with a
#                         residual to check — a real numerical result.
#   UnifiedMemoryStreams  cudaStreamAttachMemAsync; NOTE it contains no result
#                         check, so its completion proves nothing.  It is here
#                         because it is a known consumer of UVM_SET_RANGE_GROUP,
#                         which this branch answers guest-side.
#   UnifiedMemoryPerf     expected correct but very slow: the backing does not
#                         migrate, so every access crosses PCIe.  A number to
#                         report, not a regression.
#   attach_verify         the branch's own test — the numerically checked
#                         version of UnifiedMemoryStreams.
#
# PyTorch, TensorFlow, llama.cpp and vLLM are deliberately NOT here: none of
# them allocate managed memory, so they cannot exercise this branch.
#
set -u -o pipefail

REPO=/root/nvkvm
APPS="$REPO/uvm-apps"
CUDA_VER="${CUDA_VER:-12.4.1}"
CUDA_DRV="${CUDA_DRV:-550.54.15}"
CUDA_HOME="${CUDA_HOME:-/usr/local/cuda-12.4}"
# PTX-only codegen: one binary that JITs on Turing, Ampere, Ada and Blackwell
# alike.  A cubin for the wrong SM is the classic way to get "no kernel image
# is available for execution on the device" and read it as a forwarding bug.
SMS="${SMS:-75}"

log() { printf '[apps] %s\n' "$*"; }

mkdir -p "$APPS/bin" "$APPS/lib" "$APPS/log"

# ONE AT A TIME.  A second copy of this script starting while the first is
# still downloading the 4.3 GB runfile will happily run `sh cuda.run` on the
# half-written file: observed on the Turing box, five concurrent installers
# against a 2.1 GB partial download.  flock makes a re-invocation wait for the
# first to finish rather than race it.
exec 9>/root/.build_apps.lock
if ! flock -w 5400 9; then
    log "FATAL: another build_apps.sh held the lock for 90 minutes"; exit 1
fi

# ---------------------------------------------------------------------------
# 1. the toolkit — runfile with --toolkit ONLY.
#
# `apt install nvidia-cuda-toolkit` is NOT safe here: on Ubuntu it can pull
# libnvidia-compute-* and replace the very driver the box is being tested on.
# The runfile with --silent --toolkit touches no driver at all.
# ---------------------------------------------------------------------------
if [ ! -x "$CUDA_HOME/bin/nvcc" ]; then
    log "installing CUDA toolkit $CUDA_VER (toolkit only, no driver)"
    RUN=/root/cuda.run
    URL="https://developer.download.nvidia.com/compute/cuda/${CUDA_VER}/local_installers/cuda_${CUDA_VER}_${CUDA_DRV}_linux.run"
    # Download to a temp name and rename only on success.  A partial file left
    # by an interrupted wget is still non-empty, so a plain `[ -s ]` test would
    # hand `sh` a truncated 2 GB self-extracting archive and the failure reads
    # like a broken toolkit rather than a broken download.
    if [ ! -s "$RUN" ]; then
        rm -f "$RUN.part"
        wget -q --tries=3 --timeout=120 -O "$RUN.part" "$URL" || {
            log "FATAL: could not download $URL"; rm -f "$RUN.part"; exit 1; }
        mv "$RUN.part" "$RUN"
    fi
    sh "$RUN" --silent --toolkit --override --no-opengl-libs \
        > "$APPS/log/cuda-install.log" 2>&1 || {
        log "FATAL: toolkit install failed"; tail -30 "$APPS/log/cuda-install.log"; exit 1; }
fi
export PATH="$CUDA_HOME/bin:$PATH"
command -v nvcc >/dev/null || { log "FATAL: no nvcc after install"; exit 1; }
log "nvcc: $(nvcc --version | tail -1)"

# ---------------------------------------------------------------------------
# 2. the branch's own attach_verify — the checked UnifiedMemoryStreams
# ---------------------------------------------------------------------------
log "building attach_verify"
nvcc -O2 -gencode "arch=compute_${SMS},code=compute_${SMS}" \
     -o "$APPS/bin/attach_verify" "$REPO/tests/integration/attach_verify.cu" \
     > "$APPS/log/attach_verify.log" 2>&1 \
    && log "  attach_verify OK" || { log "  attach_verify FAILED"; tail -20 "$APPS/log/attach_verify.log"; }

# ---------------------------------------------------------------------------
# 3. the CUDA samples that use managed memory
# ---------------------------------------------------------------------------
SAMPLES=/root/cuda-samples
if [ ! -d "$SAMPLES" ]; then
    for tag in v12.4 v12.3 v12.2 v11.8; do
        log "cloning cuda-samples $tag"
        if git clone --quiet --depth 1 --branch "$tag" \
                https://github.com/NVIDIA/cuda-samples.git "$SAMPLES" 2>/dev/null; then
            log "  got $tag"; break
        fi
        rm -rf "$SAMPLES"
    done
fi
[ -d "$SAMPLES" ] || { log "FATAL: could not clone cuda-samples"; exit 1; }

build_sample() {   # build_sample <name>
    local name="$1" dir
    dir="$(find "$SAMPLES/Samples" -maxdepth 2 -type d -name "$name" | head -1)"
    if [ -z "$dir" ]; then log "  $name: NOT FOUND in this samples tree"; return 1; fi
    log "building $name (SMS=$SMS, PTX-only)"
    if make -C "$dir" SMS="$SMS" CUDA_PATH="$CUDA_HOME" -j4 \
            > "$APPS/log/$name.log" 2>&1 && [ -x "$dir/$name" ]; then
        cp "$dir/$name" "$APPS/bin/"
        log "  $name OK"
        return 0
    fi
    log "  $name FAILED"; tail -25 "$APPS/log/$name.log"; return 1
}

build_sample conjugateGradientUM  || true
build_sample UnifiedMemoryStreams || true
build_sample UnifiedMemoryPerf    || true

# ---------------------------------------------------------------------------
# 4. the runtime libraries the guest will need — EXPLICITLY NOT libcuda
# ---------------------------------------------------------------------------
log "staging CUDA runtime libraries (libcuda deliberately excluded)"
LIBDIR="$CUDA_HOME/targets/x86_64-linux/lib"
# Copy each library ONCE, under its SONAME, and never as a symlink.
#
# The tree reaches the guest over 9p with security_model=mapped, and symlinks
# do not survive that: in the guest `ls` reports
#     cannot read symbolic link 'libcublas.so.12': Too many levels of symbolic links
# and the loader says "libcublas.so.12 => not found" for a file that is plainly
# sitting next to it.  A `cp -a` of NVIDIA's own layout (SONAME symlink ->
# versioned file) therefore stages libraries that cannot be loaded.  Measured on
# the Turing box, 2026-08-24: conjugateGradientUM died at exec with
# "error while loading shared libraries: libcublas.so.12".
# Resolving the symlink and writing the real file under the SONAME sidesteps it
# entirely, and costs one copy instead of two.
for base in libcudart libcublas libcublasLt libcusparse libnvJitLink; do
    src="$(find "$LIBDIR" -maxdepth 1 -name "${base}.so.*" -type f | sort | tail -1)"
    [ -n "$src" ] || { log "  no $base found"; continue; }
    soname="$(objdump -p "$src" 2>/dev/null | awk '/SONAME/{print $2}' | head -1)"
    [ -n "$soname" ] || soname="${base}.so.12"
    cp -L "$src" "$APPS/lib/$soname"
    log "  staged $soname ($(stat -c %s "$APPS/lib/$soname") bytes) from $(basename "$src")"
done
# Belt and braces: a libcuda that reached the guest's LD_LIBRARY_PATH would
# shadow the staged host-driver one and quietly stop testing nvkvm at all.
rm -f "$APPS/lib"/libcuda.so* 2>/dev/null || true
ls -la "$APPS/lib" | head -20

log "binaries:"
ls -la "$APPS/bin"
log "done"
