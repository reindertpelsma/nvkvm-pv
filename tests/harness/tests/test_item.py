"""Item: checksum-is-identity, and the corrupted/half-written-file case.
Runs entirely without a GPU."""

import asyncio

import pytest

from item import ChecksumMismatch, Item, sha256_file
from machine import ThisMachine


def test_requires_exactly_one_of_url_or_local_path(tmp_path):
    with pytest.raises(ValueError):
        Item(name="x")
    with pytest.raises(ValueError):
        Item(name="x", url="https://example.invalid/f", local_path=tmp_path / "f", sha256="a" * 64)


def test_url_item_requires_sha256():
    with pytest.raises(ValueError):
        Item(name="x", url="https://example.invalid/f")


def test_local_item_verifies_by_checksum(tmp_path):
    src = tmp_path / "payload.bin"
    src.write_bytes(b"hello world")
    digest = sha256_file(src)

    item = Item(name="payload", sha256=digest, local_path=src)
    assert item.verify(src) is True


def test_local_item_no_checksum_trusts_existence(tmp_path):
    src = tmp_path / "payload.bin"
    src.write_bytes(b"anything")
    item = Item(name="payload", local_path=src)
    assert item.verify(src) is True
    missing = tmp_path / "nope.bin"
    assert item.verify(missing) is False


def test_corrupted_checksum_is_detected(tmp_path):
    src = tmp_path / "payload.bin"
    src.write_bytes(b"the real content")
    real_digest = sha256_file(src)

    corrupt = tmp_path / "corrupt.bin"
    corrupt.write_bytes(b"NOT the real content, truncated")

    item = Item(name="payload", sha256=real_digest, local_path=src)
    # A file that doesn't match the declared checksum must never verify true --
    # this is the "half-downloaded file" case from the brief.
    assert item.verify(corrupt) is False


def test_need_rejects_corrupt_source_and_leaves_no_bad_file(tmp_path):
    """machine.need() must raise ChecksumMismatch rather than silently
    accepting bad bytes, and must not leave a corrupt file at the cache
    destination for a future call to mistake for a good one."""

    async def body():
        cache_dir = tmp_path / "cache"
        machine = ThisMachine(cache_dir=cache_dir)

        src = tmp_path / "src.bin"
        src.write_bytes(b"good bytes")
        wrong_digest = "0" * 64  # deliberately does not match src's real content
        item = Item(name="thing", sha256=wrong_digest, local_path=src)

        with pytest.raises(ChecksumMismatch):
            await machine.need(item)

        dest = cache_dir / "thing"
        assert not dest.exists(), "a checksum-mismatched fetch must not land at the cache destination"

    asyncio.run(body())


def test_need_never_reuses_a_stale_cached_file(tmp_path):
    """A destination that already exists but doesn't match the item's
    checksum (simulating a half-downloaded or since-corrupted file) must be
    re-fetched, never reused as-is."""

    async def body():
        cache_dir = tmp_path / "cache"
        cache_dir.mkdir()
        machine = ThisMachine(cache_dir=cache_dir)

        # Pre-populate the cache destination with WRONG content, as if a
        # previous run was interrupted mid-write.
        dest = cache_dir / "thing"
        dest.write_bytes(b"stale, wrong, half-written")

        src = tmp_path / "src.bin"
        src.write_bytes(b"the correct, complete content")
        item = Item(name="thing", sha256=sha256_file(src), local_path=src)

        resolved = await machine.need(item)
        assert resolved == dest
        assert dest.read_bytes() == b"the correct, complete content"

    asyncio.run(body())


def test_need_reuses_a_valid_cache_hit_without_refetching(tmp_path, monkeypatch):
    async def body():
        cache_dir = tmp_path / "cache"
        machine = ThisMachine(cache_dir=cache_dir)

        src = tmp_path / "src.bin"
        src.write_bytes(b"cached content")
        item = Item(name="thing", sha256=sha256_file(src), local_path=src)

        first = await machine.need(item)
        assert first.read_bytes() == b"cached content"

        calls = []
        original = Item.fetch_into

        def tracking_fetch(self, dest):
            calls.append(dest)
            return original(self, dest)

        monkeypatch.setattr(Item, "fetch_into", tracking_fetch)
        second = await machine.need(item)
        assert second == first
        assert calls == [], "a valid cache hit must not re-fetch"

    asyncio.run(body())


def test_dir_item_checksum(tmp_path):
    src_dir = tmp_path / "srcdir"
    src_dir.mkdir()
    (src_dir / "a.txt").write_text("a")
    (src_dir / "b.txt").write_text("b")

    from item import sha256_dir

    digest = sha256_dir(src_dir)
    item = Item(name="dirthing", sha256=digest, local_path=src_dir)
    assert item.verify(src_dir) is True

    (src_dir / "b.txt").write_text("MUTATED")
    assert item.verify(src_dir) is False
