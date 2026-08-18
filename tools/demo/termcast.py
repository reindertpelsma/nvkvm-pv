#!/usr/bin/env python3
"""termcast.py — turn a script(1) recording into an asciinema cast or a GIF.

The host side of the demo (tools/demo/record_demo.sh) only uses script(1),
so nothing has to be installed on the GPU box.  All the heavy lifting is
here, off-box:

    tools/demo/termcast.py cast /tmp/nvkvm-demo -o demo.cast
    tools/demo/termcast.py gif  /tmp/nvkvm-demo -o docs/img/boot.gif

Fast-forwarding is what makes a two-minute boot watchable: --max-idle caps
any single pause (a VM spends most of its boot waiting) and --speed scales
what is left.  Both act on the timeline only; no output is ever dropped.

GIF rendering needs `pyte` (terminal emulation) and `pillow` (drawing).
The cast subcommand needs neither.
"""
from __future__ import annotations

import argparse
import json
import os
import sys

# ── script(1) parsing ─────────────────────────────────────────────────────


def read_recording(prefix: str):
    """Yield (delay, bytes) pairs from a script(1) typescript+timing pair.

    Handles both timing formats: classic (`<delay> <nbytes>`) and the
    "advanced" one util-linux emits with --logging-format advanced, where
    each line is prefixed by a stream tag (O/I/S/H).  Only output (O) is
    replayed; input echo already appears in the output stream.
    """
    ts_path, tm_path = prefix + ".typescript", prefix + ".timing"
    for p in (ts_path, tm_path):
        if not os.path.exists(p):
            sys.exit(f"termcast: missing {p}")

    with open(ts_path, "rb") as f:
        data = f.read()

    # script(1) writes a "Script started on ..." banner as the first line and
    # a "Script done on ..." trailer; the timing file does not describe them.
    if data.startswith(b"Script started"):
        nl = data.find(b"\n")
        if nl != -1:
            data = data[nl + 1:]
    tail = data.rfind(b"\nScript done")
    if tail != -1:
        data = data[:tail + 1]

    pos = 0
    with open(tm_path) as tm:
        for line in tm:
            parts = line.split()
            if not parts:
                continue
            if parts[0] in ("O", "I", "S", "H"):        # advanced format
                if parts[0] != "O" or len(parts) < 3:
                    continue
                delay, nbytes = float(parts[1]), int(parts[2])
            else:                                        # classic format
                if len(parts) < 2:
                    continue
                delay, nbytes = float(parts[0]), int(parts[1])
            chunk = data[pos:pos + nbytes]
            pos += nbytes
            if chunk:
                yield delay, chunk


def timeline(prefix: str, max_idle: float, speed: float, trim_head: float):
    """Absolute-timestamped events, fast-forwarded."""
    events, t = [], 0.0
    for delay, chunk in read_recording(prefix):
        t += min(delay, max_idle) / speed
        events.append((t, chunk))
    if trim_head and events:
        # Drop the dead air before the first byte is printed.
        t0 = events[0][0]
        if t0 > trim_head:
            shift = t0 - trim_head
            events = [(t - shift, c) for t, c in events]
    return events


def read_geom(prefix: str, cols: int | None, rows: int | None):
    if cols and rows:
        return cols, rows
    try:
        with open(prefix + ".geom") as f:
            c, r = f.read().split()[:2]
            return cols or int(c), rows or int(r)
    except Exception:
        return cols or 100, rows or 30


# ── asciinema v2 ──────────────────────────────────────────────────────────


def cmd_cast(a):
    cols, rows = read_geom(a.prefix, a.cols, a.rows)
    events = timeline(a.prefix, a.max_idle, a.speed, a.trim_head)
    out = open(a.out, "w") if a.out else sys.stdout
    header = {"version": 2, "width": cols, "height": rows,
              "env": {"TERM": "xterm-256color", "SHELL": "/bin/bash"}}
    if a.title:
        header["title"] = a.title
    out.write(json.dumps(header) + "\n")
    for t, chunk in events:
        out.write(json.dumps([round(t, 6), "o",
                              chunk.decode("utf-8", "replace")]) + "\n")
    if a.out:
        out.close()
        dur = events[-1][0] if events else 0
        print(f"wrote {a.out}  ({len(events)} events, {dur:.1f}s)")


# ── GIF ───────────────────────────────────────────────────────────────────

# xterm's first 16 colours, on a dark background close to most terminals.
PALETTE = {
    "black": (0x1c, 0x1c, 0x1c),   "red": (0xcd, 0x31, 0x31),
    "green": (0x0d, 0xbc, 0x79),   "brown": (0xe5, 0xe5, 0x10),
    "yellow": (0xe5, 0xe5, 0x10),  "blue": (0x24, 0x72, 0xc8),
    "magenta": (0xbc, 0x3f, 0xbc), "cyan": (0x11, 0xa8, 0xcd),
    "white": (0xe5, 0xe5, 0xe5),
}
# Bold + a named colour means the bright variant, the way a real terminal
# renders it -- not an arbitrary brightening of the base RGB.
BRIGHT = {
    "black": (0x66, 0x66, 0x66),   "red": (0xf1, 0x4c, 0x4c),
    "green": (0x23, 0xd1, 0x8b),   "brown": (0xf5, 0xf5, 0x43),
    "yellow": (0xf5, 0xf5, 0x43),  "blue": (0x3b, 0x8e, 0xea),
    "magenta": (0xd6, 0x70, 0xd6), "cyan": (0x29, 0xb8, 0xdb),
    "white": (0xff, 0xff, 0xff),
}
FG_DEFAULT = (0xd0, 0xd0, 0xd0)
BG_DEFAULT = (0x14, 0x14, 0x18)


def resolve(name, default, bold=False):
    if name in ("default", None):
        return (0xff, 0xff, 0xff) if (bold and default is FG_DEFAULT) else default
    if name in PALETTE:
        return BRIGHT[name] if bold else PALETTE[name]
    else:
        try:                                    # pyte gives hex for 256-colour
            rgb = tuple(int(name[i:i + 2], 16) for i in (0, 2, 4))
        except Exception:
            rgb = default
    return rgb


def cmd_gif(a):
    try:
        import pyte
        from PIL import Image, ImageDraw, ImageFont
    except ImportError as e:
        sys.exit(f"termcast: gif needs pyte and pillow ({e})\n"
                 f"          pip install pyte pillow")

    cols, rows = read_geom(a.prefix, a.cols, a.rows)
    events = timeline(a.prefix, a.max_idle, a.speed, a.trim_head)
    if not events:
        sys.exit("termcast: nothing recorded")

    font = ImageFont.truetype(a.font, a.font_size)
    # Monospace advance: measure a wide glyph rather than trusting metrics.
    cw = int(round(font.getlength("M")))
    ascent, descent = font.getmetrics()
    ch = ascent + descent + a.line_gap
    W, H = cols * cw + 2 * a.pad, rows * ch + 2 * a.pad

    screen = pyte.Screen(cols, rows)
    stream = pyte.ByteStream(screen)

    frames, durations = [], []
    prev_sig = None
    step = 1.0 / a.fps
    next_t = 0.0
    last_emit = 0.0

    def snapshot():
        buf = screen.buffer
        sig, cells = [], []
        for y in range(rows):
            line = buf[y]
            for x in range(cols):
                c = line[x]
                sig.append((c.data, c.fg, c.bg, c.bold, c.reverse))
            cells.append(line)
        return tuple(sig)

    def render():
        img = Image.new("RGB", (W, H), BG_DEFAULT)
        d = ImageDraw.Draw(img)
        buf = screen.buffer
        for y in range(rows):
            line = buf[y]
            x = 0
            while x < cols:
                c = line[x]
                fg, bg, bold, rev = c.fg, c.bg, c.bold, c.reverse
                run = []
                while x < cols:
                    c2 = line[x]
                    if (c2.fg, c2.bg, c2.bold, c2.reverse) != (fg, bg, bold, rev):
                        break
                    run.append(c2.data if c2.data else " ")
                    x += 1
                fgc = resolve(fg, FG_DEFAULT, bold)
                bgc = resolve(bg, BG_DEFAULT)
                if rev:
                    fgc, bgc = bgc, fgc
                px = a.pad + (x - len(run)) * cw
                py = a.pad + y * ch
                if bgc != BG_DEFAULT:
                    d.rectangle([px, py, px + len(run) * cw - 1, py + ch - 1],
                                fill=bgc)
                text = "".join(run)
                if text.strip():
                    d.text((px, py), text, font=font, fill=fgc)
        return img

    for t, chunk in events:
        stream.feed(chunk)
        if t < next_t:
            continue
        sig = snapshot()
        if sig != prev_sig:
            if frames:
                durations.append(max(a.min_ms, int((t - last_emit) * 1000)))
            frames.append(render())
            prev_sig, last_emit = sig, t
        next_t = t + step
        if a.max_frames and len(frames) >= a.max_frames:
            print(f"termcast: hit --max-frames={a.max_frames}, "
                  f"truncating at t={t:.1f}s of {events[-1][0]:.1f}s",
                  file=sys.stderr)
            break

    if frames:
        # Final frame: re-render so the last state is what the loop rests on.
        sig = snapshot()
        if sig != prev_sig:
            durations.append(max(a.min_ms, int((events[-1][0] - last_emit) * 1000)))
            frames.append(render())
        durations.append(a.hold_ms)

    frames[0].save(a.out, save_all=True, append_images=frames[1:],
                   duration=durations, loop=0, optimize=True, disposal=1)
    size = os.path.getsize(a.out)
    print(f"wrote {a.out}  ({len(frames)} frames, "
          f"{sum(durations)/1000:.1f}s, {size/1024:.0f} KiB, {W}x{H})")


# ── CLI ───────────────────────────────────────────────────────────────────


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    def common(sp):
        sp.add_argument("prefix", help="recording prefix (without .typescript)")
        sp.add_argument("-o", "--out")
        sp.add_argument("--max-idle", type=float, default=1.0,
                        help="cap any single pause, seconds (default 1.0)")
        sp.add_argument("--speed", type=float, default=1.0,
                        help="divide all delays by this (default 1.0)")
        sp.add_argument("--trim-head", type=float, default=0.5,
                        help="seconds of leading dead air to keep")
        sp.add_argument("--cols", type=int)
        sp.add_argument("--rows", type=int)

    c = sub.add_parser("cast", help="write an asciinema v2 .cast")
    common(c)
    c.add_argument("--title")
    c.set_defaults(func=cmd_cast)

    g = sub.add_parser("gif", help="render an animated GIF")
    common(g)
    g.add_argument("--fps", type=float, default=10.0,
                   help="maximum frames per second sampled (default 10)")
    g.add_argument("--font", default="/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf")
    g.add_argument("--font-size", type=int, default=13)
    g.add_argument("--line-gap", type=int, default=1)
    g.add_argument("--pad", type=int, default=8)
    g.add_argument("--min-ms", type=int, default=40, help="floor on frame delay")
    g.add_argument("--hold-ms", type=int, default=2500,
                   help="how long to rest on the last frame")
    g.add_argument("--max-frames", type=int, default=600)
    g.set_defaults(func=cmd_gif)

    a = p.parse_args()
    if a.cmd == "gif" and not a.out:
        a.out = "demo.gif"
    a.func(a)


if __name__ == "__main__":
    main()
