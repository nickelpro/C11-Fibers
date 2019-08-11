#include <stdlib.h>
#include <stdio.h>
#include "vgc.h"

#ifdef _WIN32

static void create_thread(THREADFUNC_TYPE (*proc)(void *), void *data) {
	CreateThread(NULL, 0, proc, data, 0, NULL);
}

static unsigned long thread_id() {
	return (unsigned long) GetCurrentThreadId();
}

#else

static void create_thread(THREADFUNC_TYPE (*proc)(void *), void *data) {
	pthread_t t;
	pthread_create(&t, NULL, proc, data);
}

static unsigned long thread_id() {
	return (unsigned long) pthread_self();
}

#endif

noreturn static void print_num(vgc_fiber fiber) {
	printf("%lu : Printing num: %d\n", thread_id(),  *(int *) fiber.data);
	vgc_fiber_finish(fiber);
}

noreturn static void fibonacci(vgc_fiber fiber) {
	int *rounds = (int *) fiber.data;
	int a = 0;
	int b = 1;
	vgc_job job = vgc_job_init(print_num, &a, FIBER_MID);
	printf("%lu : The first %d numbers of fibonacci are:\n", thread_id(), *rounds);
	for(int i = 0; i < *rounds; i++) {
		int next = a + b;
		a = b;
		b = next;
		vgc_counter count;
		vgc_schedule_job(fiber, job, &count);
		fiber = vgc_wait_for_counter(fiber, &count);
	}
	vgc_fiber_finish(fiber);
}

noreturn static void sched_fib_and_quit(vgc_fiber fiber) {
	vgc_job jobs[5];
	for(int i = 0; i < 5; i++)
		jobs[i] = vgc_job_init(fibonacci, fiber.data, FIBER_HI);
	vgc_counter count;
	vgc_schedule_jobs(fiber, jobs, 5, &count);
	fiber = vgc_wait_for_counter(fiber, &count);
	printf("Finished\n");
	exit(0);
}

int main(int argc, char **argv) {
	int rounds = 10;
	vgc_job job = vgc_job_init(sched_fib_and_quit, &rounds, FIBER_HI);
	vgc_scheduler sched;
	vgc_scheduler_init(&sched, 128, job);
	for(int i = 0; i < 5; i++)
		create_thread(vgc_scheduler_run, &sched);
	vgc_scheduler_run(&sched);
}
