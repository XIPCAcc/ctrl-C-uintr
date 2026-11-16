// SPDX-License-Identifier: GPL-2.0-only
/*
 * tty_isig_freq.c - Ctrl-C latency and notification loss vs. send rate
 *
 * Sweep several write periods. For each period, report the latency
 * distribution and the number of rounds without a SIGINT/UINTR notification.
 * Latency is measured from write() to the handler and therefore includes the
 * tty workqueue wakeup, as in tty_isig_latency.c.
 *
 * Notifications do not carry a send sequence number. The receiver records
 * them in arrival order. Matching is done per frequency: a notification
 * belongs to the frequency whose sending window contains its arrival time
 * (it arrived before the next frequency's first write) and is matched FIFO
 * to the earliest unmatched write() in that window. Sends left unmatched
 * when the window closes count as lost; no match state crosses frequency
 * boundaries.
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
#include <sys/mman.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <x86gprintrin.h>

#include "uintr_common.h"

#define ROUNDS_PER_FREQ	10000
#define DRAIN_US	5000
#define HIST_BUCKETS	11

static const unsigned int periods_us[] = {
	1, 2, 3, 5, 10, 20, 50, 100, 200, 500, 1000
};
#define NFREQ		(sizeof(periods_us) / sizeof(periods_us[0]))
#define TOTAL_ROUNDS	(NFREQ * ROUNDS_PER_FREQ)

struct shm {
	volatile int		done;
	volatile int		nsig;
	volatile int		nuintr;
	unsigned long long	t0[TOTAL_ROUNDS];
	unsigned long long	t_sig[TOTAL_ROUNDS];
	unsigned long long	t_uintr[TOTAL_ROUNDS];
};

static struct shm *sh;
static unsigned long long cyc_per_us;
static int uvec_fd = -1;
static int child_cpu = -1;
static bool child_isolated;
static int isig_mode;

static void sigint_handler(int sig)
{
	int i = __atomic_load_n(&sh->nsig, __ATOMIC_RELAXED);

	if (i >= TOTAL_ROUNDS)
		return;
	sh->t_sig[i] = __rdtsc();
	__atomic_store_n(&sh->nsig, i + 1, __ATOMIC_RELEASE);
}

static void __attribute__((interrupt)) uintr_handler(struct __uintr_frame *ui_frame,
						     unsigned long long vector)
{
	int i = __atomic_load_n(&sh->nuintr, __ATOMIC_RELAXED);

	if (i >= TOTAL_ROUNDS)
		return;
	sh->t_uintr[i] = __rdtsc();
	__atomic_store_n(&sh->nuintr, i + 1, __ATOMIC_RELEASE);
}

static int setup_uintr(void)
{
	if (uintr_register_handler(uintr_handler, 0))
		return -1;

	uvec_fd = uintr_vector_fd(0, 0);
	if (uvec_fd < 0) {
		uintr_unregister_handler(0);
		return -1;
	}

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
	return ((c1 - c0) * 1000ULL + ns / 2) / ns;
}

static unsigned long long cycles_to_ns(unsigned long long cycles)
{
	if (!cyc_per_us)
		return cycles;
	return cycles * 1000ULL / cyc_per_us;
}

static int pin_cpu(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	return sched_setaffinity(0, sizeof(set), &set);
}

static int pick_receiver_cpu(void)
{
	char buf[256];
	int fd, n, cpu = -1;
	char *end;

	fd = open("/sys/devices/system/cpu/isolated", O_RDONLY);
	if (fd >= 0) {
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n > 0) {
			buf[n] = '\0';
			cpu = strtol(buf, &end, 10);
			if (end != buf && cpu >= 0)
				return cpu;
		}
	}

	n = sysconf(_SC_NPROCESSORS_ONLN);
	return (n > 1) ? n - 1 : 0;
}

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

static void run_receiver(void)
{
	while (!__atomic_load_n(&sh->done, __ATOMIC_ACQUIRE)) {
		/* Signal delivery may clear UIF; keep the receiver interruptible. */
		_stui();
		cpu_relax();
	}
}

static int run_trigger(int master)
{
	int i = 0;
	int f, r;
	int ret = 0;

	for (f = 0; f < NFREQ; f++) {
		/*
		 * TSC-deadline pacing: usleep() cannot sleep shorter than
		 * ~50us (timer slack), so spin until the next send deadline
		 * instead. Periods shorter than one write() simply send
		 * back-to-back at the syscall rate.
		 */
		unsigned long long period_cyc =
			(unsigned long long)periods_us[f] * cyc_per_us;
		unsigned long long next = __rdtsc();

		printf("[RUN]\tperiod %u us, %d rounds\n",
		       periods_us[f], ROUNDS_PER_FREQ);
		for (r = 0; r < ROUNDS_PER_FREQ; r++, i++) {
			next += period_cyc;
			while ((long long)(__rdtsc() - next) < 0)
				cpu_relax();
			sh->t0[i] = __rdtsc();
			if (write(master, "\003", 1) != 1) {
				printf("[FAIL]\twrite master: %s\n", strerror(errno));
				ret = -1;
				goto out;
			}

		}
		/* Drain late notifications before switching the send rate. */
		usleep(DRAIN_US);
	}

out:
	__atomic_store_n(&sh->done, 1, __ATOMIC_RELEASE);
	return ret;
}

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

static void report_results(void)
{
	unsigned long long min[NFREQ], max[NFREQ] = {0}, sum[NFREQ] = {0};
	int hist[NFREQ][HIST_BUCKETS] = {{0}};
	int received[NFREQ] = {0};
	const unsigned long long *tr = isig_mode ? sh->t_uintr : sh->t_sig;
	int nrecv = isig_mode ? sh->nuintr : sh->nsig;
	int k = 0, f;
	FILE *latency_fp, *summary_fp;
	char latency_name[80], summary_name[80];
	char ts[16];
	time_t now = time(NULL);
	struct tm tm_now;

	localtime_r(&now, &tm_now);
	strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_now);
	snprintf(latency_name, sizeof(latency_name),
		 "freq_latency_mode%d_%s.csv", isig_mode, ts);
	snprintf(summary_name, sizeof(summary_name),
		 "freq_summary_mode%d_%s.csv", isig_mode, ts);
	latency_fp = fopen(latency_name, "w");
	summary_fp = fopen(summary_name, "w");
	if (!latency_fp || !summary_fp) {
		printf("[FAIL]\texport CSV: %s\n", strerror(errno));
		if (latency_fp)
			fclose(latency_fp);
		if (summary_fp)
			fclose(summary_fp);
		return;
	}
	fprintf(latency_fp, "send_index,period_us,latency_ns\n");
	fprintf(summary_fp,
		"period_us,nominal_hz,sent,received,lost,loss_pct,min_ns,avg_ns,max_ns\n");

	if (nrecv > TOTAL_ROUNDS)
		nrecv = TOTAL_ROUNDS;

	/*
	 * Per-frequency matching: a notification belongs to the frequency
	 * whose sending window contains its arrival time (it arrived before
	 * the next frequency's first write). Within the window it is matched
	 * FIFO to the earliest unmatched write. Sends still unmatched when
	 * the window closes are counted as lost; the cursor restarts at
	 * every frequency, so no match state crosses the boundary.
	 */
	for (f = 0; f < NFREQ; f++) {
		int start = f * ROUNDS_PER_FREQ;
		int end = start + ROUNDS_PER_FREQ;
		unsigned long long window_end;
		int cursor = start;

		min[f] = ~0ULL;
		window_end = (end < TOTAL_ROUNDS && sh->t0[end]) ?
			     sh->t0[end] : ~0ULL;

		for (; k < nrecv && tr[k] < window_end; k++) {
			unsigned long long l, us;
			int b;

			while (cursor < end && !sh->t0[cursor])
				cursor++;
			if (cursor >= end || sh->t0[cursor] > tr[k])
				continue;
			l = cycles_to_ns(tr[k] - sh->t0[cursor]);
			fprintf(latency_fp, "%d,%u,%llu\n",
				cursor, periods_us[f], l);
			if (l < min[f])
				min[f] = l;
			if (l > max[f])
				max[f] = l;
			sum[f] += l;
			received[f]++;

			if (l < 1000) {
				b = 0;
			} else {
				us = l / 1000;
				b = 1 + (63 - __builtin_clzll(us));
				if (b >= HIST_BUCKETS)
					b = HIST_BUCKETS - 1;
			}
			hist[f][b]++;
			cursor++;
		}
	}

	for (f = 0; f < NFREQ; f++) {
		int n = received[f];
		int lost = ROUNDS_PER_FREQ - n;
		unsigned int loss_bp = lost * 10000U / ROUNDS_PER_FREQ;

		fprintf(summary_fp, "%u,%u,%d,%d,%d,%u.%02u,%llu,%llu,%llu\n",
			periods_us[f], 1000000U / periods_us[f], ROUNDS_PER_FREQ,
			n, lost, loss_bp / 100, loss_bp % 100,
			n ? min[f] : 0, n ? sum[f] / n : 0, n ? max[f] : 0);
		printf("\n[RESULT]\tperiod %u us (~%u Hz), mode=%d\n",
		       periods_us[f], 1000000U / periods_us[f], isig_mode);
		printf("\treceived %d/%d, lost %d (%u.%02u%%)\n",
		       n, ROUNDS_PER_FREQ, lost, loss_bp / 100, loss_bp % 100);
		if (!n) {
			printf("\tno latency samples\n");
			continue;
		}
		printf("\tlatency: min %llu  avg %llu  max %llu ns\n",
		       min[f], sum[f] / n, max[f]);
		print_hist(hist[f], n);
	}
	fclose(latency_fp);
	fclose(summary_fp);
	printf("\n[INFO]\texported %s and %s\n", latency_name, summary_name);
}

static int run_child(char *slave_name, int sync_fd)
{
	struct termios t;
	int slave;

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
		printf("[SKIP]\tuintr setup failed\n");
		return EXIT_SUCCESS;
	}

	if (write(sync_fd, "B", 1) != 1)
		printf("[FAIL]\tsync pipe: %s\n", strerror(errno));

	run_receiver();
	close(uvec_fd);
	uintr_unregister_handler(0);
	close(slave);
	close(sync_fd);
	return EXIT_SUCCESS;
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
		child_cpu = pick_receiver_cpu();
		child_isolated = (child_cpu != nrcpu - 1);
		if (pin_cpu(0) < 0)
			printf("[INFO]\tpin parent to cpu 0 failed: %s\n",
			       strerror(errno));
		else
			printf("[INFO]\tparent on cpu 0, receiver on cpu %d%s\n",
			       child_cpu, child_isolated ? " (isolated)" : "");
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
	if (read(pipefd[0], &go, 1) != 1) {
		printf("[FAIL]\tchild did not initialize\n");
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
		return EXIT_FAILURE;
	}

	cyc_per_us = calibrate_tsc();
	ret = run_trigger(master);
	waitpid(pid, &status, 0);
	report_results();
	close(pipefd[0]);
	close(master);

	if (ret < 0)
		return EXIT_FAILURE;
	return WIFEXITED(status) ? WEXITSTATUS(status) : EXIT_FAILURE;
}
