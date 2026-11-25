<div align="center">

# simon

[![CI](https://github.com/sean-stapleton-doyle/simon/actions/workflows/ci.yml/badge.svg)](https://github.com/sean-stapleton-doyle/simon/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/sean-stapleton-doyle/simon)](https://github.com/sean-stapleton-doyle/simon/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

**Si**licon **Mon**itor — rank macOS processes by physical memory footprint (`phys_footprint`) instead of RSS, with Apple Silicon GPU headroom, swap/pressure, process trees, and a read-only live TUI.

</div>

## Why

`ps`, `htop`, and `btop` report RSS on macOS—double-counting shared frameworks, hiding IOKit/GPU allocations, and distorting compressed memory—while missing Apple Silicon's Metal wired memory ceiling and CPU performance tiers.

| Process | RSS (`ps`) | Footprint | Why RSS is wrong |
| --- | --- | --- | --- |
| WindowServer | 172 MB | 867 MB | Misses IOKit and GPU surfaces |
| Finder | 106 MB | 456 MB | Compressed pages hidden |
| kernel_task | 0 MB | 395 MB | Invisible to RSS |
| claude | 727 MB | 416 MB | Overcounts shared frameworks |

`simon` reports `phys_footprint` (Activity Monitor's **Memory** column), lifetime peaks, `.app` grouping, parent/child process tree rollups, swap & memory pressure, Metal GPU headroom, opt-in Apple Silicon CPU tiers / disk I/O, JSON output, and an optional live monitor (`-w`).

## Install

### Prebuilt binary (Apple Silicon / arm64)

```sh
curl -fsSL https://github.com/sean-stapleton-doyle/simon/releases/latest/download/simon-macos-arm64.tar.gz \
  | tar -xz simon
xattr -d com.apple.quarantine simon 2>/dev/null || true
mv simon ~/.local/bin/simon
```

### Build from source

Requires CMake 3.20+, Ninja, and Xcode Command Line Tools (C++20 / ObjC++20). Dependencies (`FTXUI`, `CLI11`, `toml++`, `nlohmann/json`) are fetched automatically via `FetchContent`.

```sh
make install          # builds and symlinks ~/.local/bin/simon -> build/simon
make test             # runs the unit test suite
```

## Usage

```sh
simon                 # top 20 processes (styled in TTY, plain when piped)
simon -S              # short summary only (RAM, swap, pressure, GPU headroom — no table)
simon -g              # group helper/renderer processes by .app bundle
simon -t            # parent/child process tree with rolled-up subtree footprints
simon -w            # read-only live TUI monitor (search /, toggle g/t/s/r/p, quit q)
simon --cpu --disk  # include Apple Silicon CPU tiers, watts, and disk I/O rates
simon -o json       # machine-readable JSON for scripts and LLMs
sudo simon --gpu    # add per-process GPU memory column (sort with -s gpu)
simon python --sum  # aggregate total for matching processes
```

Example output (`simon 5 --cpu --disk`):

```text
    PID   FOOTPRINT      CPU        DISK        PEAK  COMMAND
  38469        2.6G     0.1%      0.0B/s        2.6G  com.apple.WebKit.WebContent
  47550        2.1G     0.0%      0.0B/s        3.6G  rust-analyzer

789 of 1121 processes readable, 42.9G total footprint

system: 48.0G total | 3.7G wired | 26.0G anonymous | 4.6G compressor (6.3x) | 10.9G file cache (reclaimable)
swap: 8.1G / 9.0G used | pressure: normal (82% free)
gpu: 28.8G alloc (225.1M in use) | limit 37.4G (8.7G headroom) | 6% util (0.1W)
cpu: 3.1% | Super (5c): 2.3% | Performance (10c): 3.6% | 0.5W
disk: read 61.2K/s | write 0.0B/s
```

Key flags:

| Flag | Description |
| --- | --- |
| `-n, --count N` | Max rows to print (default `20`, `0` for all) |
| `-m, --match RE` | Filter by extended regex (or pass a bare non-numeric arg) |
| `-s, --sort FIELD` | `footprint`, `peak`, `gpu`, `cpu`, `disk`, `pid`, `name`, or `count` (with `-g`) |
| `-r, --reverse` | Reverse sort order |
| `-g, --group` | Aggregate helpers by `.app` bundle / process name |
| `-t, --tree` | Parent/child process tree with rolled-up subtree footprints |
| `-S, --short` | Show only host RAM, swap, pressure, and GPU stats (skips process scan) |
| `-w, --watch` | Read-only live TUI monitor (`--interval-ms N`, default `1000`) |
| `--cpu` / `--disk` | Opt-in Apple Silicon CPU tier & disk I/O telemetry (`--sample-ms N`, default `250`) |
| `--gpu` / `--no-gpu` | Per-process GPU memory column (requires `sudo`) |
| `-l, --long` | Show full command line column |
| `-c, --case` | Case-sensitive regex matching |
| `--sum` | Print only the aggregate total for matches |
| `-o, --output FMT` | `auto` (default), `table`, `plain`, or `json` |
| `--bars` / `--no-bars` | Toggle inline usage bars |
| `--color MODE` | `auto`, `always`, or `never` (`--plain` disables FTXUI styling) |
| `--skill` | Print the embedded agent `SKILL.md` document |
| `--config PATH` / `--no-config` | Override or disable TOML config |

### Agent skill

Install the bundled [SKILL.md](skills/simon/SKILL.md) so coding agents know how to query `simon -o json`:

```sh
mkdir -p ~/.agents/skills/simon && simon --skill > ~/.agents/skills/simon/SKILL.md
```

## Configuration

`simon` loads defaults from the first existing file in:

1. `$XDG_CONFIG_HOME/simon/config.toml`
2. `~/.config/simon/config.toml`
3. `~/.simon.toml`

CLI flags always override config values.

```toml
count = 25
sort = "footprint"     # footprint | peak | gpu | cpu | disk | pid | name | count
reverse = false
group = false
tree = false
gpu = false
cpu = false
disk = false
short = false
long = false
case_sensitive = false
interval_ms = 1000     # --watch refresh interval
sample_ms = 250        # one-shot delta window for --cpu/--disk
bars = true
system = true
color = "auto"         # auto | always | never
output = "auto"        # auto | table | plain | json
plain = false

# Optional custom regex rules evaluated first under -g/--group
[[grouping.rules]]
pattern = "python3.*train"
name = "ML Trainer"
```

## Technical notes

- **Why `phys_footprint`**: It is the kernel's ledger metric for true memory pressure—excluding clean reclaimable file-backed pages (disk cache), accounting for compressed pages at compressed size, and including IOKit/GPU allocations.
- **Process metrics (`proc_pid_rusage`)**: `rusage_info_v6` provides `ri_phys_footprint`, `ri_lifetime_max_phys_footprint`, `ri_user_time`/`ri_system_time`, P/Super-core time (`ri_user_ptime`), and per-process disk I/O (`ri_diskio_bytesread/written`) for all same-UID processes without `sudo`. Parent PIDs for `-t` come from `proc_pidinfo(PROC_PIDT_SHORTBSDINFO)`, full `argv` from `KERN_PROCARGS2`, and `.app` bundle grouping from `proc_pidpath()`.
- **System memory, swap & pressure**: `host_statistics64(HOST_VM_INFO64)` supplies wired, anonymous, compressor (with uncompressed ratio), and reclaimable file cache; `sysctl` adds `vm.swapusage`, `kern.memorystatus_vm_pressure_level`, and `kern.memorystatus_level`.
- **GPU headroom, Apple Silicon CPU tiers & `IOReport`**: Metal's `recommendedMaxWorkingSetSize` (+ `iogpu.wired_limit_mb`) gives the effective GPU memory ceiling; IOKit `IOAccelerator` gives GPU allocated/in-use bytes and utilization %; `IOBlockStorageDriver` gives system disk throughput; `hw.perflevel*` + `host_processor_info` discover Apple Silicon core tiers dynamically (`Super`/`Performance` on M5, `Performance`/`Efficiency` on M1–M4). Unprivileged `libIOReport` telemetry layers hardware CPU/GPU wattage and P-state residency on top with automatic fallback to public APIs. Per-process `--gpu` reads `ledger_tag_graphics_footprint` (+ compressed) via `task_for_pid()` + `task_info(TASK_VM_INFO)` (requires `sudo`; blocked by SIP on Apple-signed system binaries).
