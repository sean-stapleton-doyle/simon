#pragma once

#include "gpu.hpp"

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace realmem {

struct ProcessInfo {
  pid_t pid = 0;
  std::uint64_t footprint = 0;                   // bytes (ri_phys_footprint)
  std::uint64_t lifetime_max = 0;                // bytes (ri_lifetime_max_phys_footprint)
  std::optional<std::uint64_t> gpu_footprint;    // bytes (TASK_VM_INFO graphics ledger)
  std::string name;                              // proc_name() or "<pid N>"
  std::string path;                              // proc_pidpath(), empty if not requested
  std::string cmdline;                           // sanitized full argv, empty if not requested
};

struct Snapshot {
  std::vector<ProcessInfo> processes;
  std::size_t total_pids = 0;
  std::uint64_t total_footprint = 0;
};

struct SystemMemory {
  std::uint64_t total = 0;
  std::uint64_t wired = 0;
  std::uint64_t anonymous = 0;   // internal_page_count * page
  std::uint64_t compressor = 0;  // compressor_page_count * page
  std::uint64_t file_cache = 0;  // external_page_count * page (reclaimable)
  std::uint64_t free_bytes = 0;  // free_count * page
  SystemGpuMemory gpu;
  bool valid = false;
};

// Replaces ASCII control characters (newlines, tabs, etc.) with spaces so
// command lines never corrupt single-line table rows.
std::string sanitize_cmdline(std::string_view raw);

// Enumerates all processes accessible to the current UID via proc_pid_rusage().
Snapshot collect_processes(bool need_cmdline,
                           bool need_path,
                           bool need_gpu = false,
                           std::optional<pid_t> exclude_pid = std::nullopt);

// Collects host-wide VM and IOKit GPU memory statistics.
SystemMemory collect_system_memory();

bool stdout_is_tty();
int terminal_width(int fallback = 160);

}  // namespace realmem
