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
	pthread_mutex_init(mux, NULL);
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

spinl_counter vgc_counter_init(int count, vgc_fiber fiber) {
	return (spinl_counter) {
		.lock = 0,
		.counter = count,
		.fiber = fiber
	};
}

//http://danglingpointers.com/post/spinlock-implementation/
int vgc_counter_acq_dec_que(spinl_counter *spc) {
	for(;;) {
		if(atomic_load(&spc->lock) == 0) {
			int i = 0;
			if(atomic_compare_exchange_weak(&spc->lock, &i, 1))
				break;
		}
		_mm_pause();
	}
	spc->counter--;
	int ret = spc->counter;
	if(!spc->counter) {
		fiber_data *fd = spc->fiber.fd;
		switch(fd->priority) {
			case FIBER_HI:
				vgc_enqueue(&fd->sched->hi_q, &spc->fiber);
				break;
		   case FIBER_MID:
				vgc_enqueue(&fd->sched->mid_q, &spc->fiber);
				break;
			case FIBER_LO:
				vgc_enqueue(&fd->sched->lo_q, &spc->fiber);
				break;
			default:
				break;
		}
	}
	atomic_store(&spc->lock, 0);
	return ret;
}

int vgc_schedule_job(
	scheduler *sched,
	vgc_proc proc,
	void *data,
	fiber_priority priority,
	spinl_counter *spc
) {
	vgc_fiber *fiber;
	if(vgc_pop(&sched->free_pool, (void **) &fiber))
		return -1;
	*fiber = vgc_fiber_assign(*fiber, proc);
	fiber->data = data;
	fiber->fd->priority = priority;
	fiber->fd->depend_counter = spc;
	switch(priority) {
		case FIBER_HI:
			return vgc_enqueue(&sched->hi_q, fiber);
		case FIBER_MID:
			return vgc_enqueue(&sched->mid_q, fiber);
		case FIBER_LO:
			return vgc_enqueue(&sched->lo_q, fiber);
	}
	return -1;
}

int vgc_schedule_job2(
	scheduler *sched,
	vgc_job job,
	spinl_counter *spc
) {
	return vgc_schedule_job(sched, job.proc, job.data, job.priority, spc);
}

vgc_fiber vgc_schedule_and_wait(
	vgc_fiber fiber,
	vgc_job *jobs,
	size_t jobs_len
) {
	fiber.fd->dependencies = jobs;
	fiber.fd->dependencies_len = jobs_len;
	fiber.fd->state = FIBER_WAIT;
	fiber = vgc_jump(fiber);
	return fiber;
}

// ToDo: Create various versions of this. One for pure lockless/spinning, one
// for the current hybrid version, and one conventionally locked version
THREADFUNC_TYPE vgc_thread_func(void *p) {
	scheduler *sched = (scheduler *) p;
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
		fd->sched = sched;
		fd->state = FIBER_RUN;
		*fiber = vgc_jump(*fiber);

		if(fd->state == FIBER_WAIT) {
			fd->parent_counter = vgc_counter_init(
				(int) fd->dependencies_len, *fiber
			);
			vgc_job *depends = fd->dependencies;
			for(size_t i = 0; i < fd->dependencies_len; i++, depends++)
				vgc_schedule_job2(sched, *depends, &fd->parent_counter);
		} else if(fd->state == FIBER_DONE) {
			if(fd->depend_counter)
				vgc_counter_acq_dec_que(fd->depend_counter);
			if(vgc_push(&sched->free_pool, fiber)) {
				log_error("Failed to push a free fiber");
				exit(1);
			}
		}
	}
	return THREADFUNC_RET;
}

void vgc_scheduler_init(scheduler *sched, size_t size) {
	*sched = (scheduler) {0};
	if ((size < 2) || ((size & (size - 1)) != 0))
		return;
	vgc_mutex_init(&sched->waiter_mux);
	vgc_cond_init(&sched->waiter_cond);
	size_t alloc_size = size * sizeof(vgc_cell);
	sched->hi_q = vgc_queue_init(malloc(alloc_size), size, sched);
	sched->mid_q = vgc_queue_init(malloc(alloc_size), size, sched);
	sched->lo_q = vgc_queue_init(malloc(alloc_size), size, sched);
	sched->free_pool = vgc_ringbuf_init(malloc(alloc_size), size);
	for(size_t i = 0; i < size; i++) {
		vgc_fiber *fiber = malloc(sizeof(*fiber));
		fiber->fd = malloc(sizeof(*fiber->fd));
		*fiber = vgc_fiber_init(malloc(1<<17), 1<<17, fiber->fd);
		vgc_push(&sched->free_pool, fiber);
	}
}
