# nvkvm in a container.
#
# Everything here builds from files committed to this repository; QEMU and the
# guest cloud image are downloaded during the build and first run, the same way
# scripts/build_qemu.sh does it on a bare host.
#
#   docker compose build && docker compose up
#
# The container needs /dev/kvm and, for GPU forwarding, the NVIDIA container
# runtime (`--gpus all`). It does NOT need --privileged. Compose drops every
# capability and adds back only the four used to establish the isolates'
# uid+chroot sandbox; see docker-compose.yml for why dropping those is weaker.

# ── build stage: compile the isolate stub and patched QEMU ────────────────
FROM ubuntu:24.04 AS build

ARG DEBIAN_FRONTEND=noninteractive
# The QEMU build dependencies, by name, rather than `build_qemu.sh
# --install-deps`.  Two reasons, and the second one is why this line exists at
# all: it keeps apt in its own cached layer, so editing src/ rebuilds QEMU
# without re-running a package manager -- and it makes the script take its
# NORMAL path (probe, find everything present, proceed), which is the path a
# user on a bare host follows and the same reasoning .github/workflows/ci
# records for qemu-build.yml.
#
# 2026-08-21: this image did not build before this list was added.  The header
# below used to say the script installs its own dependencies; it stopped doing
# that when it grew a probe-and-report step, and the Dockerfile was not updated
# -- so the build stage died in "[1/9] Checking build dependencies" with
# thirteen missing packages.  Keep this list in step with qemu-build.yml's.
RUN apt-get update -q && apt-get install -y --no-install-recommends \
        build-essential git ca-certificates \
        ninja-build meson libglib2.0-dev libpixman-1-dev \
        python3 python3-venv python3-tomli libslirp-dev pkg-config \
        libattr1-dev libepoxy-dev libgbm-dev libegl-dev libdrm-dev xxd \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/nvkvm
# Only what the build actually reads, so that editing a doc or the compose
# file does not invalidate the QEMU build layer and re-clone 200 MB.  Ordered
# least-volatile first, for the same reason.
#
# patches/ is not optional and was missing here: the QEMU delta moved out of
# inline sed into a patch series, and build_qemu.sh step 3 now exits 1 with
# "no *.patch files found" when the directory is absent -- correctly, since
# without it the build would produce a stock QEMU that has never heard of
# virtio-nvgpu.  The image did not build at all until this line was added
# (2026-08-21).
COPY scripts/build_qemu.sh ./scripts/build_qemu.sh
COPY patches ./patches
COPY src ./src

# build_qemu.sh clones QEMU, applies the nvkvm patch series, builds the isolate
# stub and installs to /opt/qemu-nvkvm + /usr/lib/nvkvm.  It does NOT install
# packages -- see the dependency list above.
RUN bash scripts/build_qemu.sh

# ── runtime stage: QEMU plus the repo, without the build tree ─────────────
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update -q && apt-get install -y --no-install-recommends \
        `# QEMU runtime libraries` \
        libglib2.0-0t64 libpixman-1-0 libslirp0 libepoxy0 libgbm1 libegl1 \
        libdrm2 libattr1 seabios ipxe-qemu \
        `# setup_guest.sh: fetch and prepare the guest disk on first run` \
        wget curl ca-certificates qemu-utils genisoimage \
        `# SteamOS: pinned recovery download + disposable Alpine installer VM` \
        bzip2 cpio gzip tar ovmf util-linux \
        `# reaching the guest, and building nvkvm-guest.ko inside it` \
        openssh-client sshpass \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /opt/qemu-nvkvm /opt/qemu-nvkvm
COPY --from=build /usr/lib/nvkvm  /usr/lib/nvkvm
COPY . /opt/nvkvm
WORKDIR /opt/nvkvm

# .git is intentionally excluded from the image context, but SteamOS needs a
# stable source identity next to the module it builds so it can tell whether a
# later boot is already converged. Hash only module/provisioning inputs: a docs
# edit must not trigger a guest kernel rebuild.
RUN test -d src && test -d boot && test -f tests/validate.sh \
    && find src boot tests/validate.sh -type f -print0 \
        | sort -z | xargs -0 sha256sum | sha256sum | cut -d ' ' -f1 \
        > /opt/nvkvm/.nvkvm-source-id \
    && ln -s /opt/nvkvm/scripts/steamos-ssh.sh /usr/local/bin/nvkvm-steamos-ssh

EXPOSE 2222 15022
ENTRYPOINT ["/opt/nvkvm/scripts/container-entrypoint.sh"]
