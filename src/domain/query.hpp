#pragma once

#include "config/config.hpp"
#include "domain/snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace simon {

struct DisplayRow {
  pid_t pid = 0;
  pid_t ppid = 0;
  std::size_t proc_count = 1;  // group member count OR subtree count in tree mode
  std::uint64_t footprint = 0;          // own footprint (or group total)
  std::uint64_t subtree_footprint = 0;  // rolled-up total for tree parents (== footprint otherwise)
  std::uint64_t peak = 0;
  std::optional<std::uint64_t> gpu_footprint;
  std::optional<double> cpu_pct;
  std::optional<double> p_cpu_pct;
  std::uint64_t disk_read_bytes = 0;
  std::uint64_t disk_write_bytes = 0;
  std::optional<double> disk_read_bps;
  std::optional<double> disk_write_bps;
  int tree_depth = 0;
  std::string tree_prefix;
  std::string name;
  std::string cmdline;

  // Primary memory value displayed and ranked for this row.
  std::uint64_t effective_footprint() const {
    return subtree_footprint > 0 ? subtree_footprint : footprint;
  }

  double effective_disk_bps() const {
    return disk_read_bps.value_or(0.0) + disk_write_bps.value_or(0.0);
  }

  std::uint64_t total_disk_bytes() const { return disk_read_bytes + disk_write_bytes; }
};

struct QuerySummary {
  std::vector<DisplayRow> rows;
  std::size_t total_readable = 0;
  std::size_t total_pids = 0;
  std::size_t matched_procs = 0;
  std::size_t total_groups = 0;
  std::size_t gpu_readable_procs = 0;
  std::uint64_t total_footprint = 0;
  std::uint64_t matched_footprint = 0;
  std::uint64_t largest_footprint = 0;
  pid_t largest_pid = 0;
  std::string largest_name;
};

// Strict weak ordering for DisplayRow sorting.
bool compare_display_rows(const DisplayRow& a,
                          const DisplayRow& b,
                          SortField field,
                          bool reverse);

class QueryEngine {
 public:
  static QuerySummary run(const ProcessSnapshot& snap, const Config& cfg);
};

}  // namespace simon
