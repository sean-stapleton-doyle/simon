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

  for (std::string_view marker : {" Helper", " Web Content", " Networking", " GPU", " Renderer"}) {
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
  std::vector<CompiledGroupRule> compiled;
  compiled.reserve(custom_rules.size());
  for (const auto& r : custom_rules) {
    compiled.push_back(CompiledGroupRule{PosixRegex(r.pattern, /*case_sensitive=*/false), r.name});
  }
  return derive_group_name_compiled(proc, compiled);
}

QuerySummary build_query_summary(const Snapshot& snap, const Config& cfg) {
  QuerySummary summary;
  summary.total_readable = snap.processes.size();
  summary.total_pids = snap.total_pids;
  summary.total_footprint = snap.total_footprint;

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
    kept.push_back(&proc);
  }

  if (cfg.group) {
    std::vector<CompiledGroupRule> compiled_rules;
    compiled_rules.reserve(cfg.group_rules.size());
    for (const auto& r : cfg.group_rules) {
      compiled_rules.push_back(
          CompiledGroupRule{PosixRegex(r.pattern, /*case_sensitive=*/false), r.name});
    }

    std::unordered_map<std::string, std::size_t> index_by_group;
    summary.rows.reserve(kept.size());

    for (const ProcessInfo* proc : kept) {
      std::string group = derive_group_name_compiled(*proc, compiled_rules);
      auto [it, inserted] = index_by_group.emplace(group, summary.rows.size());
      if (inserted) {
        DisplayRow row;
        row.proc_count = 1;
        row.footprint = proc->footprint;
        row.peak = proc->lifetime_max;
        row.name = std::move(group);
        summary.rows.push_back(std::move(row));
      } else {
        DisplayRow& row = summary.rows[it->second];
        ++row.proc_count;
        row.footprint += proc->footprint;
        row.peak += proc->lifetime_max;
      }
    }
    summary.total_groups = summary.rows.size();
  } else {
    summary.rows.reserve(kept.size());
    for (const ProcessInfo* proc : kept) {
      DisplayRow row;
      row.pid = proc->pid;
      row.proc_count = 1;
      row.footprint = proc->footprint;
      row.peak = proc->lifetime_max;
      row.name = proc->name;
      row.cmdline = proc->cmdline;
      summary.rows.push_back(std::move(row));
    }
  }

  auto cmp = [&](const DisplayRow& a, const DisplayRow& b) {
    auto cmp_primary = [&]() -> int {
      switch (cfg.sort) {
        case SortField::Footprint:
          if (a.footprint != b.footprint) {
            return (a.footprint > b.footprint) ? -1 : 1;
          }
          break;
        case SortField::Peak:
          if (a.peak != b.peak) {
            return (a.peak > b.peak) ? -1 : 1;
          }
          break;
        case SortField::Pid:
          if (a.pid != b.pid) {
            return (a.pid < b.pid) ? -1 : 1;
          }
          break;
        case SortField::Name: {
          const std::string la = to_lower_copy(a.name);
          const std::string lb = to_lower_copy(b.name);
          if (la != lb) {
            return (la < lb) ? -1 : 1;
          }
          break;
        }
        case SortField::Count:
          if (a.proc_count != b.proc_count) {
            return (a.proc_count > b.proc_count) ? -1 : 1;
          }
          break;
      }
      // Deterministic tie-breakers.
      if (a.footprint != b.footprint) {
        return (a.footprint > b.footprint) ? -1 : 1;
      }
      if (a.name != b.name) {
        return (a.name < b.name) ? -1 : 1;
      }
      return (a.pid < b.pid) ? -1 : (a.pid > b.pid ? 1 : 0);
    };

    const int ord = cmp_primary();
    return cfg.reverse ? (ord > 0) : (ord < 0);
  };

  std::sort(summary.rows.begin(), summary.rows.end(), cmp);

  if (cfg.count > 0 && summary.rows.size() > static_cast<std::size_t>(cfg.count)) {
    summary.rows.resize(static_cast<std::size_t>(cfg.count));
  }

  return summary;
}

}  // namespace realmem
