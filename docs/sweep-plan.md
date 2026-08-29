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

### Why not all 216

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

**"Never reuse a driver version until all 216 are tested" is the wrong default.**
Applied literally it means ~200 sweeps that exercise identical code paths before
the 13 interesting ones are covered twice. Inverted — always the 13 boundaries,
plus a rotating random sample that never repeats — coverage of the *interesting*
space is complete on run one, and the long tail still accumulates.

---

## 6. Open before the sweep runs

The sweep is worth money only against stable code. Confirmed-open items are
tracked separately; the sweep should not start while a release-blocker is
outstanding, because every failure it finds will be suspected of being that
blocker.
