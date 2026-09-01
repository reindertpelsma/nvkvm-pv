import sys
from pathlib import Path

# tests/harness/tests/ -> tests/harness/ -- so `import machine`, `import item`,
# `import result`, `import preflight` resolve regardless of pytest's rootdir
# or invocation cwd.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
