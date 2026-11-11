// SPDX-License-Identifier: GPL-2.0
/*
 * ctrl-c-uintr-test.c
 *
 * Verify that pressing Ctrl-C in a terminal causes the kernel to:
 *   1) deliver SIGINT to the foreground process, AND
 *   2) send a UINTR (user interrupt) to the receiving thread.
 *
 * Receiver side only (no sender). Modeled after the kernel selftest
 * tools/testing/selftests/uintr/ipc_base.c.
 *
 * Prerequisites (on a UINTR-capable kernel & CPU):
 *   echo 1 > /proc/sys/kernel/uintr_notify_isig
 *
 * Run in the foreground of a terminal and press Ctrl-C.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#include <x86gprintrin.h>	/* __attribute__((interrupt)), struct __uintr_frame, _stui */

/* UINTR syscall numbers (x86_64), from arch/x86/entry/syscalls/syscall_64.tbl */
#ifndef __NR_uintr_register_handler
#define __NR_uintr_register_handler	471
#define __NR_uintr_unregister_handler	472
#define __NR_uintr_vector_fd		473
#endif

#define uintr_register_handler(h, f)	syscall(__NR_uintr_register_handler, h, f)
#define uintr_unregister_handler(f)	syscall(__NR_uintr_unregister_handler, f)
#define uintr_vector_fd(v, f)		syscall(__NR_uintr_vector_fd, v, f)

#define UVEC		0

static volatile sig_atomic_t sigint_fired;
static volatile unsigned int uintr_received;

/* ---- SIGINT handler ---- */
static void sigint_handler(int sig)
{
	sigint_fired = 1;
}

/* ---- UINTR handler (GCC emits prologue/epilogue incl. uiret) ---- */
static void __attribute__((interrupt))
uintr_handler(struct __uintr_frame *ui_frame, unsigned long long vector)
{
	uintr_received = 1;
}

static inline void cpu_delay(void)
{
	long long dl = 1000;
	volatile long long cnt = dl << 10;
	while (cnt--)
		dl++;
}

int main(void)
{
	struct sigaction sa;
	int uvec_fd;
	int wait_for = 1000000;

	printf("[info] pid=%d  Run in foreground, press Ctrl-C.\n", getpid());
	printf("[info] Make sure /proc/sys/kernel/uintr_notify_isig == 1\n");

	/* 1) Register UINTR handler */
	if (uintr_register_handler(uintr_handler, 0)) {
		fprintf(stderr, "[FAIL] uintr_register_handler: %s\n", strerror(errno));
		return 1;
	}
	printf("[info] uintr_register_handler OK\n");

	/* 2) Register vector 0 (creates uvec_fd / UPID) */
	uvec_fd = uintr_vector_fd(UVEC, 0);
	if (uvec_fd < 0) {
		fprintf(stderr, "[FAIL] uintr_vector_fd(%d): %s\n", UVEC, strerror(errno));
		uintr_unregister_handler(0);
		return 1;
	}
	printf("[info] uintr_vector_fd(%d) OK -> fd=%d\n", UVEC, uvec_fd);

	/* 3) Install SIGINT handler */
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = sigint_handler;
	sa.sa_flags = 0;
	if (sigaction(SIGINT, &sa, NULL) < 0) {
		perror("sigaction");
		close(uvec_fd);
		uintr_unregister_handler(0);
		return 1;
	}
	printf("[info] SIGINT handler installed\n");

	/* 4) Enable user interrupts */
	_stui();
	printf("[info] UIF enabled (stui). Waiting for Ctrl-C...\n");

	/*
	 * Spin in user space with UIF=1 so the kernel-posted UINTR is
	 * delivered immediately. SIGINT also interrupts this loop.
	 */
	while (wait_for-- && !sigint_fired)
		cpu_delay();

	/* Re-sync UIF in case the signal path cleared it */
	_stui();

	printf("[result] SIGINT=%d  UINTR_received=%d\n",
	       sigint_fired, uintr_received);

	if (sigint_fired && uintr_received)
		printf("[PASS] both SIGINT and UINTR delivered\n");
	else if (sigint_fired && !uintr_received)
		printf("[FAIL] SIGINT delivered but NO UINTR "
		       "(is /proc/sys/kernel/uintr_notify_isig == 1?)\n");
	else
		printf("[FAIL] SIGINT not delivered (did you press Ctrl-C?)\n");

	close(uvec_fd);
	uintr_unregister_handler(0);
	return (sigint_fired && uintr_received) ? 0 : 1;
}
