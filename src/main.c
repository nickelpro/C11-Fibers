#include <stdlib.h>
#include <stdio.h>
#include <stdnoreturn.h>
#include <pthread.h>
#include "vgc.h"

noreturn void print_num(vgc_fiber fiber) {
	printf("%lu : Printing num: %d\n", pthread_self(),  **(int **) fiber.fd);
	vgc_fiber_finish(fiber);
}

noreturn void fibonacci(vgc_fiber fiber) {
	int *rounds = *(int **) fiber.fd;
	int a = 0;
	int b = 1;
	vgc_job job = {
		.proc = print_num,
		.data = &a,
		.priority = FIBER_HI
	};
	printf("%lu : The first %d numbers of fibonacci are:\n", pthread_self(), *rounds);
	for(int i = 0; i < *rounds; i++) {
		int next = a + b;
		a = b;
		b = next;
		fiber = vgc_schedule_and_wait(fiber, &job, 1);
	}
	exit(0);
}

int main(int argc, char **argv) {
	thread_data td = vgc_build_thread_data(128);
	pthread_t threads[5];
	for(pthread_t *thread = threads; thread < (pthread_t *) (&threads + 1); thread++)
		pthread_create(thread, NULL, vgc_thread_func, &td);
	int rounds = 10;
	for(int i = 0; i < 5; i++)
		vgc_schedule_job(td, fibonacci, &rounds, FIBER_HI, NULL);
	vgc_thread_func(&td);
}
