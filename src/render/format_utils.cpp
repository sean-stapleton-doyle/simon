#include "render/format_utils.hpp"

#include <algorithm>
#include <array>
#include <format>

namespace simon {

std::string humanize_bytes(std::uint64_t bytes) {
  static constexpr std::array<const char*, 5> kUnits = {"B", "K", "M", "G", "T"};
  double value = static_cast<double>(bytes);
  std::size_t idx = 0;
  while (value >= 1024.0 && idx + 1 < kUnits.size()) {
    value /= 1024.0;
    ++idx;
  }
  return std::format("{:.1f}{}", value, kUnits[idx]);
}

std::string format_rate(double bytes_per_sec) {
  if (bytes_per_sec <= 0.0) {
    return "0.0B/s";
  }
  return humanize_bytes(static_cast<std::uint64_t>(bytes_per_sec)) + "/s";
}

std::size_t utf8_length(std::string_view text) {
  std::size_t count = 0;
  for (unsigned char c : text) {
    if ((c & 0xC0) != 0x80) {
      ++count;
    }
  }
  return count;
}

std::string truncate_utf8(std::string_view text, std::size_t max_cols) {
  if (max_cols == 0) {
    return {};
  }

  std::size_t cols = 0;
  std::size_t cut_bytes = 0;
  std::size_t i = 0;

  while (i < text.size()) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    std::size_t seq_len = 1;
    if ((lead & 0x80) == 0x00) {
      seq_len = 1;
    } else if ((lead & 0xE0) == 0xC0) {
      seq_len = 2;
    } else if ((lead & 0xF0) == 0xE0) {
      seq_len = 3;
    } else if ((lead & 0xF8) == 0xF0) {
      seq_len = 4;
    }
    seq_len = std::min(seq_len, text.size() - i);

    ++cols;
    if (cols == max_cols) {
      cut_bytes = i;
    }
    if (cols > max_cols) {
      std::string out(text.substr(0, cut_bytes));
      out.append("\xe2\x80\xa6");  // U+2026 HORIZONTAL ELLIPSIS
      return out;
    }
    i += seq_len;
  }
  return std::string(text);
}

std::string pad_right_utf8(std::string_view text, std::size_t width) {
  const std::string truncated = truncate_utf8(text, width);
  const std::size_t len = utf8_length(truncated);
  return len >= width ? truncated : truncated + std::string(width - len, ' ');
}

std::string format_pct(std::uint64_t part, std::uint64_t total) {
  if (total == 0) {
    return "-";
  }
  const double pct = (static_cast<double>(part) * 100.0) / static_cast<double>(total);
  return std::format("{:.1f}%", pct);
}

std::string format_gpu_value(const std::optional<std::uint64_t>& gpu) {
  return gpu ? humanize_bytes(*gpu) : "-";
}

std::string format_cpu_value(const std::optional<double>& cpu_pct) {
  return cpu_pct ? std::format("{:.1f}%", *cpu_pct) : "-";
}

std::string format_disk_value(const DisplayRow& row) {
  if (row.disk_read_bps.has_value() || row.disk_write_bps.has_value()) {
    return format_rate(row.effective_disk_bps());
  }
  return humanize_bytes(row.total_disk_bytes());
}

std::string_view pressure_label(MemoryPressureLevel level) {
  switch (level) {
    case MemoryPressureLevel::Normal: return "normal";
    case MemoryPressureLevel::Warn: return "warn";
    case MemoryPressureLevel::Critical: return "critical";
    case MemoryPressureLevel::Unknown: return "unknown";
  }
  return "unknown";
}

std::string format_row_label(const DisplayRow& row, const Config& cfg) {
  if (!cfg.tree) {
    return row.name;
  }
  std::string label = row.tree_prefix + row.name;
  if (row.proc_count > 1) {
    label += std::format(" ({})", row.proc_count);
  }
  return label;
}

std::string format_summary_line(const QuerySummary& summary, const Config& cfg) {
  std::string line;
  if (cfg.pattern) {
    if (summary.matched_procs == 0) {
      line = std::format("no processes match /{}/", *cfg.pattern);
    } else {
      line = std::format("{} process{} matching /{}/",
                         summary.matched_procs,
                         summary.matched_procs == 1 ? "" : "es",
                         *cfg.pattern);
      if (cfg.group) {
        line += std::format(" ({} app{})",
                            summary.total_groups,
                            summary.total_groups == 1 ? "" : "s");
      }
      line += std::format(", {} total", humanize_bytes(summary.matched_footprint));
      if (summary.matched_procs > 1) {
        line += std::format(" (largest {}, pid {})",
                            humanize_bytes(summary.largest_footprint),
                            summary.largest_pid);
      }
    }
  } else {
    line = std::format("{} of {} processes readable", summary.total_readable, summary.total_pids);
    if (cfg.group) {
      line += std::format(" across {} apps", summary.total_groups);
    }
    line += std::format(", {} total footprint", humanize_bytes(summary.total_footprint));
  }

  const bool any_visible_gpu =
      std::any_of(summary.rows.begin(), summary.rows.end(), [](const DisplayRow& row) {
        return row.gpu_footprint.has_value();
      });
  if (cfg.show_gpu && !summary.rows.empty() && !any_visible_gpu) {
    line += " (per-process GPU requires sudo)";
  }
  return line;
}

}  // namespace simon
