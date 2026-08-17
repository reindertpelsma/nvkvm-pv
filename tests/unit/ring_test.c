/* ring_test.c — unit test + transport benchmark for nvkvm_ring.h (Phase 1).
 *   soak    : SPSC producer/consumer, variable records, millions through wraps,
 *             content + ordering verified.
 *   fuzz    : hostile-producer headers → peek must return BAD, never OOB
 *             (run under -fsanitize=address to prove no out-of-bounds).
 *   pingpong: cross-core ring round-trip latency vs the socketpair numbers
 *             (sp_pingpong: ~10us busy-poll, ~884us full nvkvm round-trip).
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>

#include "nvkvm_ring.h"

#define N (1u << 16)   /* 64 KiB ring */
static void pin(int c){ cpu_set_t s; CPU_ZERO(&s); CPU_SET(c,&s); sched_setaffinity(0,sizeof s,&s); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static struct nvkvm_ring *ring_new(void){
	struct nvkvm_ring *r = aligned_alloc(64, sizeof(*r) + N);
	memset(r, 0, sizeof(*r) + N); r->size = N; return r;
}
static uint32_t cksum(const uint8_t *p, uint32_t n){ uint32_t c=2166136261u; for(uint32_t i=0;i<n;i++){c^=p[i];c*=16777619u;} return c; }

/* ---- soak ---- */
#define SOAK_RECS 5000000u
struct soak_rec { uint32_t seq; uint32_t plen; uint32_t ck; };
static struct nvkvm_ring *g_ring;
static void *soak_producer(void *a){
	pin(1);
	for(uint32_t i=0;i<SOAK_RECS;i++){
		uint32_t plen = sizeof(struct soak_rec) + (i % 200);   /* 12..211 bytes */
		uint64_t total; void *p;
		while(!(p = nvkvm_ring_reserve(g_ring, plen, &total))) sched_yield();
		struct soak_rec sr = { i, plen, 0 };
		memcpy(p, &sr, sizeof sr);
		uint8_t *body = (uint8_t*)p + sizeof sr;
		for(uint32_t k=0;k<plen-sizeof sr;k++) body[k] = (uint8_t)(i+k);
		((struct soak_rec*)p)->ck = cksum(body, plen-sizeof sr);
		nvkvm_ring_commit(g_ring, total);
	}
	return 0;
}
static int soak_run(void){
	g_ring = ring_new();
	pthread_t prod; pthread_create(&prod,0,soak_producer,0);
	pin(2);
	uint32_t expect=0; uint8_t *pay; uint32_t len; uint64_t total;
	double t0=now();
	while(expect<SOAK_RECS){
		int rc = nvkvm_ring_peek(g_ring,&pay,&len,&total);
		if(rc==NVKVM_RING_EMPTY){ continue; }
		if(rc!=NVKVM_RING_OK){ fprintf(stderr,"soak: peek BAD at %u\n",expect); return 1; }
		struct soak_rec sr; memcpy(&sr,pay,sizeof sr);
		if(sr.seq!=expect){ fprintf(stderr,"soak: order seq=%u expect=%u\n",sr.seq,expect); return 1; }
		uint32_t blen = sr.plen - sizeof(struct soak_rec);
		if(cksum(pay+sizeof sr, blen)!=sr.ck){ fprintf(stderr,"soak: corrupt seq=%u\n",sr.seq); return 1; }
		nvkvm_ring_pop(g_ring,total);
		expect++;
	}
	double dt=now()-t0;
	pthread_join(prod,0);
	printf("soak    : %u records OK, no loss/reorder/corruption  (%.1f M rec/s)\n",
	       SOAK_RECS, SOAK_RECS/dt/1e6);
	free(g_ring); return 0;
}

/* ---- fuzz: hostile producer headers ---- */
static int fuzz_one(uint32_t len, uint32_t type, uint64_t tail, const char *name){
	struct nvkvm_ring *r = ring_new();
	r->tail = tail;                                  /* pretend producer committed */
	struct nvkvm_ring_rec *h = (struct nvkvm_ring_rec*)NVKVM_RING_DATA(r);
	h->len = len; h->type = type;
	uint8_t *pay; uint32_t pl; uint64_t tot;
	int rc = nvkvm_ring_peek(r,&pay,&pl,&tot);
	int ok = (rc == NVKVM_RING_BAD);
	printf("fuzz    : %-22s len=%-8u → %s\n", name, len, ok?"BAD (rejected) OK":"!! ACCEPTED !!");
	free(r);
	return ok?0:1;
}
static int fuzz_run(void){
	int bad=0;
	bad |= fuzz_one(0xffffffffu, NVKVM_RING_REC_DATA, N, "len huge");
	bad |= fuzz_one(N+8,         NVKVM_RING_REC_DATA, N, "len > N");
	bad |= fuzz_one(13,          NVKVM_RING_REC_DATA, N, "len misaligned");
	bad |= fuzz_one(4,           NVKVM_RING_REC_DATA, N, "len < header");
	bad |= fuzz_one(64,          7,                   N, "bad type");
	bad |= fuzz_one(N,           NVKVM_RING_REC_DATA, 8, "len > avail");
	return bad;
}

/* ---- pingpong: cross-core ring RTT ---- */
struct pp { struct nvkvm_ring *a, *b; int iters; };
static void *pp_echo(void *arg){
	struct pp *pp=arg; pin(3);
	for(int i=0;i<pp->iters;i++){
		uint8_t *pay; uint32_t len; uint64_t tot; int rc;
		while((rc=nvkvm_ring_peek(pp->a,&pay,&len,&tot))==NVKVM_RING_EMPTY) ;
		uint8_t buf[64]; memcpy(buf,pay,len); nvkvm_ring_pop(pp->a,tot);
		void *p; uint64_t t2; while(!(p=nvkvm_ring_reserve(pp->b,len,&t2))) ;
		memcpy(p,buf,len); nvkvm_ring_commit(pp->b,t2);
	}
	return 0;
}
static int pp_run(void){
	struct pp pp = { ring_new(), ring_new(), 200000 };
	pthread_t e; pthread_create(&e,0,pp_echo,&pp);
	pin(2);
	uint8_t msg[64]; memset(msg,0x5a,sizeof msg);
	double t0=now();
	for(int i=0;i<pp.iters;i++){
		void *p; uint64_t t; while(!(p=nvkvm_ring_reserve(pp.a,sizeof msg,&t))) ;
		memcpy(p,msg,sizeof msg); nvkvm_ring_commit(pp.a,t);
		uint8_t *pay; uint32_t len; uint64_t tot; int rc;
		while((rc=nvkvm_ring_peek(pp.b,&pay,&len,&tot))==NVKVM_RING_EMPTY) ;
		nvkvm_ring_pop(pp.b,tot);
	}
	double dt=now()-t0;
	pthread_join(e,0);
	printf("pingpong: cross-core ring RTT = %.2f us  (%.0f rt/s)  [vs sp_pingpong busy-poll ~10us, full nvkvm ~884us]\n",
	       dt/pp.iters*1e6, pp.iters/dt);
	free(pp.a); free(pp.b); return 0;
}

int main(int argc, char **argv){
	int rc=0;
	rc |= soak_run();
	rc |= fuzz_run();
	rc |= pp_run();
	printf(rc?"RING TEST: FAIL\n":"RING TEST: PASS\n");
	return rc;
}
