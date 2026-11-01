/* pfntest — calibrate the direct-map VA -> pfn relation on this kernel.
 *
 * Reading /sys/kernel/tracing/trace is a *snapshot* (non-consuming), so the
 * same batch of allocations can be observed twice:
 *   - text view:  mm_page_alloc ... pfn=0x... order=1   (slab page pfns)
 *   - raw view:   kmem_cache_alloc ... ptr=<kva>          (object addresses)
 * Compare { (kva & ~0x1fff - BASE) >> 12 } against the order-1 alloc pfns.
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
#include <time.h>
#include <unistd.h>

#define TRACE "/sys/kernel/tracing"
#define KITIMER_SIZE 264u
static uint64_t g_va[2048];
static int g_nva;
#define PAGE_OFFSET_DEFAULT UINT64_C(0xffffff8000000000)

static int wf(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	ssize_t n;
	if (fd < 0)
		return -1;
	n = write(fd, text, strlen(text));
	close(fd);
	return n == (ssize_t)strlen(text) ? 0 : -1;
}

static void pin_cpu(int cpu)
{
	cpu_set_t s;
	CPU_ZERO(&s);
	CPU_SET(cpu, &s);
	sched_setaffinity(0, sizeof(s), &s);
}

static void kick_rss(void)
{
	size_t len = 16u << 20;
	char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p != MAP_FAILED) {
		for (size_t i = 0; i < len; i += 4096)
			p[i] = 1;
		munmap(p, len);
	}
}

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 2048;
	uint64_t alloc_pfn[2048];
	uint64_t timer_pfn[2048];
	int nalloc = 0, ntimer = 0, inter = 0;
	timer_t *v;
	struct sigevent ev;
	char *text = malloc(32u << 20);
	uint8_t *raw = malloc(32u << 20);
	uint64_t base = argc > 2 ? strtoull(argv[2], NULL, 0) : PAGE_OFFSET_DEFAULT;

	if (!text || !raw)
		return 1;
	v = calloc((size_t)n, sizeof(*v));
	if (!v)
		return 1;

	pin_cpu(0);
	wf(TRACE "/tracing_on", "0\n");
	wf(TRACE "/buffer_size_kb", "16384\n");
	wf(TRACE "/events/kmem/rss_stat/enable", "1\n");
	wf(TRACE "/events/kmem/rss_stat/trigger", "enable_event:kmem:kmem_cache_alloc\n");
	wf(TRACE "/events/kmem/rss_stat/trigger", "enable_event:kmem:mm_page_alloc\n");
	kick_rss();
	{
		int d = open(TRACE "/per_cpu/cpu0/trace_pipe_raw", O_RDONLY | O_NONBLOCK);
		char junk[65536];
		if (d >= 0) {
			while (read(d, junk, sizeof(junk)) > 0)
				;
			close(d);
		}
	}
	{
		int c = open(TRACE "/trace", O_WRONLY | O_TRUNC);
		if (c >= 0)
			close(c);
	}
	usleep(50000);

	memset(&ev, 0, sizeof(ev));
	ev.sigev_notify = SIGEV_NONE;
	wf(TRACE "/tracing_on", "1\n");
	int made = 0;
	for (int i = 0; i < n; i++)
		if (timer_create(CLOCK_MONOTONIC, &ev, &v[i]) == 0)
			made++;
	wf(TRACE "/tracing_on", "0\n");

	/* text snapshot (non-consuming) */
	{
		int fd = open(TRACE "/trace", O_RDONLY | O_CLOEXEC);
		ssize_t got = 0;
		if (fd >= 0) {
			for (;;) {
				ssize_t r = read(fd, text + got, (32u << 20) - 1 - (size_t)got);
				if (r <= 0)
					break;
				got += r;
				if ((size_t)got >= (32u << 20) - 1)
					break;
			}
			close(fd);
		}
		if (got > 0) {
			char *line = text;
			text[got] = 0;
			while (line && *line) {
				char *nl = strchr(line, '\n');
				if (nl)
					*nl = 0;
				if (strstr(line, "mm_page_alloc") && strstr(line, "order=1")) {
					char *pp = strstr(line, "pfn=0x");
					if (pp && nalloc < 2048)
						alloc_pfn[nalloc++] = strtoull(pp + 6, NULL, 16);
				}
				line = nl ? nl + 1 : NULL;
			}
		}
	}

	/* raw view */
	{
		int fd = open(TRACE "/per_cpu/cpu0/trace_pipe_raw",
			      O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		size_t used = 0;
		if (fd >= 0) {
			while (used + 4096 < (32u << 20)) {
				ssize_t r = read(fd, raw + used, (32u << 20) - used);
				if (r > 0) {
					used += (size_t)r;
					continue;
				}
				break;
			}
			close(fd);
		}
		for (size_t o = 0; o + 40 <= used; o++) {
			uint64_t site, ptr;
			uint32_t req;
			memcpy(&site, raw + o, 8);
			memcpy(&ptr, raw + o + 8, 8);
			memcpy(&req, raw + o + 16, 4);
			if ((site >> 48) != 0xffff || (ptr >> 48) != 0xffff || (ptr & 7))
				continue;
			if (req != KITIMER_SIZE)
				continue;
			if (ntimer < 2048) {
				timer_pfn[ntimer++] = ((ptr & ~UINT64_C(0x1fff)) - base) >> 12;
				if (g_nva < 2048)
					g_va[g_nva++] = ptr & ~UINT64_C(0x1fff);
			}
		}
	}

	for (int i = 0; i < nalloc; i++)
		for (int j = 0; j < ntimer; j++)
			if (alloc_pfn[i] == timer_pfn[j] || alloc_pfn[i] + 1 == timer_pfn[j]) {
				inter++;
				break;
			}
	/* delta histogram: va_page - pfn*4096, true mapping repeats per slab */
	{
		int best_i = -1, best_c = 0;
		int64_t best_d = 0;
		int lim_v = g_nva < 200 ? g_nva : 200;
		int lim_p = nalloc < 200 ? nalloc : 200;
		for (int i = 0; i < lim_v; i++) {
			for (int j = 0; j < lim_p; j++) {
				int64_t d = (int64_t)g_va[i] - (int64_t)(alloc_pfn[j] << 12);
				int c = 0;
				for (int a = 0; a < lim_v; a++)
					for (int b = 0; b < lim_p; b++)
						if ((int64_t)g_va[a] - (int64_t)(alloc_pfn[b] << 12) == d)
							c++;
				if (c > best_c) {
					best_c = c;
					best_d = d;
					best_i = i;
				}
			}
		}
		int inter2 = 0;
		if (best_c >= 3) {
			for (int i = 0; i < g_nva; i++) {
				uint64_t pfn = (uint64_t)(((int64_t)g_va[i] - best_d) >> 12);
				for (int j = 0; j < nalloc; j++)
					if (alloc_pfn[j] == pfn || alloc_pfn[j] + 1 == pfn) {
						inter2++;
						break;
					}
			}
		}
		printf("DELTA_BEST=%#llx COUNT=%d VERIFIED_HITS=%d/%d\n",
		       (unsigned long long)best_d, best_c, inter2, g_nva);
		(void)best_i;
	}
	printf("timers=%d alloc_pfn=%d timer_pfn=%d INTERSECT=%d base=%#llx\n",
	       made, nalloc, ntimer, inter, (unsigned long long)base);
	if (nalloc)
		printf("alloc_sample=%#llx %#llx %#llx\n",
		       (unsigned long long)alloc_pfn[0], (unsigned long long)alloc_pfn[1],
		       (unsigned long long)alloc_pfn[2]);
	if (g_nva) {
		printf("VA_PAGES:");
		for (int i = 0; i < g_nva && i < 24; i++)
			printf(" %#llx", (unsigned long long)g_va[i]);
		printf("\n");
	}
	if (nalloc) {
		printf("ALLOC_PFNS:");
		for (int i = 0; i < nalloc && i < 24; i++)
			printf(" %#llx", (unsigned long long)alloc_pfn[i]);
		printf("\n");
	}
	if (ntimer)
		printf("timer_sample=%#llx %#llx %#llx\n",
		       (unsigned long long)timer_pfn[0], (unsigned long long)timer_pfn[1],
		       (unsigned long long)timer_pfn[2]);
	fflush(stdout);
	return 0;
}
