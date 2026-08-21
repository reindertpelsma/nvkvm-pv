#!/bin/bash
# Run integration tests on the remote vast.ai host's VM.
# Assumes:
#   - a remote GPU host you can ssh to, given by NVKVM_REMOTE
#         export NVKVM_REMOTE='ssh -p 22 root@your-gpu-host'
#         export NVKVM_REMOTE_DIR=/root/nvkvm      # where the tree lives there
#   - VM SSH at port 2222 on the vast.ai host
#   - 9p mount tag `nvkvm_src` exposing the repo root to the guest
#
# Usage:
#   scripts/run_remote_test.sh test_ioctl_fwd
#   scripts/run_remote_test.sh cuinit_test
#   scripts/run_remote_test.sh both
#   scripts/run_remote_test.sh rebuild        # rebuild qemu+stub+module, then test
#   scripts/run_remote_test.sh restart        # kill QEMU, restart, wait for VM
#   scripts/run_remote_test.sh log <pattern>  # grep /tmp/qemu.log on the host
set -e

HOST_SSH="${NVKVM_REMOTE:?set NVKVM_REMOTE, e.g. 'ssh -p 22 root@gpu-host'}"
# Referenced inside the single-quoted remote command blocks below, which are
# evaluated by the REMOTE shell -- so each of those blocks is prefixed with an
# assignment that ships this value across.  Without that prefix $REMOTE_DIR
# expanded to the empty string on the far side and every path became /scripts/...
REMOTE_DIR="${NVKVM_REMOTE_DIR:-/root/nvkvm}"
LOCAL_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RSYNC_TARGET="${NVKVM_RSYNC_TARGET:?set NVKVM_RSYNC_TARGET, e.g. 'root@gpu-host:/root/nvkvm/'}"
GUEST_SSH="ssh -p 2222 -o StrictHostKeyChecking=no -o ConnectTimeout=5 ubuntu@localhost"

cmd="${1:-both}"
shift || true

wait_for_vm() {
    until $HOST_SSH "$GUEST_SSH echo VM_OK 2>/dev/null" 2>/dev/null | grep -q VM_OK; do
        sleep 4
    done
}

case "$cmd" in
    restart)
        $HOST_SSH "REMOTE_DIR='$REMOTE_DIR'"'
                   kill -9 $(pgrep qemu-system) $(pgrep nvkvm_stub) 2>/dev/null; sleep 3
                   rm -f /tmp/qemu.log
                   nohup bash $REMOTE_DIR/scripts/run_test_vm.sh > /tmp/qemu.log 2>&1 & echo PID=$!'
        echo "Waiting for VM..."
        wait_for_vm
        echo "VM ready."
        ;;

    rebuild)
        echo "Syncing source to remote..."
        rsync -avz -e "${NVKVM_RSYNC_SSH:-ssh}" --exclude '.git' --exclude 'host-libs' \
            "$LOCAL_DIR"/ "$RSYNC_TARGET" > /dev/null
        echo "Rebuilding QEMU + stub..."
        $HOST_SSH "REMOTE_DIR='$REMOTE_DIR'"'
            cp $REMOTE_DIR/src/qemu/*.c $REMOTE_DIR/src/qemu/*.h /opt/qemu-src/hw/misc/ 2>/dev/null
            cd /opt/qemu-src/build && ninja qemu-system-x86_64 2>&1 | tail -3
            # Canonical stub is freestanding (no libc); build via its Makefile so
            # stub_clone3.S links (resolves fs_clone3_run).  Plain "gcc nvkvm_stub.c"
            # fails to link since the C7 freestanding migration.
            make -C $REMOTE_DIR/src/stub nvkvm_stub 2>&1 | tail -3
            kill -9 $(pgrep qemu-system) $(pgrep nvkvm_stub) 2>/dev/null
            sleep 2
            cp /opt/qemu-src/build/qemu-system-x86_64 /opt/qemu-nvkvm/bin/qemu-system-x86_64
            [ -x $REMOTE_DIR/src/stub/nvkvm_stub ] && cp $REMOTE_DIR/src/stub/nvkvm_stub /usr/lib/nvkvm/nvkvm_stub
            rm -f /tmp/qemu.log
            nohup bash $REMOTE_DIR/scripts/run_test_vm.sh > /tmp/qemu.log 2>&1 & echo PID=$!
        '
        echo "Waiting for VM..."
        wait_for_vm
        echo "Rebuilding guest module + tests in VM..."
        $HOST_SSH "$GUEST_SSH '
            sudo rmmod nvkvm_guest 2>/dev/null
            sudo mount -t 9p -o trans=virtio,version=9p2000.L,msize=1048576 nvkvm_src /mnt/nvkvm 2>/dev/null
            cd /mnt/nvkvm/src/guest && sudo make -s 2>&1 | tail -1
            sudo insmod /mnt/nvkvm/src/guest/nvkvm-guest.ko 2>&1 | head -1
            mkdir -p /tmp/build/abi && cp /mnt/nvkvm/src/abi/*.h /tmp/build/abi/
            gcc -O0 -g -Wall -I/tmp/build -o /tmp/test_ioctl_fwd /mnt/nvkvm/tests/integration/test_ioctl_fwd.c
            gcc -O0 -g -o /tmp/cuinit_test /mnt/nvkvm/tests/integration/cuinit_test.c -ldl
            # Version-match guest userspace to the host driver (NVML + libcuda).
            # Standalone script so its shell vars expand on the GUEST, not the
            # local shell across the nested ssh hops.  See stage_guest_libs.sh.
            bash /mnt/nvkvm/scripts/stage_guest_libs.sh
            echo READY
        '"
        ;;

    test_ioctl_fwd|ioctl|fwd)
        $HOST_SSH "$GUEST_SSH 'timeout 30 /tmp/test_ioctl_fwd 2>&1 | tail -3'"
        ;;

    cuinit_test|cuinit)
        $HOST_SSH "$GUEST_SSH 'timeout 25 /tmp/cuinit_test 2>&1; echo \"exit=\$?\"'"
        ;;

    both)
        echo "=== test_ioctl_fwd ==="
        $HOST_SSH "$GUEST_SSH 'timeout 30 /tmp/test_ioctl_fwd 2>&1 | tail -3'"
        echo
        echo "=== cuinit_test ==="
        $HOST_SSH "$GUEST_SSH 'timeout 25 /tmp/cuinit_test 2>&1; echo \"exit=\$?\"'"
        ;;

    log)
        pattern="${1:-}"
        if [ -z "$pattern" ]; then
            $HOST_SSH 'tail -50 /tmp/qemu.log'
        else
            $HOST_SSH "grep -aE '$pattern' /tmp/qemu.log | tail -30"
        fi
        ;;

    *)
        echo "Usage: $0 {restart|rebuild|test_ioctl_fwd|cuinit_test|both|log [pattern]}"
        exit 1
        ;;
esac
