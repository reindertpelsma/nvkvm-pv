# Host BAR1 aperture: why it matters, and how to enlarge it

## What BAR1 is

BAR1 is **not** a flat window into VRAM. It is an MMU-translated aperture with
its own virtual address space (`VASPACE_FLAGS_BAR_BAR1` in OGKM) and an eheap
allocator over it; `kbusMapFbAperture()` / `kbusUnmapFbAperture()` are the
map/unmap pair, and the VA space is per-`Device`. That is why `nvidia-smi`
reports BAR1 as *used/free* rather than only a size, and why it can also map
sysmem and peer memory rather than only local VRAM.

The practical consequence: **what nvkvm exhausts is address space, not video
memory.** When this host's BAR1 read 255 MiB of 256 MiB used, VRAM was
simultaneously 437 MiB of 12282 MiB used — almost entirely free.

## Why you should enlarge it

BAR1 is simply too small at the old default for a desktop workload — a bare
Plasma desktop alone sits at 411-423 MiB. It is **not** a leak: what looked like
one was deferred cleanup, and every byte comes back once the last host referrer
exits (`docs/investigations/va-space-leak/FINDINGS.md`, resolved 2026-09-03;
this page previously said *"nvkvm leaks BAR1 VA that is reclaimed only by
tearing the driver down"*). At the pre-Resizable-BAR
default of **256 MiB** that becomes a host-wide denial of service quickly: a
guest whose compositor crash-loops exhausted this box's aperture in an afternoon,
after which the HOST's own `vulkaninfo` failed with
`ERROR_INITIALIZATION_FAILED`.

Enlarging the aperture is **mitigation, not a fix** — it does not stop the leak,
it buys roughly 32x the headroom so the leak stops being an outage.

## Is Resizable BAR not on by default?

Often not. It needs all of: the device to advertise the capability (NVIDIA
Ampere and later do), firmware with **Above 4G Decoding** and **Re-Size BAR
Support** enabled, and a kernel that will place the enlarged BAR above 4G.
Consumer boards from ~2020 support it but many ship with it disabled, legacy/CSM
boot forces it off, and **rented and server hosts commonly have it off** — which
is exactly the environment the sweep runs in. Measured on this host: the
capability was present and advertised up to 16GB, while the BAR sat at 256 MB at
`0xb0000000`, i.e. below 4G.

Because of that spread, **record the BAR1 size per host when testing.** The same
build behaves very differently at 256 MiB and at 8 GiB, and a leak that looks
fatal on one is invisible on the other.

## Enlarging it without touching firmware

The Linux PCI layer can resize at runtime through
`/sys/bus/pci/devices/<dev>/resource1_resize` — a PCI config write, not a
firmware setting. No BIOS change was needed on this host; the kernel relocated
the BAR above 4G by itself (`0xa00000000`). It does **not** persist across a
reboot, hence the unit below.

```
scripts/nvkvm-bar1-resize.sh          # idempotent; safe to run any time
packaging/nvkvm-bar1-resize.service   # runs it before the display manager
```

Install:

```
install -m 0755 scripts/nvkvm-bar1-resize.sh /usr/local/sbin/
install -m 0644 packaging/nvkvm-bar1-resize.service /etc/systemd/system/
systemctl daemon-reload && systemctl enable --now nvkvm-bar1-resize.service
```

`NVKVM_BAR1_EXP` selects the size as a power of two in MB (default `13` = 8 GiB;
`14` = 16 GiB). The script checks the current size first and does nothing if it
is already large enough — it must not unload a working driver for no reason.

Verified on the PC: shrunk to 256 MiB, then the service took it to 8192 MiB and
reloaded the driver with the desktop coming back up.

If the resize is refused, there is no MMIO space above 4G to relocate into; that
one does need the firmware's Above-4G-Decoding setting.
