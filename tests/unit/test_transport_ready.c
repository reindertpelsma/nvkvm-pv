/*
 * test_transport_ready.c — the guard that stops nvkvm from NULL-dereferencing
 *                          when it is loaded with no nvkvm virtio device.
 *
 * THE BUG. nvkvm_init() calls register_devices() and nvkvm_hostfile_init()
 * unconditionally: with no nvkvm virtio device, register_virtio_driver()
 * succeeds, probe() never runs, and five 0666 character devices, a procfs tree
 * and sysfs attributes are created anyway, backed by nothing. In that state
 * nvkvm_init()'s memset(&nvkvm, 0, ...) is the ONLY initialisation
 * struct nvkvm_state ever gets — everything else lives in nvkvm_virtio_init(),
 * which is called from probe. So inflight_list is a list_head whose next and
 * prev are both NULL rather than pointing at itself, and the first open() of
 * the device reaches inflight_enqueue() -> list_add_tail() -> __list_add(),
 * which does WRITE_ONCE(prev->next, new) with prev == NULL:
 *
 *   BUG: kernel NULL pointer dereference, address: 0000000000000000
 *   RIP: 0010:nvkvm_send_sync+0xeb/0x240 [nvkvm_guest]
 *   simple_req -> nvkvm_virtio_create_isolate -> nvkvm_fd_ctx_open_dev
 *                                             -> nvkvm_open   (Comm: ksplashqml)
 *
 * WHAT THIS SUITE PINS. nvkvm_transport_ready() is the single predicate every
 * device-less-reachable entry point now consults. It is EXTRACTED FROM
 * src/guest/nvkvm_virtio.c at build time (see the Makefile rule for
 * transport_ready.inc) rather than copied here, so this cannot drift away from
 * the code it is pinning: change the real function and this suite changes with
 * it; delete the real function and the compile fails on an undefined symbol.
 *
 * WHAT IT DOES NOT PROVE. It is a host binary; it does not run in a kernel and
 * cannot show an open() failing. That was verified separately by building the
 * module against a real kernel, loading it on a machine with no nvkvm device,
 * and opening all four nodes — every one returned ENODEV with no fault. What
 * this suite is for is the property that must not silently regress: that the
 * predicate rejects a state the way struct nvkvm_state ACTUALLY looks when
 * probe never ran, which is the case a `!state->vdev` check alone would have
 * got wrong the moment anything partially initialised the struct.
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/* Just enough of the kernel's list_head and of struct nvkvm_state for the
 * extracted predicate to compile. The field NAMES and their semantics are what
 * matter and they are pinned by the extraction: if the real predicate starts
 * looking at a field that is not here, this file stops compiling. */
struct list_head { struct list_head *next, *prev; };
struct virtio_device;
struct virtqueue;

struct nvkvm_state {
	struct virtio_device *vdev;
	struct virtqueue     *vq_tx;
	struct list_head      inflight_list;
};

#include "transport_ready.inc"

static int run, passed;

static void chk(const char *name, bool got, bool want)
{
	run++;
	if (got == want) { passed++; printf("[ PASS ] %s\n", name); }
	else printf("[ FAIL ] %s: got %s, wanted %s\n", name,
		    got ? "ready" : "not ready", want ? "ready" : "not ready");
}

int main(void)
{
	struct nvkvm_state s;
	/* Stand-ins: the predicate only ever tests these for NULL. */
	struct virtio_device *some_vdev = (struct virtio_device *)0x1000;
	struct virtqueue     *some_vq   = (struct virtqueue *)0x2000;

	/* A NULL state must never be dereferenced. */
	chk("null state is not ready", nvkvm_transport_ready(NULL), false);

	/* EXACTLY what nvkvm_init()'s memset leaves behind when probe never
	 * ran. This is the state that produced the oops. */
	memset(&s, 0, sizeof s);
	chk("zeroed state (probe never ran) is not ready",
	    nvkvm_transport_ready(&s), false);

	/* The case a `!state->vdev` guard on its own would have waved through,
	 * and the reason the predicate looks at the list head as well: the list
	 * head is the field that was actually dereferenced, and it is what
	 * distinguishes "probe ran" from "something set a pointer". */
	memset(&s, 0, sizeof s);
	s.vdev  = some_vdev;
	s.vq_tx = some_vq;
	chk("pointers set but inflight_list never INIT_LIST_HEAD'd is not ready",
	    nvkvm_transport_ready(&s), false);

	/* Device removed: nvkvm_virtio_fini() clears vdev AND the virtqueues,
	 * which del_vqs() has freed. Either alone must be enough to refuse. */
	memset(&s, 0, sizeof s);
	s.inflight_list.next = s.inflight_list.prev = &s.inflight_list;
	s.vq_tx = some_vq;
	chk("no vdev is not ready", nvkvm_transport_ready(&s), false);

	memset(&s, 0, sizeof s);
	s.inflight_list.next = s.inflight_list.prev = &s.inflight_list;
	s.vdev = some_vdev;
	chk("no tx virtqueue is not ready", nvkvm_transport_ready(&s), false);

	/* And the positive case, without which every line above is vacuous:
	 * exactly what nvkvm_virtio_init() leaves behind. */
	memset(&s, 0, sizeof s);
	s.vdev  = some_vdev;
	s.vq_tx = some_vq;
	s.inflight_list.next = s.inflight_list.prev = &s.inflight_list;
	chk("a fully probed state is ready", nvkvm_transport_ready(&s), true);

	printf("%d/%d tests passed\n", passed, run);
	return passed == run ? 0 : 1;
}
