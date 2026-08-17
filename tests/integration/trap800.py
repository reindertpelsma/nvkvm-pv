#
# gdb python script: trap every "mov $0x320,%eax" site in libcuda
# and log a 5-frame backtrace each hit.  First hits during a failing
# cuCtxCreate run = the conditional we want.
#
import gdb
import re
import subprocess

LIBCUDA = "/usr/local/nvidia-guest/lib/libcuda.so.575.51.03"

class HitLog(gdb.Breakpoint):
    def __init__(self, addr, name):
        super().__init__(f"*0x{addr:x}", internal=True)
        self.silent = True
        self.name = name
        self.addr = addr
        self.hits = 0
    def stop(self):
        self.hits += 1
        try:
            frame = gdb.selected_frame()
            chain = []
            for _ in range(6):
                if frame is None: break
                f = frame.function()
                pc = frame.pc()
                chain.append(f"{f.name if f else '?'} @0x{pc:x}")
                frame = frame.older()
            print(f"HIT800 site=0x{self.addr:x} hits={self.hits} stack: {' <- '.join(chain)}", flush=True)
        except Exception as e:
            print(f"HIT800 site=0x{self.addr:x} (bt failed: {e})", flush=True)
        return False  # don't actually stop, just log

def libcuda_base():
    """Find load address of libcuda in the inferior."""
    out = gdb.execute("info proc mappings", to_string=True)
    for line in out.splitlines():
        if LIBCUDA in line:
            cols = line.split()
            return int(cols[0], 16)
    return None

def install():
    # Find all 800-return sites via objdump.
    r = subprocess.run(["objdump", "-d", LIBCUDA], capture_output=True, text=True)
    sites = []
    for line in r.stdout.splitlines():
        m = re.match(r"\s*([0-9a-f]+):.*mov\s+\$0x320,%eax", line)
        if m:
            sites.append(int(m.group(1), 16))
    print(f"installing breakpoints at {len(sites)} sites")
    base = libcuda_base()
    print(f"libcuda base = 0x{base:x}" if base else "libcuda not loaded yet?")
    for off in sites:
        addr = (base or 0) + off
        HitLog(addr, f"800@0x{off:x}")

# Wait for libcuda to be loaded, then install
def install_when_ready():
    """Called from a gdb-script-side trigger after dlopen-ish."""
    install()

# Provide a gdb command "trap800_install" to call from the script after
# letting the program dlopen libcuda.
class Trap800Install(gdb.Command):
    def __init__(self):
        super().__init__("trap800-install", gdb.COMMAND_USER)
    def invoke(self, arg, from_tty):
        install_when_ready()
Trap800Install()
