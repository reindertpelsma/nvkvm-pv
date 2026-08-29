# Branch triage — 2026-08-29

68 branches down to 8. Verdicts are evidence-based (patch-id equality,
byte-identical blobs, `git cherry`), because a wrong SUPERSEDED verdict destroys
work while a wrong PENDING verdict only costs a review.

## Kept — evidence, never merge

- **`va-space-leak`** — the BAR1 leak investigation: FINDINGS.md, ~18.8k lines of
  evidence, and `va_capacity.c` / `mapva.py` / `va_probe.sh`, all in active use.
  Note it also carries `pc-ctrl-gate-gamescope`'s commit; take that from the
  gamescope branch, not from here.
- **`present-fd-cache`** — its own final commit reads *"VERDICT: drop the fd
  cache"*: the export round-trip measured 17–23 µs (0.12% of a frame) and is
  load-bearing for the recycled-GEM-handle ino check. The instrumentation was
  kept and landed; the cache deliberately was not. This is the record of a
  rejected optimisation, which is worth more than the code.

## Pending — real work, needs a rebase

| branch | what it is |
|---|---|
| `present-backpressure` | **the most valuable unmerged work.** Flip-completion loop; integration still paces on a fixed-rate hrtimer vblank, so the guest laps its own scanout ring. Ships a unit test and a design doc. |
| `steamos-compose` | durable container provisioning + Plasma boot (+2941 lines). Conflict-heavy rebase; integration has since rewritten `sweep.sh`, `setup_guest.sh`, `build_qemu.sh`. |
| `pc-ctrl-gate-gamescope` | allow `FIFO_GET_INFO`, no-op four privileged gamescope scheduling controls. Directly relevant — gamescope currently spams denied controls. |
| `uvm-fallback-guest-side` | cmd 72 is a query; stop answering it on the DENY channel. Moves a security-sensitive table, so it wants review not just a rebase. |
| `w331-firstlight` | cherry-pick `c388025` only — the hardware record of a real guest presenting on both backends. |
| `uvm-va-decouple` | cherry-pick the four **doc-only** commits; the code landed by another route. Includes an explicit retraction that contradicts a live argument in the shipped doc. |

## Deleted — superseded, with where the current solution lives

- `nvkvm-drm-get-dev-info-struct` — **would have regressed.** It pins the flat
  32-byte layout; `src/guest/nvkvm_drm_abi.h:30-33` states that doing so
  "reintroduces the same llvmpipe fallback on 575 and every branch above it…
  There is no single correct layout. Hence a table." Integration uses a
  version-keyed offset table across 515.43.04 → 610.57.04. **I had this on my
  own merge list; the triage caught it.**
- `present-fixes` — patch-id identical to commits already on integration
  (3681a77 ≡ 0678c4e, 49c3e82 ≡ ad7ab05), which has since evolved past them with
  the `pbo_shown` idle drain and the (owner, ino) cache identity.
- `display-broker`, `display-broker-v2`, `display-broker-v2-firstlight` — the v1
  design was rejected and its findings already landed; the v2 broker on
  integration is the evolved descendant (1402 → 3207 lines).
- `pc-x11-docs` — `tests/repro/signal_restart_export.c` is a byte-identical blob
  on integration, and its known-limitations entry is there at :1307.
- `pc-guest-scanout-deployed` — 22 of 24 symbols already present; integration is
  the evolved version of the same file.
- `fix-egl-wayland` — shares **no merge base**; it is the orphan root line main
  was re-imported from. Every sampled artifact is already on integration.

## Deleted — duplicates

Byte-identical mirrors (same commit objects) and stale snapshots:
`rescue/*`, `srvrescue/*`, `pcrescue/*`, `consolidation/*`, `ioctl-latency-stats`,
`pc-test-latency`, `sec-typeconf-integration`, `backpressure-on-main`.

## The lesson

Two of my own "clear win, merge this" calls were wrong: `present-fixes` was a
divergent earlier implementation of work already landed, and
`nvkvm-drm-get-dev-info-struct` would have reintroduced a bug the code
explicitly warns about. **A branch title describing a real bug is not evidence
that the bug is still open.** Check where the current solution lives first.
