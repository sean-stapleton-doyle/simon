#pragma once

#include <sys/types.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace realmem {

enum class MemoryPressureLevel {
  Unknown,
  Normal,
  Warn,
  Critical,
};

struct ProcessRecord {
  pid_t pid = 0;
  pid_t ppid = 0;
  std::uint64_t footprint = 0;       // ri_phys_footprint (bytes)
  std::uint64_t lifetime_max = 0;    // ri_lifetime_max_phys_footprint (bytes)
  std::uint64_t wired = 0;           // ri_wired_size (bytes)
  std::uint64_t neural_footprint = 0;
  std::uint64_t cpu_time_ns = 0;     // ri_user_time + ri_system_time (mach abs/ns)
  std::uint64_t p_cpu_time_ns = 0;   // ri_user_ptime + ri_system_ptime
  std::uint64_t disk_read_bytes = 0;
  std::uint64_t disk_write_bytes = 0;
  std::uint64_t pageins = 0;
  std::optional<std::uint64_t> gpu_footprint;  // TASK_VM_INFO graphics ledger (sudo)
  std::optional<double> cpu_pct;               // populated from delta when available
  std::optional<double> p_cpu_pct;             // top-tier core CPU %
  std::optional<double> disk_read_bps;         // bytes/sec from delta
  std::optional<double> disk_write_bps;        // bytes/sec from delta
  std::string name;
  std::string path;
  std::string cmdline;
};

struct ProcessSnapshot {
  std::vector<ProcessRecord> processes;
  std::size_t total_pids = 0;
  std::uint64_t total_footprint = 0;
};

struct SwapInfo {
  std::uint64_t total = 0;
  std::uint64_t used = 0;
  std::uint64_t free_bytes = 0;
  bool encrypted = false;
  bool valid = false;
};

struct GpuInfo {
  std::string model;
  std::uint32_t core_count = 0;
  std::uint64_t allocated = 0;                  // IOKit "Alloc system memory"
  std::uint64_t in_use = 0;                     // IOKit "In use system memory"
  std::uint64_t recommended_max_working_set = 0;  // Metal effective GPU memory ceiling
  std::uint64_t wired_limit_override = 0;       // sysctl iogpu.wired_limit_mb (0 = default)
  std::optional<double> device_util_pct;
  std::optional<double> renderer_util_pct;
  std::optional<double> tiler_util_pct;
  std::optional<double> freq_mhz;               // from IOReport when available
  std::optional<double> power_watts;            // from IOReport when available
  std::uint64_t recovery_count = 0;
  bool valid = false;

  std::uint64_t effective_ceiling() const {
    if (recommended_max_working_set > 0) {
      return recommended_max_working_set;
    }
    return wired_limit_override;
  }

  std::optional<std::uint64_t> headroom_bytes() const {
    const std::uint64_t ceiling = effective_ceiling();
    if (ceiling == 0 || !valid) {
      return std::nullopt;
    }
    return allocated < ceiling ? (ceiling - allocated) : 0ULL;
  }
};

struct CpuTierInfo {
  std::string name;  // e.g. "Super", "Performance", "Efficiency"
  std::uint32_t core_count = 0;
  double util_pct = 0.0;
  std::optional<double> freq_mhz;
};

struct CpuInfo {
  std::string brand;
  std::vector<CpuTierInfo> tiers;
  double total_util_pct = 0.0;
  std::optional<double> power_watts;
  bool valid = false;
};

struct DiskInfo {
  std::uint64_t total_read_bytes = 0;
  std::uint64_t total_write_bytes = 0;
  std::uint64_t total_read_ops = 0;
  std::uint64_t total_write_ops = 0;
  std::optional<double> read_bps;
  std::optional<double> write_bps;
  std::optional<double> read_iops;
  std::optional<double> write_iops;
  bool valid = false;
};

struct TelemetryInfo {
  std::optional<double> cpu_watts;
  std::optional<double> gpu_watts;
  std::optional<double> ane_watts;
  std::optional<double> dram_watts;
  std::optional<double> dram_bandwidth_gbps;
  bool from_ioreport = false;
};

struct SystemMemory {
  std::uint64_t total = 0;
  std::uint64_t wired = 0;
  std::uint64_t anonymous = 0;
  std::uint64_t compressor = 0;
  std::uint64_t compressor_uncompressed = 0;
  std::uint64_t file_cache = 0;
  std::uint64_t purgeable = 0;
  std::uint64_t free_bytes = 0;
  std::uint64_t swapped_pages_bytes = 0;
  std::uint64_t swapins = 0;
  std::uint64_t swapouts = 0;
  std::uint64_t compressions = 0;
  std::uint64_t decompressions = 0;
  std::uint64_t pageins = 0;
  std::uint64_t pageouts = 0;
  std::optional<double> swapin_rate_bps;
  std::optional<double> swapout_rate_bps;
  SwapInfo swap;
  MemoryPressureLevel pressure = MemoryPressureLevel::Unknown;
  std::optional<int> memory_free_pct;  // kern.memorystatus_level (0..100)
  GpuInfo gpu;
  bool valid = false;

  double compression_ratio() const {
    if (compressor == 0 || compressor_uncompressed == 0) {
      return 0.0;
    }
    return static_cast<double>(compressor_uncompressed) / static_cast<double>(compressor);
  }
};

struct SystemSnapshot {
  std::chrono::steady_clock::time_point timestamp = std::chrono::steady_clock::now();
  ProcessSnapshot procs;
  SystemMemory memory;
  CpuInfo cpu;
  DiskInfo disk;
  TelemetryInfo telemetry;
};

}  // namespace realmem
