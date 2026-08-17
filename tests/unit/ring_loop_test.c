/* ring_loop_test.c — Phase 3 consumer-loop lifecycle (enter_loop / exit-edge).
 *
 * Proves the NO-FUTEX wait/wake model (docs/design/command_buffer.md): the
 * consumer runs a ring loop driven by an `enter_loop`-style session, exits on
 * an idle timeout, and the guest re-enters when it falls behind — with no
 * record ever stranded.  This is the lost-wakeup proof for the design that
 * replaced the futex doorbell.
 *
 * Two correctness pieces, both modelled here and stressed under TSan:
 *   1. exit-edge re-check (efficiency): before committing to exit, the consumer
 *      calls nvkvm_ring_has_work() once more; a record that raced in aborts the
 *      exit so we don't churn through a needless exit/re-enter.
 *   2. level-triggered guest (THE keystone): after publishing, the guest
 *      re-evaluates "consumer idle && processed < published → start a session"
 *      and keeps re-evaluating until caught up.  It never assumes an in-flight
 *      session will cover a just-published record — so even if the consumer
 *      exited in the gap, the next evaluation re-enters and the record lands.
 *
 * The session-dispatch here uses a mutex+cond ONLY as test scaffolding; in the
 * real system that channel is the isolate's recvmsg + the virtqueue enter_loop
 * transaction.  What is under test is the RING exit edge + the re-enter logic,
 * which are transport-independent.
 *
 * A watchdog alarm fails loudly instead of hanging if a record is ever stranded.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sched.h>

#include "nvkvm_ring.h"

#define N (1u << 16)

static inline void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
	__builtin_ia32_pause();
#else
	__asm__ __volatile__("" ::: "memory");
#endif
}

static struct nvkvm_ring *ring_new(void)
{
	struct nvkvm_ring *r = aligned_alloc(64, sizeof(*r) + N);
	memset(r, 0, sizeof(*r) + N);
	r->size = N;
	return r;
}

/* ── shared state ─────────────────────────────────────────────────────────── */
static struct nvkvm_ring *R;
static _Atomic uint64_t   published;   /* records committed to the ring        */
static _Atomic uint64_t   processed;   /* records popped + handled             */
static uint32_t           TOTAL;
static int                IDLE_X;       /* consumer idle-spin budget before exit */
static int                GAPPY;

/* session dispatch — scaffolding for enter_loop (real = recvmsg/virtqueue) */
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv = PTHREAD_COND_INITIALIZER;
static int  session_running;   /* a consumer session is live (enter_loop in flight) */
static int  session_req;       /* a start was requested                        */
static int  shutdown_;
static long sessions;          /* lifetime sessions = 1 + re-enters            */

/* consumer-only */
static int      order_ok = 1;
static uint32_t expect_seq;

/* Level-triggered: start a session iff none is live and we're behind. */
static void request_if_behind(void)
{
	pthread_mutex_lock(&lk);
	if (!session_running &&
	    atomic_load(&processed) < atomic_load(&published)) {
		session_running = 1;
		session_req     = 1;
		pthread_cond_signal(&cv);
	}
	pthread_mutex_unlock(&lk);
}

static void *consumer_fn(void *arg)
{
	(void)arg;
	for (;;) {
		pthread_mutex_lock(&lk);
		while (!session_req && !shutdown_)
			pthread_cond_wait(&cv, &lk);
		if (shutdown_ && !session_req) {
			pthread_mutex_unlock(&lk);
			return NULL;
		}
		session_req = 0;
		sessions++;
		pthread_mutex_unlock(&lk);

		/* enter_loop body */
		int idle = 0;
		for (;;) {
			uint8_t *pay; uint32_t len; uint64_t tot;
			int rc = nvkvm_ring_peek(R, &pay, &len, &tot);
			if (rc == NVKVM_RING_OK) {
				uint32_t seq;
				memcpy(&seq, pay, sizeof(seq));
				if (seq != expect_seq) order_ok = 0;
				expect_seq++;
				nvkvm_ring_pop(R, tot);
				atomic_fetch_add(&processed, 1);
				idle = 0;
				continue;
			}
			if (rc == NVKVM_RING_BAD) {
				fprintf(stderr, "loop: peek BAD\n");
				return NULL;
			}
			if (++idle < IDLE_X) {       /* empty, keep spinning a bit */
				cpu_relax();
				continue;
			}
			/* idle timeout → commit to exit, but RE-CHECK first */
			if (nvkvm_ring_has_work(R)) {
				idle = 0;            /* raced-in record → abort exit */
				continue;
			}
			pthread_mutex_lock(&lk);
			session_running = 0;          /* enter_loop completes here */
			pthread_mutex_unlock(&lk);
			break;
		}
	}
}

static void tiny_pause(void)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 500000 };  /* 0.5 ms */
	nanosleep(&ts, NULL);
}

static void *producer_fn(void *arg)
{
	(void)arg;
	for (uint32_t i = 0; i < TOTAL; i++) {
		uint64_t tot; void *p;
		/*
		 * Backpressure on a full ring is a stall point too: the consumer
		 * may have idled out while the ring was full, so we MUST re-ensure
		 * a session here, not only after a commit — else the producer waits
		 * for space the exited consumer will never free.  (In production
		 * this stall doesn't exist: a full ring takes the virtqueue relief
		 * valve instead of blocking.  This models the level-triggered rule:
		 * re-evaluate "idle && behind → enter_loop" at EVERY wait point.)
		 */
		while (!(p = nvkvm_ring_reserve(R, sizeof(uint32_t), &tot))) {
			request_if_behind();
			sched_yield();
		}
		memcpy(p, &i, sizeof(i));
		nvkvm_ring_commit(R, tot);              /* publish T (release tail) */
		atomic_fetch_add(&published, 1);        /* bookkeeping for the guest */
		request_if_behind();                    /* publish-before-check */

		if (GAPPY && (i % 32) == 0)             /* force frequent idle exits */
			tiny_pause();
	}
	/* drain: level-triggered until everything is processed */
	while (atomic_load(&processed) < TOTAL) {
		request_if_behind();
		cpu_relax();
	}
	return NULL;
}

static int run(const char *name, uint32_t total, int idle_x, int gappy,
	       int want_reenters)
{
	R = ring_new();
	published = 0; processed = 0; TOTAL = total; IDLE_X = idle_x; GAPPY = gappy;
	session_running = 0; session_req = 0; shutdown_ = 0; sessions = 0;
	order_ok = 1; expect_seq = 0;

	pthread_t ct, pt;
	pthread_create(&ct, NULL, consumer_fn, NULL);
	pthread_create(&pt, NULL, producer_fn, NULL);
	pthread_join(pt, NULL);

	pthread_mutex_lock(&lk);
	shutdown_ = 1;
	pthread_cond_signal(&cv);
	pthread_mutex_unlock(&lk);
	pthread_join(ct, NULL);

	int fail = 0;
	if (atomic_load(&processed) != total) {
		printf("%-9s: FAIL — processed %llu/%u (stranded record!)\n", name,
		       (unsigned long long)atomic_load(&processed), total); fail = 1;
	}
	if (!order_ok) {
		printf("%-9s: FAIL — out-of-order delivery\n", name); fail = 1;
	}
	if (want_reenters && sessions < 2) {
		printf("%-9s: FAIL — only %ld session(s); re-enter path not exercised\n",
		       name, sessions); fail = 1;
	}
	if (!fail)
		printf("%-9s: %u recs OK, %ld sessions (re-enters), in order\n",
		       name, total, sessions);
	free(R);
	return fail;
}

static void on_alarm(int sig)
{
	(void)sig;
	const char *m = "ring loop: WATCHDOG — record stranded (hang)\n"
			"RING LOOP TEST: FAIL\n";
	if (write(2, m, strlen(m)) < 0) { /* ignore */ }
	_exit(2);
}

int main(void)
{
	signal(SIGALRM, on_alarm);
	alarm(60);

	int fail = 0;
	/* gappy + tiny idle budget: sessions exit constantly → exit/re-enter race
	 * hammered on nearly every record */
	fail |= run("gappy",    5000,   8, 1, /*want_reenters=*/1);
	/* hammer: back-to-back, large run; exit-edge re-check should keep it to
	 * few sessions while never losing a record */
	fail |= run("hammer", 2000000, 64, 0, /*want_reenters=*/0);
	/* idle: ring far bigger than traffic, slow producer, mostly-idle consumer.
	 * Correctness only — too few records to *guarantee* an idle-out+re-enter
	 * (the re-enter path is proven deterministically by gappy above). */
	fail |= run("idle",       64,   4, 1, /*want_reenters=*/0);

	printf(fail ? "RING LOOP TEST: FAIL\n" : "RING LOOP TEST: PASS\n");
	return fail;
}
