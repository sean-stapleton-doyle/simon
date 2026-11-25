#include "domain/grouping.hpp"

#include "domain/query.hpp"

#include <cctype>
#include <format>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace simon {

namespace {

std::string_view trim_spaces(std::string_view sv) {
  while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
    sv.remove_prefix(1);
  }
  while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) {
    sv.remove_suffix(1);
  }
  return sv;
}

std::optional<std::string> extract_app_bundle_name(std::string_view path) {
  constexpr std::string_view kSuffix = ".app/";
  const std::size_t pos = path.find(kSuffix);
  if (pos == std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t slash = path.rfind('/', pos);
  const std::size_t start = (slash == std::string_view::npos) ? 0 : slash + 1;
  if (pos <= start) {
    return std::nullopt;
  }
  return std::string(path.substr(start, pos - start));
}

std::string normalize_process_name(std::string_view name) {
  if (name.starts_with("com.apple.WebKit")) {
    return "WebKit";
  }
  for (std::string_view marker :
       {" Helper", " Web Content", " Networking", " GPU", " Renderer"}) {
    const std::size_t pos = name.find(marker);
    if (pos != std::string_view::npos && pos > 0) {
      std::string_view prefix = trim_spaces(name.substr(0, pos));
      if (!prefix.empty()) {
        return std::string(prefix);
      }
    }
  }
  return std::string(name);
}

template <typename T>
void merge_optional_sum(std::optional<T>& target, const std::optional<T>& added) {
  if (added) {
    target = target.value_or(T{0}) + *added;
  }
}

}  // namespace

PosixRegex::PosixRegex(const std::string& pattern, bool case_sensitive) {
  const int flags = REG_EXTENDED | REG_NOSUB | (case_sensitive ? 0 : REG_ICASE);
  const int rc = regcomp(&re_, pattern.c_str(), flags);
  if (rc != 0) {
    char err[256];
    regerror(rc, &re_, err, sizeof(err));
    throw std::runtime_error(std::format("bad regex '{}': {}", pattern, err));
  }
  compiled_ = true;
}

PosixRegex::~PosixRegex() {
  if (compiled_) {
    regfree(&re_);
  }
}

PosixRegex::PosixRegex(PosixRegex&& other) noexcept
    : re_(other.re_), compiled_(other.compiled_) {
  other.compiled_ = false;
}

PosixRegex& PosixRegex::operator=(PosixRegex&& other) noexcept {
  if (this != &other) {
    if (compiled_) {
      regfree(&re_);
    }
    re_ = other.re_;
    compiled_ = other.compiled_;
    other.compiled_ = false;
  }
  return *this;
}

bool PosixRegex::matches(std::string_view text) const {
  if (!compiled_) {
    return false;
  }
  std::string owned(text);
  return regexec(&re_, owned.c_str(), 0, nullptr, 0) == 0;
}

GroupAggregator::GroupAggregator(const std::vector<GroupRule>& rules) {
  rules_.reserve(rules.size());
  for (const auto& rule : rules) {
    rules_.push_back(CompiledRule{PosixRegex(rule.pattern, /*case_sensitive=*/false), rule.name});
  }
}

std::string GroupAggregator::classify(const ProcessRecord& proc) const {
  const std::string_view hay = !proc.cmdline.empty()
                                   ? std::string_view(proc.cmdline)
                                   : (!proc.path.empty() ? std::string_view(proc.path)
                                                         : std::string_view(proc.name));
  for (const auto& rule : rules_) {
    if (rule.regex.matches(hay) || rule.regex.matches(proc.name)) {
      return rule.name;
    }
  }
  if (!proc.path.empty()) {
    if (auto app = extract_app_bundle_name(proc.path)) {
      return *app;
    }
  }
  return normalize_process_name(proc.name);
}

std::vector<DisplayRow> GroupAggregator::aggregate(
    const std::vector<const ProcessRecord*>& procs) const {
  std::unordered_map<std::string, std::size_t> index_by_group;
  std::vector<DisplayRow> rows;
  rows.reserve(procs.size());

  for (const ProcessRecord* proc : procs) {
    std::string group = classify(*proc);
    auto [it, inserted] = index_by_group.emplace(group, rows.size());
    if (inserted) {
      DisplayRow row;
      row.proc_count = 1;
      row.footprint = proc->footprint;
      row.subtree_footprint = proc->footprint;
      row.peak = proc->lifetime_max;
      row.gpu_footprint = proc->gpu_footprint;
      row.cpu_pct = proc->cpu_pct;
      row.p_cpu_pct = proc->p_cpu_pct;
      row.disk_read_bytes = proc->disk_read_bytes;
      row.disk_write_bytes = proc->disk_write_bytes;
      row.disk_read_bps = proc->disk_read_bps;
      row.disk_write_bps = proc->disk_write_bps;
      row.name = std::move(group);
      rows.push_back(std::move(row));
    } else {
      DisplayRow& row = rows[it->second];
      ++row.proc_count;
      row.footprint += proc->footprint;
      row.subtree_footprint += proc->footprint;
      row.peak += proc->lifetime_max;
      merge_optional_sum(row.gpu_footprint, proc->gpu_footprint);
      merge_optional_sum(row.cpu_pct, proc->cpu_pct);
      merge_optional_sum(row.p_cpu_pct, proc->p_cpu_pct);
      row.disk_read_bytes += proc->disk_read_bytes;
      row.disk_write_bytes += proc->disk_write_bytes;
      merge_optional_sum(row.disk_read_bps, proc->disk_read_bps);
      merge_optional_sum(row.disk_write_bps, proc->disk_write_bps);
    }
  }

  return rows;
}

std::string derive_group_name(const ProcessRecord& proc,
                              const std::vector<GroupRule>& custom_rules) {
  return GroupAggregator(custom_rules).aggregate({&proc}).front().name;
}

}  // namespace simon
