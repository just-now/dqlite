#include "src/lib/threadpool.h"
#include "src/lib/sm.h"
#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include "src/lib/queue.h"
#include "src/utils.h"


/**
 *  Planner thread state machine.
 *
 * signal() &&
 * empty(o) &&                     signal() && exiting
 * empty(u) &&     +-----> NOTHING ----------------> EXITED
 * !exiting        +-------  ^ |
 *                           | |
 *               empty(o) && | | signal()
 *               empty(u)    | | !empty(o) || !empty(u)
 *                           | |
 *                           | |
 *                           | V
 *    !empty(o) && +-----> DRAINING
 *    !empty(u) && +-------  ^ |
 * type(head(o)) != BAR      | |
 *                           | | type(head(o)) == BAR
 *            in_flight == 0 | |
 *                           | V
 *                         BARRIER --------+ signal()
 *                           ^ |   <-------+
 *                           | |
 *                  empty(u) | | !empty(u)
 *                           | V
 *                      DRAINING_UNORD
 */

enum planner_states {
	PS_NOTHING,
	PS_DRAINING,
	PS_BARRIER,
	PS_DRAINING_UNORD,
	PS_EXITED,
};

static const struct sm_conf planner_states[SM_STATES_MAX] = {
       [PS_NOTHING] = {
               .flags   = SM_INITIAL,
               .name    = "nothing",
               .allowed = BITS(PS_DRAINING)
			| BITS(PS_EXITED),
       },
       [PS_DRAINING] = {
               .name    = "draining",
               .allowed = BITS(PS_DRAINING)
			| BITS(PS_NOTHING)
			| BITS(PS_BARRIER),
       },
       [PS_BARRIER] = {
               .name    = "barrier",
               .allowed = BITS(PS_DRAINING_UNORD)
			| BITS(PS_DRAINING)
			| BITS(PS_BARRIER),
       },
       [PS_DRAINING_UNORD] = {
               .name    = "unord-draining",
               .allowed = BITS(PS_BARRIER)
       },
       [PS_EXITED] = {
               .flags   = SM_FINAL,
               .name    = "exited",
               .allowed = 0,
       },
};

enum {
	POOL_THREADPOOL_SIZE = 4,
	MAX_THREADPOOL_SIZE  = 1024,
	POOL_LOOP_MAGIC      = 0x00ba5e1e55ba5500, /* baseless bass */
};

typedef struct pool_thread pool_thread_t;
typedef struct pool_impl pool_impl_t;

struct targs {
	pool_impl_t *pi;
	uv_sem_t    *sem;
	uint32_t     idx;
};

struct pool_thread {
	queue        inq;
	uv_cond_t    cond;
	uv_thread_t  thread;
	struct targs arg;
};

struct pool_impl {
	uint32_t       nthreads;
	uv_mutex_t     mutex;
	pool_thread_t *threads;

	queue          outq;
	uv_mutex_t     outq_mutex;
	uv_async_t     outq_async;
	uint64_t       active_ws;

	queue 	       ordered;
	queue 	       unordered;
	struct sm      planner_sm;
	uv_cond_t      planner_cond;
	uv_thread_t    planner_thread;

	uv_key_t       thread_key;
	uint32_t       in_flight;
	bool           exiting;
	uint32_t       o_prev;
	uint32_t       qos;
};

static inline bool has_active_ws(pool_t *pool)
{
	return pool->pi->active_ws > 0;
}

static inline void w_register(pool_t *pool, pool_work_t *w)
{
	if (w->type != WT_BAR)
		pool->pi->active_ws++;
}

static inline void w_unregister(pool_t *pool, pool_work_t *)
{
	PRE(has_active_ws(pool));
	pool->pi->active_ws--;
}

static bool empty(const queue *q)
{
	return QUEUE__IS_EMPTY(q);
}

static queue *head(const queue *q)
{
	return QUEUE__HEAD(q);
}

static void push(queue *to, queue *what)
{
	QUEUE__INSERT_TAIL(to, what);
}

static queue *pop(queue *from)
{
	queue *q = QUEUE__HEAD(from);
	PRE(q != NULL);
	QUEUE__REMOVE(q);
	QUEUE__INIT(q);
	return q;
}

static queue *qos_pop(uint32_t *qos, queue *first, queue *second)
{
	PRE(!empty(first) || !empty(second));

	if (empty(first))
		return pop(second);
	else if (empty(second))
		return pop(first);

	return pop((*qos)++ % 2 ? first : second);
}

static pool_work_t *q_to_w(const queue *q)
{
	return QUEUE__DATA(q, pool_work_t, qlink);
}

static enum pool_work_type q_type(const queue *q)
{
	return q_to_w(q)->type;
}

static uint32_t q_tid(const queue *q)
{
	return q_to_w(q)->thread_id;
}

static bool planner_invariant(const struct sm *m, int prev_state)
{
	pool_impl_t *pi = container_of(m, pool_impl_t, planner_sm);
	queue *o = &pi->ordered;
	queue *u = &pi->unordered;

	return ERGO(sm_state(m) == PS_NOTHING, empty(o) && empty(u)) &&
		ERGO(sm_state(m) == PS_DRAINING,
		     ERGO(prev_state == PS_BARRIER,
			  pi->in_flight == 0 && empty(u)) &&
		     ERGO(prev_state == PS_NOTHING,
			  !empty(u) || !empty(o))) &&
		ERGO(sm_state(m) == PS_EXITED,
		     pi->exiting && empty(o) && empty(u)) &&
		ERGO(sm_state(m) == PS_BARRIER,
		     ERGO(prev_state == PS_DRAINING,
			  q_type(head(o)) == WT_BAR) &&
		     ERGO(prev_state == PS_DRAINING_UNORD, empty(u))) &&
		ERGO(sm_state(m) == PS_DRAINING_UNORD, !empty(u));
}

static void planner(void *arg)
{
	struct targs  *ta = arg;
	uv_sem_t      *sem = ta->sem;
	pool_impl_t   *pi = ta->pi;
	uv_mutex_t    *mutex = &pi->mutex;
	pool_thread_t *ts = pi->threads;
	struct sm     *planner_sm = &pi->planner_sm;
	queue 	      *o = &pi->ordered;
	queue 	      *u = &pi->unordered;
	queue 	      *q;

	sm_init(planner_sm, planner_invariant, NULL, planner_states, PS_NOTHING);
	uv_sem_post(sem);
	uv_mutex_lock(mutex);
	for (;;) {
		switch(sm_state(planner_sm)) {
		case PS_NOTHING:
			while (empty(o) && empty(u) && !pi->exiting)
				uv_cond_wait(&pi->planner_cond, mutex);
			sm_move(planner_sm,
				pi->exiting ? PS_EXITED : PS_DRAINING);
			break;
		case PS_DRAINING:
			while (!(empty(o) && empty(u))) {
				sm_move(planner_sm, PS_DRAINING);
				if (!empty(o) && q_type(head(o)) == WT_BAR) {
					sm_move(planner_sm, PS_BARRIER);
					goto ps_barrier;
				}
				q = qos_pop(&pi->qos, o, u);
				push(&ts[q_tid(q)].inq, q);
				uv_cond_signal(&ts[q_tid(q)].cond);
				if (q_type(q) >= WT_ORD1)
					pi->in_flight++;
			}
			sm_move(planner_sm, PS_NOTHING);
		ps_barrier:
			break;
		case PS_BARRIER:
			if (!empty(u)) {
				sm_move(planner_sm, PS_DRAINING_UNORD);
				break;
			}
			if (pi->in_flight == 0) {
				q = pop(o);
				PRE(q_to_w(q)->type == WT_BAR);
				free(q_to_w(q));
				sm_move(planner_sm, PS_DRAINING);
				break;
			}
			uv_cond_wait(&pi->planner_cond, mutex);
			sm_move(planner_sm, PS_BARRIER);
			break;
		case PS_DRAINING_UNORD:
			while (!empty(u)) {
				q = pop(u);
				push(&ts[q_tid(q)].inq, q);
				uv_cond_signal(&ts[q_tid(q)].cond);
			}
			sm_move(planner_sm, PS_BARRIER);
			break;
		case PS_EXITED:
			sm_fini(planner_sm);
			uv_mutex_unlock(mutex);
			return;
		default:
			POST(false && "Impossible!");
		}
	}
}

static void queue_work(pool_work_t *w)
{
	w->work_cb(w);
}

static void queue_done(pool_work_t *w)
{
	w_unregister(uv_loop_to_pool(w->loop), w);
	if (w->after_work_cb != NULL)
		w->after_work_cb(w);
}

static void worker(void *arg)
{
	struct targs        *ta = arg;
	pool_impl_t         *pi = ta->pi;
	uv_mutex_t          *mutex = &pi->mutex;
	pool_thread_t       *ts = pi->threads;
	enum pool_work_type  wtype;
	pool_work_t         *w;
	queue               *q;

	uv_key_set(&pi->thread_key, &ta->idx);
	uv_sem_post(ta->sem);
	uv_mutex_lock(mutex);
	for (;;) {
		while (empty(&ts[ta->idx].inq)) {
		    	if (pi->exiting) {
		    		uv_mutex_unlock(mutex);
		    		return;
		    	}
			uv_cond_wait(&ts[ta->idx].cond, mutex);
		}

		q = pop(&ts[ta->idx].inq);
		uv_mutex_unlock(mutex);

		w = q_to_w(q);
		wtype = w->type;
		queue_work(w);

		uv_mutex_lock(&pi->outq_mutex);
		push(&pi->outq, &w->qlink);
		uv_async_send(&pi->outq_async);
		uv_mutex_unlock(&pi->outq_mutex);

		uv_mutex_lock(mutex);
		if (wtype > WT_BAR) {
		    assert(pi->in_flight > 0);
		    if (--pi->in_flight == 0)
			    uv_cond_signal(&pi->planner_cond);
		}
	}
}

static void pool_cleanup(pool_t *loop)
{
	pool_impl_t   *pi = loop->pi;
	pool_thread_t *ts = pi->threads;
	uint32_t i;

	if (pi->nthreads == 0)
		return;

	pi->exiting = true;
	uv_cond_signal(&pi->planner_cond);

	if (uv_thread_join(&pi->planner_thread))
		abort();
	uv_cond_destroy(&pi->planner_cond);
	POST(empty(&pi->ordered) && empty(&pi->unordered));

	for (i = 0; i < pi->nthreads; i++) {
	    	uv_cond_signal(&ts[i].cond);
		if (uv_thread_join(&ts[i].thread))
			abort();
		POST(empty(&ts[i].inq));
		uv_cond_destroy(&ts[i].cond);
	}

	free(pi->threads);
	uv_mutex_destroy(&pi->mutex);
	uv_key_delete(&pi->thread_key);
	pi->nthreads = 0;
}

static void pool_threads_init(pool_t *pool)
{
	uv_thread_options_t config;
	const char *val;
	uv_sem_t sem;
	uint32_t i;
	pool_impl_t *pi = pool->pi;
	pool_thread_t *ts;
	struct targs pa = (struct targs) {
		.sem = &sem,
		.pi = pi,
	};

	pi->qos = 0;
	pi->o_prev = WT_BAR;
	pi->exiting = false;
	pi->in_flight = 0;
	pi->nthreads = POOL_THREADPOOL_SIZE;

	val = getenv("POOL_THREADPOOL_SIZE");
	if (val != NULL)
		pi->nthreads = (uint32_t)atoi(val);
	if (pi->nthreads == 0)
		pi->nthreads = 1;
	if (pi->nthreads > MAX_THREADPOOL_SIZE)
		pi->nthreads = MAX_THREADPOOL_SIZE;
	if (uv_key_create(&pi->thread_key))
		abort();
	if (uv_mutex_init(&pi->mutex))
		abort();
	if (uv_sem_init(&sem, 0))
		abort();
	pi->threads = calloc(pi->nthreads, sizeof(pi->threads[0]));
	if (pi->threads == NULL)
		abort();

	QUEUE__INIT(&pi->ordered);
	QUEUE__INIT(&pi->unordered);

	config.flags = UV_THREAD_HAS_STACK_SIZE;
	config.stack_size = 8u << 20;

	for (i = 0, ts = pi->threads; i < pi->nthreads; i++) {
		ts[i].arg = (struct targs) {
			.pi = pi,
			.sem = &sem,
			.idx = i,
		};

		QUEUE__INIT(&ts[i].inq);
	    	if (uv_cond_init(&ts[i].cond))
			abort();
		if (uv_thread_create_ex(&ts[i].thread, &config, worker,
					&ts[i].arg))
			abort();
	}

	if (uv_cond_init(&pi->planner_cond))
		abort();
	if (uv_thread_create_ex(&pi->planner_thread, &config, planner, &pa))
		abort();
	for (i = 0; i < pi->nthreads + 1 /* +planner */; i++)
		uv_sem_wait(&sem);

	uv_sem_destroy(&sem);
}

static void pool_work_submit(pool_t *pool, pool_work_t *w)
{
	pool_impl_t *pi = pool->pi;
	queue *o = &pi->ordered;
	queue *u = &pi->unordered;

	w->loop = &pool->loop;
	if (w->type > WT_UNORD) {
		/* Make sure that elements in the ordered queue come in order. */
		PRE(ERGO(pi->o_prev != WT_BAR && w->type != WT_BAR,
			 pi->o_prev == w->type));
		pi->o_prev = w->type;
	}

	uv_mutex_lock(&pi->mutex);
	push(w->type == WT_UNORD ? u : o, &w->qlink);
	uv_cond_signal(&pi->planner_cond);
	uv_mutex_unlock(&pi->mutex);
}

void work_done(uv_async_t *handle)
{
	queue q = {};
	pool_impl_t *pi = container_of(handle, pool_impl_t, outq_async);

	uv_mutex_lock(&pi->outq_mutex);
	QUEUE__MOVE(&pi->outq, &q);
	uv_mutex_unlock(&pi->outq_mutex);

	while (!empty(&q))
		queue_done(q_to_w(pop(&q)));
}

void pool_queue_work(pool_t *pool,
		     pool_work_t *w,
		     uint32_t cookie,
		     void (*work_cb)(pool_work_t *w),
		     void (*after_work_cb)(pool_work_t *w))
{
	PRE(work_cb != NULL);

	w_register(pool, w);
	w->loop = &pool->loop;
	w->work_cb = work_cb;
	w->after_work_cb = after_work_cb;
	w->thread_id = cookie % pool->pi->nthreads;
	pool_work_submit(pool, w);
}

int pool_init(pool_t *pool)
{
	int rc;
	pool_impl_t *pi = pool->pi;

	pool->magic = POOL_LOOP_MAGIC;
	pi = pool->pi = calloc(1, sizeof(*pool->pi));
	if (pi == NULL)
		return UV_ENOMEM;

	rc = uv_mutex_init(&pi->outq_mutex);
	if (rc != 0) {
		free(pi);
		return rc;
	}

	rc = uv_async_init(&pool->loop, &pi->outq_async, work_done);
	if (rc != 0) {
		free(pi);
		uv_mutex_destroy(&pi->outq_mutex);
		return rc;
	}

	QUEUE__INIT(&pi->outq);
	pool_threads_init(pool);
	return 0;
}

void pool_fini(pool_t *pool)
{
	pool_impl_t *pi = pool->pi;

	pool_cleanup(pool);

	uv_mutex_lock(&pi->outq_mutex);
	POST(empty(&pi->outq) && !has_active_ws(pool));
	uv_mutex_unlock(&pi->outq_mutex);

	uv_mutex_destroy(&pi->outq_mutex);
	free(pi);
}

void pool_close(pool_t *pool)
{
	uv_close((uv_handle_t *)&pool->pi->outq_async, NULL);
}

uint32_t pool_thread_id(const pool_t *pool)
{
	return *(uint32_t *)uv_key_get(&pool->pi->thread_key);
}

pool_t *uv_loop_to_pool(const uv_loop_t *loop)
{
	pool_t *pl = container_of(loop, pool_t, loop);
	PRE(pl->magic == POOL_LOOP_MAGIC);
	return pl;
}
