#pragma once

#include "config.hpp"
#include "process.hpp"

#include <regex.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace realmem {

// RAII wrapper around POSIX Extended Regular Expressions (<regex.h>).
class PosixRegex {
 public:
  PosixRegex(const std::string& pattern, bool case_sensitive);
  ~PosixRegex();

  PosixRegex(const PosixRegex&) = delete;
  PosixRegex& operator=(const PosixRegex&) = delete;
  PosixRegex(PosixRegex&& other) noexcept;
  PosixRegex& operator=(PosixRegex&& other) noexcept;

  bool matches(std::string_view text) const;

 private:
  regex_t re_{};
  bool compiled_ = false;
};

struct DisplayRow {
  pid_t pid = 0;               // valid when ungrouped
  std::size_t proc_count = 1;  // valid when grouped
  std::uint64_t footprint = 0;
  std::uint64_t peak = 0;
  std::optional<std::uint64_t> gpu_footprint;
  std::string name;
  std::string cmdline;
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

// Derives a human-friendly application group name from a process's custom rules,
// macOS .app bundle path, or process name heuristics.
std::string derive_group_name(const ProcessInfo& proc,
                              const std::vector<GroupRule>& custom_rules = {});

// Compares two DisplayRows according to `field` and `reverse` (strict weak ordering).
bool compare_display_rows(const DisplayRow& a,
                          const DisplayRow& b,
                          SortField field,
                          bool reverse);

// Filters, optionally groups, sorts, and limits the process snapshot according to `cfg`.
QuerySummary build_query_summary(const Snapshot& snap, const Config& cfg);

}  // namespace realmem
