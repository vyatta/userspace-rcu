/*
 * test_urcu_gc.c
 *
 * Userspace RCU library - test program (with baatch reclamation)
 *
 * Copyright February 2009 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#define _GNU_SOURCE
#include "../config.h"
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>

#include <urcu/arch.h>
#include <urcu/tls-compat.h>
#include "cpuset.h"

#ifdef __linux__
#include <syscall.h>
#endif

/* hardcoded number of CPUs */
#define NR_CPUS 16384

#if defined(_syscall0)
_syscall0(pid_t, gettid)
#elif defined(__NR_gettid)
static inline pid_t gettid(void)
{
	return syscall(__NR_gettid);
}
#else
#warning "use pid as tid"
static inline pid_t gettid(void)
{
	return getpid();
}
#endif

#define _LGPL_SOURCE
#include <urcu-qsbr.h>

struct test_array {
	int a;
};

static volatile int test_go, test_stop;

static unsigned long wdelay;

static struct test_array *test_rcu_pointer;

static unsigned long duration;

/* read-side C.S. duration, in loops */
static unsigned long rduration;
static unsigned int reclaim_batch = 1;

struct reclaim_queue {
	void **queue;	/* Beginning of queue */
	void **head;	/* Insert position */
};

static struct reclaim_queue *pending_reclaims;


/* write-side C.S. duration, in loops */
static unsigned long wduration;

static inline void loop_sleep(unsigned long l)
{
	while(l-- != 0)
		caa_cpu_relax();
}

static int verbose_mode;

#define printf_verbose(fmt, args...)		\
	do {					\
		if (verbose_mode)		\
			printf(fmt, args);	\
	} while (0)

static unsigned int cpu_affinities[NR_CPUS];
static unsigned int next_aff = 0;
static int use_affinity = 0;

pthread_mutex_t affinity_mutex = PTHREAD_MUTEX_INITIALIZER;

static void set_affinity(void)
{
	cpu_set_t mask;
	int cpu;
	int ret;

	if (!use_affinity)
		return;

#if HAVE_SCHED_SETAFFINITY
	ret = pthread_mutex_lock(&affinity_mutex);
	if (ret) {
		perror("Error in pthread mutex lock");
		exit(-1);
	}
	cpu = cpu_affinities[next_aff++];
	ret = pthread_mutex_unlock(&affinity_mutex);
	if (ret) {
		perror("Error in pthread mutex unlock");
		exit(-1);
	}

	CPU_ZERO(&mask);
	CPU_SET(cpu, &mask);
#if SCHED_SETAFFINITY_ARGS == 2
	sched_setaffinity(0, &mask);
#else
	sched_setaffinity(0, sizeof(mask), &mask);
#endif
#endif /* HAVE_SCHED_SETAFFINITY */
}

/*
 * returns 0 if test should end.
 */
static int test_duration_write(void)
{
	return !test_stop;
}

static int test_duration_read(void)
{
	return !test_stop;
}

static DEFINE_URCU_TLS(unsigned long long, nr_writes);
static DEFINE_URCU_TLS(unsigned long long, nr_reads);

static unsigned int nr_readers;
static unsigned int nr_writers;

pthread_mutex_t rcu_copy_mutex = PTHREAD_MUTEX_INITIALIZER;
static
unsigned long long __attribute__((aligned(CAA_CACHE_LINE_SIZE))) *tot_nr_writes;


void rcu_copy_mutex_lock(void)
{
	int ret;
	ret = pthread_mutex_lock(&rcu_copy_mutex);
	if (ret) {
		perror("Error in pthread mutex lock");
		exit(-1);
	}
}

void rcu_copy_mutex_unlock(void)
{
	int ret;

	ret = pthread_mutex_unlock(&rcu_copy_mutex);
	if (ret) {
		perror("Error in pthread mutex unlock");
		exit(-1);
	}
}

void *thr_reader(void *_count)
{
	unsigned long long *count = _count;
	struct test_array *local_ptr;

	printf_verbose("thread_begin %s, thread id : %lx, tid %lu\n",
			"reader", (unsigned long) pthread_self(),
			(unsigned long) gettid());

	set_affinity();

	rcu_register_thread();

	while (!test_go)
	{
	}
	cmm_smp_mb();

	for (;;) {
		_rcu_read_lock();
		local_ptr = _rcu_dereference(test_rcu_pointer);
		debug_yield_read();
		if (local_ptr)
			assert(local_ptr->a == 8);
		if (caa_unlikely(rduration))
			loop_sleep(rduration);
		_rcu_read_unlock();
		URCU_TLS(nr_reads)++;
		/* QS each 1024 reads */
		if (caa_unlikely((URCU_TLS(nr_reads) & ((1 << 10) - 1)) == 0))
			_rcu_quiescent_state();
		if (caa_unlikely(!test_duration_read()))
			break;
	}

	rcu_unregister_thread();

	*count = URCU_TLS(nr_reads);
	printf_verbose("thread_end %s, thread id : %lx, tid %lu\n",
			"reader", (unsigned long) pthread_self(),
			(unsigned long) gettid());
	return ((void*)1);

}

static void rcu_gc_clear_queue(unsigned long wtidx)
{
	void **p;

	/* Wait for Q.S and empty queue */
	synchronize_rcu();

	for (p = pending_reclaims[wtidx].queue;
			p < pending_reclaims[wtidx].head; p++) {
		/* poison */
		if (*p)
			((struct test_array *)*p)->a = 0;
		free(*p);
	}
	pending_reclaims[wtidx].head = pending_reclaims[wtidx].queue;
}

/* Using per-thread queue */
static void rcu_gc_reclaim(unsigned long wtidx, void *old)
{
	/* Queue pointer */
	*pending_reclaims[wtidx].head = old;
	pending_reclaims[wtidx].head++;

	if (caa_likely(pending_reclaims[wtidx].head - pending_reclaims[wtidx].queue
			< reclaim_batch))
		return;

	rcu_gc_clear_queue(wtidx);
}

void *thr_writer(void *data)
{
	unsigned long wtidx = (unsigned long)data;
#ifdef TEST_LOCAL_GC
	struct test_array *old = NULL;
#else
	struct test_array *new, *old;
#endif

	printf_verbose("thread_begin %s, thread id : %lx, tid %lu\n",
			"writer", (unsigned long) pthread_self(),
			(unsigned long) gettid());

	set_affinity();

	while (!test_go)
	{
	}
	cmm_smp_mb();

	for (;;) {
#ifndef TEST_LOCAL_GC
		new = malloc(sizeof(*new));
		new->a = 8;
		old = _rcu_xchg_pointer(&test_rcu_pointer, new);
#endif
		if (caa_unlikely(wduration))
			loop_sleep(wduration);
		rcu_gc_reclaim(wtidx, old);
		URCU_TLS(nr_writes)++;
		if (caa_unlikely(!test_duration_write()))
			break;
		if (caa_unlikely(wdelay))
			loop_sleep(wdelay);
	}

	printf_verbose("thread_end %s, thread id : %lx, tid %lu\n",
			"writer", (unsigned long) pthread_self(),
			(unsigned long) gettid());
	tot_nr_writes[wtidx] = URCU_TLS(nr_writes);
	return ((void*)2);
}

void show_usage(int argc, char **argv)
{
	printf("Usage : %s nr_readers nr_writers duration (s)", argv[0]);
#ifdef DEBUG_YIELD
	printf(" [-r] [-w] (yield reader and/or writer)");
#endif
	printf(" [-b batch] (batch reclaim)");
	printf(" [-d delay] (writer period (us))");
	printf(" [-c duration] (reader C.S. duration (in loops))");
	printf(" [-e duration] (writer C.S. duration (in loops))");
	printf(" [-v] (verbose output)");
	printf(" [-a cpu#] [-a cpu#]... (affinity)");
	printf("\n");
}

int main(int argc, char **argv)
{
	int err;
	pthread_t *tid_reader, *tid_writer;
	void *tret;
	unsigned long long *count_reader;
	unsigned long long tot_reads = 0, tot_writes = 0;
	int i, a;

	if (argc < 4) {
		show_usage(argc, argv);
		return -1;
	}

	err = sscanf(argv[1], "%u", &nr_readers);
	if (err != 1) {
		show_usage(argc, argv);
		return -1;
	}

	err = sscanf(argv[2], "%u", &nr_writers);
	if (err != 1) {
		show_usage(argc, argv);
		return -1;
	}
	
	err = sscanf(argv[3], "%lu", &duration);
	if (err != 1) {
		show_usage(argc, argv);
		return -1;
	}

	for (i = 4; i < argc; i++) {
		if (argv[i][0] != '-')
			continue;
		switch (argv[i][1]) {
#ifdef DEBUG_YIELD
		case 'r':
			yield_active |= YIELD_READ;
			break;
		case 'w':
			yield_active |= YIELD_WRITE;
			break;
#endif
		case 'a':
			if (argc < i + 2) {
				show_usage(argc, argv);
				return -1;
			}
			a = atoi(argv[++i]);
			cpu_affinities[next_aff++] = a;
			use_affinity = 1;
			printf_verbose("Adding CPU %d affinity\n", a);
			break;
		case 'b':
			if (argc < i + 2) {
				show_usage(argc, argv);
				return -1;
			}
			reclaim_batch = atol(argv[++i]);
			break;
		case 'c':
			if (argc < i + 2) {
				show_usage(argc, argv);
				return -1;
			}
			rduration = atol(argv[++i]);
			break;
		case 'd':
			if (argc < i + 2) {
				show_usage(argc, argv);
				return -1;
			}
			wdelay = atol(argv[++i]);
			break;
		case 'e':
			if (argc < i + 2) {
				show_usage(argc, argv);
				return -1;
			}
			wduration = atol(argv[++i]);
			break;
		case 'v':
			verbose_mode = 1;
			break;
		}
	}

	printf_verbose("running test for %lu seconds, %u readers, %u writers.\n",
		duration, nr_readers, nr_writers);
	printf_verbose("Writer delay : %lu loops.\n", wdelay);
	printf_verbose("Reader duration : %lu loops.\n", rduration);
	printf_verbose("thread %-6s, thread id : %lx, tid %lu\n",
			"main", (unsigned long) pthread_self(),
			(unsigned long) gettid());

	tid_reader = malloc(sizeof(*tid_reader) * nr_readers);
	tid_writer = malloc(sizeof(*tid_writer) * nr_writers);
	count_reader = malloc(sizeof(*count_reader) * nr_readers);
	tot_nr_writes = malloc(sizeof(*tot_nr_writes) * nr_writers);
	pending_reclaims = malloc(sizeof(*pending_reclaims) * nr_writers);
	if (reclaim_batch * sizeof(*pending_reclaims[i].queue)
			< CAA_CACHE_LINE_SIZE)
		for (i = 0; i < nr_writers; i++)
			pending_reclaims[i].queue = calloc(1, CAA_CACHE_LINE_SIZE);
	else
		for (i = 0; i < nr_writers; i++)
			pending_reclaims[i].queue = calloc(reclaim_batch,
					sizeof(*pending_reclaims[i].queue));
	for (i = 0; i < nr_writers; i++)
		pending_reclaims[i].head = pending_reclaims[i].queue;

	next_aff = 0;

	for (i = 0; i < nr_readers; i++) {
		err = pthread_create(&tid_reader[i], NULL, thr_reader,
				     &count_reader[i]);
		if (err != 0)
			exit(1);
	}
	for (i = 0; i < nr_writers; i++) {
		err = pthread_create(&tid_writer[i], NULL, thr_writer,
				     (void *)(long)i);
		if (err != 0)
			exit(1);
	}

	cmm_smp_mb();

	test_go = 1;

	sleep(duration);

	test_stop = 1;

	for (i = 0; i < nr_readers; i++) {
		err = pthread_join(tid_reader[i], &tret);
		if (err != 0)
			exit(1);
		tot_reads += count_reader[i];
	}
	for (i = 0; i < nr_writers; i++) {
		err = pthread_join(tid_writer[i], &tret);
		if (err != 0)
			exit(1);
		tot_writes += tot_nr_writes[i];
		rcu_gc_clear_queue(i);
	}
	
	printf_verbose("total number of reads : %llu, writes %llu\n", tot_reads,
	       tot_writes);
	printf("SUMMARY %-25s testdur %4lu nr_readers %3u rdur %6lu wdur %6lu "
		"nr_writers %3u "
		"wdelay %6lu nr_reads %12llu nr_writes %12llu nr_ops %12llu "
		"batch %u\n",
		argv[0], duration, nr_readers, rduration, wduration,
		nr_writers, wdelay, tot_reads, tot_writes,
		tot_reads + tot_writes, reclaim_batch);
	free(tid_reader);
	free(tid_writer);
	free(count_reader);
	free(tot_nr_writes);
	for (i = 0; i < nr_writers; i++)
		free(pending_reclaims[i].queue);
	free(pending_reclaims);

	return 0;
}
