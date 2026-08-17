/*
 * nvkvm_objects.c — RM object graph tracking (host side)
 *
 * Mirrors gVisor pkg/sentry/devices/nvproxy/object.go.
 *
 * Why we need this:
 *  - When the guest allocates an RM object (NV_ESC_RM_ALLOC), the host must
 *    record it so that:
 *      a) We can validate handles in subsequent NV_ESC_RM_CONTROL calls.
 *      b) We can cascade-free dependents when a parent object is freed.
 *      c) We can clean up all objects when a session ends (guest process exit).
 *
 * The dependency graph mirrors the NVIDIA RM's own dependency semantics:
 *  - Freeing an object also frees all objects that depended on it (rdeps).
 *  - The free order is: dependents first, then the object itself.
 *
 * Lock ordering: session.clients_lock > client.lock
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "qemu/osdep.h"
#include "virtio_nvgpu.h"

/* ── Client allocation ────────────────────────────────────────────────────── */

struct nvkvm_client *nvkvm_client_alloc(uint32_t handle)
{
	struct nvkvm_client *c = g_new0(struct nvkvm_client, 1);
	c->handle = handle;
	pthread_mutex_init(&c->lock, NULL);
	return c;
}

/* ── Object lookup ────────────────────────────────────────────────────────── */

/*
 * Objects are stored in a flat array indexed by handle % MAX. Handles are
 * driver-assigned 32-bit values; collisions are extremely rare in practice
 * but we check for them on insert.
 */
static int obj_index(uint32_t handle)
{
	return (int)(handle % NVKVM_MAX_OBJECTS_PER_CLIENT);
}

struct nvkvm_object *nvkvm_obj_lookup(struct nvkvm_client *client,
				      uint32_t handle)
{
	struct nvkvm_object *o = client->resources[obj_index(handle)];
	if (o && o->handle == handle)
		return o;
	return NULL;
}

/* ── Object add ──────────────────────────────────────────────────────────── */

int nvkvm_obj_add(struct nvkvm_client *client, uint32_t handle,
		  uint32_t class_id, uint32_t parent_handle,
		  const struct nvkvm_object_impl *impl)
{
	int idx = obj_index(handle);
	struct nvkvm_object *o;

	if (handle == NV01_NULL_OBJECT)
		return -EINVAL;

	if (client->resources[idx]) {
		NVKVM_DBG(
			"nvkvm: obj_add: handle 0x%x slot collision\n",
			handle);
		return -EEXIST;
	}

	o = g_new0(struct nvkvm_object, 1);
	o->handle        = handle;
	o->class_id      = class_id;
	o->parent_handle = parent_handle;
	o->impl          = impl;

	client->resources[idx] = o;
	client->nresources++;

	/* Wire up parent dependency */
	if (parent_handle != NV01_NULL_OBJECT) {
		nvkvm_obj_add_dep(client, handle, parent_handle);
	}

	return 0;
}

/* ── Dependency wiring ────────────────────────────────────────────────────── */

void nvkvm_obj_add_dep(struct nvkvm_client *client,
		       uint32_t h1, uint32_t h2)
{
	struct nvkvm_object *o1 = nvkvm_obj_lookup(client, h1);
	struct nvkvm_object *o2 = nvkvm_obj_lookup(client, h2);

	if (!o1 || !o2)
		return;

	/* Add o2 to o1.deps */
	o1->deps = g_realloc(o1->deps,
			     (o1->ndeps + 1) * sizeof(*o1->deps));
	o1->deps[o1->ndeps++] = o2;

	/* Add o1 to o2.rdeps */
	o2->rdeps = g_realloc(o2->rdeps,
			      (o2->nrdeps + 1) * sizeof(*o2->rdeps));
	o2->rdeps[o2->nrdeps++] = o1;
}

/* ── Object free ──────────────────────────────────────────────────────────── */

/*
 * Build a topological free list: obj first, then all transitive rdeps.
 * We traverse rdeps recursively (depth-first) and prepend to the free list
 * so that dependents are freed before the object they depend on.
 * Mirrors gVisor's prependFreedLockedRecursive().
 */
static void collect_free_list(struct nvkvm_object *o,
			      struct nvkvm_object **free_list,
			      int *free_count)
{
	int i;

	/* Check if already in list */
	for (i = 0; i < *free_count; i++)
		if (free_list[i] == o)
			return;

	/* Recurse into rdeps first */
	for (i = 0; i < o->nrdeps; i++)
		collect_free_list(o->rdeps[i], free_list, free_count);

	/* Then add self */
	free_list[(*free_count)++] = o;
}

void nvkvm_obj_free(struct nvkvm_client *client, uint32_t handle)
{
	struct nvkvm_object *o = nvkvm_obj_lookup(client, handle);
	struct nvkvm_object *free_list[NVKVM_MAX_OBJECTS_PER_CLIENT];
	int free_count = 0;
	int i, j;

	if (!o) {
		/* Driver permits freeing nonexistent handles as no-op */
		return;
	}

	collect_free_list(o, free_list, &free_count);

	/* Free in list order (dependents before dependencies) */
	for (i = 0; i < free_count; i++) {
		struct nvkvm_object *obj = free_list[i];

		/* Call impl-specific release */
		if (obj->impl && obj->impl->release)
			obj->impl->release(obj);

		/* Remove rdep links pointing to this object */
		for (j = 0; j < obj->ndeps; j++) {
			struct nvkvm_object *dep = obj->deps[j];
			int k;
			for (k = 0; k < dep->nrdeps; k++) {
				if (dep->rdeps[k] == obj) {
					dep->rdeps[k] =
						dep->rdeps[--dep->nrdeps];
					break;
				}
			}
		}

		/* Remove from client table */
		client->resources[obj_index(obj->handle)] = NULL;
		client->nresources--;

		g_free(obj->deps);
		g_free(obj->rdeps);
		g_free(obj);
	}
}

/* ── Client free (session teardown) ──────────────────────────────────────── */

void nvkvm_client_free(struct nvkvm_session *session,
		       struct nvkvm_client *client)
{
	int i;

	pthread_mutex_lock(&client->lock);
	/* Free all remaining objects */
	for (i = 0; i < NVKVM_MAX_OBJECTS_PER_CLIENT; i++) {
		struct nvkvm_object *o = client->resources[i];
		if (!o) continue;
		if (o->impl && o->impl->release)
			o->impl->release(o);
		g_free(o->deps);
		g_free(o->rdeps);
		g_free(o);
		client->resources[i] = NULL;
	}
	client->nresources = 0;
	pthread_mutex_unlock(&client->lock);
	pthread_mutex_destroy(&client->lock);
	g_free(client);
}
