<div align="center">

# realmem

[![CI](https://github.com/sean-stapleton-doyle/realmem/actions/workflows/ci.yml/badge.svg)](https://github.com/sean-stapleton-doyle/realmem/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/sean-stapleton-doyle/realmem)](https://github.com/sean-stapleton-doyle/realmem/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Rank macOS processes by physical memory footprint (`phys_footprint`) instead of resident set size (RSS).

</div>

## Why

`ps`, `htop`, and `btop` report RSS, which is misleading on macOS: it double-counts shared framework pages, misses IOKit/GPU allocations, and counts compressed pages at their uncompressed size.

| Process | RSS (`ps`) | Footprint | Why RSS is wrong |
| --- | --- | --- | --- |
| WindowServer | 172 MB | 867 MB | Misses IOKit and GPU surfaces |
| Finder | 106 MB | 456 MB | Compressed pages hidden |
| kernel_task | 0 MB | 395 MB | Invisible to RSS |
| claude | 727 MB | 416 MB | Overcounts shared frameworks |

`realmem` reports `phys_footprint`—the same metric shown in Activity Monitor's **Memory** column—along with lifetime peak, app-level grouping, and a system summary separating wired, anonymous, compressed, and reclaimable file-cache memory.

## Install

### Prebuilt binary (Apple Silicon / arm64)

```sh
curl -fsSL https://github.com/sean-stapleton-doyle/realmem/releases/latest/download/realmem-macos-arm64.tar.gz \
  | tar -xz realmem
xattr -d com.apple.quarantine realmem 2>/dev/null || true
mv realmem ~/.local/bin/realmem
```

### Build from source

Requires CMake 3.20+, Ninja, and Xcode Command Line Tools (C++20). Dependencies (`FTXUI`, `CLI11`, `toml++`) are fetched automatically via `FetchContent`.

```sh
make install          # builds and symlinks ~/.local/bin/realmem -> build/realmem
make test             # runs the unit test suite
```

## Usage

```sh
realmem               # top 20 processes (styled in TTY, plain when piped)
realmem 40            # top 40 processes
realmem -g            # group helper/renderer processes by application
realmem -s peak -l    # sort by lifetime peak with full command lines
sudo realmem --gpu    # add per-process GPU memory column (sort with -s gpu)
realmem python        # filter by extended regex over full command lines
realmem python --sum  # aggregate total for matching processes
sudo realmem          # include other users' processes (e.g. WindowServer)
```

Example output (`realmem -g 5`):

```text
  PROCS   FOOTPRINT    %RAM  USAGE             PEAK  APPLICATION
────────────────────────────────────────────────────────────────────────────────
     35       11.5G   24.0%  ██████████       19.8G  WebKit
     23        3.3G    6.9%  ██▊               7.5G  Chrome
     27        2.9G    6.1%  ██▍               7.1G  Visual Studio Code

858 of 1189 processes readable across 453 apps, 42.6G total footprint

system: 48.0G total | 3.9G wired | 25.8G anonymous | 4.6G compressor | 10.6G file cache (reclaimable)
gpu: 2.4G alloc (512.0M in use)
```

Key flags:

| Flag | Description |
| --- | --- |
| `-n, --count N` | Max rows to print (default `20`, `0` for all) |
| `-m, --match RE` | Filter by extended regex (or pass a bare non-numeric arg) |
| `-s, --sort FIELD` | Sort by `footprint`, `peak`, `gpu`, `pid`, `name`, or `count` (with `-g`) |
| `-r, --reverse` | Reverse sort order |
| `-g, --group` | Aggregate helpers by `.app` bundle / process name |
| `--gpu` / `--no-gpu` | Show per-process GPU memory column (requires `sudo`) |
| `-l, --long` | Show full command line column |
| `-c, --case` | Case-sensitive regex matching |
| `--sum` | Print only the aggregate total for matches |
| `--bars` / `--no-bars` | Toggle inline usage bars |
| `--color MODE` | `auto`, `always`, or `never` (`--plain` disables FTXUI styling) |
| `--config PATH` / `--no-config` | Override or disable TOML config |

## Configuration

`realmem` loads defaults from the first existing file in:

1. `$XDG_CONFIG_HOME/realmem/config.toml`
2. `~/.config/realmem/config.toml`
3. `~/.realmem.toml`

CLI flags always override config values.

```toml
count = 25
sort = "footprint"     # footprint | peak | gpu | pid | name | count
reverse = false
group = false
gpu = false
long = false
case_sensitive = false
bars = true
system = true
color = "auto"         # auto | always | never
plain = false

# Optional custom regex rules evaluated first under -g/--group
[[grouping.rules]]
pattern = "python3.*train"
name = "ML Trainer"
```

## Technical notes

- **Why `phys_footprint`**: It is the kernel's ledger metric for true memory pressure—excluding clean reclaimable file-backed pages (the OS disk cache), accounting for compressed pages at compressed size, and including IOKit/GPU allocations.
- **Why `proc_pid_rusage()`**: Reading `ri_phys_footprint` and `ri_lifetime_max_phys_footprint` via `proc_pid_rusage()` (`<libproc.h>`) works for all same-UID processes without `sudo`. The Mach alternative (`task_info(TASK_VM_INFO)`) requires `task_for_pid()`, which needs root or `com.apple.system-task-ports`.
- **Command lines & grouping**: Full `argv` is read via `sysctl(CTL_KERN, KERN_PROCARGS2)` (also unprivileged for same-UID processes), and `-g` resolves the outermost `.app` bundle via `proc_pidpath()` so browser/Electron helpers collapse under their parent app.
- **System & GPU memory**: Host totals come from `host_statistics64(HOST_VM_INFO64)`, separating `wire_count`, `internal_page_count` (anonymous), `compressor_page_count`, and `external_page_count` (reclaimable file cache). System-wide GPU memory (`Alloc system memory` and `In use system memory`) is read unprivileged from IOKit `IOAccelerator` `PerformanceStatistics`; per-process `--gpu` reads `ledger_tag_graphics_footprint` (+ compressed) via `task_for_pid()` + `task_info(TASK_VM_INFO)`, which requires `sudo` (and is blocked by SIP on Apple-signed system binaries).
