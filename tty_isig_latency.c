// SPDX-License-Identifier: GPL-2.0-only
/*
 * tty_isig_latency.c - Measure Ctrl-C delivery latency through the tty
 *
 * When the tty line discipline handles VINTR (Ctrl-C), the kernel notifies the
 * foreground process group with exactly one mechanism, selected by the sysctl
 * kernel.uintr_isig_mode: 0 = SIGINT, 1 = user interrupt. This test measures
 * the latency of that single delivery path.
 *
 * Asynchronous trigger: the pty master is written to, so the line
 * discipline runs in the tty workqueue - the path a real Ctrl-C takes.
 * The instant the kworker posts the notification is not observable from
 * user space, so the latency measured from write() is an upper bound that
 * includes the workqueue wakeup.
 *
 * Raw per-round samples are written to latency_mode<N>.csv for plotting.
 *
 * Timestamps use rdtsc: the UINTR handler is compiled with -mgeneral-regs-only
 * and must not touch the FPU, and rdtsc keeps that promise.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <x86gprintrin.h>

#include "uintr_common.h"

#define ROUNDS		10000
#define SPIN_LIMIT	200000000UL

struct shm {
	volatile int		round;
	int			nrecv[ROUNDS];
	unsigned long long	t0[ROUNDS];
	unsigned long long	t_sig[ROUNDS];
	unsigned long long	t_uintr[ROUNDS];
};

static struct shm *sh;
static unsigned long long cyc_per_us;
static int uvec_fd = -1;
static int nerrs;

/* CPU the receiver is pinned to, -1 when pinning is disabled. */
static int child_cpu = -1;
static bool child_isolated = false;

/*
 * Delivery mechanism selected by kernel.uintr_isig_mode: 0 = SIGINT,
 * 1 = user interrupt. The two are mutually exclusive.
 */
static int isig_mode;

/* ------------------------------------------------------------------ */
/* Handlers: timestamp only, no FPU, no locks.				*/
/* ------------------------------------------------------------------ */

static void sigint_handler(int sig)
{
	int i = sh->round;

	sh->t_sig[i] = __rdtsc();
	__atomic_add_fetch(&sh->nrecv[i], 1, __ATOMIC_RELEASE);
}

static void __attribute__((interrupt)) uintr_handler(struct __uintr_frame *ui_frame,
						     unsigned long long vector)
{
	int i = sh->round;

	sh->t_uintr[i] = __rdtsc();
	__atomic_add_fetch(&sh->nrecv[i], 1, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------ */
/* Setup								*/
/* ------------------------------------------------------------------ */

static int setup_uintr(void)
{
	if (uintr_register_handler(uintr_handler, 0))
		return -1;

	uvec_fd = uintr_vector_fd(0, 0);
	if (uvec_fd < 0) {
		uintr_unregister_handler(0);
		return -1;
	}

	/* Registered before _stui() so the first round is not lost. */
	_stui();
	return 0;
}

static int setup_sigint(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = sigint_handler;
	sa.sa_flags = 0;

	return sigaction(SIGINT, &sa, NULL);
}

/* ------------------------------------------------------------------ */
/* Time helpers (integer only, this file builds -mgeneral-regs-only)	*/
/* ------------------------------------------------------------------ */

static unsigned long long calibrate_tsc(void)
{
	struct timespec ts0, ts1;
	unsigned long long c0, c1, ns;

	clock_gettime(CLOCK_MONOTONIC, &ts0);
	c0 = __rdtsc();
	usleep(20000);
	c1 = __rdtsc();
	clock_gettime(CLOCK_MONOTONIC, &ts1);

	ns = (ts1.tv_sec - ts0.tv_sec) * 1000000000ULL +
	     (ts1.tv_nsec - ts0.tv_nsec);
	if (!ns)
		return 1;

	/* cycles per microsecond, rounded */
	return ((c1 - c0) * 1000ULL + ns / 2) / ns;
}

static unsigned long long cycles_to_ns(unsigned long long cycles)
{
	if (!cyc_per_us)
		return cycles;
	return cycles * 1000ULL / cyc_per_us;
}

/*
 * Pinning keeps the spinning receiver and the triggering parent on separate
 * CPUs. Without it the timer-to-handler window also picks up CFS time slice
 * handoffs, which show up as a spurious 32-128us hump in the distribution.
 */
static int pin_cpu(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	return sched_setaffinity(0, sizeof(set), &set);
}

/*
 * Pick a CPU to run the spinning receiver on.  Prefer an isolated CPU
 * (isolcpus=) because nothing else is scheduled there; otherwise fall back
 * to the last online CPU so the parent and the tty workqueue have the rest
 * of the machine to themselves.
 */
static int pick_receiver_cpu(void)
{
	char buf[256];
	int fd, n, cpu = -1;
	char *p, *end;

	fd = open("/sys/devices/system/cpu/isolated", O_RDONLY);
	if (fd >= 0) {
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n > 0) {
			buf[n] = '\0';
			cpu = strtol(buf, &end, 10);
			/* accept "5", "5-7", "1,3" -- just take the first number */
			if (end != buf && cpu >= 0)
				return cpu;
		}
	}

	n = sysconf(_SC_NPROCESSORS_ONLN);
	(void)p;
	return (n > 1) ? n - 1 : 0;
}

/*
 * Receiver scheduling setup.
 *
 * If the receiver is pinned to an isolated CPU (isolcpus=), use SCHED_FIFO
 * so CFS cannot preempt it mid-spin.  On a normal CPU, SCHED_FIFO would
 * starve the tty workqueue worker when it happens to land on the same core,
 * which deadlocks the test -- so fall back to a high nice value instead.
 */
static void set_realtime(bool on_isolated_cpu)
{
	if (on_isolated_cpu) {
		struct sched_param sp = { .sched_priority = 50 };

		if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
			printf("[INFO]\tSCHED_FIFO failed (%s)\n", strerror(errno));
		else
			printf("[INFO]\treceiver: SCHED_FIFO prio 50\n");
	} else {
		if (nice(-20) < 0 && errno != EPERM)
			printf("[INFO]\tnice(-20) failed: %s\n", strerror(errno));
		else
			printf("[INFO]\treceiver: SCHED_OTHER nice -20 (no isolcpus)\n");
	}
}

static int read_isig_mode(void)
{
	char buf[16];
	int fd, n, mode = -1;

	fd = open("/proc/sys/kernel/uintr_isig_mode", O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	if (n > 0) {
		buf[n] = '\0';
		mode = atoi(buf);
	}
	close(fd);
	return mode;
}

static int wait_sample(int i, unsigned long limit)
{
	while (!__atomic_load_n(&sh->t_sig[i], __ATOMIC_ACQUIRE) &&
	       !__atomic_load_n(&sh->t_uintr[i], __ATOMIC_ACQUIRE)) {
		cpu_relax();
		if (--limit == 0)
			return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Asynchronous trigger: parent writes to the pty master		*/
/* ------------------------------------------------------------------ */

static int run_phase_b_receiver(void)
{
	int last = -1, nlost = 0;

	/*
	 * Follow the trigger's round counter instead of counting locally:
	 * if the trigger skips a round (notification lost), we jump forward
	 * too rather than spinning forever and aborting the whole test.
	 */
	while (last < ROUNDS - 1) {
		int i = __atomic_load_n(&sh->round, __ATOMIC_ACQUIRE);

		if (i == last) {
			cpu_relax();
			continue;
		}
		if (i > last + 1)
			nlost += i - last - 1;
		last = i;

		/* Spinning in user mode with UIF set is what we want to measure. */
		_stui();
		if (wait_sample(i, SPIN_LIMIT) < 0) {
			printf("[WARN]\tround %d: notification missing, skipping\n", i);
			nlost++;
			continue;
		}
	}
	if (nlost)
		printf("[INFO]\t%d rounds skipped/lost\n", nlost);
	return 0;
}

static int run_phase_b_trigger(int master)
{
	int i, j;

	for (i = 0; i < ROUNDS; i++) {
		sh->nrecv[i] = 0;
		sh->t_sig[i] = 0;
		sh->t_uintr[i] = 0;
		__atomic_store_n(&sh->round, i, __ATOMIC_RELEASE);

		/* Give the receiver time to start spinning on this round. */
		usleep(300);
		sh->t0[i] = __rdtsc();
		if (write(master, "\003", 1) != 1) {
			printf("[FAIL]\twrite master: %s\n", strerror(errno));
			return -1;
		}

		for (j = 0; j < 400; j++) {
			if (__atomic_load_n(&sh->nrecv[i], __ATOMIC_ACQUIRE))
				break;
			usleep(50);
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Reporting								*/
/* ------------------------------------------------------------------ */

/* Latency histogram: power-of-two microsecond buckets.		*/
/* Bucket 0: <1us; bucket i (1..9): [2^(i-1), 2^i) us; bucket 10: >=512us. */
#define HIST_BUCKETS	11

static void print_hist(const int *hist, int n)
{
	int i, peak = 0;

	for (i = 0; i < HIST_BUCKETS; i++)
		if (hist[i] > peak)
			peak = hist[i];

	printf("\tlatency distribution (total %d):\n", n);
	for (i = 0; i < HIST_BUCKETS; i++) {
		int len = peak ? (int)((long long)hist[i] * 50 / peak) : 0;
		char bar[51];

		if (hist[i] && !len)
			len = 1;
		memset(bar, '#', len);
		bar[len] = '\0';

		if (i == 0)
			printf("\t  <   1 us: %-50s %d\n", bar, hist[i]);
		else if (i == HIST_BUCKETS - 1)
			printf("\t>= %4d us: %-50s %d\n",
			       1 << (i - 1), bar, hist[i]);
		else
			printf("\t[%4d,%4d)us: %-50s %d\n",
			       1 << (i - 1), 1 << i, bar, hist[i]);
	}
}

static void report_phase(const char *phase, const char *note)
{
	const unsigned long long *t = isig_mode ? sh->t_uintr : sh->t_sig;
	const char *name = isig_mode ? "UINTR " : "SIGINT";
	unsigned long long min = ~0ULL, max = 0, sum = 0;
	int hist[HIST_BUCKETS] = {0};
	int n = 0, i;

	for (i = 0; i < ROUNDS; i++) {
		unsigned long long l, us;
		int b;

		if (!t[i])
			continue;
		l = cycles_to_ns(t[i] - sh->t0[i]);
		if (l < min)
			min = l;
		if (l > max)
			max = l;
		sum += l;
		n++;

		if (l < 1000) {
			b = 0;
		} else {
			us = l / 1000;
			b = 1 + (63 - __builtin_clzll(us));
			if (b >= HIST_BUCKETS)
				b = HIST_BUCKETS - 1;
		}
		hist[b]++;
	}

	if (!n) {
		printf("[FAIL]\t%s: no samples\n", phase);
		nerrs++;
		return;
	}

	printf("[RESULT]\t%s (%s, mode=%d)\n", phase, note, isig_mode);
	printf("\t%s latency: min %llu  avg %llu  max %llu ns (%d rounds)\n",
	       name, min, sum / n, max, n);
	print_hist(hist, n);
}

static void dump_data(void)
{
	const unsigned long long *t = isig_mode ? sh->t_uintr : sh->t_sig;
	char path[96];
	char ts[24];
	time_t now;
	struct tm tm;
	FILE *f;
	int i, n = 0;

	now = time(NULL);
	localtime_r(&now, &tm);
	strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm);
	snprintf(path, sizeof(path), "latency_mode%d_%s.csv", isig_mode, ts);
	f = fopen(path, "w");
	if (!f) {
		printf("[FAIL]\tfopen %s: %s\n", path, strerror(errno));
		nerrs++;
		return;
	}

	fprintf(f, "latency_ns\n");
	for (i = 0; i < ROUNDS; i++) {
		if (!t[i])
			continue;
		fprintf(f, "%llu\n", cycles_to_ns(t[i] - sh->t0[i]));
		n++;
	}
	fclose(f);
	printf("[INFO]\t%d raw samples written to %s\n", n, path);
}

/* ------------------------------------------------------------------ */
/* Child								*/
/* ------------------------------------------------------------------ */

static int run_child(char *slave_name, int sync_fd)
{
	struct termios t;
	int slave;

	/* New session, then open the pty slave to acquire it as ctty. */
	if (setsid() < 0) {
		printf("[SKIP]\tsetsid: %s\n", strerror(errno));
		return EXIT_SUCCESS;
	}

	if (child_cpu >= 0 && pin_cpu(child_cpu) < 0)
		printf("[INFO]\tpin receiver to cpu %d failed: %s\n",
		       child_cpu, strerror(errno));

	set_realtime(child_isolated);

	slave = open(slave_name, O_RDWR);
	if (slave < 0) {
		printf("[SKIP]\topen slave: %s\n", strerror(errno));
		return EXIT_SUCCESS;
	}

	if (tcgetattr(slave, &t) == 0) {
		t.c_lflag |= ISIG;
		t.c_lflag &= ~ECHO;
		t.c_cc[VINTR] = 0x03;
		tcsetattr(slave, TCSANOW, &t);
	}

	cyc_per_us = calibrate_tsc();
	printf("[INFO]\tTSC: %llu cycles/us\n", cyc_per_us);

	if (setup_sigint()) {
		printf("[SKIP]\tsigaction: %s\n", strerror(errno));
		return EXIT_SUCCESS;
	}
	if (setup_uintr()) {
		printf("[SKIP]\tuintr_vector_fd failed\n");
		return EXIT_SUCCESS;
	}

	/* Hand over to the parent for the asynchronous rounds. */
	__atomic_store_n(&sh->round, -1, __ATOMIC_RELEASE);
	if (write(sync_fd, "B", 1) != 1)
		printf("[FAIL]\tsync pipe: %s\n", strerror(errno));

	printf("[RUN]\tPhase B: asynchronous trigger (write to pty master)\n");
	if (run_phase_b_receiver() < 0) {
		nerrs++;
	} else {
		report_phase("Phase B", "async workqueue path, from write()");
		dump_data();
	}

	close(uvec_fd);
	uintr_unregister_handler(0);
	close(slave);
	close(sync_fd);
	return nerrs ? EXIT_FAILURE : EXIT_SUCCESS;
}

int main(void)
{
	int master, pipefd[2], pid, status = 0;
	char *slave_name;
	char go;
	int nrcpu, ret;

	if (!uintr_supported())
		return EXIT_SUCCESS;

	setbuf(stdout, NULL);

	isig_mode = read_isig_mode();
	if (isig_mode < 0) {
		printf("[SKIP]\t/proc/sys/kernel/uintr_isig_mode not found\n");
		return EXIT_SUCCESS;
	}
	printf("[INFO]\tCtrl-C delivery mode: %s\n",
	       isig_mode ? "UINTR only" : "signal only");

	nrcpu = sysconf(_SC_NPROCESSORS_ONLN);
	if (nrcpu < 3) {
		printf("[INFO]\tonly %d cpus online, running unpinned\n", nrcpu);
	} else {
		/*
		 * Parent on cpu 0, receiver on an isolated CPU if available
		 * (otherwise the last online CPU).  Run the test as root or
		 * with CAP_SYS_NICE so the receiver gets SCHED_FIFO.
		 */
		child_cpu = pick_receiver_cpu();
		child_isolated = (child_cpu != nrcpu - 1);
		if (pin_cpu(0) < 0)
			printf("[INFO]\tpin parent to cpu 0 failed: %s\n",
			       strerror(errno));
		else
			printf("[INFO]\tparent on cpu 0, receiver on cpu %d%s\n",
			       child_cpu,
			       child_isolated ? " (isolated)" : "");
	}

	sh = mmap(NULL, sizeof(*sh), PROT_READ | PROT_WRITE,
		  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (sh == MAP_FAILED) {
		printf("[SKIP]\tmmap: %s\n", strerror(errno));
		return EXIT_SUCCESS;
	}
	memset(sh, 0, sizeof(*sh));

	if (pipe(pipefd) < 0) {
		printf("[SKIP]\tpipe: %s\n", strerror(errno));
		return EXIT_SUCCESS;
	}

	master = posix_openpt(O_RDWR | O_NOCTTY);
	if (master < 0) {
		printf("[SKIP]\tposix_openpt: %s\n", strerror(errno));
		return EXIT_SUCCESS;
	}
	grantpt(master);
	unlockpt(master);
	slave_name = strdup(ptsname(master));

	pid = fork();
	if (pid < 0) {
		printf("[SKIP]\tfork: %s\n", strerror(errno));
		return EXIT_SUCCESS;
	}

	if (pid == 0) {
		close(master);
		close(pipefd[0]);
		ret = run_child(slave_name, pipefd[1]);
		_exit(ret);
	}

	close(pipefd[1]);

	/* Wait until the child finished the synchronous phase. */
	if (read(pipefd[0], &go, 1) != 1) {
		printf("[FAIL]\tchild did not reach phase B\n");
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
		return EXIT_FAILURE;
	}

	run_phase_b_trigger(master);

	waitpid(pid, &status, 0);
	close(pipefd[0]);
	close(master);

	return WIFEXITED(status) ? WEXITSTATUS(status) : EXIT_FAILURE;
}
