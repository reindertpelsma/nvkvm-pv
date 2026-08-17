#
# After cuCtxCreate_v2 entry, set focused breakpoints at:
#   call site to translator (0x363411) — captures NV_STATUS in %edi
#   prologue of fn at 0x4a4a60 — identifies the caller's purpose
#
import gdb

LIBCUDA_SUFFIX = "libcuda.so.575.51.03"

def libcuda_base():
    """Find ELF base via gdb.objfiles() — robust against multi-segment maps."""
    for obj in gdb.objfiles():
        if LIBCUDA_SUFFIX in (obj.filename or ""):
            # gdb tracks the load offset for each segment; the lowest-VA
            # PT_LOAD with read-exec permissions is the .text segment, but
            # ASLR base is reflected in obj's first segment.  Use sections.
            try:
                for s in obj.sections:
                    if s.name == ".text":
                        # gdb 15: section has 'addr'
                        return s.addr - 0x1c5000  # .text VMA offset within libcuda
            except Exception:
                pass
    # Fallback: scan info proc mappings for the LOWEST address with libcuda
    out = gdb.execute("info proc mappings", to_string=True)
    bases = []
    for line in out.splitlines():
        if LIBCUDA_SUFFIX in line:
            try:
                bases.append(int(line.split()[0], 16))
            except ValueError:
                pass
    return min(bases) if bases else None

class LogBP(gdb.Breakpoint):
    def __init__(self, addr, label):
        super().__init__(f"*0x{addr:x}", internal=True)
        self.silent = True
        self.label = label
        self.hits = 0
    def stop(self):
        self.hits += 1
        try:
            edi = int(gdb.parse_and_eval("$edi"))
            rdi = int(gdb.parse_and_eval("$rdi"))
            rsi = int(gdb.parse_and_eval("$rsi"))
            print(f"BP {self.label}#{self.hits}: edi=0x{edi & 0xffffffff:x}  rdi=0x{rdi:x}  rsi=0x{rsi:x}", flush=True)
        except Exception as e:
            print(f"BP {self.label}: bt err {e}", flush=True)
        return False

class Installer(gdb.Command):
    def __init__(self):
        super().__init__("trap800v2-install", gdb.COMMAND_USER)
    def invoke(self, arg, from_tty):
        base = libcuda_base()
        if not base:
            print("libcuda not loaded"); return
        print(f"libcuda base = 0x{base:x}")
        LogBP(base + 0x363411, "translate_call")
        LogBP(base + 0x4a4a60, "rmAlloc_or_similar")

Installer()
