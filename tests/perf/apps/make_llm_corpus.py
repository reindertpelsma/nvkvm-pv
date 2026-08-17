#!/usr/bin/env python3
"""make_llm_corpus.py — build the deterministic long-context corpus.

The long-context prompts must be a *real* document (long-chain reasoning over
a large source tree is the point), and they must be byte-identical on the host
and in the guest.  This assembles one file from the nvkvm source tree in a
fixed, sorted order and prints its sha256; copy that one file to both sides and
check the hash rather than re-deriving it independently on each.
"""
from __future__ import annotations

import hashlib
import os
import sys

EXTS = (".c", ".h", ".go", ".py", ".sh", ".md")
SKIP_DIRS = {".git", "build", "node_modules", "__pycache__"}


def main() -> int:
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    out = sys.argv[2] if len(sys.argv) > 2 else "corpus.txt"
    target = int(sys.argv[3]) if len(sys.argv) > 3 else 400_000

    files: list[str] = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
        for name in sorted(filenames):
            if name.endswith(EXTS):
                files.append(os.path.join(dirpath, name))
    files.sort()

    chunks: list[str] = []
    total = 0
    for path in files:
        try:
            with open(path, "r", errors="replace") as fh:
                body = fh.read()
        except OSError:
            continue
        rel = os.path.relpath(path, root)
        chunks.append(f"\n\n===== FILE: {rel} =====\n{body}")
        total += len(body)
        if total >= target:
            break

    text = "".join(chunks)[:target]
    with open(out, "w") as fh:
        fh.write(text)
    print(f"files={len(chunks)} chars={len(text)} "
          f"sha256={hashlib.sha256(text.encode()).hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
