#include "vgc.h"
#include "logc/log.h"
#include <stdlib.h>
#include <pthread.h>
#include <xmmintrin.h>


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
				vgc_enqueue(fd->td.hi_q, &spc->fiber);
				break;
		   case FIBER_MED:
				vgc_enqueue(fd->td.med_q, &spc->fiber);
				break;
			case FIBER_LO:
				vgc_enqueue(fd->td.lo_q, &spc->fiber);
				break;
			default:
				break;
		}
	}
	atomic_store(&spc->lock, 0);
	return ret;
}

int vgc_schedule_job(
	thread_data td,
	vgc_proc proc,
	void *data,
	fiber_priority priority,
	spinl_counter *spc
) {
	vgc_fiber *fiber;
	if(vgc_pop(td.free_pool, (void **) &fiber))
		return -1;
	*fiber = vgc_fiber_assign(*fiber, proc);
	fiber->fd->priority = priority;
	fiber->fd->data = data;
	fiber->fd->depend_counter = spc;
	switch(priority) {
		case FIBER_HI:
			return vgc_enqueue(td.hi_q, fiber);
		case FIBER_MED:
			return vgc_enqueue(td.med_q, fiber);
		case FIBER_LO:
			return vgc_enqueue(td.lo_q, fiber);
	}
	return -1;
}

int vgc_schedule_job2(
	thread_data td,
	vgc_job job,
	spinl_counter *spc
) {
	return vgc_schedule_job(td, job.proc, job.data, job.priority, spc);
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
void *vgc_thread_func(void *p) {
	thread_data *td = (thread_data *) p;
	for(;;) {
		vgc_fiber *fiber;
		// ToDo: How long would we like to spin on these before locking?
		if(vgc_dequeue(td->hi_q, (void **) &fiber))
		if(vgc_dequeue(td->med_q, (void **) &fiber))
		if(vgc_dequeue(td->lo_q, (void **) &fiber)) {
			// Lock to avoid spinning forever on light workloads
			// https://stackoverflow.com/a/32696363
			pthread_mutex_lock(td->waiter_mux);
			atomic_store_explicit(td->is_waiter, 1, memory_order_relaxed);
			for(;;) {
				if(!vgc_dequeue(td->hi_q, (void **) &fiber)) break;
				if(!vgc_dequeue(td->med_q, (void **) &fiber)) break;
				if(!vgc_dequeue(td->lo_q, (void **) &fiber)) break;
				pthread_cond_wait(td->waiter_cond, td->waiter_mux);
				atomic_store_explicit(td->is_waiter, 1, memory_order_relaxed);
			}
			atomic_store_explicit(td->is_waiter, 0, memory_order_relaxed);
			pthread_mutex_unlock(td->waiter_mux);
		}
		fiber_data *fd = fiber->fd;
		fd->td = *td;
		fd->state = FIBER_RUN;
		*fiber = vgc_jump(*fiber);

		if(fd->state == FIBER_WAIT) {
			fd->parent_counter = vgc_counter_init(
				(int) fd->dependencies_len, *fiber
			);
			vgc_job *depends = fd->dependencies;
			for(size_t i = 0; i < fd->dependencies_len; i++, depends++)
				vgc_schedule_job2(*td, *depends, &fd->parent_counter);
		} else if(fd->state == FIBER_DONE) {
			if(fd->depend_counter)
				vgc_counter_acq_dec_que(fd->depend_counter);
			if(vgc_push(td->free_pool, fiber)) {
				log_error("Failed to push a free fiber");
				exit(1);
			}
		}
	}
	return NULL;
}

thread_data vgc_build_thread_data(size_t size) {
	thread_data td;
	if ((size < 2) || ((size & (size - 1)) != 0))
		return (thread_data) {0};
	pthread_mutex_t *mux = malloc(sizeof(*mux));
	pthread_cond_t *cond = malloc(sizeof(*cond));
	atomic_bool *wait = malloc(sizeof(*wait));
	pthread_mutex_init(mux, NULL);
	pthread_cond_init(cond, NULL);
	td.waiter_cond = cond;
	td.waiter_mux = mux;
	td.is_waiter = wait;
	td.hi_q = malloc(sizeof(*td.hi_q));
	td.med_q = malloc(sizeof(*td.med_q));
	td.lo_q = malloc(sizeof(*td.lo_q));
	td.free_pool = malloc(sizeof(*td.free_pool));
	size_t alloc_size = size * sizeof(vgc_cell);
	*td.hi_q = vgc_queue_init(malloc(alloc_size), size, mux, cond, wait);
	*td.med_q = vgc_queue_init(malloc(alloc_size), size, mux, cond, wait);
	*td.lo_q = vgc_queue_init(malloc(alloc_size), size, mux, cond, wait);
	*td.free_pool = vgc_ringbuf_init(malloc(alloc_size), size);
	for(size_t i = 0; i < size; i++) {
		vgc_fiber *fiber = malloc(sizeof(*fiber));
		fiber->fd = malloc(sizeof(*fiber->fd));
		*fiber = vgc_fiber_init(malloc(1<<17), 1<<17, fiber->fd);
		vgc_push(td.free_pool, fiber);
	}
	return td;
}
