CC      = gcc
CFLAGS  = -Wall -Wextra -g \
          -Istubs \
          -I../../src \
          -I../../src/qemu \
          -I../../src/common
LDFLAGS = -lpthread

# DEAD-1 (2026-08-24): DISPATCH_SRCS/FRONTEND_SRCS were here.  The two QEMU
# sources they named -- nvkvm_dispatch.c and nvkvm_frontend.c -- were unreachable
# end to end and have been deleted, and frontend_stubs.c existed only to satisfy
# nvkvm_dispatch.c's calls into the frontend.  test_frontend went with them (all
# 8 of its cases drove the deleted handler); test_dispatch is now test_objects,
# which keeps the object graph and the struct-size assertions.  See the DEAD-1
# accounting at the top of src/qemu/virtio_nvgpu.c.
OBJECTS_SRCS  = test_objects.c \
                ../../src/qemu/nvkvm_objects.c \
                ../../src/qemu/nvkvm_handle.c \
                unit_stubs.c

HANDLE_SRCS   = test_handle.c \
                ../../src/qemu/nvkvm_handle.c \
                unit_stubs.c

ISOLATE_SRCS  = test_isolate.c \
                ../../src/qemu/nvkvm_isolate.c \
                ../../src/qemu/nvkvm_handle.c \
                unit_stubs.c

TABLES_SRCS   = test_tables.c \
                ../../src/qemu/nvkvm_tables.c

OPEN_SCM_SRCS = test_open_scm.c

CTRL_GATE_SRCS = test_ctrl_gate.c

.PHONY: all clean run check

all: test_objects test_handle mock_stub test_isolate test_tables test_open_scm test_ctrl_gate test_nvkms_allowlist test_stub_ptr_sanitize test_r1_type_dev

test_objects: $(OBJECTS_SRCS)
	$(CC) $(CFLAGS) -o $@ $(OBJECTS_SRCS) $(LDFLAGS)

test_handle: $(HANDLE_SRCS)
	$(CC) $(CFLAGS) -o $@ $(HANDLE_SRCS) $(LDFLAGS)

# -static is load-bearing.  The isolate child pivot_root()s into an empty tmpfs
# before it fexecve()s the stub, so a dynamically linked mock cannot be started
# at all: the loader is gone and execveat() returns ENOENT.  The real stub is
# exec'd the same way; this just makes the mock survive the same environment.
mock_stub: mock_stub.c
	$(CC) $(CFLAGS) -static -o $@ mock_stub.c

test_isolate: $(ISOLATE_SRCS) mock_stub
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ $(ISOLATE_SRCS) $(LDFLAGS)

test_tables: $(TABLES_SRCS)
	$(CC) $(CFLAGS) -D_GNU_SOURCE -DNVKVM_TABLES_NO_KVM -o $@ $(TABLES_SRCS) $(LDFLAGS)

test_open_scm: $(OPEN_SCM_SRCS)
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ $(OPEN_SCM_SRCS) $(LDFLAGS)

test_ctrl_gate: $(CTRL_GATE_SRCS)
	$(CC) $(CFLAGS) -o $@ $(CTRL_GATE_SRCS)

# Header-only gate, so this needs no sources beyond the test itself.  It pins
# the NVKMS command numbering at every vendor tag nvkvm supports -- see the
# table in the test; the gate is version-keyed precisely because those numbers
# move between branches.
test_nvkms_allowlist: test_nvkms_allowlist.c ../../src/qemu/nvkvm_nvkms_allowlist.h
	$(CC) $(CFLAGS) -o $@ test_nvkms_allowlist.c

# U-5 / U-7.  The stub is freestanding (-nostdlib -ffreestanding) and carries its own
# entry point, so it cannot be linked into a hosted test binary.  Extract the one
# function under test straight out of the stub source instead of copying it here,
# so this suite pins the REAL code and cannot drift away from it.  If the
# extraction ever comes back empty the compile fails on an undefined symbol,
# which is the failure mode we want.
stub_sanitize.inc: ../../src/stub/nvkvm_stub.c
	awk '/^static void (clamp_inner_params_size|zero_nvos64_rights)/,/^}$$/' $< > $@

test_stub_ptr_sanitize: test_stub_ptr_sanitize.c stub_sanitize.inc
	$(CC) $(CFLAGS) -I. -o $@ test_stub_ptr_sanitize.c

# R-1.  Two independent implementations of one property (QEMU's boundary gate
# and the stub's own assertion, which also covers the ring path QEMU never
# sees), so both are extracted from the real sources between explicit markers
# rather than copied into the test.  A copied gate is a gate that can silently
# stop matching the one that runs; the markers make a rename or a reformat fail
# the BUILD, which is the failure mode we want.  Empty extraction -> undefined
# symbol -> compile error.
r1_type_dev.inc: ../../src/qemu/nvkvm_isolate_handlers.c
	awk '/NVKVM_R1_TYPE_DEV_BEGIN/,/NVKVM_R1_TYPE_DEV_END/' $< > $@

r1_stub_type_dev.inc: ../../src/stub/nvkvm_stub.c
	awk '/^#define NVKVM_DEV_UNKNOWN/ { print } \
	     /^static int handle_type_matches_dev/,/^}$$/ { print }' $< > $@

test_r1_type_dev: test_r1_type_dev.c r1_type_dev.inc r1_stub_type_dev.inc
	$(CC) $(CFLAGS) -I. -o $@ test_r1_type_dev.c

# NOTE: test_isolate is the ONLY suite that drives a real spawned isolate over
# the real socket protocol, so keep it green.  It failed 4 of its 9 cases at
# runtime until 2026-08-21, and the note that used to sit here blamed "isolate
# id disagreement" -- which was wrong, and was masking TWO real production bugs
# (the child fexecve'd its own command socket when fd 0 was free, and
# nvkvm_isolate_table_init left sync_open_fd at 0 so claiming a slot close()d
# fd 0).  run_tests.sh carries the full diagnosis.  `make run` exits 0 again;
# `make check` / run_tests.sh is still the pass/fail signal, because it also
# pins the case counts and catches a suite that silently stops building.
run: test_objects test_handle test_isolate test_tables test_open_scm test_ctrl_gate test_nvkms_allowlist test_stub_ptr_sanitize test_r1_type_dev
	./test_objects
	./test_handle
	./test_isolate
	./test_tables
	./test_open_scm
	./test_ctrl_gate
	./test_nvkms_allowlist
	./test_stub_ptr_sanitize
	./test_r1_type_dev

# `check` is what CI runs, and what you should run too.  `run` above is the raw
# thing: it does not pin the assertion counts, so a suite that quietly loses
# half its cases still exits 0.  run_tests.sh pins the counts AND names the
# known-failing isolate cases explicitly (that list is empty now), so the suite
# is GREEN only when exactly the expected set fails -- and RED for anything
# else, including a suite that fails to build, which plain `make` hides by
# aborting on the first error.
check:
	@bash run_tests.sh

clean:
	rm -f test_objects test_handle mock_stub test_isolate test_tables test_open_scm test_ctrl_gate test_nvkms_allowlist test_stub_ptr_sanitize test_r1_type_dev stub_sanitize.inc r1_type_dev.inc r1_stub_type_dev.inc
