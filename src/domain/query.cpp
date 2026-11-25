#include "domain/query.hpp"

#include "domain/grouping.hpp"
#include "domain/tree.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

namespace realmem {

namespace {

std::string to_lower_copy(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

template <typename T>
int compare_optional_desc(const std::optional<T>& a, const std::optional<T>& b) {
  if (a.has_value() != b.has_value()) {
    return a.has_value() ? -1 : 1;
  }
  if (!a.has_value()) {
    return 0;
  }
  return (*a > *b) ? -1 : (*a < *b ? 1 : 0);
}

int compare_primary_key(const DisplayRow& a, const DisplayRow& b, SortField field) {
  switch (field) {
    case SortField::Footprint: {
      const auto fa = a.effective_footprint();
      const auto fb = b.effective_footprint();
      return (fa > fb) ? -1 : (fa < fb ? 1 : 0);
    }
    case SortField::Peak:
      return (a.peak > b.peak) ? -1 : (a.peak < b.peak ? 1 : 0);
    case SortField::Gpu:
      return compare_optional_desc(a.gpu_footprint, b.gpu_footprint);
    case SortField::Cpu:
      return compare_optional_desc(a.cpu_pct, b.cpu_pct);
    case SortField::Disk: {
      const bool a_has_rate = a.disk_read_bps.has_value() || a.disk_write_bps.has_value();
      const bool b_has_rate = b.disk_read_bps.has_value() || b.disk_write_bps.has_value();
      if (a_has_rate || b_has_rate) {
        const double ra = a.effective_disk_bps();
        const double rb = b.effective_disk_bps();
        return (ra > rb) ? -1 : (ra < rb ? 1 : 0);
      }
      const auto ta = a.total_disk_bytes();
      const auto tb = b.total_disk_bytes();
      return (ta > tb) ? -1 : (ta < tb ? 1 : 0);
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
  const auto fa = a.effective_footprint();
  const auto fb = b.effective_footprint();
  if (fa != fb) {
    return (fa > fb) ? -1 : 1;
  }
  if (a.name != b.name) {
    return (a.name < b.name) ? -1 : 1;
  }
  return (a.pid < b.pid) ? -1 : (a.pid > b.pid ? 1 : 0);
}

std::vector<const ProcessRecord*> filter_processes(const ProcessSnapshot& snap,
                                                   const Config& cfg,
                                                   QuerySummary& summary) {
  std::optional<PosixRegex> filter;
  if (cfg.pattern) {
    filter.emplace(*cfg.pattern, cfg.case_sensitive);
  }

  std::vector<const ProcessRecord*> kept;
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

std::vector<DisplayRow> make_flat_rows(const std::vector<const ProcessRecord*>& procs) {
  std::vector<DisplayRow> rows;
  rows.reserve(procs.size());
  for (const ProcessRecord* proc : procs) {
    DisplayRow row;
    row.pid = proc->pid;
    row.ppid = proc->ppid;
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
    row.name = proc->name;
    row.cmdline = proc->cmdline;
    rows.push_back(std::move(row));
  }
  return rows;
}

}  // namespace

bool compare_display_rows(const DisplayRow& a,
                          const DisplayRow& b,
                          SortField field,
                          bool reverse) {
  if (field == SortField::Gpu && a.gpu_footprint.has_value() != b.gpu_footprint.has_value()) {
    return a.gpu_footprint.has_value();
  }
  if (field == SortField::Cpu && a.cpu_pct.has_value() != b.cpu_pct.has_value()) {
    return a.cpu_pct.has_value();
  }

  int ord = compare_primary_key(a, b, field);
  if (ord == 0) {
    ord = compare_tie_breakers(a, b);
  }
  return reverse ? (ord > 0) : (ord < 0);
}

QuerySummary QueryEngine::run(const ProcessSnapshot& snap, const Config& cfg) {
  QuerySummary summary;
  summary.total_readable = snap.processes.size();
  summary.total_pids = snap.total_pids;
  summary.total_footprint = snap.total_footprint;

  const std::vector<const ProcessRecord*> kept = filter_processes(snap, cfg, summary);

  if (cfg.tree) {
    summary.rows = ProcessTreeBuilder::build(kept, cfg.sort, cfg.reverse);
  } else if (cfg.group) {
    summary.rows = GroupAggregator(cfg.group_rules).aggregate(kept);
    summary.total_groups = summary.rows.size();
    std::sort(summary.rows.begin(), summary.rows.end(), [&](const DisplayRow& a,
                                                            const DisplayRow& b) {
      return compare_display_rows(a, b, cfg.sort, cfg.reverse);
    });
  } else {
    summary.rows = make_flat_rows(kept);
    std::sort(summary.rows.begin(), summary.rows.end(), [&](const DisplayRow& a,
                                                            const DisplayRow& b) {
      return compare_display_rows(a, b, cfg.sort, cfg.reverse);
    });
  }

  if (cfg.count > 0 && summary.rows.size() > static_cast<std::size_t>(cfg.count)) {
    summary.rows.resize(static_cast<std::size_t>(cfg.count));
  }

  return summary;
}

}  // namespace realmem
