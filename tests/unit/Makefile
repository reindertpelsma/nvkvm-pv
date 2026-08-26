CC      = gcc
CFLAGS  = -Wall -Wextra -g \
          -Istubs \
          -I../../src \
          -I../../src/qemu \
          -I../../src/common \
          -I../../src/guest
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

all: test_objects test_handle mock_stub test_isolate test_tables test_open_scm test_ctrl_gate test_nvkms_allowlist test_stub_ptr_sanitize test_kvm_slot test_stub_window test_drm_devinfo test_r1_type_dev test_transport_ready test_relay_state test_relay_clip test_relay_wiring test_uidmap

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

# A-21.  nvkvm_mmap_host.c needs QEMU's osdep.h and the whole VirtIONvgpu
# world, so the KVM slot pool is EXTRACTED from it between explicit markers
# rather than linked -- same technique as stub_sanitize.inc above, same
# reason: a copied allocator drifts from the real one and then the test is
# green about the wrong code.  The block is deliberately self-contained (no
# QEMU headers, no NVKVM_DBG) so this extraction stays possible.  Lose the
# markers and the .inc comes back empty, which fails the link.
kvm_slot_pool.inc: ../../src/qemu/nvkvm_mmap_host.c
	awk '/NVKVM_KVM_SLOT_POOL_BEGIN/,/NVKVM_KVM_SLOT_POOL_END/' $< > $@

test_uidmap: test_uidmap.c ../../src/common/nvkvm_uidmap.h
	$(CC) $(CFLAGS) -I../../src/common -o $@ test_uidmap.c $(LDFLAGS)

test_kvm_slot: test_kvm_slot.c kvm_slot_pool.inc
	$(CC) $(CFLAGS) -I. -o $@ test_kvm_slot.c $(LDFLAGS)
# U-9.  Same extraction trick, two sources: the gate lives in the freestanding
# stub (which cannot be linked into a hosted binary at all) and the allocator
# that feeds it lives in QEMU.  Both are marker-delimited so a lost marker
# yields an empty .inc and an #error / link failure rather than a suite that
# quietly stops testing anything.
stub_window.inc: ../../src/stub/nvkvm_stub.c
	awk '/NVKVM_STUB_WINDOW_BEGIN/,/NVKVM_STUB_WINDOW_END/' $< > $@

win_alloc.inc: ../../src/qemu/nvkvm_isolate_handlers.c
	awk '/NVKVM_WIN_ALLOC_BEGIN/,/NVKVM_WIN_ALLOC_END/' $< > $@

test_stub_window: test_stub_window.c stub_window.inc win_alloc.inc
	$(CC) $(CFLAGS) -I. -o $@ test_stub_window.c

# The nvidia-drm GET_DEV_INFO layout table.  The selector and the fixup are
# header-only (src/guest/nvkvm_drm_abi.h), so those are just included -- same as
# test_nvkms_allowlist.  The OTHER seven drm_nvidia_*_params structs live inside
# a kernel .c that cannot be compiled hosted, so they are EXTRACTED between
# markers like kvm_slot_pool.inc above: the test static-asserts their sizes and
# offsets against the vendor headers, and a lost marker yields an empty .inc and
# a compile error rather than a suite that silently pins nothing.
drm_param_structs.inc: ../../src/guest/nvkvm_drm.c
	awk '/NVKVM_DRM_PARAM_STRUCTS_BEGIN/,/NVKVM_DRM_PARAM_STRUCTS_END/' $< > $@

test_drm_devinfo: test_drm_devinfo.c drm_param_structs.inc \
                  ../../src/guest/nvkvm_drm_abi.h ../../src/common/nvkvm_abi.h
	$(CC) $(CFLAGS) -I. -o $@ test_drm_devinfo.c
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
# The device-presence guard.  src/guest/ is kernel code and cannot be linked
# into a hosted binary, so extract the one predicate under test the same way the
# stub's is extracted above: this suite pins the REAL function, and an empty
# extraction fails the compile on an undefined symbol rather than passing
# vacuously.  -Wno-unused-function because the extracted text may carry more
# than the test calls.
transport_ready.inc: ../../src/guest/nvkvm_virtio.c
	awk '/^bool nvkvm_transport_ready/,/^}$$/' $< > $@

test_transport_ready: test_transport_ready.c transport_ready.inc
	$(CC) $(CFLAGS) -I. -Wno-unused-function -o $@ test_transport_ready.c

# RR-06/RR-07.  Extract the production helpers so the test pins disconnected
# frame ownership and connection-generation reset without needing QEMU's UI
# headers.
relay_state_helpers.inc: ../../src/qemu/nvkvm_display_relay.c
	awk '/NVKVM_RELAY_STATE_HELPERS_BEGIN/,/NVKVM_RELAY_STATE_HELPERS_END/' $< > $@

test_relay_state: test_relay_state.c relay_state_helpers.inc
	$(CC) $(CFLAGS) -I. -o $@ test_relay_state.c

# RR-08.  The QEMU display relay needs the full QEMU UI stack, so extract its
# self-contained clipboard transaction builder.  This pins the real chunking
# and resumable EAGAIN cursor; losing the markers makes the test fail to compile
# rather than silently testing a copy.
relay_clip_batch.inc: ../../src/qemu/nvkvm_display_relay.c
	awk '/NVKVM_RELAY_CLIP_BATCH_BEGIN/,/NVKVM_RELAY_CLIP_BATCH_END/' $< > $@

test_relay_clip: test_relay_clip.c relay_clip_batch.inc ../../src/common/nvkvm_broker_proto.h
	$(CC) $(CFLAGS) -I. -o $@ test_relay_clip.c

# RR-03/RR-07/RR-08 integration wiring.  These are the actual production
# registration, teardown/reset and clipboard-flush functions with only their
# QEMU event-loop dependencies stubbed.  The suite observes POLLIN/POLLOUT and
# close/reset effects, so a correct pure cursor cannot mask a missing callsite.
relay_fd_ownership.inc: ../../src/qemu/nvkvm_display_relay.c
	awk '/NVKVM_RELAY_FD_OWNERSHIP_BEGIN/,/NVKVM_RELAY_FD_OWNERSHIP_END/' $< > $@

relay_clip_wiring.inc: ../../src/qemu/nvkvm_display_relay.c
	awk '/NVKVM_RELAY_CLIP_WIRING_BEGIN/,/NVKVM_RELAY_CLIP_WIRING_END/' $< > $@

relay_writable_wiring.inc: ../../src/qemu/nvkvm_display_relay.c
	awk '/NVKVM_RELAY_WRITABLE_WIRING_BEGIN/,/NVKVM_RELAY_WRITABLE_WIRING_END/' $< > $@

relay_connect_wiring.inc: ../../src/qemu/nvkvm_display_relay.c
	awk '/NVKVM_RELAY_CONNECT_WIRING_BEGIN/,/NVKVM_RELAY_CONNECT_WIRING_END/' $< > $@

test_relay_wiring: test_relay_wiring.c relay_state_helpers.inc \
		relay_clip_batch.inc relay_fd_ownership.inc relay_clip_wiring.inc \
		relay_writable_wiring.inc relay_connect_wiring.inc
	$(CC) $(CFLAGS) -I. -Wno-unused-function -o $@ test_relay_wiring.c

# NOTE: test_isolate is the ONLY suite that drives a real spawned isolate over
# the real socket protocol, so keep it green.  It failed 4 of its 9 cases at
# runtime until 2026-08-21, and the note that used to sit here blamed "isolate
# id disagreement" -- which was wrong, and was masking TWO real production bugs
# (the child fexecve'd its own command socket when fd 0 was free, and
# nvkvm_isolate_table_init left sync_open_fd at 0 so claiming a slot close()d
# fd 0).  run_tests.sh carries the full diagnosis.  `make run` exits 0 again;
# `make check` / run_tests.sh is still the pass/fail signal, because it also
# pins the case counts and catches a suite that silently stops building.
run: test_objects test_handle test_isolate test_tables test_open_scm test_ctrl_gate test_nvkms_allowlist test_stub_ptr_sanitize test_kvm_slot test_stub_window test_drm_devinfo test_r1_type_dev test_transport_ready test_relay_state test_relay_clip test_relay_wiring test_uidmap
	./test_objects
	./test_dispatch
	./test_frontend
	./test_handle
	./test_isolate
	./test_tables
	./test_open_scm
	./test_ctrl_gate
	./test_nvkms_allowlist
	./test_stub_ptr_sanitize
	./test_uidmap
	./test_kvm_slot
	./test_stub_window
	./test_drm_devinfo
	./test_r1_type_dev
	./test_transport_ready
	./test_relay_state
	./test_relay_clip
	./test_relay_wiring test_uidmap

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
	rm -f test_uidmap test_objects test_handle mock_stub test_isolate test_tables test_open_scm test_ctrl_gate test_nvkms_allowlist test_stub_ptr_sanitize test_kvm_slot test_stub_window test_drm_devinfo test_r1_type_dev test_relay_state test_relay_clip test_relay_wiring \
	      stub_sanitize.inc kvm_slot_pool.inc stub_window.inc win_alloc.inc \
	      drm_param_structs.inc r1_type_dev.inc r1_stub_type_dev.inc \
	      relay_state_helpers.inc relay_clip_batch.inc relay_fd_ownership.inc \
	      relay_clip_wiring.inc relay_writable_wiring.inc relay_connect_wiring.inc
	rm -f test_dispatch test_frontend test_handle mock_stub test_isolate test_tables test_open_scm test_ctrl_gate test_nvkms_allowlist test_stub_ptr_sanitize stub_sanitize.inc test_transport_ready transport_ready.inc test_relay_state relay_state_helpers.inc test_relay_clip relay_clip_batch.inc test_relay_wiring relay_fd_ownership.inc relay_clip_wiring.inc relay_writable_wiring.inc relay_connect_wiring.inc
