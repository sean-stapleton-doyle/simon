#include "render/plain_renderer.hpp"

#include "render/format_utils.hpp"

#include <algorithm>
#include <format>
#include <sstream>

namespace simon {

namespace {

struct PlainLayout {
  bool show_cmdline = false;
  bool show_gpu = false;
  bool show_cpu = false;
  bool show_disk = false;
  std::size_t name_width = 0;
  std::size_t cmd_width = 20;
};

PlainLayout compute_layout(const QuerySummary& summary, const Config& cfg, int width) {
  PlainLayout l;
  l.show_cmdline = cfg.long_format && !cfg.group;
  l.show_gpu = cfg.show_gpu;
  l.show_cpu = cfg.show_cpu;
  l.show_disk = cfg.show_disk;

  std::size_t fixed = 7 + 2 + 10 + 2 + 10 + 2;
  if (l.show_gpu) fixed += 12;
  if (l.show_cpu) fixed += 9;
  if (l.show_disk) fixed += 12;

  std::size_t nw = cfg.group ? std::string_view("APPLICATION").size()
                             : std::string_view("COMMAND").size();
  for (const auto& row : summary.rows) {
    nw = std::max(nw, utf8_length(format_row_label(row, cfg)));
  }
  l.name_width = std::min<std::size_t>(nw, cfg.tree ? 36 : 24);
  const int rem = width - static_cast<int>(fixed) - static_cast<int>(l.name_width) - 2;
  l.cmd_width = static_cast<std::size_t>(std::max(20, rem));
  return l;
}

std::string format_header(const PlainLayout& l, bool grouped) {
  const char* first_col = grouped ? "PROCS" : "PID";
  const char* name_col = grouped ? "APPLICATION" : "COMMAND";

  std::string line = std::format("{:>7}  {:>10}", first_col, "FOOTPRINT");
  if (l.show_gpu) line += std::format("  {:>10}", "GPU");
  if (l.show_cpu) line += std::format("  {:>7}", "CPU");
  if (l.show_disk) line += std::format("  {:>10}", "DISK");
  line += std::format("  {:>10}  ", "PEAK");

  if (l.show_cmdline) {
    line += std::format("{}  COMMAND LINE", pad_right_utf8(name_col, l.name_width));
  } else {
    line += name_col;
  }
  return line;
}

std::string format_row(const DisplayRow& row, const PlainLayout& l, const Config& cfg) {
  const std::int64_t id_or_cnt =
      cfg.group ? static_cast<std::int64_t>(row.proc_count) : static_cast<std::int64_t>(row.pid);
  const std::string fp = humanize_bytes(row.effective_footprint());
  const std::string pk = row.peak ? humanize_bytes(row.peak) : "-";
  const std::string label = format_row_label(row, cfg);

  std::string line = std::format("{:>7}  {:>10}", id_or_cnt, fp);
  if (l.show_gpu) line += std::format("  {:>10}", format_gpu_value(row.gpu_footprint));
  if (l.show_cpu) line += std::format("  {:>7}", format_cpu_value(row.cpu_pct));
  if (l.show_disk) line += std::format("  {:>10}", format_disk_value(row));
  line += std::format("  {:>10}  ", pk);

  if (l.show_cmdline) {
    const std::string cmd =
        row.cmdline.empty() ? "-" : truncate_utf8(row.cmdline, l.cmd_width);
    line += std::format("{}  {}", pad_right_utf8(label, l.name_width), cmd);
  } else {
    line += label;
  }
  return line;
}

std::string format_system_section(const SystemSnapshot& snap, const Config& cfg) {
  const SystemMemory& sys = snap.memory;
  std::ostringstream out;
  out << std::format(
      "system: {} total | {} wired | {} anonymous | {} compressor",
      humanize_bytes(sys.total),
      humanize_bytes(sys.wired),
      humanize_bytes(sys.anonymous),
      humanize_bytes(sys.compressor));
  if (const double ratio = sys.compression_ratio(); ratio > 0.0) {
    out << std::format(" ({:.1f}x)", ratio);
  }
  out << std::format(" | {} file cache (reclaimable)", humanize_bytes(sys.file_cache));

  if (sys.swap.valid || sys.pressure != MemoryPressureLevel::Unknown) {
    out << '\n';
    bool need_sep = false;
    if (sys.swap.valid) {
      out << std::format("swap: {} / {} used",
                         humanize_bytes(sys.swap.used),
                         humanize_bytes(sys.swap.total));
      need_sep = true;
    }
    if (sys.pressure != MemoryPressureLevel::Unknown) {
      if (need_sep) out << " | ";
      out << std::format("pressure: {}", pressure_label(sys.pressure));
      if (sys.memory_free_pct) {
        out << std::format(" ({}% free)", *sys.memory_free_pct);
      }
    }
  }

  if (sys.gpu.valid || sys.gpu.effective_ceiling() > 0) {
    out << "\ngpu: ";
    bool need_sep = false;
    if (sys.gpu.valid) {
      out << std::format("{} alloc ({} in use)",
                         humanize_bytes(sys.gpu.allocated),
                         humanize_bytes(sys.gpu.in_use));
      need_sep = true;
    }
    if (const auto ceiling = sys.gpu.effective_ceiling(); ceiling > 0) {
      if (need_sep) out << " | ";
      out << std::format("limit {}", humanize_bytes(ceiling));
      if (auto head = sys.gpu.headroom_bytes()) {
        out << std::format(" ({} headroom)", humanize_bytes(*head));
      }
      need_sep = true;
    }
    if (sys.gpu.device_util_pct) {
      if (need_sep) out << " | ";
      out << std::format("{:.0f}% util", *sys.gpu.device_util_pct);
      if (sys.gpu.freq_mhz) out << std::format(" @ {:.0f}MHz", *sys.gpu.freq_mhz);
      if (sys.gpu.power_watts) out << std::format(" ({:.1f}W)", *sys.gpu.power_watts);
    }
  }

  if (cfg.show_cpu && snap.cpu.valid) {
    out << std::format("\ncpu: {:.1f}%", snap.cpu.total_util_pct);
    for (const auto& tier : snap.cpu.tiers) {
      out << std::format(" | {} ({}c): {:.1f}%", tier.name, tier.core_count, tier.util_pct);
      if (tier.freq_mhz) out << std::format(" @ {:.0f}MHz", *tier.freq_mhz);
    }
    if (snap.cpu.power_watts) {
      out << std::format(" | {:.1f}W", *snap.cpu.power_watts);
    }
    if (snap.telemetry.dram_bandwidth_gbps) {
      out << std::format(" | DRAM {:.1f} GB/s", *snap.telemetry.dram_bandwidth_gbps);
    }
  }

  if (cfg.show_disk && snap.disk.valid) {
    out << "\ndisk: ";
    if (snap.disk.read_bps || snap.disk.write_bps) {
      out << std::format("read {} | write {}",
                         format_rate(snap.disk.read_bps.value_or(0.0)),
                         format_rate(snap.disk.write_bps.value_or(0.0)));
    } else {
      out << std::format("read {} | write {} (lifetime)",
                         humanize_bytes(snap.disk.total_read_bytes),
                         humanize_bytes(snap.disk.total_write_bytes));
    }
  }

  return out.str();
}

}  // namespace

std::string PlainRenderer::render(const QuerySummary& summary,
                                  const SystemSnapshot& snap,
                                  const Config& cfg,
                                  const RenderContext& ctx) const {
  if (cfg.short_mode) {
    return snap.memory.valid ? (format_system_section(snap, cfg) + "\n") : std::string{};
  }

  std::ostringstream out;
  if (!cfg.sum_only) {
    const PlainLayout layout = compute_layout(summary, cfg, ctx.width);
    out << format_header(layout, cfg.group) << '\n';
    for (const auto& row : summary.rows) {
      out << format_row(row, layout, cfg) << '\n';
    }
    out << '\n';
  }

  out << format_summary_line(summary, cfg) << '\n';
  if (!cfg.sum_only && cfg.show_system && snap.memory.valid) {
    out << '\n' << format_system_section(snap, cfg) << '\n';
  }
  return out.str();
}

}  // namespace simon
