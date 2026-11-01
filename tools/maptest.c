/* maptest — calibrate phys = f(kva) by pairing the two trace views of the same
 * 8 KiB allocations:
 *   text  view: mm_page_alloc ... pfn=0x.. order=1        (page pfns)
 *   raw   view: kmem_cache_alloc ... ptr=<kva>, req=8192   (object addresses)
 * Both come from the same send loop; pairing consecutive entries yields the
 * constant VA->PA offset (memstart_addr - PAGE_OFFSET).
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
	int nsock = argc > 1 ? atoi(argv[1]) : 512;
	uint64_t pfns[4096];
	uint64_t vas[4096];
	int np = 0, nv = 0;
	char *text = malloc(32u << 20);
	uint8_t *raw = malloc(32u << 20);
	int (*sock)[2];

	if (!text || !raw)
		return 1;
	text[0] = 0;
	sock = calloc((size_t)nsock, sizeof(sock[0]));
	if (!sock)
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

	int n = 0;
	for (int i = 0; i < nsock; i++)
		if (!socketpair(AF_UNIX, SOCK_STREAM, 0, sock[i]))
			n++;
	printf("socks=%d\n", n);

	{
		char pat[8192];
		memset(pat, 0x43, sizeof(pat));
		wf(TRACE "/tracing_on", "1\n");
		for (int i = 0; i < n; i++)
			send(sock[i][0], pat, sizeof(pat), MSG_DONTWAIT);
		wf(TRACE "/tracing_on", "0\n");
	}

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
					if (pp && np < 4096)
						pfns[np++] = strtoull(pp + 6, NULL, 16);
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
			if (req != 8192)
				continue;
			if (nv < 4096)
				vas[nv++] = ptr;
		}
	}

	printf("order1_pfns=%d kmalloc8k=%d\n", np, nv);
	{
		int lim = np < nv ? np : nv;
		if (lim > 12)
			lim = 12;
		for (int i = 0; i < lim; i++) {
			int64_t delta = (int64_t)(vas[i] & ~UINT64_C(0x1fff)) -
					(int64_t)(pfns[i] << 12);
			printf("pair[%d] va=%#llx pfn=%#llx delta=%#llx (%lld MiB)\n", i,
			       (unsigned long long)vas[i], (unsigned long long)pfns[i],
			       (unsigned long long)delta, (long long)(delta >> 20));
		}
	}
	fflush(stdout);
	return 0;
}
