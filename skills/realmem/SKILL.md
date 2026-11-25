---
name: realmem
description: Measure true macOS process and GPU memory usage using `realmem` (`phys_footprint` instead of RSS). Use when diagnosing high RAM usage, memory leaks, Metal/GPU allocations, or comparing memory costs across apps and processes on macOS.
---

# realmem — Accurate macOS Memory Inspection

Use `realmem` instead of `ps`, `top`, or `htop` when investigating memory on macOS. Those tools report RSS, which double-counts shared frameworks, misses IOKit/GPU allocations, and distorts compressed memory. `realmem` reports `phys_footprint` (the Activity Monitor **Memory** metric), plus IOKit system GPU memory and optional per-process GPU ledgers.

## Preferred Commands for Agents

Always pass `--output json` (or `-o json`) so output is machine-parseable with exact byte counts (`*_bytes`):

```sh
# Top 20 processes + host memory & GPU breakdown
realmem -o json

# Top N processes (0 for all readable)
realmem -o json -n 30

# Roll up helpers/renderers by macOS .app bundle
realmem -g -o json

# Filter by extended regex over full command lines (case-insensitive by default; add -c for case-sensitive)
realmem 'python|node' -l -o json

# Compact total for matching processes only (lowest token usage)
realmem python --sum -o json

# Sort by peak, name, pid, count (with -g), or gpu (with --gpu)
realmem -s peak -o json -n 15

# Include per-process GPU memory column (requires sudo; shows null where denied)
sudo realmem --gpu -s gpu -o json -n 15
```

## JSON Output Schema

```json
{
  "mode": "processes",
  "summary": {
    "readable_processes": 812,
    "total_processes": 1140,
    "total_footprint_bytes": 43800000000
  },
  "rows": [
    {
      "pid": 1234,
      "name": "python3.12",
      "footprint_bytes": 2147483648,
      "peak_bytes": 3221225472,
      "cmdline": "python3 train.py"
    }
  ],
  "system": {
    "total_bytes": 51539607552,
    "wired_bytes": 4100000000,
    "anonymous_bytes": 29500000000,
    "compressor_bytes": 3800000000,
    "file_cache_bytes": 12400000000,
    "free_bytes": 1700000000,
    "gpu": {
      "allocated_bytes": 25769803776,
      "in_use_bytes": 410000000
    }
  }
}
```

- Under `-g` / `--group`: `mode` is `"grouped"`, `summary.total_groups` is included, and each row has `name` and `process_count` instead of `pid` / `cmdline`.
- Under regex filtering: `summary` adds `pattern`, `matched_processes`, `matched_footprint_bytes`, `largest_pid`, and `largest_footprint_bytes`.
- Under `--sum`: `rows` and `system` are omitted for minimal output.
- Under `--gpu`: each row includes `gpu_bytes` (integer bytes or `null` when unreadable), and `summary.gpu_readable_processes` reports how many processes exposed graphics ledgers.

## Interpretation Rules

- **Reclaimable disk cache**: `system.file_cache_bytes` is reclaimable on demand by macOS—do not treat it as memory pressure or a leak.
- **Unprivileged scope**: Without `sudo`, `realmem` reads same-UID processes only (`summary.readable_processes` vs `summary.total_processes`). Root/system daemons like `WindowServer` require `sudo`.
- **GPU attribution**: `system.gpu` (`allocated_bytes`, `in_use_bytes`) works without `sudo`. Per-process `gpu_bytes` (`--gpu`) uses `task_for_pid()` and requires `sudo`; SIP still blocks Apple-signed system binaries (reported as `null`).
- **Config independence**: Pass `--no-config` in scripts/automation if you want to ignore `~/.config/realmem/config.toml` or `~/.realmem.toml`.
