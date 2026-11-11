// SPDX-License-Identifier: GPL-2.0
#ifndef _UINTR_COMMON_H
#define _UINTR_COMMON_H
/*
 * uintr_common.h - standalone UINTR test helpers
 *
 * Derived from tools/testing/selftests/uintr/uintr_common.h
 */
#define _GNU_SOURCE
#include <syscall.h>
#include <errno.h>
#include <x86gprintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#ifndef __NR_uintr_register_handler
#define __NR_uintr_register_handler	471
#define __NR_uintr_unregister_handler	472
#define __NR_uintr_vector_fd		473
#define __NR_uintr_register_sender	474
#define __NR_uintr_unregister_sender	475
#define __NR_uintr_wait			476
#define __NR_uintr_register_self	477
#define __NR_uintr_alt_stack		478
#define __NR_uintr_ipi_fd		479
#endif

#define uintr_register_handler(handler, flags)	syscall(__NR_uintr_register_handler, handler, flags)
#define uintr_unregister_handler(flags)		syscall(__NR_uintr_unregister_handler, flags)
#define uintr_vector_fd(vector, flags)		syscall(__NR_uintr_vector_fd, vector, flags)
#define uintr_register_sender(fd, flags)	syscall(__NR_uintr_register_sender, fd, flags)
#define uintr_unregister_sender(ipi_idx, flags)	syscall(__NR_uintr_unregister_sender, ipi_idx, flags)
#define uintr_wait(usec, flags)			syscall(__NR_uintr_wait, usec, flags)
#define uintr_register_self(vector, flags)	syscall(__NR_uintr_register_self, vector, flags)
#define uintr_alt_stack(sp, size, flags)	syscall(__NR_uintr_alt_stack, sp, size, flags)
#define uintr_ipi_fd(flags)			syscall(__NR_uintr_ipi_fd, flags)

void __attribute__((interrupt)) uintr_empty_handler(struct __uintr_frame *ui_frame,
						    unsigned long long vector)
{
}

static inline int uintr_supported(void)
{
	if (!uintr_register_handler(uintr_empty_handler, 0)) {
		uintr_unregister_handler(0);
		return 1;
	}

	if (errno == EBUSY)
		return 1;

	return 0;
}

static inline void cpu_relax(void)
{
	asm volatile("rep; nop" ::: "memory");
}

static inline void cpu_delay(void)
{
	long long dl = 1000;
	volatile long long cnt = dl << 10;

	while (cnt--)
		dl++;
}

#endif /* _UINTR_COMMON_H */
