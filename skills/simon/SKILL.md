---
name: simon
description: Measure true macOS process, GPU, swap, Apple Silicon CPU tier, and disk usage using `simon` (`phys_footprint` instead of RSS). Use when diagnosing high RAM usage, swap thrashing, memory leaks, Metal/GPU headroom, or comparing memory costs across apps, process trees, and ML/LLM runs on macOS.
---

# simon — Silicon Monitor (Accurate macOS Memory & Apple Silicon ML Inspection)

Use `simon` instead of `ps`, `top`, or `htop` when investigating memory or ML/LLM workloads on macOS. Those tools report RSS (which double-counts shared frameworks, misses IOKit/GPU allocations, and distorts compressed memory) and obscure Apple Silicon core tiers, swap pressure, and Metal GPU limits.

## Preferred Commands for Agents

Always pass `--output json` (or `-o json`) so output is machine-parseable with exact byte counts (`*_bytes`):

```sh
# Top 20 processes + host memory, compression ratio, swap, pressure, and GPU headroom
simon -o json

# Roll up helpers/renderers by macOS .app bundle
simon -g -o json

# Parent/child process tree with rolled-up subtree footprints (ideal for uv/python + DataLoader workers)
simon -t -o json

# Include opt-in Apple Silicon CPU tiers (Super/Performance/Efficiency), power watts, and disk I/O
simon --cpu --disk -o json

# Filter by extended regex over full command lines (add -c for case-sensitive)
simon 'python|llama|mlx' -l -o json

# Compact total for matching processes only (lowest token usage)
simon python --sum -o json

# Sort by peak, gpu, cpu, disk, name, pid, or count (with -g)
simon -s peak -o json -n 15

# Include per-process GPU memory column (requires sudo; shows null where denied)
sudo simon --gpu -s gpu -o json -n 15
```

*(Note: `-w` / `--watch` launches an interactive read-only FTXUI TUI for humans—never pass `-w` in non-interactive agent shells.)*

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
    "compression_ratio": 5.8,
    "file_cache_bytes": 12400000000,
    "free_bytes": 1700000000,
    "swap": { "total_bytes": 9663676416, "used_bytes": 8700000000, "free_bytes": 960000000, "encrypted": true },
    "pressure": "normal",
    "memory_free_pct": 82,
    "gpu": {
      "model": "Apple M5 Pro",
      "core_count": 16,
      "allocated_bytes": 30900000000,
      "in_use_bytes": 380000000,
      "ceiling_bytes": 40158000000,
      "headroom_bytes": 9258000000,
      "device_util_pct": 6.0,
      "power_watts": 0.1
    }
  }
}
```

- Under `-g`: `mode` is `"grouped"`, `summary.total_groups` is set, rows have `name` + `process_count`.
- Under `-t`: `mode` is `"tree"`, rows include `ppid`, `depth`, `subtree_processes`, `subtree_footprint_bytes` (rolled-up total) alongside own `footprint_bytes`.
- Under `--cpu` / `--disk`: `system.cpu` (`total_util_pct`, `tiers[]`, `power_watts`) and `system.disk` (`read_bps`, `write_bps`) plus per-row `cpu_pct`, `p_cpu_pct`, `disk_read_bytes`, `disk_write_bytes`, `disk_read_bps`, `disk_write_bps` are included.
- Under `--sum`: `rows` and `system` are omitted for minimal output.
- Under `--gpu`: each row includes `gpu_bytes` (integer bytes or `null` when unreadable).

## Interpretation Rules

- **Reclaimable disk cache**: `system.file_cache_bytes` is reclaimable on demand—do not treat it as memory pressure.
- **Top-end ML/LLM headroom**: Check `system.gpu.headroom_bytes` (`ceiling_bytes - allocated_bytes`), `system.swap.used_bytes`, and `system.pressure` (`normal` / `warn` / `critical`) to judge whether a model or batch size is close to OOM or thrashing.
- **Unprivileged scope**: Everything except per-process `gpu_bytes` and other-UID processes works without `sudo`. Pass `--no-config` in scripts to ignore user TOML defaults.
