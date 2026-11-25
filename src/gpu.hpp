#pragma once

#include <sys/types.h>

#include <cstdint>
#include <optional>

namespace realmem {

struct SystemGpuMemory {
  std::uint64_t allocated = 0;  // IOKit "Alloc system memory" (bytes)
  std::uint64_t in_use = 0;     // IOKit "In use system memory" (bytes)
  bool valid = false;
};

// Combines graphics ledger counters into a non-negative byte count.
std::uint64_t combine_graphics_ledger(std::int64_t footprint, std::int64_t compressed);

// Queries system-wide GPU memory from IOKit IOAccelerator PerformanceStatistics.
// Works without root on Apple Silicon and Intel Macs.
SystemGpuMemory collect_system_gpu_memory();

// Queries per-process GPU/graphics footprint via task_for_pid() + task_info(TASK_VM_INFO).
// Returns std::nullopt when unprivileged or denied by SIP.
std::optional<std::uint64_t> collect_process_gpu_footprint(pid_t pid);

}  // namespace realmem
