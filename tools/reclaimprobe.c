/* reclaimprobe — measures the 6.1 order-1 reclaim path with no UAF involved.
 *
 *   1. create N POSIX timers, read trace_pipe_raw to learn their k_itimer
 *      addresses (bytes_req == 264, single modal call_site)
 *   2. delete them all and watch mm_page_free for the 8 KiB slab pages those
 *      objects live on
 *   3. allocate 4096 AF_UNIX socketpairs, send 8 KiB on each while watching
 *      mm_page_alloc, and count how many of the freed pages we got back
 *
 * Read-only with respect to kernel memory; only tracefs + timer/socket syscalls.
 */
#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
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

static long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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

/* page-base PFNs of the 8 KiB slabs holding our timers */
static int collect_timer_pfns(const uint8_t *buf, size_t used,
			      uint64_t *out, int cap, uint64_t *modal_site)
{
	int n = 0;
	uint64_t best = 0;
	int bestc = 0;
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
		if (n < cap) {
			out[n * 2] = site;
			out[n * 2 + 1] = ptr;
			n++;
		}
	}
	for (int i = 0; i < n; i++) {
		int c = 0;
		for (int j = 0; j < n; j++)
			if (out[j * 2] == out[i * 2])
				c++;
		if (c > bestc) {
			bestc = c;
			best = out[i * 2];
		}
	}
	/* keep only the modal call_site */
	{
		int k = 0;
		for (int i = 0; i < n; i++)
			if (out[i * 2] == best)
				out[k++] = out[i * 2 + 1];
		n = k;
	}
	*modal_site = best;
	return n;
}

static void slabinfo(const char *tag)
{
	FILE *f = fopen("/proc/slabinfo", "r");
	char line[512];
	if (!f) {
		printf("SLABINFO[%s]=unavailable\n", tag);
		return;
	}
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "posix_timers_cache", 18)) {
			char name[64];
			unsigned long active, num, size, objperslab, pagesperslab;
			sscanf(line, "%63s %lu %lu %lu %lu %lu", name, &active, &num,
			       &size, &objperslab, &pagesperslab);
			printf("SLABINFO[%s] active=%lu num=%lu size=%lu per_slab=%lu pages=%lu\n",
			       tag, active, num, size, objperslab, pagesperslab);
			break;
		}
	}
	fclose(f);
}

static long long g_total = 0;
static int g_order_hist[16];


static int snapshot_pages(int n, uint64_t *pfns, int cap, uint64_t *site_out)
{
	uint8_t *raw = malloc(32u << 20);
	uint64_t *recs = calloc(1 << 16, sizeof(uint64_t));
	timer_t *v = calloc((size_t)n, sizeof(timer_t));
	struct sigevent ev;
	int made = 0, nrec, npfn = 0;
	size_t used = 0;
	if (!raw || !recs || !v)
		return -1;
	memset(&ev, 0, sizeof(ev));
	ev.sigev_notify = SIGEV_NONE;
	pin_cpu(0);
	wf(TRACE "/tracing_on", "0\n");
	{
		int d = open(TRACE "/per_cpu/cpu0/trace_pipe_raw", O_RDONLY | O_NONBLOCK);
		char junk[65536];
		if (d >= 0) {
			while (read(d, junk, sizeof(junk)) > 0)
				;
			close(d);
		}
	}
	wf(TRACE "/tracing_on", "1\n");
	for (int i = 0; i < n; i++)
		if (timer_create(CLOCK_MONOTONIC, &ev, &v[i]) == 0)
			made++;
	wf(TRACE "/tracing_on", "0\n");
	read_raw_cpu0(raw, 32u << 20, &used);
	nrec = collect_timer_pfns(raw, used, recs, 1 << 16, site_out);
	for (int i = 0; i < nrec; i++) {
		uint64_t page = recs[i] & ~UINT64_C(0x1fff);
		uint64_t pfn = (page - PAGE_OFFSET) >> 12;
		int seen = 0;
		for (int j = 0; j < npfn; j++)
			if (pfns[j] == pfn) { seen = 1; break; }
		if (!seen && npfn < cap)
			pfns[npfn++] = pfn;
	}
	for (int i = 0; i < made; i++)
		timer_delete(v[i]);
	free(raw);
	free(recs);
	free(v);
	return npfn;
}

static int find_pfn(const uint64_t *pfns, int n, uint64_t pfn)
{
	for (int i = 0; i < n; i++)
		if (pfns[i] == pfn || pfns[i] + 1 == pfn)
			return 1;
	return 0;
}

/* scan global trace_pipe text for pfn hits */
static int scan_pipe(int fd, char *carry, size_t *carrylen, const char *kind,
		     const uint64_t *pfns, int npfn, int *orders, int *hits,
		     long long deadline)
{
	int found = 0;
	while (now_ms() < deadline) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		char buf[65536];
		ssize_t n;
		char *text;
		if (poll(&p, 1, 50) <= 0)
			continue;
		n = read(fd, buf, sizeof(buf) - 1);
		if (n <= 0)
			continue;
		text = malloc(*carrylen + (size_t)n + 1);
		if (!text)
			return found;
		memcpy(text, carry, *carrylen);
		memcpy(text + *carrylen, buf, (size_t)n);
		text[*carrylen + n] = 0;
		{
			char *line = text, *nl;
			while ((nl = strchr(line, '\n'))) {
				*nl = 0;
				if (strstr(line, kind)) {
					g_total++;
					{
						char *op2 = strstr(line, "order=");
						if (op2) {
							int o2 = atoi(op2 + 6);
							if (o2 >= 0 && o2 < 16)
								g_order_hist[o2]++;
						}
					}
					char *pp = strstr(line, "pfn=0x");
					char *op = strstr(line, "order=");
					if (pp) {
						uint64_t pfn = strtoull(pp + 6, NULL, 16);
						int order = op ? atoi(op + 6) : -1;
						if (find_pfn(pfns, npfn, pfn)) {
							(*hits)++;
							if (orders)
								orders[pfn & 7] = order;
							found = 1;
						}
					}
				}
				line = nl + 1;
			}
			*carrylen = strlen(line);
			if (*carrylen >= 512)
				*carrylen = 0;
			else
				memmove(carry, line, *carrylen);
		}
		free(text);
	}
	return found;
}

struct spray {
	int (*p)[2];
	int n;
};

static int spray_open(struct spray *s, int want)
{
	s->p = calloc((size_t)want, sizeof(s->p[0]));
	if (!s->p)
		return -1;
	for (int i = 0; i < want; i++) {
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, s->p[i]))
			break;
		s->n++;
	}
	return s->n;
}

static int spray_send_all(struct spray *s)
{
	char pat[8192];
	int ok = 0;
	memset(pat, 0x41, sizeof(pat));
	for (int i = 0; i < s->n; i++) {
		ssize_t w = send(s->p[i][0], pat, sizeof(pat), MSG_DONTWAIT);
		if (w == (ssize_t)sizeof(pat))
			ok++;
	}
	return ok;
}

static void spray_close(struct spray *s)
{
	for (int i = 0; i < s->n; i++) {
		close(s->p[i][0]);
		close(s->p[i][1]);
	}
	free(s->p);
}

int main(int argc, char **argv)
{
	int want = argc > 1 ? atoi(argv[1]) : 2048;
	int nsock = argc > 2 ? atoi(argv[2]) : 4096;
	uint8_t *raw = malloc(32u << 20);
	uint64_t *recs, *pfns;
	int nrec, npfn = 0, distinct = 0;
	uint64_t modal = 0;
	timer_t *timers;
	struct sigevent ev;
	struct spray sp = { 0 };
	int fd, hits = 0, orders[8] = { 0 };
	char carry[1024];
	size_t carrylen = 0;
	long long t0;

	if (argc > 3 && !strcmp(argv[3], "reuse")) {
		uint64_t *p1 = calloc(4096, sizeof(uint64_t));
		uint64_t *p2 = calloc(4096, sizeof(uint64_t));
		uint64_t site1 = 0, site2 = 0;
		int n1, n2, inter = 0;
		n1 = snapshot_pages(want, p1, 4096, &site1);
		printf("BATCH1 pages=%d site=%#llx\n", n1, (unsigned long long)site1);
		usleep(600000);
		n2 = snapshot_pages(want, p2, 4096, &site2);
		printf("BATCH2 pages=%d site=%#llx\n", n2, (unsigned long long)site2);
		for (int i = 0; i < n1; i++)
			for (int j = 0; j < n2; j++)
				if (p1[i] == p2[j]) { inter++; break; }
		printf("PAGE_REUSE=%d/%d\n", inter, n1 < n2 ? n1 : n2);
		fflush(stdout);
		return 0;
	}

	if (!raw)
		return 1;
	recs = calloc(1 << 16, sizeof(*recs));
	pfns = calloc(1 << 16, sizeof(*pfns));
	timers = calloc((size_t)want, sizeof(*timers));
	if (!recs || !pfns || !timers)
		return 1;

	pin_cpu(0);
	slabinfo("baseline");
	wf(TRACE "/tracing_on", "0\n");
	wf(TRACE "/buffer_size_kb", "16384\n");
	wf(TRACE "/events/kmem/rss_stat/enable", "1\n");
	wf(TRACE "/events/kmem/rss_stat/trigger", "enable_event:kmem:kmem_cache_alloc\n");
	wf(TRACE "/events/kmem/rss_stat/trigger", "enable_event:kmem:mm_page_alloc\n");
	wf(TRACE "/events/kmem/rss_stat/trigger", "enable_event:kmem:mm_page_free\n");
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

	memset(&ev, 0, sizeof(ev));
	ev.sigev_notify = SIGEV_NONE;

	wf(TRACE "/tracing_on", "1\n");
	int made = 0;
	for (int i = 0; i < want; i++)
		if (timer_create(CLOCK_MONOTONIC, &ev, &timers[i]) == 0)
			made++;
	wf(TRACE "/tracing_on", "0\n");
	printf("TIMERS=%d\n", made);
	slabinfo("after_create");

	{
		size_t used = 0;
		if (read_raw_cpu0(raw, 32u << 20, &used))
			return 2;
		nrec = collect_timer_pfns(raw, used, recs, 1 << 16, &modal);
		printf("KTIMERS=%d MODAL_SITE=%#llx RAW=%zu\n", nrec,
		       (unsigned long long)modal, used);
	}
	for (int i = 0; i < nrec; i++) {
		uint64_t page = recs[i] & ~UINT64_C(0x1fff);
		uint64_t pfn = (page - PAGE_OFFSET) >> 12;
		int seen = 0;
		for (int j = 0; j < npfn; j++)
			if (pfns[j] == pfn) {
				seen = 1;
				break;
			}
		if (!seen) {
			pfns[npfn++] = pfn;
			distinct++;
		}
	}
	printf("SLAB_PAGES=%d\n", distinct);

	/* ---- free the timers and watch the pages go back to buddy ---- */
	fd = open(TRACE "/trace_pipe", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		perror("trace_pipe");
		return 3;
	}
	wf(TRACE "/tracing_on", "1\n");
	t0 = now_ms();
	for (int i = 0; i < made; i++)
		timer_delete(timers[i]);
	printf("TIMERS_DELETED=%d\n", made);
	usleep(300000);
	slabinfo("after_delete_300ms");
	scan_pipe(fd, carry, &carrylen, "mm_page_free:", pfns, npfn, orders, &hits,
		  t0 + 3000);
	wf(TRACE "/tracing_on", "0\n");
	printf("FREE_HITS=%d of %d (ms=%lld) TOTAL_FREE_EVENTS=%lld\n",
	       hits, npfn, now_ms() - t0, g_total);
	{ int i; for (i = 0; i < 6; i++) printf("FREE_HIST[%d]=%d\n", i, g_order_hist[i]); }
	g_total = 0;
	memset(g_order_hist, 0, sizeof(g_order_hist));
	for (int i = 0; i < 8; i++)
		if (orders[i] || i == 0)
			printf("FREE_ORDER[%d]=%d\n", i, orders[i]);

	/* ---- spray 8 KiB AF_UNIX skbs and see if we get those pages ---- */
	if (spray_open(&sp, nsock) <= 0)
		return 4;
	printf("SOCKS=%d\n", sp.n);
	carrylen = 0;
	hits = 0;
	wf(TRACE "/tracing_on", "1\n");
	t0 = now_ms();
	{
		int sent = spray_send_all(&sp);
		printf("SPRAY_SENT=%d\n", sent);
	}
	scan_pipe(fd, carry, &carrylen, "mm_page_alloc:", pfns, npfn, NULL, &hits,
		  now_ms() + 1500);
	wf(TRACE "/tracing_on", "0\n");
	printf("ALLOC_HITS=%d of %d (ms=%lld) TOTAL_ALLOC_EVENTS=%lld\n",
	       hits, npfn, now_ms() - t0, g_total);
	{ int i; for (i = 0; i < 6; i++) printf("ALLOC_HIST[%d]=%d\n", i, g_order_hist[i]); }

	spray_close(&sp);
	close(fd);
	fflush(stdout);
	return 0;
}
