# Trap cuCtxCreate=800 — break on every "mov $0x320,%eax" site in libcuda,
# but only PRINT a backtrace; let execution continue.  Hits at the actual
# bail site let us see the conditional and the caller.

# Suppress noise
set pagination off
set logging file /tmp/trap800.log
set logging redirect on
set logging enabled on

# Catch only the path where 800 is the final return value of cuCtxCreate.
# A simpler approach: rbreak everything that mov's $0x320 into eax.
set $libcuda_base = 0
python
import gdb

def on_lib(event):
    global libcuda_base
    for obj in gdb.objfiles():
        if 'libcuda.so' in obj.filename:
            try:
                base = int(gdb.parse_and_eval('(unsigned long)0x' + ''.join(c for c in str(obj.progspace.objfiles()[0].build_id) if c in '0123456789abcdef')[:16]))
            except Exception:
                pass
            print('libcuda loaded:', obj.filename)

# Use rbreak to set breakpoints on every "mov $0x320" site is too noisy.
# Instead: break on cuCtxCreate_v2 entry, then single-step over after
# context body.  Actually best: break right before the syscall that
# follows the bail.
end

# Try simpler: break on cuCtxCreate_v2 entry, then keep running.
# When it returns, examine $rax — if 800, walk back via stepi.
break cuCtxCreate_v2
commands
silent
printf "==> cuCtxCreate_v2 entry rdi=%p rsi=%p rdx=%p\n", $rdi, $rsi, $rdx
finish
end

# After finish, $rax holds return value.  Set a hook to check.
define hook-stop
  if $rax == 800
    printf "==> cuCtxCreate returned 800 — backtrace:\n"
    bt 20
    info registers rax rbx rcx rdx rsi rdi rbp rsp r8 r9 r10 r11 r12 r13 r14 r15
  end
end

run
quit
