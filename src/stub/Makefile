# Makefile — nvkvm isolate stub binary
#
# Produces a fully static, position-independent executable with no libc
# dependency. After build, nvkvm_stub_bin.h is generated for embedding
# into the QEMU backend.

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
