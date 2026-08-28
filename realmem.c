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
#include <libproc.h>
#include <sys/sysctl.h>
#include <mach/mach.h>

typedef struct {
	pid_t    pid;
	uint64_t footprint;   // bytes
	uint64_t lifetime_max;
	char     name[2 * MAXCOMLEN + 1];
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

int main(int argc, char **argv)
{
	int limit = (argc > 1) ? atoi(argv[1]) : 20;
	int nbytes, npids, i, n = 0;
	pid_t *pids;
	entry_t *list;
	uint64_t total = 0;

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

		if (pids[i] == 0)
			continue;
		if (footprint_for(pids[i], &fp, &peak) != 0)
			continue;   // silently skip processes we can't read

		list[n].pid = pids[i];
		list[n].footprint = fp;
		list[n].lifetime_max = peak;
		if (proc_name(pids[i], list[n].name, sizeof list[n].name) <= 0)
			snprintf(list[n].name, sizeof list[n].name, "<pid %d>", pids[i]);
		total += fp;
		n++;
	}

	qsort(list, n, sizeof(entry_t), by_footprint);

	printf("%7s  %10s  %10s  %s\n", "PID", "FOOTPRINT", "PEAK", "COMMAND");
	for (i = 0; i < n && i < limit; i++) {
		char fp[16], pk[16];

		humanize(list[i].footprint, fp, sizeof fp);
		humanize(list[i].lifetime_max, pk, sizeof pk);
		printf("%7d  %10s  %10s  %s\n",
		       list[i].pid, fp, list[i].lifetime_max ? pk : "-", list[i].name);
	}

	{
		char t[16];
		humanize(total, t, sizeof t);
		printf("\n%d of %d processes readable, %s total footprint\n", n, npids, t);
	}
	print_system_totals();

	free(pids);
	free(list);
	return 0;
}
