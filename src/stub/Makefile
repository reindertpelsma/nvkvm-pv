# Makefile — nvkvm isolate stub binary
#
# Produces a fully static executable with no libc dependency. After build,
# nvkvm_stub_bin.h is generated for embedding into the QEMU backend.
#
# NOT position-independent, despite -fPIE/-pie below.  GCC's spec lets
# -static win over -pie, so the link emits ET_EXEC (readelf -h nvkvm_stub
# => "Type: EXEC"), loaded at a fixed 0x400000.  A real static PIE needs
# -static-pie AND a self-relocation entry stub to apply R_X86_64_RELATIVE
# before the first global access -- which -nostdlib -ffreestanding means we
# do not have (there is no rcrt1.o here).  The flags are kept because
# -fPIE still buys RIP-relative addressing, but do not read them as a
# claim of ASLR: the stub's text is at a known address.  Making this a
# genuine PIE is a build change, not a comment change; until then the
# comment must not claim it.

CC      ?= gcc
STRIP   ?= strip
XXD     ?= xxd

CFLAGS  := -O2 -g -std=c11 \
           -nostdlib -static -fPIE \
           -ffreestanding -fno-stack-protector -fno-builtin \
           -Wall -Wextra -Wno-unused-parameter \
           -I../.. \
           -I../../src

LDFLAGS := -nostdlib -static -pie -Wl,-z,relro,-z,now

TARGET  := nvkvm_stub
BIN_H   := nvkvm_stub_bin.h

.PHONY: all clean

all: $(TARGET) $(BIN_H)

# Build the C sources and the clone3 asm trampoline together so the link
# resolves fs_clone3_run.  -lgcc supplies softfloat/divmod helpers if needed.
$(TARGET): nvkvm_stub.c stub_clone3.S stub_freestanding.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ nvkvm_stub.c stub_clone3.S -lgcc

$(BIN_H): $(TARGET)
	$(STRIP) --strip-all $(TARGET)
	$(XXD) -i $(TARGET) > $@

clean:
	rm -f $(TARGET) $(BIN_H)
