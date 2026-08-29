#!/usr/bin/env bash
# .dockerignore must be a LITERAL SUPERSET of .gitignore.
#
# The Dockerfile's runtime stage does `COPY . /opt/nvkvm` and the result is
# published under the ghcr name in release.yml.  .gitignore is where this repo
# keeps *.key, .env and data/authorized_keys out of version control -- because
# they are secret.  A rule that is in .gitignore and not in .dockerignore
# therefore does not protect anything: the file stays out of the repo and goes
# straight into a published image layer, which `docker history` reads back out.
# .dockerignore had four rules; .gitignore had fifty-nine.
#
# Compared LITERALLY, rule by rule, and not for equivalence.  A broader glob
# that "obviously covers" a narrower one is exactly the kind of judgement that
# stops being true after someone adds a `!` negation, and this file is not the
# place to be clever.  Same rule, and same reasoning, as nvkvm-steamos'
# tests/compose_policy_test.sh.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 - "$ROOT/.gitignore" "$ROOT/.dockerignore" <<'PY'
import sys

def rules(path):
    with open(path, encoding="utf-8") as fh:
        return [
            line.rstrip("\n")
            for line in fh
            if line.strip() and not line.lstrip().startswith("#")
        ]

git_rules = rules(sys.argv[1])
docker_rules = rules(sys.argv[2])

missing = [r for r in git_rules if r not in docker_rules]
if missing:
    sys.exit(
        ".dockerignore must be a literal superset of .gitignore; missing:\n  "
        + "\n  ".join(missing)
        + "\n\nCopy each rule across verbatim. Anything Git hides and Docker\n"
          "does not is baked into the published image by `COPY . /opt/nvkvm`."
    )

# The three that are the actual point of this test, named so that a future
# reshuffle of .gitignore cannot quietly drop them from both files at once.
for secret in ("*.key", ".env", "data/authorized_keys"):
    if secret not in docker_rules:
        sys.exit(f".dockerignore does not exclude {secret}")

# Order is load-bearing: a `!` negation only re-includes what an EARLIER rule
# excluded, so a negation that drifts above its rule silently stops working.
di = {r: i for i, r in enumerate(docker_rules)}
prev = -1
for r in git_rules:
    if di[r] < prev:
        sys.exit(f"mirrored rule {r!r} is out of .gitignore order in .dockerignore")
    prev = di[r]

print(f"ok: .dockerignore mirrors all {len(git_rules)} .gitignore rules, in order")
PY
