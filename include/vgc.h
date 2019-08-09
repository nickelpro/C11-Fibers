#pragma once

#include <stdalign.h>
#include <stdatomic.h>

#ifdef _WIN32

#define THREADFUNC_TYPE DWORD
#define THREADFUNC_RET 0

#include <Windows.h>

typedef CRITICAL_SECTION vgc_mutex;
typedef CONDITION_VARIABLE vgc_cond;

#else

#define THREADFUNC_TYPE void *
#define THREADFUNC_RET NULL

#include <pthread.h>

typedef pthread_mutex_t vgc_mutex;
typedef pthread_cond_t vgc_cond;

#endif

#include <stdnoreturn.h>

void vgc_mutex_init(vgc_mutex *mutex);
void vgc_mutex_lock(vgc_mutex *mutex);
void vgc_mutex_unlock(vgc_mutex *mutex);
void vgc_cond_init(vgc_cond *cond);
void vgc_cond_wait(vgc_cond *cond, vgc_mutex *mutex);
void vgc_cond_signal(vgc_cond *cond);
void vgc_cond_broadcast(vgc_cond *cond);

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

struct scheduler;

typedef struct vgc_queue {
	vgc_ringbuf rb;
	struct scheduler *sched;
} vgc_queue;

vgc_queue vgc_queue_init(
	void *buf,
	size_t size,
	struct scheduler *sched
);
int vgc_enqueue(vgc_queue *q, void *data);
int vgc_dequeue(vgc_queue *q, void **data);

typedef struct scheduler {
	vgc_queue hi_q;
	vgc_queue mid_q;
	vgc_queue lo_q;
	vgc_ringbuf free_pool;
	vgc_mutex waiter_mux;
	vgc_cond waiter_cond;
	atomic_bool is_waiter;
} scheduler;

struct fiber_data;

typedef enum {
	FIBER_LO,
	FIBER_MID,
	FIBER_HI
} fiber_priority;

typedef struct vgc_fiber {
	void *data; //user data
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
	enum {
		FIBER_START,
		FIBER_RESUME,
		FIBER_RUN,
		FIBER_WAIT,
		FIBER_DONE
	} state;
	fiber_priority priority;
	scheduler *sched;
	void *stack_orig;
	void *stack_alligned_base;
	void *stack_limit;
	struct spinl_counter parent_counter;
	struct spinl_counter *depend_counter;
	vgc_job *dependencies;
	size_t dependencies_len;
} fiber_data;

spinl_counter vgc_counter_init(int count, vgc_fiber fiber);

vgc_fiber vgc_fiber_assign(vgc_fiber fiber, vgc_proc proc);
vgc_fiber vgc_fiber_init(void *buf, size_t size, fiber_data *fd);
noreturn void vgc_fiber_finish(vgc_fiber fiber);

extern void *vgc_make(void *base, void *limit, vgc_proc proc);
extern vgc_fiber vgc_jump(vgc_fiber fiber);

int vgc_counter_acq_dec_que(spinl_counter *spc);

int vgc_schedule_job(
	scheduler *scheduler,
	vgc_proc proc,
	void *data,
	fiber_priority priority,
	spinl_counter *spc
);
int vgc_schedule_job2(
	scheduler *scheduler,
	vgc_job job,
	spinl_counter *spc
);
vgc_fiber vgc_schedule_and_wait(
	vgc_fiber fiber,
	vgc_job *jobs,
	size_t jobs_len
);

void vgc_scheduler_init(scheduler *scheduler, size_t size);
THREADFUNC_TYPE vgc_thread_func(void *p);
