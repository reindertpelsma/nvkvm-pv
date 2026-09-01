#!/bin/bash
# stage_guest_libs.sh — version-match the guest's NVIDIA userspace to the host
# driver.  Runs INSIDE the guest, normally over 9p at /mnt/nvkvm; on a guest
# with no 9p client at all (CentOS Stream 9 ships none) the bundle is instead
# copied straight into the guest at /opt/nvkvm/host-libs-<version>, the same
# layout make_host_bundle.sh/container-entrypoint.sh already use on the host
# side of a container.  Kept as a standalone script (not inline in
# run_remote_test.sh) so shell variables expand on the guest, not the local
# shell across the nested ssh host "ssh guest '...'" hops.
#
# Two dirs matter and BOTH must match the host driver or NVML/libcuda refuse to
# init ("Driver/library version mismatch" / cuInit 803):
#   - $SYS (the distro's default ld.so system lib dir, detected below)
#                                    : where NVML/nvidia-smi resolve libnvidia-ml
#   - /usr/local/nvidia-guest/lib    : where CUDA apps resolve libcuda (on the
#                                      ld.so path via nvidia-guest.conf, AHEAD of
#                                      the system dir) + allocator + ptxjit
#
# It also installs the guest's Xorg configuration (/etc/X11/xorg.conf, from
# data/xorg/nvkvm-xorg.conf) at the end.  That is deliberate scope: a guest needs
# three things installed -- the kernel module, this userspace, and one config
# file -- and leaving the third to the user made it read as a defect instead of
# a step.  It never overwrites an xorg.conf someone else wrote, and
# NVKVM_STAGE_XORG=0 skips it.  See the block at the bottom.
#
# The bundle dir defaults to the host's current driver bundle; override with $1.
set -u
# Default bundle path follows the HOST driver version rather than a hardcoded
# 580, so the script works on any host.
#
# This used to be `ls -d /mnt/nvkvm/host-libs-* | head -1`, i.e. whichever
# bundle sorted first ALPHABETICALLY. That is fine while a host has only ever
# had one driver, and silently wrong the moment it has had two: a box that ran
# 580.95.05 and was then moved to 595.84 has both bundles on the 9p share, and
# "580.95.05" < "595.84", so the guest was staged with the OLD userspace
# against the NEW kernel driver and libcuda refused to initialise --
# cuInit rc=803, CUDA_ERROR_SYSTEM_DRIVER_MISMATCH -- with nothing in the
# staging output suggesting the wrong bundle had been chosen.
#
# Select by the version the guest module reports for the HOST driver, and if
# that cannot be read, refuse to guess between multiple candidates.
#
# Search BOTH /mnt/nvkvm (the 9p share) and /opt/nvkvm (the no-9p delivery
# path -- see the file header) rather than only the former.  Found on a
# CentOS Stream 9 guest: the bundle had been copied to
# /opt/nvkvm/host-libs-580.178.04 and this script still only ever globbed
# /mnt/nvkvm/host-libs-*, so both the exact-version match and the
# single-candidate fallback saw zero bundles and it refused to run without
# $1 -- not because no bundle existed, but because this script only knew how
# to look in the one place a 9p-having guest keeps it.
BUNDLE_ROOTS="/mnt/nvkvm /opt/nvkvm"
if [ -n "${1:-}" ]; then
    GFXBUNDLE="$1"
else
    HOSTV="$( { dmesg 2>/dev/null || sudo -n dmesg 2>/dev/null; } \
              | sed -n 's/.*nvkvm: host NVIDIA driver \([0-9][0-9.]*\).*/\1/p' | tail -1 )"
    GFXBUNDLE=""
    if [ -n "$HOSTV" ]; then
        for _root in $BUNDLE_ROOTS; do
            if [ -d "$_root/host-libs-$HOSTV" ]; then
                GFXBUNDLE="$_root/host-libs-$HOSTV"
                echo "stage_guest_libs: host driver is $HOSTV -> $GFXBUNDLE"
                break
            fi
        done
    fi
    if [ -z "$GFXBUNDLE" ]; then
        CANDIDATES="$(ls -d /mnt/nvkvm/host-libs-* /opt/nvkvm/host-libs-* 2>/dev/null)"
        N=$(printf '%s\n' "$CANDIDATES" | grep -c .)
        if [ "$N" -eq 1 ]; then
            GFXBUNDLE="$CANDIDATES"
        else
            echo "stage_guest_libs: cannot choose between $N bundles and the host" >&2
            echo "  driver version could not be read from dmesg${HOSTV:+ (got $HOSTV, no matching bundle)}." >&2
            echo "  Pass one explicitly:" >&2
            printf '%s\n' "$CANDIDATES" | sed 's/^/    /' >&2
            exit 1
        fi
    fi
fi
# Guest system lib dir the dynamic linker actually searches by default.
# Debian/Ubuntu ships the multiarch path /usr/lib/x86_64-linux-gnu, wired into
# ld.so's default search path by the distro's own ld.so.conf.d; RHEL-family
# distros (CentOS Stream, RHEL, Fedora, Rocky, Alma) never wire that path in
# at all and use the traditional /usr/lib64 instead.  This used to be
# hardcoded to the Debian path, so on a RHEL guest every stage() call
# targeting $SYS "succeeded" (the copy itself works fine into a dir that
# happens to exist) but landed libs somewhere ld.so never looks, so
# nvidia-smi still failed with "couldn't find libnvidia-ml.so" even though
# the file was sitting right there and the script reported all 24 staged.
#
# Detect via /etc/redhat-release (present on every RHEL-family distro,
# including CentOS Stream, since forever) rather than by testing whether
# /usr/lib/x86_64-linux-gnu EXISTS: on a guest that had already been
# (mis-)staged once, an earlier run of the old hardcoded version had already
# created that directory and left stale libs in it, so existence alone is not
# evidence it is on the linker's search path -- confirmed on the real T4
# guest, where nvidia-smi kept failing after a first "fixed" run for exactly
# this reason.
if [ -f /etc/redhat-release ]; then
    SYS=/usr/lib64
else
    SYS=/usr/lib/x86_64-linux-gnu
fi
CUDADIR=/usr/local/nvidia-guest/lib
sudo mkdir -p "$CUDADIR"   # ln -s below fails if it does not exist

V=$(ls "$GFXBUNDLE"/libcuda.so.* 2>/dev/null | sed "s#.*/libcuda.so.##" | head -1)
if [ -z "$V" ]; then
    echo "stage_guest_libs: no libcuda in $GFXBUNDLE" >&2
    exit 1
fi
echo "stage_guest_libs: staging $V from $GFXBUNDLE"

# ── stage helper ─────────────────────────────────────────────────────────────
# Every copy in this script used to be `cp -f ... 2>/dev/null`, so a lib that
# was absent from the bundle produced NO output at all and the guest silently
# came up without it (this is how the whole GL/Vulkan stack went missing while
# the script still printed "done").  Route every copy through stage(): it says
# STAGED or MISSING, and MISSING is remembered for the summary at the end.
MISSING_LIBS=""
# Files we install from the repo share rather than from the driver bundle (the
# Xorg config below).  Tracked separately because a missing one of these does
# not mean a broken GPU stack, so it must not be reported as a missing library.
MISSING_FILES=""
STAGED_N=0
# The CUDA-critical set.  These are the same four make_host_bundle.sh calls
# REQUIRED, and the distinction matters to the CALLER: without them CUDA does
# not work at all, whereas a missing Wayland/GBM EGL platform library only costs
# a display path a headless host never had.  Both used to land in one bucket and
# exit 2, so nvkvm-guest.service could only spell its tolerance as `|| true` --
# which then swallowed the fatal case as well.  MEASURED 2026-09-01 on an
# RTX 4060 Ti: libnvidia-ptxjitcompiler failed to stage, the service reported
# success, and the first sign of trouble was cuda_ptx_jit failing
# CUDA_ERROR_JIT_COMPILER_NOT_FOUND with three more CUDA checks cascading to
# SKIP -- three drivers in a row, diagnosed only by reading validate.sh's own
# per-check detail.
is_critical_lib(){
    case "$1" in
        libcuda.so.*|libnvidia-ptxjitcompiler.so.*|libnvidia-nvvm.so.*|libnvidia-ml.so.*) return 0 ;;
        *) return 1 ;;
    esac
}
CRITICAL_MISSING=""

stage(){ # $1 basename-in-bundle  $2 destdir  [$3.. extra symlink names]
    local f="$1" dest="$2"; shift 2
    if [ ! -f "$GFXBUNDLE/$f" ]; then
        echo "stage_guest_libs: MISSING from bundle: $f  (-> $dest)" >&2
        MISSING_LIBS="$MISSING_LIBS $f"
        is_critical_lib "$f" && CRITICAL_MISSING="$CRITICAL_MISSING $f"
        return 1
    fi
    if [ -n "${NVKVM_LINK_LIBS:-}" ]; then
        # Link mode.  The driver payload stays where it is -- normally a
        # read-only 9p share from the host or container -- and only the link
        # lands in the guest filesystem.  A guest `apt upgrade` then cannot
        # overwrite a driver library, because the guest has no driver library
        # to overwrite: it has a symlink into a read-only mount.
        sudo ln -sf "$GFXBUNDLE/$f" "$dest/$f" || { echo "stage_guest_libs: LINK FAILED: $f" >&2; return 1; }
    else
        sudo cp -f "$GFXBUNDLE/$f" "$dest/" || { echo "stage_guest_libs: COPY FAILED: $f" >&2; return 1; }
    fi
    local l; for l in "$@"; do sudo ln -sf "$f" "$dest/$l"; done
    STAGED_N=$((STAGED_N+1))
    return 0
}

# -- system dir: NVML for nvidia-smi (+ libcuda for completeness) --
# The bare "libnvidia-ml.so" is staged alongside the SONAME for the same reason
# libcuda.so is below, but for dlopen rather than for the linker: third-party
# tools that query NVML commonly dlopen the UNVERSIONED name.  Geekbench 7 does,
# and without this link it fails with
#     Failed to load nvmlInit_v2: <its own binary>: undefined symbol: nvmlInit_v2
# which reads as a broken NVML or a broken binary and is neither -- the dlopen
# of "libnvidia-ml.so" simply found no such file, so it fell back to
# dlsym(RTLD_DEFAULT) and reported the failure against itself.  Measured: with
# the .so.1 link alone, dlopen("libnvidia-ml.so.1") succeeds and
# dlopen("libnvidia-ml.so") fails; with both, NVML works and nvidia-smi -L
# reports the same GPU and UUID as the host.
#
# NOTE, corrected 2026-08-20: this is NOT an nvkvm defect and the guest is not
# unusual here.  The unversioned name ships in the driver's -dev package, and a
# host with only the runtime package (libnvidia-compute-NNN) does not have it
# either -- measured on an A100 host, where Geekbench failed exactly the same
# way outside any VM.  We stage the link anyway, because a guest that works
# where the host does not is the better failure direction for a staging script,
# and because the alternative is every NVML consumer in the guest failing with
# a message that blames itself.
stage "libnvidia-ml.so.$V" "$SYS" "libnvidia-ml.so.1" "libnvidia-ml.so"
stage "libcuda.so.$V"      "$SYS" "libcuda.so.1" "libcuda.so"
# Remove a STALE differently-versioned copy, never the one just staged.
# This line used to hardcode 575.51.03; on a host actually running 575.51.03 it
# deleted the freshly-copied libs, and the cp above is 2>/dev/null so nothing
# said so -- nvidia-smi then reported "couldn't find libnvidia-ml.so".
# NB: the glob also matches the SONAME symlink (libcuda.so.1), so skip symlinks
# — only differently-versioned REGULAR files are stale.  (It happened to survive
# before only because ldconfig re-creates SONAME links from the staged files.)
for stale in "$SYS"/libcuda.so.[0-9]* "$SYS"/libnvidia-ml.so.[0-9]*; do
    [ -e "$stale" ] || continue
    [ -L "$stale" ] && continue
    case "$stale" in *".so.$V") continue ;; esac
    sudo rm -f "$stale"
done

# -- video engines: NVENC encode + NVDEC/cuvid.  libnvidia-encode.so depends on
# libnvcuvid.so, so BOTH must be present + version-matched or ffmpeg/NVENC says
# "Cannot load libnvidia-encode.so.1".  (NVENC session InitializeEncoder beyond
# this is a separate deeper forwarder gap — tracked as its own task.) --
for vlib in libnvidia-encode libnvcuvid; do
    stage "$vlib.so.$V" "$SYS" "$vlib.so.1" "$vlib.so"
done

# -- GL / EGL / Vulkan userspace.  The bundle already carried the *core* libs
# (glcore/eglcore/glsi/gpucomp/rtcore/tls/nvvm) but NOTHING in this script ever
# copied them into the guest, and the GLVND *vendor* libs (libEGL_nvidia,
# libGLX_nvidia — which is also the Vulkan ICD — libGLESv2_nvidia) plus
# libnvidia-glvkspirv were not in the bundle at all.  Net effect: the guest had
# the 10_nvidia.json EGL vendor config written below pointing at a
# libEGL_nvidia.so.0 that did not exist, no Vulkan ICD, and every GL/Vulkan app
# either failed to init or silently fell back to Mesa llvmpipe (software) —
# while this script still reported success.  Stage the whole closure.
for core in libnvidia-glcore libnvidia-eglcore libnvidia-glsi libnvidia-gpucomp \
            libnvidia-rtcore libnvidia-tls libnvidia-nvvm; do
    stage "$core.so.$V" "$SYS"          # loaded by SONAME-less direct name; no symlink
done
stage "libnvidia-glvkspirv.so.$V" "$SYS"
stage "libEGL_nvidia.so.$V"       "$SYS" "libEGL_nvidia.so.0"
stage "libGLX_nvidia.so.$V"       "$SYS" "libGLX_nvidia.so.0"
stage "libGLESv2_nvidia.so.$V"    "$SYS" "libGLESv2_nvidia.so.2"
stage "libGLESv1_CM_nvidia.so.$V" "$SYS" "libGLESv1_CM_nvidia.so.1"
stage "libnvidia-cfg.so.$V"       "$SYS" "libnvidia-cfg.so.1"
# OpenCL vendor library.  This was opt-in and off by default from 2026-08-16 to
# 2026-08-19, because OpenCL through nvkvm returned WRONG RESULTS: Geekbench 7
# --gpu failed validation on 11 workloads in the guest while the same binary was
# clean on the host.  That was never an OpenCL bug -- it was a guest-side
# migration cache keyed on a virtual address that a freed buffer had given back
# (docs/reference/correctness.md), and CUDA hit it too.  With that fixed the
# same Geekbench run completes every workload with zero validation failures, and
# tests/repro/opencl_correctness.c passes, so OpenCL is staged by default again.
# Set NVKVM_STAGE_OPENCL=0 to leave it out.
if [ "${NVKVM_STAGE_OPENCL:-1}" = "1" ]; then
    stage "libnvidia-opencl.so.$V"    "$SYS" "libnvidia-opencl.so.1"
fi

# -- EGL GBM stack (#102 modeset): GPU-accelerated GL/EGL on the virtual KMS
# head needs THREE pieces, all of which must be present or the NVIDIA path is
# silently skipped and Mesa falls back to llvmpipe (software):
#
#   1. The GBM *backend* — Mesa's libgbm dlopens "<drmdriver>_gbm.so" from the
#      gbm backends dir by the card's DRM driver name ("nvidia-drm"). The NVIDIA
#      backend IS libnvidia-allocator (the host ships nvidia-drm_gbm.so as a
#      symlink to it). Without this, gbm_create_device() on card0 returns a Mesa
#      "dri" device and the NVIDIA EGL platform never even gets a chance. THIS
#      was the whole "EGL fails to init on the head" wall (#102 chunk 5).
#   2. libnvidia-egl-gbm.so.1 — the EGL external platform that handles
#      EGL_PLATFORM_GBM on an NVIDIA gbm device (config 15_nvidia_gbm.json).
#   3. libnvidia-allocator in the SYSTEM lib dir so the backend symlink resolves
#      for non-CUDA GL apps (compositors don't add /usr/local/nvidia-guest/lib).
GBMDIR="$SYS/gbm"
sudo mkdir -p "$GBMDIR"
# (1)+(3): allocator in the system dir + the GBM backend symlink to it.
if [ -f "$GFXBUNDLE/libnvidia-allocator.so.$V" ]; then
    sudo cp -f "$GFXBUNDLE/libnvidia-allocator.so.$V" "$SYS/"
    sudo ln -sf "libnvidia-allocator.so.$V"    "$SYS/libnvidia-allocator.so.1"
    sudo ln -sf "../libnvidia-allocator.so.$V" "$GBMDIR/nvidia-drm_gbm.so"
fi
# (2): the EGL external platforms (their own versions, not driver $V) and the
# egl_external_platform.d configs that tell EGL they exist.
#
# The configs matter as much as the libraries.  A Wayland GL client (es2gears,
# glmark2, any toolkit) calls eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND); with
# no 10_nvidia_wayland.json the NVIDIA external platform is never loaded, so
# glvnd hands the client to MESA's libEGL, which tries DRI2 on the nvkvm PCI
# device and fails:
#     libEGL warning: pci id for fd 4: 10de:2504, driver (null)
#     libEGL warning: egl: failed to create dri2 screen
# The client still connects and maps a surface, so the compositor composites a
# correctly-sized window that is entirely BLACK — which looks like a working
# desktop in a screenshot unless you check the client actually drew something.
# That is exactly what happened here before these two files were staged.
EGLEXT=/usr/share/egl/egl_external_platform.d
sudo mkdir -p "$EGLEXT"
for plat in wayland gbm; do
    lib="libnvidia-egl-$plat"
    found=""
    for f in "$GFXBUNDLE/$lib".so.*; do
        [ -e "$f" ] || continue
        b=$(basename "$f")
        sudo cp -f "$f" "$SYS/" && found="$b"
    done
    if [ -n "$found" ]; then
        sudo ln -sf "$found" "$SYS/$lib.so.1"
        STAGED_N=$((STAGED_N+1))
        case "$plat" in
            wayland) cfg="$EGLEXT/10_nvidia_wayland.json" ;;
            gbm)     cfg="$EGLEXT/15_nvidia_gbm.json"     ;;
        esac
        printf '{\n    "file_format_version" : "1.0.0",\n    "ICD" : {\n        "library_path" : "%s.so.1"\n    }\n}\n' \
            "$lib" | sudo tee "$cfg" >/dev/null
    else
        echo "stage_guest_libs: MISSING from bundle: $lib.so.*" >&2
        MISSING_LIBS="$MISSING_LIBS $lib.so.*"
    fi
done

# -- canonical CUDA dir: libcuda + allocator + ptxjit (what apps actually load) --
# The bare "libcuda.so" linker name is staged alongside the SONAME because
# JIT-compiling stacks link against it at RUNTIME: Triton (and therefore vLLM,
# torch.compile, and anything using Inductor) shells out to
# `gcc ... -lcuda -L<cuda dir>` to build its cuda_utils shim on first use, and
# `-lcuda` resolves ONLY through the unversioned symlink.  Without it the guest
# fails with "/usr/bin/ld: cannot find -lcuda" wrapped in an InductorError,
# which reads as a compiler problem rather than a missing symlink.  The host has
# this link from the normal driver package; the guest only gets what we stage.
stage "libcuda.so.$V"                  "$CUDADIR" "libcuda.so.1" "libcuda.so"
stage "libnvidia-allocator.so.$V"      "$CUDADIR" "libnvidia-allocator.so.1"
stage "libnvidia-ptxjitcompiler.so.$V" "$CUDADIR" "libnvidia-ptxjitcompiler.so.1"
# ptxjitcompiler dlopens libnvidia-nvvm.so.4 (driver >= 12.0). Staging the JIT
# compiler WITHOUT nvvm just moves the failure one layer down: cuInit and plain
# memcpy still pass, and every cuModuleLoadData(PTX) dies with
# CUDA_ERROR_JIT_COMPILER_NOT_FOUND (221) -- which reads as "kernels are broken",
# not as "a library is missing". Found during Ada/Turing bring-up 2026-08-17.
stage "libnvidia-nvvm.so.$V"           "$CUDADIR" "libnvidia-nvvm.so.1" "libnvidia-nvvm.so.4"
# Sweep STALE differently-versioned copies out of the CUDA dir.  This used to be
# an `rm -f $CUDADIR/lib*.so.575.51.03` with the version HARDCODED — the exact
# bug the $SYS block above documents as fixed, left unfixed here.  On a host
# actually running 575.51.03 it deleted the three files the lines above had just
# copied, leaving libcuda.so.1 & friends as DANGLING symlinks in the dir that
# ld.so searches FIRST.  Apps still worked only by accident (ld.so skips the
# dangling link and falls through to $SYS), which is why the dir looked like it
# "contains symlinks whose targets are only in /usr/lib/x86_64-linux-gnu".
# Same version-aware sweep as $SYS: never delete what was just staged.
for stale in "$CUDADIR"/*.so.[0-9]*; do
    [ -e "$stale" ] || continue
    [ -L "$stale" ] && continue      # SONAME links are ours, not stale versions
    case "$stale" in *".so.$V") continue ;; esac
    sudo rm -f "$stale"
done

# version-matched nvidia-smi binary.  The bundle name used to be hardcoded as
# "nvidia-smi-580", so on any bundle that names it plainly "nvidia-smi" (or
# nvidia-smi-575) this silently did nothing and the guest was left with either
# no nvidia-smi or a mismatched distro one.  Accept any of the spellings.
for smi in "$GFXBUNDLE/nvidia-smi-$V" "$GFXBUNDLE/nvidia-smi-${V%%.*}" "$GFXBUNDLE/nvidia-smi"; do
    [ -f "$smi" ] || continue
    sudo cp -f "$smi" /usr/local/bin/nvidia-smi && sudo chmod +x /usr/local/bin/nvidia-smi
    echo "stage_guest_libs: nvidia-smi <- $(basename "$smi")"
    break
done
[ -x /usr/local/bin/nvidia-smi ] || echo "stage_guest_libs: MISSING from bundle: nvidia-smi" >&2

# Put $CUDADIR on the loader path.  The header of this script has always claimed
# CUDA apps resolve libcuda there "via nvidia-guest.conf, AHEAD of the system
# dir" — but nothing ever created that file, so the dir was off the ld.so path
# entirely and every app silently resolved libcuda from $SYS instead.
# CHECK THE CONTENT, NOT THE EXISTENCE.
#
# This was `[ ! -f ... ]`, and a file that exists but is EMPTY then never got
# repaired -- on any boot, ever.  MEASURED 2026-09-01 in a sweep guest:
#
#     conf exists: yes    conf bytes: 0    ptxjit resolvable: 0
#     FAIL cuda_ptx_jit rc=221 CUDA_ERROR_JIT_COMPILER_NOT_FOUND
#
# ...on five consecutive drivers, while the preinstalled-driver control passed
# 35P/0F/0S.  The libraries were staged perfectly; $CUDADIR simply was not on
# the loader path, so libnvidia-ptxjitcompiler -- which lives ONLY there -- was
# invisible.  libnvidia-nvvm survived because it also lands in $SYS, which is
# why the error names both and only one was really absent.
#
# Zero length with intact metadata is the signature of ext4 delayed allocation
# losing a recent write when the guest goes down uncleanly, and a driver swap
# does exactly that.  An existence-only idempotence check cannot see a
# half-finished write; this project has been bitten by that shape before (a
# version-only check that skipped repairing an OOM-truncated driver install).
if ! grep -qxF "$CUDADIR" /etc/ld.so.conf.d/nvidia-guest.conf 2>/dev/null; then
    echo "$CUDADIR" | sudo tee /etc/ld.so.conf.d/nvidia-guest.conf >/dev/null
    sudo sync 2>/dev/null || true   # do not let the next unclean stop zero it again
    echo "stage_guest_libs: wrote /etc/ld.so.conf.d/nvidia-guest.conf -> $CUDADIR"
fi

# -- sweep differently-versioned NVIDIA libraries out of $SYS BEFORE ldconfig --
#
# THIS MUST HAPPEN BEFORE ldconfig, AND IT MATTERS.  stage() creates the SONAME
# symlink itself with `ln -sf`, so immediately after staging the links are
# correct.  But every NVIDIA library of a given kind carries the SAME SONAME
# regardless of version -- libEGL_nvidia.so.550.54.14 and
# libEGL_nvidia.so.610.43.02 both declare SONAME libEGL_nvidia.so.0 -- so when
# ldconfig then scans this directory and finds two candidates it version-sorts
# them and re-points libEGL_nvidia.so.0 at the HIGHER-numbered one.  If the
# guest was previously staged against a newer driver, that is the STALE file,
# and ldconfig silently undoes the staging that just happened.
#
# Observed exactly this way: a guest staged 610.43.02, then re-staged 550.54.14
# after the host driver was changed.  All 22 libs copied, the script printed
# "done", and libEGL_nvidia.so.0/libGLX_nvidia.so.0 still pointed at the 610
# files.  CUDA was fine (libcuda resolves out of $CUDADIR, which has no second
# version in it), but the 610 GL/Vulkan vendor libraries could not initialise
# against a 550 kernel driver, so EGL and Vulkan fell back to Mesa llvmpipe --
# a SOFTWARE rasteriser -- while every log line said success.
#
# Only regular files are candidates (never the SONAME symlinks), and only ones
# whose suffix looks like a real driver version (3-digit major), so that
# genuinely differently-versioned components such as
# libnvidia-egl-gbm.so.1.1.1 are left alone.
for stale in "$SYS"/libnvidia-*.so.[0-9]* "$SYS"/libcuda.so.[0-9]* \
             "$SYS"/libnvcuvid.so.[0-9]* "$SYS"/libEGL_nvidia.so.[0-9]* \
             "$SYS"/libGLX_nvidia.so.[0-9]* "$SYS"/libGLESv2_nvidia.so.[0-9]* \
             "$SYS"/libGLESv1_CM_nvidia.so.[0-9]*; do
    [ -e "$stale" ] || continue
    [ -L "$stale" ] && continue
    case "$stale" in *".so.$V") continue ;; esac
    case "${stale##*.so.}" in [1-9][0-9][0-9].*) ;; *) continue ;; esac
    echo "stage_guest_libs: removing stale $(basename "$stale") (host driver is $V)"
    sudo rm -f "$stale"
done

sudo ldconfig

# Re-assert the GLVND vendor / ICD SONAME links after ldconfig, and verify.
# ldconfig is the component that historically broke these, so do not trust it:
# check where each link actually points and say so.
for pair in "libEGL_nvidia.so.0:libEGL_nvidia.so.$V" \
            "libGLX_nvidia.so.0:libGLX_nvidia.so.$V" \
            "libGLESv2_nvidia.so.2:libGLESv2_nvidia.so.$V"; do
    link="$SYS/${pair%%:*}"; want="${pair##*:}"
    [ -f "$SYS/$want" ] || continue
    sudo ln -sf "$want" "$link"
    got="$(readlink "$link")"
    if [ "$got" = "$want" ]; then
        echo "stage_guest_libs: $(basename "$link") -> $got"
    else
        echo "stage_guest_libs: WARNING $(basename "$link") -> $got (wanted $want)" >&2
    fi
done

# GLVND EGL vendor config for NVIDIA.  Without /usr/share/glvnd/egl_vendor.d/
# 10_nvidia.json, libGLX_nvidia never enters its NVIDIA device-detection path
# (it only sees mesa) and the Vulkan ICD bows out to llvmpipe.  Points at
# libEGL_nvidia.so.0 (staged above).
sudo mkdir -p /usr/share/glvnd/egl_vendor.d
sudo tee /usr/share/glvnd/egl_vendor.d/10_nvidia.json >/dev/null <<'JSON'
{
    "file_format_version" : "1.0.0",
    "ICD" : {
        "library_path" : "libEGL_nvidia.so.0"
    }
}
JSON

# OpenCL ICD manifest.  The OpenCL loader (libOpenCL.so.1) finds vendors only
# through /etc/OpenCL/vendors/*.icd; with no manifest it enumerates zero
# platforms and apps fail with "unknown OpenCL platform" (e.g. Geekbench --gpu)
# even though libnvidia-opencl is present.  Written only when that library
# actually staged, so a host without it does not get a manifest pointing at a
# missing library (which would be worse than none: the loader would log an
# error for every OpenCL app).
if [ "${NVKVM_STAGE_OPENCL:-1}" = "1" ] &&
   { [ -e /usr/local/nvidia-guest/lib/libnvidia-opencl.so.1 ] ||
     [ -e "$SYS/libnvidia-opencl.so.1" ]; }; then
    sudo mkdir -p /etc/OpenCL/vendors
    printf 'libnvidia-opencl.so.1\n' | sudo tee /etc/OpenCL/vendors/nvidia.icd >/dev/null
    echo "stage_guest_libs: OpenCL ICD -> /etc/OpenCL/vendors/nvidia.icd"
else
    echo "stage_guest_libs: libnvidia-opencl not staged -- OpenCL apps will find no platform" >&2
fi

# Vulkan ICD manifest.  The Vulkan ICD on Linux IS libGLX_nvidia.so.0 (staged
# above).  Without this manifest the loader enumerates only Mesa's lavapipe, so
# vulkaninfo reports "llvmpipe" and a Vulkan parity run silently measures the
# CPU rasterizer instead of the GPU.
sudo mkdir -p /usr/share/vulkan/icd.d
sudo tee /usr/share/vulkan/icd.d/nvidia_icd.json >/dev/null <<'JSON'
{
    "file_format_version" : "1.0.1",
    "ICD": {
        "library_path": "libGLX_nvidia.so.0",
        "api_version" : "1.4.303"
    }
}
JSON

# Blacklist nouveau: the emulated NVIDIA-id PCI device (nvkvm-gpu, the DRM render
# node's parent) would otherwise have nouveau auto-bind and probe it.  The device
# has no BARs, so nouveau can only fail/noise — keep it off the device entirely.
if [ ! -f /etc/modprobe.d/blacklist-nvkvm-nouveau.conf ]; then
    echo "blacklist nouveau" | sudo tee /etc/modprobe.d/blacklist-nvkvm-nouveau.conf >/dev/null
    echo "stage_guest_libs: blacklisted nouveau (reboot to take effect)"
fi

# ── the guest's own Xorg session ─────────────────────────────────────────────
#
# This is part of INSTALLATION, not a workaround the user is expected to find.
# A guest already needs a kernel module built against its own kernel and an
# NVIDIA userspace matched to the host driver; one config file is the smallest
# item on that list, and this script is already the thing that does the other
# one.  So install it here and it stops being a user-facing step at all.
#
# What it steers around: a stock distro left alone picks one of the two X paths
# that CANNOT work on the nvkvm head -- NVIDIA's own DDX (it reaches the GPU
# fine and then asks NVKMS about the HOST's physical displays) or `modesetting`
# with glamor (whose scanout-pixmap import NVIDIA's EGL rejects, on bare metal
# too).  The file selects the third path: `modesetting` with
# AccelMethod "none", GL clients accelerated on NVIDIA via render offload.
#
# IT MUST BE /etc/X11/xorg.conf, AND NOT AN xorg.conf.d DROP-IN.  This is the
# line someone will "tidy up" into a drop-in, so: with a drop-in, BOTH
# OutputClasses still apply, Xorg tries the NVIDIA DDX FIRST, that fails the
# screen, and the server EXITS rather than falling through to the second one.
# An explicit Device section in /etc/X11/xorg.conf outranks OutputClass driver
# selection, which is also why this needs nothing the distro ships removed.
#
# NVKVM_STAGE_XORG=0 skips this entirely -- for a guest that will never run an X
# server, or whose X configuration is not ours to touch.
XORGCONF=/etc/X11/xorg.conf
if [ "${NVKVM_STAGE_XORG:-1}" != "1" ]; then
    echo "stage_guest_libs: NVKVM_STAGE_XORG=$NVKVM_STAGE_XORG -- leaving $XORGCONF alone"
else
    SRC_XORG="$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/../data/xorg/nvkvm-xorg.conf"
    if [ ! -f "$SRC_XORG" ]; then
        # Only reachable if the script was copied out of the repo tree; the 9p
        # share carries the whole checkout.  Not fatal -- compute does not need
        # an X server -- so warn and carry on rather than failing the staging.
        echo "stage_guest_libs: MISSING from the repo share: data/xorg/nvkvm-xorg.conf" >&2
        echo "  (looked in $SRC_XORG) -- the guest's own X session will need it" >&2
        echo "  installed by hand as $XORGCONF; see docs/howto/run.md." >&2
        MISSING_FILES="$MISSING_FILES data/xorg/nvkvm-xorg.conf"
    else
        # BusID has to name nvkvm's PCI device in THIS guest.  The shipped file
        # says PCI:0:7:0 because run_test_vm.sh puts it at addr=7, but that is a
        # default and not a guarantee, so read the real address out of the
        # guest's own PCI tree.  nvkvm-gpu is the only NVIDIA-vendor device a
        # guest has (the virtio transport is 0x1af4), so the first 0x10de match
        # is it.  NOTE the bases differ: a BDF is HEX and Xorg's BusID is
        # DECIMAL, so 0000:00:1f.0 is "PCI:0:31:0" and copying the string across
        # would silently name a device that does not exist.
        NVBDF=""
        for _d in /sys/bus/pci/devices/*; do
            [ -r "$_d/vendor" ] || continue
            [ "$(cat "$_d/vendor")" = "0x10de" ] || continue
            NVBDF="$(basename "$_d")"
            break
        done
        XORGTMP="$(mktemp)"
        if [ -n "$NVBDF" ]; then
            _rest="${NVBDF#*:}"                  # 0000:00:07.0 -> 00:07.0
            _bus="${_rest%%:*}"                  #               -> 00
            _devfn="${_rest#*:}"                 #               -> 07.0
            BUSID="$(printf 'PCI:%d:%d:%d' "0x$_bus" "0x${_devfn%%.*}" "0x${_devfn#*.}")"
            # Anchored on the BusID keyword so the worked example in the file's
            # own header comment is left as written.
            sed -E "s#(BusID[[:space:]]+)\"PCI:[0-9]+:[0-9]+:[0-9]+\"#\\1\"$BUSID\"#" \
                "$SRC_XORG" > "$XORGTMP"
        else
            # No NVIDIA-vendor device: either graphics=off, or the guest module
            # is not up yet.  Install it verbatim -- an X server is the only
            # reader of this file and there is none running right now -- but say
            # that the BusID is the shipped default and unverified.
            BUSID="$(sed -n 's#.*BusID[[:space:]]*"\(PCI:[0-9:]*\)".*#\1#p' "$SRC_XORG" | head -1)"
            cp -f "$SRC_XORG" "$XORGTMP"
            echo "stage_guest_libs: no NVIDIA-vendor PCI device in this guest --" >&2
            echo "  $XORGCONF gets the shipped BusID $BUSID, UNVERIFIED." >&2
            echo "  Check it with: lspci -nn | grep NVIDIA" >&2
        fi

        if [ -f "$XORGCONF" ] && ! grep -q 'nvkvm-xorg.conf' "$XORGCONF"; then
            # Someone else's X configuration.  Leave it exactly as it is:
            # overwriting a user's xorg.conf unasked is not acceptable, and a
            # desktop that stops coming up is a far worse outcome than a manual
            # merge.  Say so loudly -- silently skipping would leave a guest
            # whose X session does not work with nothing pointing at why.
            echo "stage_guest_libs: WARNING $XORGCONF EXISTS AND IS NOT OURS -- LEFT UNTOUCHED" >&2
            echo "  Your file is in charge; a stock X session may not come up on the nvkvm" >&2
            echo "  head without the settings in:" >&2
            echo "      $SRC_XORG" >&2
            echo "  Compare with:  diff $XORGCONF $SRC_XORG" >&2
            echo "  Then either merge its Device section in (Driver \"modesetting\"," >&2
            echo "  BusID \"$BUSID\", Option \"AccelMethod\" \"none\") or install it wholesale." >&2
            echo "  Set NVKVM_STAGE_XORG=0 to stop this script looking at all." >&2
        elif [ -f "$XORGCONF" ] && cmp -s "$XORGTMP" "$XORGCONF"; then
            echo "stage_guest_libs: $XORGCONF already current (BusID $BUSID)"
        else
            # Either absent, or an older copy of ours (the marker in line 1 is
            # what makes that distinguishable) -- e.g. staged before the device
            # moved slots.  Ours to update.
            sudo mkdir -p "$(dirname "$XORGCONF")"
            if sudo cp -f "$XORGTMP" "$XORGCONF" && sudo chmod 644 "$XORGCONF"; then
                echo "stage_guest_libs: installed $XORGCONF (BusID $BUSID)"
                echo "stage_guest_libs:   run GL clients with __NV_PRIME_RENDER_OFFLOAD=1" \
                     "__GLX_VENDOR_LIBRARY_NAME=nvidia"
            else
                echo "stage_guest_libs: FAILED to install $XORGCONF" >&2
                MISSING_FILES="$MISSING_FILES $XORGCONF"
            fi
        fi
        rm -f "$XORGTMP"
    fi
fi

# Summary.  This script used to print "done" unconditionally, which read as
# success even when most of the bundle was absent.  Report what was missing and
# exit non-zero so a caller/CI notices.
echo "stage_guest_libs: staged $STAGED_N libs from $GFXBUNDLE (driver $V)"
if [ -n "$MISSING_FILES" ]; then
    # Not exit-worthy on its own: these are guest CONFIG files, so the GPU stack
    # works without them and only the guest's own X session is affected.
    echo "stage_guest_libs: not installed from the repo share:" >&2
    for m in $MISSING_FILES; do echo "    $m" >&2; done
fi
if [ -n "$CRITICAL_MISSING" ]; then
    # EXIT 3 == "CUDA IS BROKEN IN THIS GUEST".  Distinct from 2 so a caller can
    # tolerate a cosmetic gap and still refuse to pretend this one is fine.
    echo "stage_guest_libs: FATAL — CUDA-critical libraries missing from bundle:" >&2
    for m in $CRITICAL_MISSING; do echo "    $m" >&2; done
    echo "stage_guest_libs: CUDA will fail in this guest (expect CUDA_ERROR_JIT_COMPILER_NOT_FOUND" >&2
    echo "stage_guest_libs: or CUDA_ERROR_SYSTEM_DRIVER_MISMATCH). This is not a display-only gap." >&2
    [ -n "$MISSING_LIBS" ] && { echo "stage_guest_libs: all missing:" >&2
                                for m in $MISSING_LIBS; do echo "    $m" >&2; done; }
    exit 3
fi
if [ -n "$MISSING_LIBS" ]; then
    echo "stage_guest_libs: INCOMPLETE — missing from bundle:" >&2
    for m in $MISSING_LIBS; do echo "    $m" >&2; done
    echo "stage_guest_libs: GL/EGL/Vulkan/NVENC workloads may fall back to software or fail" >&2
    exit 2
fi
echo "stage_guest_libs: done ($V)"
