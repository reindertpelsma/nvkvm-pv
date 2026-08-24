# Makefile — nvkvm isolate stub binary
#
# Produces a fully static, position-independent executable with no libc
# dependency. After build, nvkvm_stub_bin.h is generated for embedding into
# the QEMU backend.
#
# POSITION-INDEPENDENT, and the claim is checked below.  This used to be a
# lie: `-static -pie` does NOT produce a PIE, because GCC's spec lets -static
# win over -pie and the link silently emits ET_EXEC at a fixed 0x400000.  The
# stub is the containment boundary for every guest-pointer finding in
# docs/internal/audit-guest-pointers.md, and several of those are rated on the
# attacker not knowing where things are, so "no ASLR in the isolate" was worth
# fixing rather than documenting.
#
# What the fix needs, and why it is two changes and not one flag:
#   - `-static-pie` (one word, not `-static -pie`) to get ET_DYN;
#   - `-DNVKVM_STUB_SELF_RELOC` so nvkvm_stub.c's apply_relocations() is
#     compiled in and called first thing in main().  Nothing else applies the
#     R_X86_64_RELATIVE entries: fexecve from a memfd means no dynamic linker,
#     and -nostdlib -ffreestanding means no rcrt1.o.  See the comment on
#     apply_relocations() for why the _DYNAMIC lookup has to be a `lea`.
# The two MUST move together — applying PIE relocations to an ET_EXEC link
# corrupts memory rather than doing nothing.  `verify-pie` below fails the
# build if the linker ever goes back to emitting EXEC.

CC      ?= gcc
STRIP   ?= strip
XXD     ?= xxd
READELF ?= readelf

CFLAGS  := -O2 -g -std=c11 \
           -nostdlib -static -fPIE -DNVKVM_STUB_SELF_RELOC \
           -ffreestanding -fno-stack-protector -fno-builtin \
           -Wall -Wextra -Wno-unused-parameter \
           -I../.. \
           -I../../src

LDFLAGS := -nostdlib -static-pie -Wl,-z,relro,-z,now

TARGET  := nvkvm_stub
BIN_H   := nvkvm_stub_bin.h

.PHONY: all clean verify-pie

all: $(TARGET) $(BIN_H)

# Build the C sources and the clone3 asm trampoline together so the link
# resolves fs_clone3_run.  -lgcc supplies softfloat/divmod helpers if needed.
$(TARGET): nvkvm_stub.c stub_clone3.S stub_freestanding.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ nvkvm_stub.c stub_clone3.S -lgcc
	$(MAKE) verify-pie

# Fail the build, loudly, if the link came out ET_EXEC.  This is the check
# whose absence let the Makefile claim PIE for as long as it did.
verify-pie: $(TARGET)
	@t=`$(READELF) -h $(TARGET) | awk '/^  Type:/ {print $$2}'`; \
	if [ "$$t" != "DYN" ]; then \
		echo "ERROR: $(TARGET) is ELF type $$t, expected DYN." >&2; \
		echo "  The stub is not position-independent — ASLR is off in" >&2; \
		echo "  every isolate.  Check that LDFLAGS still says -static-pie" >&2; \
		echo "  (one word) and not -static -pie." >&2; \
		exit 1; \
	fi

$(BIN_H): $(TARGET)
	$(STRIP) --strip-all $(TARGET)
	$(XXD) -i $(TARGET) > $@

clean:
	rm -f $(TARGET) $(BIN_H)
