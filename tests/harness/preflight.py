"""Preflight: what tools each tier needs on THIS machine, and -- only with
--install -- how to try to get them. Never installs anything unless the
caller explicitly opted in; the default is to report what's missing so the
affected tier can SKIP with a reason.

This is deliberately separate from tests/validate.sh's own internal handling
of a missing compiler / missing libcuda (that stays inside validate.sh,
which must keep working standalone with nothing installed). This module is
about what tests/harness/*.py itself needs to even attempt a tier: a shell
to invoke validate.sh through, a C compiler to build the medium-tier probes.
"""

from __future__ import annotations

import dataclasses
import shutil
import subprocess
from typing import List, Optional


@dataclasses.dataclass(frozen=True)
class ToolRequirement:
    name: str  # logical name used in messages/results
    candidates: tuple  # binary names checked in order; first hit wins
    apt_package: Optional[str] = None  # best-effort apt package for --install


TIER_REQUIREMENTS = {
    "small": [
        ToolRequirement("bash", ("bash",), "bash"),
    ],
    "medium": [
        ToolRequirement("bash", ("bash",), "bash"),
        ToolRequirement("c_compiler", ("cc", "gcc", "clang"), "build-essential"),
    ],
    "large": [
        ToolRequirement("bash", ("bash",), "bash"),
        ToolRequirement("c_compiler", ("cc", "gcc", "clang"), "build-essential"),
    ],
}


@dataclasses.dataclass
class PreflightResult:
    tier: str
    missing: List[ToolRequirement]
    resolved: dict  # name -> resolved absolute/PATH-found binary

    @property
    def ok(self) -> bool:
        return not self.missing


def _resolve(req: ToolRequirement) -> Optional[str]:
    for candidate in req.candidates:
        found = shutil.which(candidate)
        if found:
            return found
    return None


def check(tier: str) -> PreflightResult:
    reqs = TIER_REQUIREMENTS.get(tier, [])
    missing: List[ToolRequirement] = []
    resolved: dict = {}
    for req in reqs:
        found = _resolve(req)
        if found:
            resolved[req.name] = found
        else:
            missing.append(req)
    return PreflightResult(tier=tier, missing=missing, resolved=resolved)


def install_missing(missing: List[ToolRequirement], *, timeout: float = 300.0) -> List[ToolRequirement]:
    """Best-effort `apt-get install` for whatever's missing. Only ever called
    when the caller passed --install explicitly -- this function itself has
    no opinion on that flag, it just does the installing. Returns the subset
    still missing afterward (empty list == everything got installed)."""
    if not missing:
        return []

    packages = sorted({req.apt_package for req in missing if req.apt_package})
    if packages:
        try:
            subprocess.run(
                ["apt-get", "update"],
                check=False,
                timeout=timeout,
                capture_output=True,
            )
            subprocess.run(
                ["apt-get", "install", "-y", *packages],
                check=False,
                timeout=timeout,
                capture_output=True,
            )
        except (subprocess.TimeoutExpired, FileNotFoundError, OSError):
            pass  # fall through to the re-check below; still_missing reports the truth

    still_missing = [req for req in missing if _resolve(req) is None]
    return still_missing
