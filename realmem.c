// realmem — rank processes by phys_footprint (the metric Activity Monitor shows).
//
// Unlike RSS, phys_footprint excludes clean file-backed pages (the OS disk
// cache), counts compressed pages at compressed size, and includes IOKit/GPU
// allocations.
//
// Uses proc_pid_rusage(), which reads same-uid processes without root.
// The Mach equivalent, task_info(TASK_VM_INFO), needs task_for_pid() and
// therefore root or the com.apple.system-task-ports entitlement.
//
// cc -O2 -Wall -o realmem realmem.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <regex.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <libproc.h>
#include <sys/sysctl.h>
#include <mach/mach.h>

#define DEFAULT_ROWS 20

typedef struct {
	pid_t    pid;
	uint64_t footprint;   // bytes
	uint64_t lifetime_max;
	char     name[2 * MAXCOMLEN + 1];
	char    *cmdline;     // full argv, or NULL if unreadable
} entry_t;

static int by_footprint(const void *a, const void *b)
{
	uint64_t x = ((const entry_t *)a)->footprint;
	uint64_t y = ((const entry_t *)b)->footprint;
	return (y > x) - (y < x);
}

static void humanize(uint64_t bytes, char *out, size_t n)
{
	const char *unit[] = { "B", "K", "M", "G", "T" };
	double v = (double)bytes;
	int i = 0;

	while (v >= 1024.0 && i < 4) {
		v /= 1024.0;
		i++;
	}
	snprintf(out, n, "%.1f%s", v, unit[i]);
}

// phys_footprint lives at the same offset in every rusage_info version >= v0,
// but ask for the newest the kernel will give us and fall back on EINVAL.
static int footprint_for(pid_t pid, uint64_t *out, uint64_t *peak)
{
	rusage_info_current ri;
	int flavor;

	for (flavor = RUSAGE_INFO_CURRENT; flavor >= RUSAGE_INFO_V0; flavor--) {
		if (proc_pid_rusage(pid, flavor, (rusage_info_t *)&ri) == 0) {
			*out  = ri.ri_phys_footprint;
			*peak = (flavor >= RUSAGE_INFO_V4) ? ri.ri_lifetime_max_phys_footprint : 0;
			return 0;
		}
		if (errno != EINVAL)
			return -1;   // EPERM/ESRCH: not ours, or it exited
	}
	return -1;
}

// Full argv via KERN_PROCARGS2. Several processes can share one executable
// path (five trainers, mkdocs, and a server all run the same python3), so the
// arguments are what actually tells them apart. Same-uid only, no root needed.
// Layout: [int argc][exec path][NUL padding][argv[0..argc-1]].
static char *cmdline_for(pid_t pid)
{
	static char *buf = NULL;
	static size_t bufsz = 0;
	int mib[3] = { CTL_KERN, KERN_PROCARGS2, pid };
	size_t len;
	int argc;
	char *p, *end, *out;
	size_t outlen = 0;

	if (buf == NULL) {
		int argmax = 0;
		size_t sz = sizeof argmax;
		int amib[2] = { CTL_KERN, KERN_ARGMAX };

		if (sysctl(amib, 2, &argmax, &sz, NULL, 0) != 0 || argmax <= 0)
			argmax = 262144;
		buf = malloc(argmax);
		if (buf == NULL)
			return NULL;
		bufsz = argmax;
	}

	len = bufsz;
	if (sysctl(mib, 3, buf, &len, NULL, 0) != 0)
		return NULL;
	if (len < sizeof(int))
		return NULL;

	memcpy(&argc, buf, sizeof(int));
	p = buf + sizeof(int);
	end = buf + len;

	while (p < end && *p != '\0')   // skip the exec path
		p++;
	while (p < end && *p == '\0')   // and its alignment padding
		p++;

	out = malloc(len);
	if (out == NULL)
		return NULL;
	out[0] = '\0';

	for (; argc > 0 && p < end; argc--) {
		size_t n = strnlen(p, (size_t)(end - p));

		if (outlen + n + 2 > (size_t)len)
			break;
		if (outlen > 0)
			out[outlen++] = ' ';
		memcpy(out + outlen, p, n);
		outlen += n;
		out[outlen] = '\0';
		p += n + 1;
	}

	if (outlen == 0) {
		free(out);
		return NULL;
	}
	return out;
}

static void print_system_totals(void)
{
	vm_size_t page = 0;
	vm_statistics64_data_t vm;
	mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
	mach_port_t host = mach_host_self();
	uint64_t memsize = 0;
	size_t len = sizeof(memsize);
	char b1[16], b2[16], b3[16], b4[16];

	if (host_page_size(host, &page) != KERN_SUCCESS)
		return;
	if (host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&vm, &count) != KERN_SUCCESS)
		return;
	sysctlbyname("hw.memsize", &memsize, &len, NULL, 0);

	// file-backed pages are the disk cache: counted in "used" but reclaimable.
	humanize(memsize, b1, sizeof b1);
	humanize((uint64_t)vm.external_page_count * page, b2, sizeof b2);
	humanize((uint64_t)vm.internal_page_count * page, b3, sizeof b3);
	humanize((uint64_t)vm.compressor_page_count * page, b4, sizeof b4);

	printf("\nsystem: %s total | %s file cache (reclaimable) | "
	       "%s anonymous | %s compressor\n", b1, b2, b3, b4);
}

// Command lines can run to hundreds of characters (a shell's -c payload, for
// one), which wraps into an unreadable block. Clip to the terminal.
static int term_width(void)
{
	struct winsize ws;

	if (isatty(STDOUT_FILENO) &&
	    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 40)
		return ws.ws_col;
	return 160;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: realmem [options] [regex | count]\n"
		"\n"
		"Rank processes by phys_footprint, the metric Activity Monitor\n"
		"reports. Unlike RSS it ignores the reclaimable file cache.\n"
		"\n"
		"  -n, --count N   maximum rows to print (default %d, 0 for all)\n"
		"  -m, --match RE  keep only processes matching extended regex RE\n"
		"      --sum       print just the total for matches, no rows\n"
		"  -c, --case      match case-sensitively (default: insensitive)\n"
		"  -l, --long      show the full command line instead of the name\n"
		"  -h, --help      this message\n"
		"\n"
		"A bare argument is a count if it is all digits, otherwise a regex.\n"
		"The regex matches the full command line where readable.\n"
		"\n"
		"  realmem                 top %d processes\n"
		"  realmem 40              top 40\n"
		"  realmem python          just the python processes\n"
		"  realmem python --sum    what python costs in total\n"
		"  realmem 'train|serve'   alternation works\n",
		DEFAULT_ROWS, DEFAULT_ROWS);
}

int main(int argc, char **argv)
{
	int limit = DEFAULT_ROWS, sum_only = 0, icase = 1, longfmt = 0;
	const char *pattern = NULL;
	regex_t re;
	int have_re = 0, nbytes, npids, i, n = 0, matched = 0;
	pid_t *pids;
	entry_t *list;
	uint64_t total = 0, match_total = 0, largest = 0;
	pid_t largest_pid = 0, self = getpid();
	int width = term_width();

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage();
			return 0;
		} else if (!strcmp(a, "--sum")) {
			sum_only = 1;
		} else if (!strcmp(a, "-c") || !strcmp(a, "--case")) {
			icase = 0;
		} else if (!strcmp(a, "-l") || !strcmp(a, "--long")) {
			longfmt = 1;
		} else if (!strcmp(a, "-n") || !strcmp(a, "--count")) {
			if (++i >= argc) {
				fprintf(stderr, "realmem: %s needs a value\n", a);
				return 2;
			}
			limit = atoi(argv[i]);
		} else if (!strcmp(a, "-m") || !strcmp(a, "--match")) {
			if (++i >= argc) {
				fprintf(stderr, "realmem: %s needs a value\n", a);
				return 2;
			}
			pattern = argv[i];
		} else if (a[0] == '-' && a[1] != '\0') {
			fprintf(stderr, "realmem: unknown option %s\n", a);
			usage();
			return 2;
		} else {
			// bare argument: all digits means a row count, else a regex
			if (a[strspn(a, "0123456789")] == '\0' && a[0] != '\0')
				limit = atoi(a);
			else
				pattern = a;
		}
	}

	if (pattern != NULL) {
		int flags = REG_EXTENDED | REG_NOSUB | (icase ? REG_ICASE : 0);
		int rc = regcomp(&re, pattern, flags);

		if (rc != 0) {
			char err[256];

			regerror(rc, &re, err, sizeof err);
			fprintf(stderr, "realmem: bad regex '%s': %s\n", pattern, err);
			return 2;
		}
		have_re = 1;
	} else if (sum_only) {
		fprintf(stderr, "realmem: --sum needs a regex to sum over\n");
		return 2;
	}

	nbytes = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
	if (nbytes <= 0) {
		perror("proc_listpids");
		return 1;
	}
	nbytes *= 2;   // headroom: processes can spawn between the two calls

	pids = calloc(1, nbytes);
	list = calloc(nbytes / sizeof(pid_t), sizeof(entry_t));
	if (!pids || !list) {
		perror("calloc");
		return 1;
	}

	nbytes = proc_listpids(PROC_ALL_PIDS, 0, pids, nbytes);
	if (nbytes <= 0) {
		perror("proc_listpids");
		return 1;
	}
	npids = nbytes / sizeof(pid_t);

	for (i = 0; i < npids; i++) {
		uint64_t fp, peak;
		entry_t *e;

		if (pids[i] == 0)
			continue;
		// Our own argv contains the pattern, so we would match every
		// search we run. Always a false positive; never interesting.
		if (have_re && pids[i] == self)
			continue;
		if (footprint_for(pids[i], &fp, &peak) != 0)
			continue;   // silently skip processes we can't read

		e = &list[n];
		e->pid = pids[i];
		e->footprint = fp;
		e->lifetime_max = peak;
		if (proc_name(pids[i], e->name, sizeof e->name) <= 0)
			snprintf(e->name, sizeof e->name, "<pid %d>", pids[i]);
		total += fp;

		// Only pay for argv when a pattern or the long format needs it.
		if (have_re || longfmt)
			e->cmdline = cmdline_for(pids[i]);

		if (have_re) {
			const char *hay = e->cmdline ? e->cmdline : e->name;

			if (regexec(&re, hay, 0, NULL, 0) != 0) {
				free(e->cmdline);
				e->cmdline = NULL;
				continue;   // drop non-matching entries entirely
			}
			matched++;
			match_total += fp;
			if (fp > largest) {
				largest = fp;
				largest_pid = e->pid;
			}
		}
		n++;
	}

	qsort(list, n, sizeof(entry_t), by_footprint);

	if (!sum_only) {
		printf("%7s  %10s  %10s  %s\n", "PID", "FOOTPRINT", "PEAK", "COMMAND");
		for (i = 0; i < n && (limit <= 0 || i < limit); i++) {
			char fp[16], pk[16];
			const char *label = list[i].name;

			if (longfmt && list[i].cmdline)
				label = list[i].cmdline;

			humanize(list[i].footprint, fp, sizeof fp);
			humanize(list[i].lifetime_max, pk, sizeof pk);
			printf("%7d  %10s  %10s  %.*s\n", list[i].pid, fp,
			       list[i].lifetime_max ? pk : "-",
			       width - 33, label);
		}
	}

	if (have_re) {
		char t[16], lg[16];

		humanize(match_total, t, sizeof t);
		humanize(largest, lg, sizeof lg);
		if (matched == 0) {
			printf("\nno processes match /%s/\n", pattern);
		} else {
			printf("\n%d process%s matching /%s/, %s total",
			       matched, matched == 1 ? "" : "es", pattern, t);
			if (matched > 1)
				printf(" (largest %s, pid %d)", lg, largest_pid);
			putchar('\n');
		}
	} else {
		char t[16];

		humanize(total, t, sizeof t);
		printf("\n%d of %d processes readable, %s total footprint\n",
		       n, npids, t);
	}

	if (!sum_only)
		print_system_totals();

	for (i = 0; i < n; i++)
		free(list[i].cmdline);
	if (have_re)
		regfree(&re);
	free(pids);
	free(list);
	return 0;
}
