#include "vgc.h"
#include "logc/log.h"
#include <stdlib.h>
#include <xmmintrin.h>

//ToDo: Error check?
#ifdef _WIN32

void vgc_mutex_init(vgc_mutex *mutex) {
	InitializeCriticalSection(mutex);
}

void vgc_mutex_lock(vgc_mutex *mutex) {
	EnterCriticalSection(mutex);
}

void vgc_mutex_unlock(vgc_mutex *mutex) {
	LeaveCriticalSection(mutex);
}

void vgc_cond_init(vgc_cond *cond) {
	InitializeConditionVariable(cond);
}

void vgc_cond_wait(vgc_cond *cond, vgc_mutex *mutex) {
	SleepConditionVariableCS(cond, mutex, INFINITE);
}

void vgc_cond_signal(vgc_cond *cond) {
	WakeConditionVariable(cond);
}

void vgc_cond_broadcast(vgc_cond *cond) {
	WakeAllConditionVariable(cond);
}

#else

void vgc_mutex_init(vgc_mutex *mutex) {
	pthread_mutex_init(mutex, NULL);
}

void vgc_mutex_lock(vgc_mutex *mutex) {
	pthread_mutex_lock(mutex);
}

void vgc_mutex_unlock(vgc_mutex *mutex) {
	pthread_mutex_unlock(mutex);
}

void vgc_cond_init(vgc_cond *cond) {
	pthread_cond_init(cond, NULL);
}

void vgc_cond_wait(vgc_cond *cond, vgc_mutex *mutex) {
	pthread_cond_wait(cond, mutex);
}

void vgc_cond_signal(vgc_cond *cond) {
	pthread_cond_signal(cond);
}

void vgc_cond_broadcast(vgc_cond *cond) {
	pthread_cond_broadcast(cond);
}

#endif

vgc_counter vgc_counter_init(int count) {
	return (vgc_counter) {
		.lock = 0,
		.counter = count,
		.waiters_len = 0,
		.waiters = {0}
	};
}

//http://danglingpointers.com/post/spinlock-implementation/
static void vgc_counter_acquire(vgc_counter *count) {
	for(;;) {
		if(atomic_load(&count->lock) == 0) {
			int i = 0;
			if(atomic_compare_exchange_weak(&count->lock, &i, 1))
				break;
		}
		_mm_pause();
	}
}

static void vgc_counter_release(vgc_counter *count) {
	atomic_store(&count->lock, 0);
}

//Must acquire counter prior to using this function
static int vgc_counter_add_waiter(vgc_counter *count, vgc_fiber *fiber) {
	if(count->waiters_len == WAIT_FIBER_LEN)
		return -1;
	count->waiters[count->waiters_len] = fiber;
	count->waiters_len++;
	return 0;
}

static int vgc_enque_job(
	vgc_scheduler *sched,
	vgc_job job,
	vgc_counter *count
) {
	vgc_fiber *fiber;
	if(vgc_pop(&sched->free_q_pool, (void **) &fiber))
		return -1;
	*fiber = vgc_fiber_assign(*fiber, job.proc);
	fiber->data = job.data;
	fiber->fd->priority = job.priority;
	fiber->fd->parent_count = count;
	switch(job.priority) {
		case FIBER_HI:
			return vgc_enqueue(&sched->hi_q, fiber);
		case FIBER_MID:
			return vgc_enqueue(&sched->mid_q, fiber);
		case FIBER_LO:
			return vgc_enqueue(&sched->lo_q, fiber);
	}
	return -1;
}

vgc_job vgc_job_init(vgc_proc proc, void *data, fiber_priority priority) {
	return (vgc_job) {
		.proc = proc,
		.data = data,
		.priority = priority
	};
}

int vgc_schedule_job(
	vgc_fiber fiber,
	vgc_job job,
	vgc_counter *count
) {
	if(count)
		*count = vgc_counter_init(1);
	return vgc_enque_job(fiber.fd->sched, job, count);
}

// Counters from a failed schedule_jobs will never fire
int vgc_schedule_jobs(
	vgc_fiber fiber,
	vgc_job *jobs,
	int len,
	vgc_counter *count
) {
	if(count)
		*count = vgc_counter_init(len);
	for(int i = 0; i < len; i++)
		if(vgc_enque_job(fiber.fd->sched, jobs[i], count))
			return i;
	return 0;
}

// If you're certain you won't exceed WAIT_FIBER_LEN number of waiters on the
// counter, then this is fine to use. If you need to error check use
// wait_for_counter2
vgc_fiber vgc_wait_for_counter(vgc_fiber fiber, vgc_counter *count) {
	vgc_counter_acquire(count);
	if(!count->counter) {
		vgc_counter_release(count);
		return fiber;
	}
	fiber.fd->state = FIBER_WAIT;
	fiber.fd->dependency_count = count;
	// Counter is released by scheduler
	return vgc_jump(fiber);
}

vgc_fiber vgc_wait_for_counter2(
	vgc_fiber fiber,
	vgc_counter *count,
	int *err
) {
	*err = 0;
	vgc_counter_acquire(count);
	if(!count->counter) {
		vgc_counter_release(count);
		return fiber;
	}
	if(count->waiters_len == WAIT_FIBER_LEN) {
		vgc_counter_release(count);
		*err = -1;
		return fiber;
	}
	fiber.fd->state = FIBER_WAIT;
	fiber.fd->dependency_count = count;
	// Counter is released by scheduler
	return vgc_jump(fiber);
}

void vgc_scheduler_init(vgc_scheduler *sched, size_t size, vgc_job job) {
	*sched = (vgc_scheduler) {0};
	if ((size < 2) || ((size & (size - 1)) != 0))
		return;
	vgc_mutex_init(&sched->waiter_mux);
	vgc_cond_init(&sched->waiter_cond);
	size_t alloc_size = size * sizeof(vgc_cell);
	sched->hi_q = vgc_queue_init(malloc(alloc_size), size, sched);
	sched->mid_q = vgc_queue_init(malloc(alloc_size), size, sched);
	sched->lo_q = vgc_queue_init(malloc(alloc_size), size, sched);
	sched->free_q_pool = vgc_ringbuf_init(malloc(alloc_size), size);
	for(size_t i = 0; i < size; i++) {
		vgc_fiber *fiber = malloc(sizeof(*fiber));
		fiber->fd = malloc(sizeof(*fiber->fd));
		*fiber = vgc_fiber_init(malloc(1<<17), 1<<17, fiber->fd);
		fiber->fd->sched = sched;
		fiber->fd->id = (int) i;
		vgc_push(&sched->free_q_pool, fiber);
	}
	vgc_enque_job(sched, job, NULL);
}

// ToDo: Create various versions of this. One for pure lockless/spinning, one
// for the current hybrid version, and one conventionally locked version
noreturn THREADFUNC_TYPE vgc_scheduler_run(void *p) {
	vgc_scheduler *sched = (vgc_scheduler *) p;
	for(;;) {
		vgc_fiber *fiber;
		// ToDo: How long would we like to spin on these before locking?
		if(vgc_dequeue(&sched->hi_q, (void **) &fiber))
		if(vgc_dequeue(&sched->mid_q, (void **) &fiber))
		if(vgc_dequeue(&sched->lo_q, (void **) &fiber)) {
			// Lock to avoid spinning forever on light workloads
			// https://stackoverflow.com/a/32696363
			vgc_mutex_lock(&sched->waiter_mux);
			atomic_store_explicit(&sched->is_waiter, 1, memory_order_relaxed);
			for(;;) {
				if(!vgc_dequeue(&sched->hi_q, (void **) &fiber)) break;
				if(!vgc_dequeue(&sched->mid_q, (void **) &fiber)) break;
				if(!vgc_dequeue(&sched->lo_q, (void **) &fiber)) break;
				vgc_cond_wait(&sched->waiter_cond, &sched->waiter_mux);
				atomic_store_explicit(&sched->is_waiter, 1, memory_order_relaxed);
			}
			atomic_store_explicit(&sched->is_waiter, 0, memory_order_relaxed);
			vgc_mutex_unlock(&sched->waiter_mux);
		}
		fiber_data *fd = fiber->fd;
		fd->state = FIBER_RUN;
		*fiber = vgc_jump(*fiber);

		if(fd->state == FIBER_WAIT) {
			// Counter was acquired by fiber
			vgc_counter_add_waiter(fd->dependency_count, fiber);
			vgc_counter_release(fd->dependency_count);
		} else if((fd->state == FIBER_DONE) && fd->parent_count) {
			vgc_counter_acquire(fd->parent_count);
			fd->parent_count->counter--;
			if(!fd->parent_count->counter) {
				for(size_t i = 0; i < fd->parent_count->waiters_len; i++) {
					vgc_fiber *waiter = fd->parent_count->waiters[i];
					switch(waiter->fd->priority) {
						case FIBER_HI:
							vgc_enqueue(&sched->hi_q, waiter);
							break;
						case FIBER_MID:
							vgc_enqueue(&sched->mid_q, waiter);
							break;
						case FIBER_LO:
							vgc_enqueue(&sched->lo_q, waiter);
							break;
					}
				}
			}
			vgc_counter_release(fd->parent_count);
			if(vgc_push(&sched->free_q_pool, fiber)) {
				log_error("Failed to push a free fiber");
				exit(1);
			}
		}
	}
}
