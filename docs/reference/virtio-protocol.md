# The virtio protocol

Two protocols stacked. `src/common/nvkvm_proto.h` is guest ↔ QEMU, over virtio.
`src/common/nvkvm_isolate_proto.h` is QEMU ↔ isolate, over a `SOCK_SEQPACKET`
unix socket. The guest never speaks the second one.

Both sides must be compiled from the same header
(`src/common/nvkvm_proto.h:6-7`). The version is checked at probe:
`NVKVM_PROTO_VERSION` is 2 and the match is exact, else `-EPROTO`
(`src/common/nvkvm_proto.h:70`, `src/guest/nvkvm_virtio.c:599-633`).

## The device

Virtio type 50, so PCI device id `0x1040 + 50 = 0x1072`
(`src/qemu/virtio_nvgpu.h:74-79`). 50 is unassigned by the virtio spec and must
stay under 64 so the PCI id stays inside the range the Linux virtio-pci driver
recognises.

QEMU device names: `virtio-nvgpu-pci` (generic),
`virtio-nvgpu-pci-non-transitional`, `virtio-nvgpu-pci-transitional`
(`src/qemu/virtio_nvgpu_pci.c:146-154`).

One property: `graphics` (bool, default true,
`src/qemu/virtio_nvgpu.c:1444-1449`).

MSI-X is forced on with 4 vectors — 3 virtqueues plus config — rather than
falling back to legacy INTx (`src/qemu/virtio_nvgpu_pci.c:85-99`):

> Shared-INTx demux reads each sharing device's ISR status register over MMIO on
> EVERY completion interrupt — measured ~4170 IRQs/token + ~2150 ISR-read MMIO
> exits/token during LLM decode, the dominant decode tax.

`VIRTIO_RING_F_EVENT_IDX` is **disabled** (`src/qemu/virtio_nvgpu.c:1061-1077`).
With out-of-order completions from the thread pool, interrupt suppression could
strand the last used-ring entry and hang the guest forever in
`wait_for_completion`.

## Virtqueues

| index | name | direction | size | use |
|---|---|---|---|---|
| 0 | `nvkvm-tx` | guest → host | 256 | requests, **and their responses** |
| 1 | `nvkvm-rx` | host → guest | 256 | unused |
| 2 | `nvkvm-evt` | host → guest | 64 | async poll events |

(`src/common/nvkvm_proto.h:74-77`, `src/qemu/virtio_nvgpu.c:1147-1149`,
`src/guest/nvkvm_virtio.c:645-647`.)

RX is vestigial. Responses come back on TX as the IN half of the same
descriptor chain, so RX has no pre-posted buffers and its callback never fires
(`src/guest/nvkvm_virtio.c:332-338`).

Each request is one OUT scatter-gather entry (the request) plus one IN entry (a
512-byte response buffer), submitted with `virtqueue_add_sgs(vq, sgs, 1, 1, inf,
GFP_ATOMIC)` under `vq_tx_lock` (`src/guest/nvkvm_virtio.c:423-436`). The
`inflight` record is passed as the cookie, so `virtqueue_get_buf()` returns it
directly — that, not `txn_id`, is the demultiplexing mechanism.

`vq_tx_lock` is mandatory (`src/guest/nvkvm.h:306-312`):

> Linux virtqueues are NOT thread-safe; without this, two guest processes
> submitting on different vCPUs corrupt the split-ring (double-popped/skipped
> descriptors → a request is stranded → `wait_for_completion` hangs).

### Request buffers must be `kmalloc`'d

Not on the stack. `src/guest/nvkvm_mmap.c:706-715`:

> On `CONFIG_VMAP_STACK` kernels (Ubuntu's default) `virt_to_page()` returns a
> bogus physical page for vmapped stack, so QEMU's DMA read sees zeros —
> `hdr.type` lands as 0 and the QEMU dispatch rejects it with "unknown request
> type 0". The whole virtio queue then deadlocks because the inflight record
> never completes.

The `simple_req` helper (`src/guest/nvkvm_virtio.c:745-757`) copies a
caller's stack struct into a `kmalloc` buffer for exactly this reason.

## Config space

```c
struct nvkvm_virtio_config {
        __le64 shm_base;        /* host-physical base of shared memory      */
        __le64 shm_len;         /* size of shared memory region in bytes    */
        __le64 mmap_win_gpa;    /* guest-physical base of mmap window       */
        __le64 mmap_win_len;    /* size of mmap window in bytes             */
        __le64 flags;           /* NVKVM_CONFIG_F_* — QEMU dictates features */
};
```
— `src/common/nvkvm_proto.h:48-54`

One flag so far: `NVKVM_CONFIG_F_GRAPHICS` (bit 0,
`src/common/nvkvm_proto.h:66`). QEMU is authoritative — it both advertises the
bit and enforces it at the boundary, so a guest that ignores a cleared bit still
cannot open the corresponding devices (`:56-62`).

Reading config has a side effect: `nvkvm_get_config()` resolves the sparse
window's firmware-assigned base and installs its KVM memslot on first read,
which happens during the guest's probe after PCI enumeration has programmed the
BAR (`src/qemu/virtio_nvgpu.c:1042-1059`).

## Shared memory

A 16 MiB region (256 slots × 64 KiB) allocated by QEMU as an anonymous
`MAP_SHARED` mapping and registered as a RAM memory region at
`NVKVM_SHM_GPA_BASE` (`src/qemu/virtio_nvgpu.c:1180-1228`). The guest reads its
location from config space and `ioremap`s it
(`src/guest/nvkvm_virtio.c:673-693`).

| constant | value |
|---|---|
| `NVKVM_SHM_NSLOTS` | 256 |
| `NVKVM_SHM_SLOT_DEFAULT_SIZE` | 64 KiB |
| `NVKVM_SHM_SLOT_MIN_SIZE` | 4 KiB |
| `NVKVM_SHM_CTRL_SLOT` | 0 |

(`src/common/nvkvm_proto.h:81-84`.)

Slot 0 is the control block (`struct nvkvm_shm_ctrl`,
`src/common/nvkvm_proto.h:86-92`): protocol version, slot size, slot count, and
the host driver version string. The guest validates the version match and that
`slot_size` is a power of two ≥ 4096, then selects its ABI profile from the
version string (`src/guest/nvkvm_virtio.c:599-633`).

Guest-side allocation is a 256-bit bitmap under a spinlock, searched from index
1 (`src/guest/nvkvm_virtio.c:31-59`).

**The region is VM-wide, shared by every guest process.** That is why the size
checks on it are a cross-process concern, not a hygiene concern
(`src/guest/nvkvm_virtio.c:1140-1154`):

> Each slot is one tile of a VM-WIDE shared region, so an over-large memcpy here
> would stomp adjacent slots held by OTHER guest processes (class-1 LPE) or run
> off the region.

QEMU bounds every access through one helper, `slot_blob()`
(`src/qemu/virtio_nvgpu.c:217-228`), which returns NULL unless the slot index is
valid *and* `[slot, slot+size)` fits inside both the slot and the whole region:

> a guest-controlled `size` ... must NEVER exceed the 64 KiB slot it indexes, or
> the subsequent send/recv/pread on `slot_ptr()` over-reads/over-writes past the
> slot and past the 16 MiB shm region — a guest-driven OOB R/W in the privileged
> VMM. (The legacy path had this check; the thread-pool/memory/realize paths lost
> it.)
>
> — `src/qemu/virtio_nvgpu.c:207-216`

An `IOCTL_ON_ISOLATE` uses up to three slots: params, aux, and the VMA
whitelist.

## Guest-physical address windows

Three windows, placed as one contiguous block above guest RAM. The **sizes** are
compile-time constants; the **base is not** — it is computed at device realize
from the host's and the guest's physical address width
(`nvkvm_gpa_layout_compute`, `src/qemu/nvkvm_mmap_host.c`). It used to be fixed
at 1 TB / 1.5 TB / 2 TB, which needs 41-42 physical address bits and so was
unaddressable on any consumer laptop — see
[Host CPU](supported-drivers.md#host-cpu) for the requirement and the failure it
caused.

| window | offset in block | size | purpose |
|---|---|---|---|
| shared memory | `+0` | 16 MiB (1 GiB slot) | ioctl parameter slots |
| legacy mmap window | `+1 GiB` | 16 GiB | `/dev/nvidia-uvm` mappings |
| sparse window | `+17 GiB` | 128 GiB | everything else |

The block is packed against the top of the addressable GPA space:
`base = 2^min(host_bits, guest_bits) - 145 GiB`, subject to staying above guest
RAM. On a 39-bit laptop with a 6 GiB guest that is `0x5bc0000000` (367 GiB); on
a 46-bit server it is about 63.86 TiB. Both are logged at realize.

The sparse window's real base is still the firmware-assigned address of a
prefetchable 64-bit MMIO BAR with no backing, registered purely so QEMU/PCI
reserve the range (`src/qemu/virtio_nvgpu_pci.c`, `src/qemu/nvkvm_mmap_host.c`).
Because firmware allocates 64-bit BARs downward from the top of the address
space and the computed sparse base is also `2^bits - 128 GiB`, the two normally
land on exactly the same address. The computed base is the fallback when there
is no BAR, and also the override when firmware places the BAR somewhere that
crosses the GPA limit or overlaps the shm/mmap regions.

Config space advertises **one** window spanning both the mmap and sparse ranges,
including the unbacked gap between them. That is safe because the guest only
ever validates GPAs QEMU actually returned, and those are always inside one of
the two sub-windows (`src/qemu/virtio_nvgpu.c:1262-1276`).

The guest validates every returned GPA against that advertised window before
`remap_pfn_range` (`nvkvm_gpa_in_mmap_window()`,
`src/guest/nvkvm_mmap.c:746-760`) — a range check plus an overflow check. This
is the one place the guest deliberately does not trust the host
(`src/guest/nvkvm_main.c:21-23`), and it is narrow: "A malicious host could
abuse this, but we are not defending against the hypervisor"
(`src/guest/nvkvm_mmap.c:18-20`).

Guest RAM ≥ 1 TB overlaps the shm window; realize fails loudly
(`src/qemu/virtio_nvgpu.c:1239-1254`).

## Request types

Every request starts with an 8-byte header: `{__le32 type; __le32 txn_id;}`
(`src/common/nvkvm_proto.h:171-174`). The response echoes `txn_id`.

| # | request | note |
|---|---|---|
| 10 | `LIST_NVIDIA_DEVICES` | enumerate host GPU devices |
| 11 | `OPEN_NVIDIA_HANDLE` | open `/dev/nvidia*` — in the isolate, except UVM |
| 12 | `OPEN_MEMORY_HANDLE` | `memfd_create` in QEMU |
| 13 | `CLOSE_HANDLE` | refuses while any isolate holds it |
| 14 | `CREATE_ISOLATE` | spawn the stub process |
| 15 | `KILL_ISOLATE` | |
| 16 | `COPY_HANDLE_TO_ISOLATE` | `SCM_RIGHTS` send |
| 17 | `CLOSE_HANDLE_ON_ISOLATE` | |
| 18 | `IOCTL_ON_ISOLATE` | the main path; offloaded to QEMU's thread pool |
| 19 | `MMAP_ON_ISOLATE` | see [the mmap problem](../../ARCHITECTURE.md#problem-1-the-mmap-problem) |
| 20 | `MUNMAP_ON_ISOLATE` | |
| 21 | `POLL_ON_ISOLATE` | arm a host os-event fd |
| 22 | `UNPOLL_ON_ISOLATE` | |
| 23 | `WRITE_MEMORY_HANDLE` | shm slot → memfd (CPU page upload) |
| 24 | `READ_MEMORY_HANDLE` | memfd → shm slot (writeback) |
| 25 | `REALIZE_UVM_MAPPING` | **currently unreachable** — see below |
| 26 | `READ_HOST_FILE` | live read of a host proc/sys file, by enum |
| 27 | `INTERRUPT` | interrupt an in-flight ioctl |
| 28 | `SETUP_RING` | fetch the session's command-buffer ring GPA |
| 29 | `ENTER_LOOP` | drive the isolate's SPSC consumer loop (blocking) |
| 30 | `PRESENT` | a virtual KMS head flipped a scanout bo |
| 31 | `XISO_IMPORT` | cross-isolate dma-buf brokering |

(`src/common/nvkvm_proto.h:136-167`.) Types 1–5 are the legacy synchronous
path; the guest no longer sends them and the QEMU handlers are `#if 0`
tombstones (`src/qemu/virtio_nvgpu.c:230-236`).

`REALIZE_UVM_MAPPING` is fully implemented on both sides but currently
unreachable — the guest's call site is disabled behind a `(void)` cast
(`src/guest/nvkvm_mmap.c:270-275`):

> Step E plan: UVM mmap → REALIZE path. Disabled until the
> realize-on-existing-fd refactor lands (the current fresh-fd design loses the
> RM↔UVM bindings the original fd built up).

Do not chase it when reading.

### Dispatch and offload

`nvkvm_tx_handler()` (`src/qemu/virtio_nvgpu.c:769-1038`) runs on QEMU's TX
thread. Most requests are handled inline via an `ISOLATE_REQ` macro. Two are
offloaded to the thread pool because they block:

- `IOCTL_ON_ISOLATE` (`:886-926`) — "so a blocking stub round-trip does not
  stall the single TX thread and starve other guests".
- `ENTER_LOOP` (`:853-868`) — it blocks for the whole consumer-loop lifetime.

`PRESENT` and `XISO_IMPORT` do bounded stub round trips inline on the TX thread,
with a `TODO(perf)` noting they could be offloaded if per-frame TX stall
matters (`src/qemu/virtio_nvgpu.c:813-816`).

## In-flight tracking and signals

`txn_id` is `(epoch << 12) | slot` where `slot` comes from a 4096-bit bitmap, so
consecutive reuses of a slot get distinct ids
(`src/guest/nvkvm_virtio.c:72-92`, `src/guest/nvkvm.h:293-302`). A mismatch on
return is only a rate-limited warning, since the cookie already guarantees
correct demultiplexing (`src/guest/nvkvm_virtio.c:201-212`).

Waits are **interruptible only on the `IOCTL_ON_ISOLATE` path**, flagged by
`inf->isolate_id != 0` (`src/guest/nvkvm_virtio.c:446-460`):

> Control-plane requests leave `isolate_id` zero and wait uninterruptibly (they
> are fast and interrupting them mid-teardown would be wrong).
>
> On a pending signal we must NOT abandon the descriptor: `req_buf` and
> `inf->resp_buf` are still owned by the virtqueue and QEMU will write the
> response into `resp_buf` when the stub finishes. Freeing now would be a
> use-after-free. Instead we ask the isolate to interrupt the in-flight host
> ioctl (so it returns `-EINTR` promptly), then wait uninterruptibly for the
> descriptor to come back, and finally report `-ERESTARTSYS`.

`NVKVM_REQ_INTERRUPT` routes to the stub, whose reader thread finds the worker
running that `txn_id` and posts `SIGUSR1` to it. The handler is deliberately
empty and registered without `SA_RESTART`, so the blocking host `ioctl` returns
`-EINTR` and the normal response path delivers the result
(`src/common/nvkvm_proto.h:327-333`, `src/stub/nvkvm_stub.c:699-728`).

Three response fields are **repurposed** per request type, which surprises
readers: `MMAP_ON_ISOLATE` returns `mmap_token` in `nvstatus`, `SETUP_RING`
returns `ring_bytes` in `nvstatus` and the GPA in `retval`, and
`REALIZE_UVM_MAPPING` returns `realize_token` in `fault_addr`
(`src/guest/nvkvm_virtio.c:249-320`).

## The EVT queue

16 buffers pre-posted at init (`src/guest/nvkvm_virtio.c:346`, `:357-370`).
QEMU fills one with `struct nvkvm_evt_poll {isolate_id, handle_id, events}`
whenever a forwarded NVIDIA OS event fires on the host
(`src/common/nvkvm_proto.h:522-527`).

Why it exists (`src/guest/nvkvm_virtio.c:340-345`):

> Without this the host completion never reaches the guest `poll()` promptly and
> `libnvidia-*` spins on a ~18 ms poll-timeout-then-recheck (NVENC throughput).

Delivery walks a registry matching both `handle_id` and the session's
`isolate_id`, ORs the events into the fd's cached bits and wakes its wait queue
(`src/guest/nvkvm_main.c:619-635`). The registry's lock doubles as a lifetime
guarantee: a concurrent `close()`'s unregister blocks until any in-flight
delivery ends (`src/guest/nvkvm_main.c:592-598`).

On the QEMU side the isolate's reader thread never touches a virtqueue directly
— it hops onto the device `AioContext` via a one-shot bottom half
(`src/qemu/virtio_nvgpu.c:717-767`). If no EVT buffer is free the event is
dropped, which is recoverable: the guest re-arms its poll and the still-readable
host fd re-fires (`:748-754`).

## QEMU ↔ isolate

`SOCK_SEQPACKET` over a socketpair, fd 0 in the child
(`src/qemu/nvkvm_isolate.c:783`, `src/stub/nvkvm_stub.c:432`). Fixed-size
command structs; variable-length payloads follow as separate messages whose
sizes are in the header. File descriptors travel by `SCM_RIGHTS`.

| # | command | |
|---|---|---|
| 1 | `RECEIVE_FD` | store an fd under a handle id |
| 2 | `CLOSE_FD` | |
| 3 | `IOCTL` | the only command executed by the worker pool |
| 4 | `MMAP` | `MAP_FIXED` at the guest's VA |
| 5 | `MUNMAP` | |
| 6 / 7 | `POLL` / `UNPOLL` | |
| 8 | `EXIT` | |
| 9 | `OPEN_DEVICE` | stub opens the device, replies with an `SCM_RIGHTS` copy |
| 10 | `REALIZE_UVM_FD` | batched UVM setup (unreachable, see above) |
| 11 | `INTERRUPT` | fire-and-forget `SIGUSR1` |
| 12 | `SETUP_RING` | ring memfd via `SCM_RIGHTS` |
| 13 | `ENTER_LOOP` | |
| 14 | `PRESENT_EXPORT` | `PRIME_HANDLE_TO_FD`, dma-buf back via `SCM_RIGHTS` |
| 15 | `XISO_IMPORT` | `PRIME_FD_TO_HANDLE` |

(`src/common/nvkvm_isolate_proto.h:41-72`.)

`ISOLATE_CMD_IOCTL` carries `abi_profile` (`:107-109`) so the stub uses matching
version-variant offsets without parsing a version string.

`QEMU never trusts the isolate: every response value is range-checked`
(`src/common/nvkvm_isolate_proto.h:13`). Two concrete instances:

- Only `OPEN_DEVICE` and `PRESENT_EXPORT` may legitimately carry an
  `SCM_RIGHTS` fd. Any fd attached to any other response type is closed
  immediately, because otherwise a compromised stub could exhaust QEMU's fd
  table (`src/qemu/nvkvm_isolate.c:443-465`).
- The pending-IOCTL entry is *claimed* (removed from the list) under the lock
  before the payload is read, so a duplicated `txn_id` finds nothing. Leaving it
  on the list would let a second response `recv()` into a stack-allocated
  `pending` after the caller had already woken and returned — a use-after-free
  write inside QEMU (`src/qemu/nvkvm_isolate.c:505-524`).

## `READ_HOST_FILE`

The guest has no real `nvidia.ko`, so the `/proc` and `/sys` files `libcuda`
probes during `cuInit` do not exist, and their absence makes it bail into
`CUDA_ERROR_UNKNOWN` (999) (`src/guest/nvkvm_hostfile.c:1-15`).

The guest module installs proc entries and sysfs kobjects that delegate each
read to QEMU. **The guest never names a path** — it sends an enum
(`src/common/nvkvm_proto.h:687-695`):

| id | host path |
|---|---|
| 1 | `/proc/driver/nvidia/params` |
| 2 | `/sys/module/nvidia/initstate` |
| 3 | `/sys/module/nvidia_uvm/initstate` |
| 4 | `.../gpus/<bdf>/numa_status` |
| 5 | `.../gpus/<bdf>/information` |
| 6 | `.../gpus/<bdf>/registry` |
| 7 | `/proc/driver/nvidia/version` |

Per-GPU files take a `gpu_index` integer, resolved against a BDF list QEMU
discovered by scanning its own `/proc/driver/nvidia/gpus/`, with a strict
canonical-PCI-address format check on each name — so path traversal is not
possible and a guest index can only ever resolve to a real host GPU
(`src/qemu/nvkvm_isolate_handlers.c:2671-2735`). Files are reopened and reread
on every call so content is live. Cap: 8 KiB.

The BDF the guest exposes is its *own* — hardcoded `0000:00:07.0`
(`src/guest/nvkvm_hostfile.c:28-31`) — which is why `-device nvkvm-gpu,addr=7`
is not arbitrary.
