"""Item -- a file or folder a test needs, identified by checksum, never by name.

Construct from a URL + sha256 (fetched lazily on first `machine.need()`) or
from a local path (copied lazily the same way; a sha256 is optional there --
omitting it means "trust this local source as-is", e.g. a file already
checked into this tree, but a URL-sourced item MUST carry a sha256, because
nothing else pins what actually got downloaded).

Identity is the CHECKSUM, never the name. A half-downloaded or since-corrupted
file sitting at the expected destination path must never be reused as if it
were complete -- this project's own driver cache sha256-verifies for exactly
that reason (see scripts/sweep_matrix.py's provisioning). `Machine.need()`
re-verifies a cache hit's checksum before trusting it, and re-verifies again
after any fetch, before the fetched bytes are ever moved into the canonical
cache path -- so a crash or a corrupt download can leave a stray `.fetching-*`
temp file, but never a bad `dest`.

Inputs are read-only. Nothing in this module, or in `Machine.need()`, ever
hands back a path for a caller to write into -- that is what `scratch()` is
for, and the distinction lives in the API, not in a comment at call sites.
"""

from __future__ import annotations

import dataclasses
import hashlib
import shutil
import urllib.request
from pathlib import Path
from typing import Optional


def sha256_file(path: Path, chunk_size: int = 1 << 20) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(chunk_size), b""):
            h.update(chunk)
    return h.hexdigest()


def sha256_dir(path: Path) -> str:
    """Deterministic checksum of a directory: sha256 of the sorted
    "relpath\\0filehash\\n" manifest. Stable across re-copies, sensitive to
    any content or filename change."""
    h = hashlib.sha256()
    for rel in sorted(p.relative_to(path).as_posix() for p in path.rglob("*") if p.is_file()):
        h.update(rel.encode())
        h.update(b"\0")
        h.update(sha256_file(path / rel).encode())
        h.update(b"\n")
    return h.hexdigest()


class ChecksumMismatch(RuntimeError):
    def __init__(self, item: "Item", actual: str):
        super().__init__(
            f"{item.name}: expected sha256 {item.sha256}, got {actual} "
            f"(source: {item.url or item.local_path})"
        )
        self.item = item
        self.actual = actual


@dataclasses.dataclass(frozen=True)
class Item:
    name: str  # logical name; also the cache filename under Machine.need()
    sha256: Optional[str] = None  # expected checksum; None only allowed for local_path
    url: Optional[str] = None
    local_path: Optional[Path] = None

    def __post_init__(self) -> None:
        if bool(self.url) == bool(self.local_path):
            raise ValueError(f"Item {self.name!r}: give exactly one of url= or local_path=")
        if self.url and not self.sha256:
            raise ValueError(
                f"Item {self.name!r}: a URL-sourced item must carry a sha256 -- "
                f"identity is the checksum, never the name or the URL"
            )

    def fetch_into(self, dest: Path) -> None:
        """Populate `dest` from this item's source. Not yet verified when
        this returns -- the caller (`Machine.need()`) verifies before trusting
        it. Runs in a worker thread when called via `ThisMachine.need()`."""
        dest.parent.mkdir(parents=True, exist_ok=True)
        if self.local_path is not None:
            src = Path(self.local_path)
            if not src.exists():
                raise FileNotFoundError(f"Item {self.name!r}: local_path {src} does not exist")
            if src.is_dir():
                if dest.exists():
                    shutil.rmtree(dest)
                shutil.copytree(src, dest)
            else:
                shutil.copyfile(src, dest)
        else:
            tmp = dest.with_name(dest.name + ".download")
            try:
                with urllib.request.urlopen(self.url) as resp, open(tmp, "wb") as out:
                    shutil.copyfileobj(resp, out)
                tmp.replace(dest)
            finally:
                tmp.unlink(missing_ok=True)

    def verify(self, path: Path) -> bool:
        """True iff `path` exists and (when this item has a sha256) matches
        it exactly. An item with no sha256 (local, trust-on-copy) verifies as
        True as long as the path exists."""
        if not path.exists():
            return False
        if self.sha256 is None:
            return True
        return self.describe_actual(path) == self.sha256

    def describe_actual(self, path: Path) -> str:
        return sha256_dir(path) if path.is_dir() else sha256_file(path)
