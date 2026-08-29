# Sweep plan

The sweep exists to answer one question: **is there a combination of driver,
GPU architecture, kernel and distro that breaks the architecture?** It is not a
debugging tool. It deliberately varies several things at once, because real
users vary several things at once, and a matrix that changes one variable per
run cannot find an interaction in any reasonable number of runs.

It runs **last**, once the code is stable. Running it against known-broken code
produces failures nobody can act on and burns money proving what we already
know. `sweep.py` itself may be developed and validated on a single host at any
time; that is a different activity from *the* sweep.

---

## 1. What the driver matrix must actually cover

The obvious answer — "one driver per ABI profile, so 8" — is **wrong**, and the
obvious opposite — "all 216 versions we claim to support" — is **waste**. Both
were checked against the code.

nvkvm keys behaviour off the host driver version in **three independent tables**,
and they do not split in the same places:

| table | file | what it keys |
|---|---|---|
| ABI profiles | `src/common/nvkvm_abi.h` | struct sizes, embedded-fd offsets |
| NVKMS op numbering | `src/common/nvkvm_nvkms_ops.h` | ioctl numbers for REGISTER/UNREGISTER/GRANT/vblank |
| DRM devinfo layout | `src/guest/nvkvm_drm_abi.h` | field offsets in the DRM device-info blob |

Two of the NVKMS/DRM boundaries fall **inside** a single ABI profile, so a matrix
built from `nvkvm_abi.h` alone leaves real code paths untested:

- **570.207** shifts every NVKMS op by one. It lands between `570.195.03` and
  `570.211.01` — inside `NVKVM_ABI_570` *and* inside one major.
- **590** pulls the vblank tail back down (60/61/62 again) — inside the 580
  profile, which spans 580..595.
- **545.29** inserts `supports_alloc` in the DRM devinfo — inside major 545.
- **575.51** inserts `mig_device` — inside `NVKVM_ABI_570`'s neighbourhood.

So the covering set is the **union of all three tables' boundaries**. That is
**13 drivers**, every one of which has a downloadable `.run` (verified by HTTP
HEAD against `us.download.nvidia.com`):

| # | driver | why this one |
|---|---|---|
| 1 | 515.43.04 | ABI 515; NV00DE class absent entirely |
| 2 | 525.85.05 | ABI 525; pre-CC channel (304 B) |
| 3 | 535.86.05 | ABI 535; CC channel fields (360 B) |
| 4 | 545.23.06 | DRM devinfo with sync_fd/semsurf, before `supports_alloc` |
| 5 | 545.29.06 | ABI 545 (mem 128 / nv00de 8) + `supports_alloc` inserted |
| 6 | 550.40.07 | **pre**-V550 UVM (1200 B) — the in-branch split's low side |
| 7 | 550.54.14 | V550 UVM (9264 B) |
| 8 | 570.195.03 | ABI 570, NVKMS ops **before** the 207 shift |
| 9 | 570.211.01 | same ABI profile, NVKMS ops **after** the shift |
| 10 | 575.51.02 | DRM `mig_device` inserted |
| 11 | 580.105.08 | ABI 580 (VASPACE + NVOS46 each +8) |
| 12 | 590.48.01 | NVKMS vblank tail back to 60/61/62 |
| 13 | 610.57.04 | ABI 610 (channel 376, `hHandleVASpace`) — newest on master |

`610.57.04` **does** ship a 442 MB `.run`, so it can be exercised with matching
userspace rather than only built from source.

### The sampling rule

Two constraints, and together they make the long tail free:

- **Within one host: never two versions from the same section.** Both exercise
  identical code in all three tables, so the second one is a paid run whose
  result is already known.
- **Across hosts: never the same exact version twice, ever.** Each section has
  to be covered on several GPU architectures anyway. When a new host comes up on
  a different arch, spending its slot for that section on an *unseen*
  minor.patch costs exactly the same as repeating the one we used last time.

So each host runs one version per section — 13 runs — every one of them a
version no host has run before. Coverage of the boundaries is complete on the
first host; coverage of the individual versions accumulates across sweeps at
zero marginal cost, and no run is ever spent on a combination we can predict.

The bookkeeping this needs is a persisted set of (version) already used, which
the journal already has to carry for resumability.

### Why not all 216 in one sweep

`tools/abi_derive.sh` already measures **every** OGKM tag offline and is the
complete check — cheap, exhaustive, no GPU required. An end-to-end sweep run
costs a rented host, a driver download and a full boot; spending that on
versions that are byte-identical in all three tables buys nothing. The
recommendation is: **13 boundary drivers every sweep, plus N randomly sampled
non-boundary versions** to catch a boundary we have not noticed. The random
sample is where "never reuse a driver version across hosts" belongs — it makes
coverage accumulate across sweeps instead of re-testing the same 13.

> **Discrepancy to reconcile:** `nvkvm_abi.h` says it derived from **216**
> numeric tags 515.43.04..610.57.04. `git ls-remote --tags` returns **195**
> today. Either tags were removed upstream or the count included something else.
> Worth settling before quoting "216 supported versions" in the README.

---

## 2. Machine qualification (before any test runs)

A bad host must not be recorded as a product failure. Qualification runs first,
and a host that fails it is **destroyed and not counted** — neither pass nor
fail:

1. `/dev/kvm` present, `systemd-detect-virt` = `kvm`, nested virt enabled.
   (A container inherits the host's CPU flags, so counting `vmx|svm` in
   `/proc/cpuinfo` looks fine and proves nothing.)
2. `/dev/dri/card0` **and** `renderD128` present on the host. The guest's DRM
   node is a proxy for the host's; without them no compositor can start and
   every graphics test fails for a reason that is not ours.
3. GPU actually works: `tests/validate.sh` passes **on the host** before we
   touch anything.
4. RAM, disk and CPU above threshold (guest needs 8 GB + a 64 GB image).
5. **Network measured, not advertised.** Time a real download; some hosts
   advertise 1.3 Gbps and deliver 10 KB/s, which turns a 3 GB image into an
   overnight job.

## 3. What each qualified host runs

- **Driver swap** — unload, install a version from the matrix, reload. Every
  KVM template ships the same driver, so this is a real download every time.
- **Kernel swap** — install another kernel, reboot the host VM, re-verify.
- **Distro variation** — across hosts, not within one.
- **A full desktop distro as an nvkvm GUEST** (Mint), booted headless and driven
  to a working desktop.
- **The README parity claims**, exercised app by app. Anything the README says
  works must be run here, or the claim is unsupported.
- **UVM** at minimum, plus rendering and graphics.
- **nvkvm-kata** with the same app list.
- **nvkvm-steamos end to end**: image download, install, OTA, landing on a
  non-OOBE desktop.
- **QEMU SDL rendering** and the **broker**.
- **`tests/validate.sh` again at the end, always** — pass or fail. If it passed
  at qualification and fails now, the host's GPU is wedged, and that is a
  finding in its own right, reported separately from the test that preceded it.

## 4. Rules that are not negotiable

- **Vast hosts are untrusted.** Never copy executable data back; only logs.
  Never build a shell command from anything a host returned. Treat every byte
  from a host as data, never as code.
- **The repo arrives from GitHub at a pinned SHA**, and everything else is
  installed from the internet on the host — because that is what a user does.
  No local artifacts are shipped. Only logs come back.
- **Destroy on success, keep on failure** — so a failure can be inspected.
  With a hard cap (see below).
- **Resumable**: the run is journalled per host per step, so an interrupted
  sweep continues instead of restarting.

---

## 5. Two places I would push back

**"Keep failed hosts alive" needs a cost ceiling.** A failure at 03:00 with no
cap bills until someone notices. Keep the host, but arm a standalone
auto-destroy at a hard deadline and make the report say loudly that the host is
alive, what it costs per hour, and when it dies. (This exact guard has already
saved this project once, and once destroyed evidence I had not collected — both
of which argue for "keep, but bounded and loud".)

**"Never reuse a driver version until all 216 are tested"** — agreed as stated,
once it is read per-host rather than per-sweep. One version per section per
host, never the same version on two hosts. See "The sampling rule" above; this
was settled with the owner rather than decided here.

---

## 6. Open before the sweep runs — VERIFIED AGAINST THE CODE 2026-08-29

The sweep is worth money only against stable code. Each item below was checked
in the source and git history, not in prose, because several docs in this repo
describe as open things that are fixed, and vice versa.

**Verdict: not yet. One release-blocker and three serious gaps are open.**

### Release-blocker

- **BAR1 aperture VA leak**, ~59 mappings per guest Vulkan client teardown.
  `origin/va-space-leak` is **not merged and contains no fix** — its own
  `FINDINGS.md` says "NOT ROOT-CAUSED. No fix committed." The leak sites are
  live on main: `src/qemu/nvkvm_isolate_handlers.c:3606` (`needs_share`),
  `:2071` (`g_mapva`), and a deliberate one at `src/stub/nvkvm_stub.c:2776`
  ("For now leak; a future commit will add a realize-token → fd map").
  It wedges the **host** GPU for host and guest clients alike after enough
  client churn, recoverable only by reloading the NVIDIA stack. A sweep run on
  top of this cannot distinguish its own failures from the leak.

### Serious, knowingly scoped

- **Guest-triggerable QEMU kill (P-10b).** The out-of-bounds map size check at
  `src/qemu/nvkvm_isolate_handlers.c:3956` exempts `TYPE_NVIDIA` handles
  ("h->size is 0 and means unknown") and the prefault loop it guards is
  unprotected; there is no SIGBUS handler anywhere in `src/qemu/`. A guest can
  therefore take down the VMM. Sharpest of the four.
- **Cross-isolate munmap (P-4b).** `:4302` says plainly "KNOWN GAP, do not read
  this as a closed boundary": a caller naming a neighbour's `isolate_id` with
  that neighbour's token passes. Not fixable without a wire change —
  `struct nvkvm_req_munmap_on_isolate` is `{isolate_id, mmap_token}`
  (`src/common/nvkvm_proto.h:499`).
- **Allowlisted control commands with no parameter validation**
  (`src/qemu/nvkvm_ctrl_allowlist.h:53`, `:132` — the latter carries an NvP64).
  The header concedes at `:39` that there is no per-command validation, only a
  1 MiB aux cap.
- **systemsettings wedges on the Wayland GL path** — 98% CPU inside
  `libnvidia-eglcore` under `QWaylandGLContext::swapBuffers`. One app today, but
  the busy-wait is in the shared present path, not in the app.

### Confirmed FIXED — docs describing these as open are stale

- Guest NULL-deref in `nvkvm_send_sync` on first open with no virtio device:
  fixed by `c8277f2`; both open paths now gate on `nvkvm_transport_ready()` and
  return `-ENODEV`.
- `validate.sh` greening a system whose module oopses: both validators now open
  `/dev/nvidiactl` for real, scan dmesg for oops markers, and distinguish
  `UNTESTED` from `SKIP` rather than passing.

### Corrected during verification

The audit initially read `nvkvm_iso_auto_select()` as leaving isolates without
uid separation. It does not: `auto` picks `NS | SECCOMP` via `CLONE_NEWUSER`
with uid/gid maps — the rung the code itself calls strongest — and the shipped
compose additionally pins `uid+chroot`.

### The release gate: audit until clean

A security and reliability audit runs before release, split by component so each
auditor holds one boundary in its head rather than skimming everything:

| component | scope |
|---|---|
| VMM | `src/qemu/` minus the isolate files, plus `src/common/`, `src/abi/` |
| isolate | `nvkvm_isolate*.{c,h}`, `nvkvm_ctrl_allowlist.h`, `src/stub/` |
| guest | `src/guest/` — a kernel module, hostile *guest userspace* below it |
| broker | `src/broker/` — trusted, holds the session, fed by an untrusted VMM |
| misc | compose/Dockerfiles/scripts, `nvkvm-steamos`, `nvkvm-kata` |

It is re-run **after every substantive change**, and the gate is that an audit
comes back **clean** — not that the findings were triaged. Each auditor is told
that comments in this repo are frequently stale, and to verify every claimed
guard in the code, because that is exactly how the last round produced two
"open" bugs that were fixed and one "finding" that was a misreading.

### Also worth a decision before release

**23 remote branches are unmerged**, several carrying real work (present
backpressure, the PBO ring, broker v2, security type-confusion fixes, a
`GET_DEV_INFO` struct off-by-one). Sweeping `main` while that much is stranded
risks finding bugs already fixed on a branch.
