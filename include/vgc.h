#pragma once

#include <stdalign.h>
#include <stdatomic.h>
#include <stdnoreturn.h>
#include <pthread.h>

typedef struct vgc_cell {
	atomic_size_t seq;
	void *data;
} vgc_cell;

typedef struct vgc_ringbuf {
	size_t bufmask;
	vgc_cell *buf;
	#define cacheline_size 64
	alignas(cacheline_size) atomic_size_t head;
	alignas(cacheline_size) atomic_size_t tail;
	#undef cacheline_size
} vgc_ringbuf;

vgc_ringbuf vgc_ringbuf_init(
	void *buf,
	size_t size
);
int vgc_push(vgc_ringbuf *rb, void *data);
int vgc_pop(vgc_ringbuf *rb, void **data);

typedef struct vgc_queue {
	vgc_ringbuf rb;
	pthread_mutex_t *waiter_mux;
	pthread_cond_t *waiter_cond;
	atomic_bool *is_waiter;
} vgc_queue;

vgc_queue vgc_queue_init(
	void *buf,
	size_t size,
	pthread_mutex_t *waiter_mux,
	pthread_cond_t *waiter_cond,
	atomic_bool *is_waiter
);
int vgc_enqueue(vgc_queue *q, void *data);
int vgc_dequeue(vgc_queue *q, void **data);

typedef struct thread_data {
	vgc_queue *hi_q;
	vgc_queue *med_q;
	vgc_queue *lo_q;
	vgc_ringbuf *free_pool;
	pthread_mutex_t *waiter_mux;
	pthread_cond_t *waiter_cond;
	atomic_bool *is_waiter;
} thread_data;

struct fiber_data;

typedef enum {
	FIBER_LO,
	FIBER_MED,
	FIBER_HI
} fiber_priority;

typedef struct vgc_fiber {
	void *ctx;
	struct fiber_data *fd;
} vgc_fiber;

typedef struct spinl_counter {
	atomic_int lock;
	int counter;
	vgc_fiber fiber;
} spinl_counter;

typedef void (*vgc_proc)(vgc_fiber fiber);

typedef struct {
	vgc_proc proc;
	void *data;
	fiber_priority priority;
} vgc_job;

typedef struct fiber_data {
	void *data;
	enum {
		FIBER_START,
		FIBER_RESUME,
		FIBER_RUN,
		FIBER_WAIT,
		FIBER_DONE
	} state;
	fiber_priority priority;
	thread_data td;
	void *stack_orig;
	void *stack_alligned_base;
	struct spinl_counter parent_counter;
	struct spinl_counter *depend_counter;
	vgc_job *dependencies;
	size_t dependencies_len;
} fiber_data;

spinl_counter vgc_counter_init(int count, vgc_fiber fiber);

vgc_fiber vgc_fiber_assign(vgc_fiber fiber, vgc_proc proc);
vgc_fiber vgc_fiber_init(void *buf, size_t size, fiber_data *fd);
noreturn void vgc_fiber_finish(vgc_fiber fiber);

extern vgc_fiber vgc_make(void *sp, vgc_proc proc);
extern vgc_fiber vgc_jump(vgc_fiber fiber);

int vgc_counter_acq_dec_que(spinl_counter *spc);

int vgc_schedule_job(
	thread_data td,
	vgc_proc proc,
	void *data,
	fiber_priority priority,
	spinl_counter *spc
);
int vgc_schedule_job2(
	thread_data td,
	vgc_job job,
	spinl_counter *spc
);
vgc_fiber vgc_schedule_and_wait(
	vgc_fiber fiber,
	vgc_job *jobs,
	size_t jobs_len
);

thread_data vgc_build_thread_data(size_t size);
void *vgc_thread_func(void *p);
