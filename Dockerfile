# nvkvm in a container.
#
# Everything here builds from files committed to this repository; QEMU and the
# guest cloud image are downloaded during the build and first run, the same way
# scripts/build_qemu.sh does it on a bare host.
#
#   docker compose build && docker compose up
#
# The container needs /dev/kvm and, for GPU forwarding, the NVIDIA container
# runtime (`--gpus all`).  It does NOT need --privileged or extra capabilities:
# see the FAQ entry "Can nvkvm itself run inside a container?".

# ── build stage: compile the isolate stub and patched QEMU ────────────────
FROM ubuntu:24.04 AS build

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update -q && apt-get install -y --no-install-recommends \
        build-essential git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/nvkvm
# Only what the build actually reads, so that editing a doc or the compose
# file does not invalidate the QEMU build layer and re-clone 200 MB.
COPY scripts/build_qemu.sh ./scripts/build_qemu.sh
COPY src ./src

# build_qemu.sh installs its own build dependencies, clones QEMU, applies the
# nvkvm device and installs to /opt/qemu-nvkvm + /usr/lib/nvkvm.
RUN bash scripts/build_qemu.sh

# ── runtime stage: QEMU plus the repo, without the build tree ─────────────
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update -q && apt-get install -y --no-install-recommends \
        `# QEMU runtime libraries` \
        libglib2.0-0t64 libpixman-1-0 libslirp0 libepoxy0 libgbm1 libegl1 \
        libdrm2 libattr1 seabios ipxe-qemu \
        `# setup_guest.sh: fetch and prepare the guest disk on first run` \
        wget ca-certificates qemu-utils genisoimage \
        `# reaching the guest, and building nvkvm-guest.ko inside it` \
        openssh-client sshpass \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /opt/qemu-nvkvm /opt/qemu-nvkvm
COPY --from=build /usr/lib/nvkvm  /usr/lib/nvkvm
COPY . /opt/nvkvm

WORKDIR /opt/nvkvm
EXPOSE 2222
ENTRYPOINT ["/opt/nvkvm/scripts/container-entrypoint.sh"]
