CC      = gcc
CFLAGS  = -Wall -Wextra -g \
          -Istubs \
          -I../../src \
          -I../../src/qemu \
          -I../../src/common
LDFLAGS = -lpthread

DISPATCH_SRCS = test_dispatch.c \
                ../../src/qemu/nvkvm_objects.c \
                ../../src/qemu/nvkvm_dispatch.c \
                ../../src/qemu/nvkvm_handle.c \
                frontend_stubs.c \
                unit_stubs.c

FRONTEND_SRCS = test_frontend.c \
                ../../src/qemu/nvkvm_frontend.c \
                ../../src/qemu/nvkvm_objects.c \
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

all: test_dispatch test_frontend test_handle mock_stub test_isolate test_tables test_open_scm test_ctrl_gate test_nvkms_allowlist test_u5_params_size

test_dispatch: $(DISPATCH_SRCS)
	$(CC) $(CFLAGS) -o $@ $(DISPATCH_SRCS) $(LDFLAGS)

test_frontend: $(FRONTEND_SRCS)
	$(CC) $(CFLAGS) -o $@ $(FRONTEND_SRCS) $(LDFLAGS)

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

# U-5.  The stub is freestanding (-nostdlib -ffreestanding) and carries its own
# entry point, so it cannot be linked into a hosted test binary.  Extract the one
# function under test straight out of the stub source instead of copying it here,
# so this suite pins the REAL code and cannot drift away from it.  If the
# extraction ever comes back empty the compile fails on an undefined symbol,
# which is the failure mode we want.
u5_clamp.inc: ../../src/stub/nvkvm_stub.c
	awk '/^static void clamp_inner_params_size/,/^}$$/' $< > $@

test_u5_params_size: test_u5_params_size.c u5_clamp.inc
	$(CC) $(CFLAGS) -I. -o $@ test_u5_params_size.c

# NOTE: test_isolate is the ONLY suite that drives a real spawned isolate over
# the real socket protocol, so keep it green.  It failed 4 of its 9 cases at
# runtime until 2026-08-21, and the note that used to sit here blamed "isolate
# id disagreement" -- which was wrong, and was masking TWO real production bugs
# (the child fexecve'd its own command socket when fd 0 was free, and
# nvkvm_isolate_table_init left sync_open_fd at 0 so claiming a slot close()d
# fd 0).  run_tests.sh carries the full diagnosis.  `make run` exits 0 again;
# `make check` / run_tests.sh is still the pass/fail signal, because it also
# pins the case counts and catches a suite that silently stops building.
run: test_dispatch test_frontend test_handle test_isolate test_tables test_open_scm test_ctrl_gate test_nvkms_allowlist test_u5_params_size
	./test_dispatch
	./test_frontend
	./test_handle
	./test_isolate
	./test_tables
	./test_open_scm
	./test_ctrl_gate
	./test_nvkms_allowlist
	./test_u5_params_size

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
	rm -f test_dispatch test_frontend test_handle mock_stub test_isolate test_tables test_open_scm test_ctrl_gate test_nvkms_allowlist test_u5_params_size u5_clamp.inc
