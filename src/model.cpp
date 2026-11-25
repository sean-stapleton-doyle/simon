#include "model.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <stdexcept>
#include <unordered_map>

namespace realmem {

namespace {

struct CompiledGroupRule {
  PosixRegex regex;
  std::string name;
};

std::string to_lower_copy(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

std::string_view trim_spaces(std::string_view sv) {
  while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
    sv.remove_prefix(1);
  }
  while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) {
    sv.remove_suffix(1);
  }
  return sv;
}

std::vector<CompiledGroupRule> compile_group_rules(const std::vector<GroupRule>& rules) {
  std::vector<CompiledGroupRule> compiled;
  compiled.reserve(rules.size());
  for (const auto& rule : rules) {
    compiled.push_back(
        CompiledGroupRule{PosixRegex(rule.pattern, /*case_sensitive=*/false), rule.name});
  }
  return compiled;
}

// Extracts the outermost macOS .app bundle name from an executable path, e.g.:
// /Applications/Google Chrome.app/Contents/.../Google Chrome Helper -> "Google Chrome"
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

std::string derive_group_name_compiled(const ProcessInfo& proc,
                                       const std::vector<CompiledGroupRule>& rules) {
  const std::string_view hay = !proc.cmdline.empty()
                                   ? std::string_view(proc.cmdline)
                                   : (!proc.path.empty() ? std::string_view(proc.path)
                                                         : std::string_view(proc.name));
  for (const auto& rule : rules) {
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

void merge_gpu_footprint(std::optional<std::uint64_t>& target,
                         const std::optional<std::uint64_t>& added) {
  if (!added) {
    return;
  }
  target = target.value_or(0) + *added;
}

std::vector<const ProcessInfo*> filter_processes(const Snapshot& snap,
                                                 const Config& cfg,
                                                 QuerySummary& summary) {
  std::optional<PosixRegex> filter;
  if (cfg.pattern) {
    filter.emplace(*cfg.pattern, cfg.case_sensitive);
  }

  std::vector<const ProcessInfo*> kept;
  kept.reserve(snap.processes.size());

  for (const auto& proc : snap.processes) {
    if (filter) {
      const std::string_view hay =
          !proc.cmdline.empty() ? std::string_view(proc.cmdline) : std::string_view(proc.name);
      if (!filter->matches(hay)) {
        continue;
      }
      ++summary.matched_procs;
      summary.matched_footprint += proc.footprint;
      if (proc.footprint >= summary.largest_footprint) {
        summary.largest_footprint = proc.footprint;
        summary.largest_pid = proc.pid;
        summary.largest_name = proc.name;
      }
    }
    if (proc.gpu_footprint.has_value()) {
      ++summary.gpu_readable_procs;
    }
    kept.push_back(&proc);
  }

  return kept;
}

std::vector<DisplayRow> aggregate_groups(const std::vector<const ProcessInfo*>& procs,
                                         const std::vector<GroupRule>& rules) {
  const std::vector<CompiledGroupRule> compiled = compile_group_rules(rules);
  std::unordered_map<std::string, std::size_t> index_by_group;
  std::vector<DisplayRow> rows;
  rows.reserve(procs.size());

  for (const ProcessInfo* proc : procs) {
    std::string group = derive_group_name_compiled(*proc, compiled);
    auto [it, inserted] = index_by_group.emplace(group, rows.size());
    if (inserted) {
      DisplayRow row;
      row.proc_count = 1;
      row.footprint = proc->footprint;
      row.peak = proc->lifetime_max;
      row.gpu_footprint = proc->gpu_footprint;
      row.name = std::move(group);
      rows.push_back(std::move(row));
    } else {
      DisplayRow& row = rows[it->second];
      ++row.proc_count;
      row.footprint += proc->footprint;
      row.peak += proc->lifetime_max;
      merge_gpu_footprint(row.gpu_footprint, proc->gpu_footprint);
    }
  }

  return rows;
}

std::vector<DisplayRow> make_ungrouped_rows(const std::vector<const ProcessInfo*>& procs) {
  std::vector<DisplayRow> rows;
  rows.reserve(procs.size());
  for (const ProcessInfo* proc : procs) {
    DisplayRow row;
    row.pid = proc->pid;
    row.proc_count = 1;
    row.footprint = proc->footprint;
    row.peak = proc->lifetime_max;
    row.gpu_footprint = proc->gpu_footprint;
    row.name = proc->name;
    row.cmdline = proc->cmdline;
    rows.push_back(std::move(row));
  }
  return rows;
}

int compare_primary_key(const DisplayRow& a, const DisplayRow& b, SortField field) {
  switch (field) {
    case SortField::Footprint:
      return (a.footprint > b.footprint) ? -1 : (a.footprint < b.footprint ? 1 : 0);
    case SortField::Peak:
      return (a.peak > b.peak) ? -1 : (a.peak < b.peak ? 1 : 0);
    case SortField::Gpu: {
      if (a.gpu_footprint.has_value() != b.gpu_footprint.has_value()) {
        return a.gpu_footprint.has_value() ? -1 : 1;
      }
      if (!a.gpu_footprint.has_value()) {
        return 0;
      }
      return (*a.gpu_footprint > *b.gpu_footprint) ? -1
                                                   : (*a.gpu_footprint < *b.gpu_footprint ? 1 : 0);
    }
    case SortField::Pid:
      return (a.pid < b.pid) ? -1 : (a.pid > b.pid ? 1 : 0);
    case SortField::Name: {
      const std::string la = to_lower_copy(a.name);
      const std::string lb = to_lower_copy(b.name);
      return (la < lb) ? -1 : (la > lb ? 1 : 0);
    }
    case SortField::Count:
      return (a.proc_count > b.proc_count) ? -1 : (a.proc_count < b.proc_count ? 1 : 0);
  }
  return 0;
}

int compare_tie_breakers(const DisplayRow& a, const DisplayRow& b) {
  if (a.footprint != b.footprint) {
    return (a.footprint > b.footprint) ? -1 : 1;
  }
  if (a.name != b.name) {
    return (a.name < b.name) ? -1 : 1;
  }
  return (a.pid < b.pid) ? -1 : (a.pid > b.pid ? 1 : 0);
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

std::string derive_group_name(const ProcessInfo& proc,
                              const std::vector<GroupRule>& custom_rules) {
  return derive_group_name_compiled(proc, compile_group_rules(custom_rules));
}

bool compare_display_rows(const DisplayRow& a,
                          const DisplayRow& b,
                          SortField field,
                          bool reverse) {
  // Always keep unreadable GPU entries at the bottom when sorting by GPU.
  if (field == SortField::Gpu && a.gpu_footprint.has_value() != b.gpu_footprint.has_value()) {
    return a.gpu_footprint.has_value();
  }

  int ord = compare_primary_key(a, b, field);
  if (ord == 0) {
    ord = compare_tie_breakers(a, b);
  }
  return reverse ? (ord > 0) : (ord < 0);
}

QuerySummary build_query_summary(const Snapshot& snap, const Config& cfg) {
  QuerySummary summary;
  summary.total_readable = snap.processes.size();
  summary.total_pids = snap.total_pids;
  summary.total_footprint = snap.total_footprint;

  const std::vector<const ProcessInfo*> kept = filter_processes(snap, cfg, summary);
  if (cfg.group) {
    summary.rows = aggregate_groups(kept, cfg.group_rules);
    summary.total_groups = summary.rows.size();
  } else {
    summary.rows = make_ungrouped_rows(kept);
  }

  std::sort(summary.rows.begin(), summary.rows.end(), [&](const DisplayRow& a,
                                                          const DisplayRow& b) {
    return compare_display_rows(a, b, cfg.sort, cfg.reverse);
  });

  if (cfg.count > 0 && summary.rows.size() > static_cast<std::size_t>(cfg.count)) {
    summary.rows.resize(static_cast<std::size_t>(cfg.count));
  }

  return summary;
}

}  // namespace realmem
