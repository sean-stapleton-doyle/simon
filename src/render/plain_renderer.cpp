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

std::string format_plain_metric_row(std::string_view metric,
                                    std::string_view value,
                                    std::string_view share,
                                    std::string_view detail) {
  std::string line = std::format("{:<11}  {:>9}  {:>7}", metric, value, share);
  if (!detail.empty()) {
    line += std::format("  {}", detail);
  }
  return line;
}

std::string format_system_section(const SystemSnapshot& snap, const Config& cfg) {
  const SystemMemory& sys = snap.memory;
  std::ostringstream out;

  std::string hdr_detail = std::format("{} total", humanize_bytes(sys.total));
  if (sys.free_bytes > 0) {
    hdr_detail += std::format(" ({} free)", humanize_bytes(sys.free_bytes));
  }
  out << format_plain_metric_row("METRIC", "VALUE", "SHARE", hdr_detail) << '\n';
  out << format_plain_metric_row("Wired", humanize_bytes(sys.wired),
                                 format_pct(sys.wired, sys.total), "kernel & pinned")
      << '\n';
  out << format_plain_metric_row("Anonymous", humanize_bytes(sys.anonymous),
                                 format_pct(sys.anonymous, sys.total), "app memory")
      << '\n';

  std::string comp_detail = "compressed pages";
  if (const double ratio = sys.compression_ratio(); ratio > 0.0) {
    comp_detail = std::format("{:.1f}x ratio ({} uncompressed)",
                              ratio,
                              humanize_bytes(sys.compressor_uncompressed));
  }
  out << format_plain_metric_row("Compressor", humanize_bytes(sys.compressor),
                                 format_pct(sys.compressor, sys.total), comp_detail)
      << '\n';
  out << format_plain_metric_row("File cache", humanize_bytes(sys.file_cache),
                                 format_pct(sys.file_cache, sys.total), "reclaimable");

  if (sys.swap.valid) {
    out << '\n'
        << format_plain_metric_row("Swap", humanize_bytes(sys.swap.used),
                                   format_pct(sys.swap.used, sys.swap.total),
                                   std::format("of {} total ({} free)",
                                               humanize_bytes(sys.swap.total),
                                               humanize_bytes(sys.swap.free_bytes)));
  }

  if (sys.pressure != MemoryPressureLevel::Unknown) {
    const std::string pct = sys.memory_free_pct ? std::format("{}%", *sys.memory_free_pct) : "";
    const std::string det = sys.memory_free_pct ? "free headroom" : "";
    out << '\n'
        << format_plain_metric_row("Pressure", pressure_label(sys.pressure), pct, det);
  }

  if (sys.gpu.valid || sys.gpu.effective_ceiling() > 0) {
    const auto ceiling = sys.gpu.effective_ceiling();
    const std::string val = sys.gpu.valid ? humanize_bytes(sys.gpu.allocated) : "-";
    const std::string share =
        (sys.gpu.valid && ceiling > 0) ? format_pct(sys.gpu.allocated, ceiling) : "";
    std::string det;
    auto add = [&](const std::string& part) {
      if (!det.empty()) det += " | ";
      det += part;
    };
    if (sys.gpu.valid) add(std::format("{} in use", humanize_bytes(sys.gpu.in_use)));
    if (ceiling > 0) {
      std::string lim = std::format("limit {}", humanize_bytes(ceiling));
      if (auto head = sys.gpu.headroom_bytes()) {
        lim += std::format(" ({} headroom)", humanize_bytes(*head));
      }
      add(lim);
    }
    if (sys.gpu.device_util_pct) {
      std::string u = std::format("{:.0f}% util", *sys.gpu.device_util_pct);
      if (sys.gpu.freq_mhz) u += std::format(" @ {:.0f}MHz", *sys.gpu.freq_mhz);
      if (sys.gpu.power_watts) u += std::format(" ({:.1f}W)", *sys.gpu.power_watts);
      add(u);
    }
    out << '\n' << format_plain_metric_row("GPU", val, share, det);
  }

  if (cfg.show_cpu && snap.cpu.valid) {
    const std::string watts =
        snap.cpu.power_watts ? std::format("{:.1f}W", *snap.cpu.power_watts) : "";
    std::string det;
    for (std::size_t i = 0; i < snap.cpu.tiers.size(); ++i) {
      if (i > 0) det += " | ";
      const auto& t = snap.cpu.tiers[i];
      det += std::format("{} ({}c): {:.1f}%", t.name, t.core_count, t.util_pct);
      if (t.freq_mhz) det += std::format(" @ {:.0f}MHz", *t.freq_mhz);
    }
    if (snap.telemetry.dram_bandwidth_gbps) {
      if (!det.empty()) det += " | ";
      det += std::format("DRAM {:.1f} GB/s", *snap.telemetry.dram_bandwidth_gbps);
    }
    out << '\n'
        << format_plain_metric_row("CPU", std::format("{:.1f}%", snap.cpu.total_util_pct), watts,
                                   det);
  }

  if (cfg.show_disk && snap.disk.valid) {
    std::string val;
    std::string det;
    if (snap.disk.read_bps || snap.disk.write_bps) {
      const double rb = snap.disk.read_bps.value_or(0.0);
      const double wb = snap.disk.write_bps.value_or(0.0);
      val = format_rate(rb + wb);
      det = std::format("read {} | write {}", format_rate(rb), format_rate(wb));
    } else {
      val = humanize_bytes(snap.disk.total_read_bytes + snap.disk.total_write_bytes);
      det = std::format("read {} | write {} (lifetime)",
                        humanize_bytes(snap.disk.total_read_bytes),
                        humanize_bytes(snap.disk.total_write_bytes));
    }
    out << '\n' << format_plain_metric_row("Disk", val, "", det);
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
