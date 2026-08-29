#!/usr/bin/env python3
"""Parse the SteamOS stage's verdict file from an UNTRUSTED sweep box.

Every byte of the input is controlled by a rented machine, so nothing here is
trusted: the phase and status are checked against fixed allowlists, the detail
is stripped to printable characters and truncated, and tabs are removed because
the caller reads the output with `IFS=$'\t' read`.  Output is TSV:

    phase <TAB> status <TAB> detail

A phase that never reported is emitted as `untested`.  Silence is not a pass --
that distinction is the whole point of the file.
"""
import json
import sys

PHASES = ("preflight", "clone", "image", "install", "provision",
          "boot", "display", "ota", "slotb")
STATUSES = ("pass", "fail")


def main(path: str) -> int:
    seen: set[str] = set()
    rows: list[tuple[str, str, str]] = []
    try:
        with open(path, errors="replace") as fh:
            lines = fh.readlines()
    except OSError as exc:
        print(f"harness\tfail\tcannot read the verdict file: {exc}")
        return 0

    for line in lines[:200]:            # a box cannot flood the ledger
        line = line.strip()
        if not line:
            continue
        try:
            obj = json.loads(line)
        except ValueError:
            continue
        if not isinstance(obj, dict):
            continue
        phase, status = obj.get("phase"), obj.get("status")
        if phase not in PHASES or status not in STATUSES:
            continue
        detail = "".join(c for c in str(obj.get("detail", ""))[:300]
                         if c.isprintable())
        seen.add(phase)
        rows.append((phase, status, detail.replace("\t", " ")))

    for phase in PHASES:
        if phase not in seen:
            rows.append((phase, "untested", "the stage never reached this phase"))

    for row in rows:
        print("\t".join(row))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
