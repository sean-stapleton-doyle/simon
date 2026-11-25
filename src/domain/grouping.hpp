#pragma once

#include "config/config.hpp"
#include "domain/snapshot.hpp"

#include <regex.h>

#include <string>
#include <string_view>
#include <vector>

namespace realmem {

struct DisplayRow;

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

// Derives a human-friendly application group name from custom rules,
// macOS .app bundle path, or process name heuristics.
std::string derive_group_name(const ProcessRecord& proc,
                              const std::vector<GroupRule>& custom_rules = {});

// Aggregates processes by derived application name.
class GroupAggregator {
 public:
  explicit GroupAggregator(const std::vector<GroupRule>& rules);

  std::vector<DisplayRow> aggregate(const std::vector<const ProcessRecord*>& procs) const;

 private:
  struct CompiledRule {
    PosixRegex regex;
    std::string name;
  };

  std::string classify(const ProcessRecord& proc) const;

  std::vector<CompiledRule> rules_;
};

}  // namespace realmem
