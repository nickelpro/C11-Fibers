#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdnoreturn.h>

#include "vgc.h"

vgc_fiber vgc_fiber_init(void *buf, size_t size, fiber_data *fd) {
	uintptr_t sp = ((uintptr_t) buf + (size - 1)) & ~(uintptr_t) 15;
	vgc_fiber fiber;
	fd->stack_orig = buf;
	fd->stack_alligned_base = (void *) sp;
	fd->state = FIBER_START;
	fiber.fd = fd;
	return fiber;
}

vgc_fiber vgc_fiber_assign(vgc_fiber fiber, vgc_proc proc) {
	vgc_fiber temp = vgc_make((void *) fiber.fd->stack_alligned_base, proc);
	temp.fd = fiber.fd;
	return temp;
}

noreturn void vgc_fiber_finish(vgc_fiber fiber) {
	fiber.fd->state = FIBER_DONE;
	vgc_jump(fiber);
	// The compiler doesn't believe us about noreturn unless we enforce it with
	// a call to exit
	exit(0);
}
