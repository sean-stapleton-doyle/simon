# realmem

Rank macOS processes by physical footprint instead of resident set size (RSS).

`ps`, `htop`, and `btop` report RSS, which is a poor measure of what a process
actually costs. RSS double-counts shared framework pages across processes,
omits IOKit and GPU allocations, and counts compressed pages at their
uncompressed size. The errors run in both directions:

| Process | RSS (`ps`) | Footprint | |
| --- | --- | --- | --- |
| WindowServer | 172 MB | 867 MB | RSS misses IOKit and GPU surfaces |
| Finder | 106 MB | 456 MB | 387 MB was sitting compressed |
| kernel_task | 0 MB | 395 MB | invisible to RSS entirely |
| claude | 727 MB | 416 MB | RSS overcounts shared frameworks |

`realmem` reports `phys_footprint`, the same metric the **Memory** column of
Activity Monitor shows and the one the kernel uses for memory limits. It
excludes clean file-backed pages, so the operating system's disk cache doesn't
inflate the numbers.

## Build and install

To build `realmem` and put it on your `PATH`, run:

```sh
make install
```

`make install` symlinks `~/.local/bin/realmem` to the binary in `build/`, so a
later `make` refreshes the command without reinstalling. To install elsewhere,
set `PREFIX`:

```sh
make install PREFIX=/usr/local
```

## Usage

Print the 20 largest processes:

```sh
realmem
```

Pass a count to change how many rows appear:

```sh
realmem 40
```

Output includes each process's current footprint, its lifetime peak, and a
system summary separating reclaimable file cache from anonymous memory:

```
    PID   FOOTPRINT        PEAK  COMMAND
    473      721.3M      863.8M  ghostty
  95924      640.4M      958.9M  Preview
    555      456.7M      523.1M  Finder

562 of 853 processes readable, 25.0G total footprint

system: 48.0G total | 15.9G file cache (reclaimable) | 22.9G anonymous | 4.0G compressor
```

The `file cache` figure is worth reading before you conclude that memory is
short. It counts toward "used" in most tools but the kernel reclaims it on
demand.

## Permissions

`realmem` reads processes that share your user ID. System processes owned by
other users, such as `WindowServer` under `_windowserver`, return `EPERM` and
are skipped. The footer reports how many processes were readable.

To cover every process, run with `sudo`:

```sh
sudo realmem
```

## How it works

`realmem` calls `proc_pid_rusage()` from `<libproc.h>` and reads
`ri_phys_footprint`. That call works on same-user processes without elevated
privileges.

The Mach equivalent, `task_info()` with the `TASK_VM_INFO` flavor, exposes the
same value as `task_vm_info_data_t.phys_footprint`, along with ledger peaks and
compression counters. It needs a task port, and `task_for_pid()` requires root
or the `com.apple.system-task-ports` entitlement for any process other than the
caller. Apple's own `footprint -a` refuses to run unprivileged for this reason.

Note that `TASK_VM_INFO_REV0_COUNT` predates the field. Version the `count`
argument if you target older kernels.

System totals come from `host_statistics64()` with `HOST_VM_INFO64`, which
separates `external_page_count` (file-backed and reclaimable) from
`internal_page_count` (anonymous) and `compressor_page_count`.

Foundation exposes no equivalent API. `NSProcessInfo.physicalMemory` reports
only total installed RAM, and `os_proc_available_memory()` is iOS-only.

## Related tools

- `top -o mem` sorts by the same metric, no compilation required. Add
  `-stats pid,command,mem,cmprs,purg` to break out compressed pages.
- `footprint -p <pid>` gives a per-category breakdown for one process, and
  `sudo footprint -a` covers the whole system.
- `vmmap --summary <pid>` splits one process into dirty, clean, and swapped
  regions.
- `memory_pressure` reports whether memory is actually short.
- `realmem.sh` in this repository groups helper processes by application. It
  wraps `top`, so it needs no compiler.
