#include "render/table_renderer.hpp"

#include "render/format_utils.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/screen.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace simon {

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kGiB = 1024ULL * kMiB;

struct TableLayout {
  int width = 80;
  bool show_cmdline = false;
  bool show_gpu = false;
  bool show_cpu = false;
  bool show_disk = false;
  bool show_pct = false;
  bool show_bars = false;
  std::size_t fixed_cols = 0;
  std::size_t name_width = 0;
  std::size_t cmd_width = 0;
};

ftxui::Color footprint_color(std::uint64_t bytes) {
  if (bytes >= 4ULL * kGiB) return ftxui::Color::RedLight;
  if (bytes >= 1ULL * kGiB) return ftxui::Color::Yellow;
  if (bytes >= 256ULL * kMiB) return ftxui::Color::Cyan;
  return ftxui::Color::Default;
}

ftxui::Color pressure_color(MemoryPressureLevel level) {
  switch (level) {
    case MemoryPressureLevel::Normal: return ftxui::Color::Green;
    case MemoryPressureLevel::Warn: return ftxui::Color::Yellow;
    case MemoryPressureLevel::Critical: return ftxui::Color::RedLight;
    case MemoryPressureLevel::Unknown: return ftxui::Color::GrayLight;
  }
  return ftxui::Color::GrayLight;
}

std::string repeat_str(std::string_view unit, std::size_t n) {
  std::string out;
  out.reserve(unit.size() * n);
  for (std::size_t i = 0; i < n; ++i) {
    out.append(unit);
  }
  return out;
}

std::string trim_trailing_line_spaces(std::string_view rendered) {
  std::string out;
  out.reserve(rendered.size());
  std::size_t pos = 0;
  while (pos < rendered.size()) {
    const std::size_t nl = rendered.find('\n', pos);
    const std::size_t end = (nl == std::string_view::npos) ? rendered.size() : nl;
    std::string_view line = rendered.substr(pos, end - pos);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
      line.remove_suffix(1);
    }
    out.append(line);
    if (nl != std::string_view::npos) {
      out.push_back('\n');
      pos = nl + 1;
    } else {
      break;
    }
  }
  return out;
}

TableLayout compute_layout(const QuerySummary& summary,
                           const SystemMemory& sys,
                           const Config& cfg,
                           int raw_width) {
  TableLayout l;
  l.width = std::max(60, raw_width);
  l.show_cmdline = cfg.long_format && !cfg.group;
  l.show_gpu = cfg.show_gpu;
  l.show_cpu = cfg.show_cpu;
  l.show_disk = cfg.show_disk;

  const int extra = (l.show_gpu ? 12 : 0) + (l.show_cpu ? 9 : 0) + (l.show_disk ? 12 : 0);
  l.show_bars =
      cfg.show_bars && (l.show_cmdline ? l.width >= 105 + extra : l.width >= 74 + extra);
  l.show_pct = sys.valid && sys.total > 0 &&
               (l.show_cmdline ? l.width >= 88 + extra : l.width >= 60 + extra);

  l.fixed_cols = 7 + 2 + 10 + 2 + 10 + 2;
  if (l.show_pct) l.fixed_cols += 8;
  if (l.show_bars) l.fixed_cols += 12;
  if (l.show_gpu) l.fixed_cols += 12;
  if (l.show_cpu) l.fixed_cols += 9;
  if (l.show_disk) l.fixed_cols += 12;

  std::size_t nw = cfg.group ? std::string_view("APPLICATION").size()
                             : std::string_view("COMMAND").size();
  for (const auto& row : summary.rows) {
    nw = std::max(nw, utf8_length(format_row_label(row, cfg)));
  }
  l.name_width = std::min<std::size_t>(nw, l.show_cmdline ? 24 : 36);

  if (l.show_cmdline) {
    const std::size_t avail = (static_cast<std::size_t>(l.width) > l.fixed_cols + 2)
                                  ? (static_cast<std::size_t>(l.width) - l.fixed_cols - 2)
                                  : 24;
    const std::size_t max_name = (avail > 20) ? std::min<std::size_t>(24, avail - 16) : 8;
    l.name_width = std::min(l.name_width, std::max<std::size_t>(8, max_name));
    l.cmd_width = (avail > l.name_width) ? (avail - l.name_width) : 8;
  }
  return l;
}

ftxui::Element build_header(const TableLayout& l, bool grouped) {
  using namespace ftxui;

  const char* first_col = grouped ? "PROCS" : "PID";
  const char* name_col = grouped ? "APPLICATION" : "COMMAND";

  Elements cells;
  cells.push_back(text(std::format("{:>7}", first_col)) | bold | color(Color::GrayLight));
  cells.push_back(text("  "));
  cells.push_back(text(std::format("{:>10}", "FOOTPRINT")) | bold | color(Color::White));
  if (l.show_pct) {
    cells.push_back(text("  "));
    cells.push_back(text(std::format("{:>6}", "%RAM")) | bold | color(Color::GrayLight));
  }
  if (l.show_bars) {
    cells.push_back(text("  "));
    cells.push_back(text(std::format("{:<10}", "USAGE")) | bold | color(Color::GrayLight));
  }
  if (l.show_gpu) {
    cells.push_back(text("  "));
    cells.push_back(text(std::format("{:>10}", "GPU")) | bold | color(Color::BlueLight));
  }
  if (l.show_cpu) {
    cells.push_back(text("  "));
    cells.push_back(text(std::format("{:>7}", "CPU")) | bold | color(Color::GreenLight));
  }
  if (l.show_disk) {
    cells.push_back(text("  "));
    cells.push_back(text(std::format("{:>10}", "DISK")) | bold | color(Color::MagentaLight));
  }
  cells.push_back(text("  "));
  cells.push_back(text(std::format("{:>10}", "PEAK")) | bold | color(Color::GrayLight));
  cells.push_back(text("  "));

  if (l.show_cmdline) {
    cells.push_back(text(pad_right_utf8(name_col, l.name_width)) | bold | color(Color::White));
    cells.push_back(text("  "));
    cells.push_back(text(truncate_utf8("COMMAND LINE", l.cmd_width)) | bold |
                    color(Color::GrayLight));
  } else {
    cells.push_back(text(name_col) | bold | color(Color::White));
  }
  return hbox(std::move(cells));
}

ftxui::Element build_row(const DisplayRow& row,
                         std::uint64_t max_row_fp,
                         const SystemMemory& sys,
                         const TableLayout& l,
                         const Config& cfg) {
  using namespace ftxui;

  const std::int64_t id_or_cnt =
      cfg.group ? static_cast<std::int64_t>(row.proc_count) : static_cast<std::int64_t>(row.pid);
  const std::uint64_t eff_fp = row.effective_footprint();
  const Color fp_col = footprint_color(eff_fp);
  const std::string pk_str = row.peak ? humanize_bytes(row.peak) : "-";
  const std::string label = format_row_label(row, cfg);

  Elements cells;
  cells.push_back(text(std::format("{:>7}", id_or_cnt)) | dim);
  cells.push_back(text("  "));
  cells.push_back(text(std::format("{:>10}", humanize_bytes(eff_fp))) | color(fp_col) | bold);

  if (l.show_pct) {
    cells.push_back(text("  "));
    cells.push_back(text(std::format("{:>6}", format_pct(eff_fp, sys.total))) | dim);
  }
  if (l.show_bars) {
    const float ratio = static_cast<float>(
        std::clamp(static_cast<double>(eff_fp) / static_cast<double>(max_row_fp), 0.0, 1.0));
    cells.push_back(text("  "));
    cells.push_back(gauge(ratio) | size(WIDTH, EQUAL, 10) | color(fp_col));
  }
  if (l.show_gpu) {
    cells.push_back(text("  "));
    auto gpu_el = text(std::format("{:>10}", format_gpu_value(row.gpu_footprint)));
    const bool active = row.gpu_footprint.has_value() && *row.gpu_footprint > 0;
    cells.push_back(active ? (gpu_el | color(Color::BlueLight)) : (gpu_el | dim));
  }
  if (l.show_cpu) {
    cells.push_back(text("  "));
    auto cpu_el = text(std::format("{:>7}", format_cpu_value(row.cpu_pct)));
    const bool hot = row.cpu_pct.has_value() && *row.cpu_pct >= 50.0;
    cells.push_back(hot ? (cpu_el | color(Color::GreenLight) | bold) : (cpu_el | dim));
  }
  if (l.show_disk) {
    cells.push_back(text("  "));
    auto dsk_el = text(std::format("{:>10}", format_disk_value(row)));
    const bool active = row.effective_disk_bps() >= 1024.0 * 1024.0;
    cells.push_back(active ? (dsk_el | color(Color::MagentaLight)) : (dsk_el | dim));
  }

  cells.push_back(text("  "));
  const bool high_peak = row.peak > 0 && row.peak >= eff_fp * 2 && row.peak >= 256 * kMiB;
  auto peak_el = text(std::format("{:>10}", pk_str));
  cells.push_back(high_peak ? (peak_el | color(Color::YellowLight)) : (peak_el | dim));

  cells.push_back(text("  "));
  if (l.show_cmdline) {
    cells.push_back(text(pad_right_utf8(label, l.name_width)));
    cells.push_back(text("  "));
    const std::string cmd =
        row.cmdline.empty() ? "-" : truncate_utf8(row.cmdline, l.cmd_width);
    cells.push_back(text(cmd) | dim);
  } else {
    const int avail_name = l.width - static_cast<int>(l.fixed_cols);
    const std::size_t max_name = static_cast<std::size_t>(std::max(8, avail_name));
    cells.push_back(text(truncate_utf8(label, max_name)));
  }
  return hbox(std::move(cells));
}

struct MetricRowSpec {
  std::string swatch;
  ftxui::Color swatch_color = ftxui::Color::Default;
  std::string metric;
  std::string value;
  ftxui::Color value_color = ftxui::Color::White;
  std::string share;
  ftxui::Element detail;
};

ftxui::Element build_metric_row(MetricRowSpec spec) {
  using namespace ftxui;

  constexpr int kMetricW = 11;
  constexpr int kValueW = 9;
  constexpr int kShareW = 7;

  Element swatch_el = spec.swatch.empty()
                          ? text("  ")
                          : (text(spec.swatch + " ") | color(spec.swatch_color));
  return hbox({
      text(" "),
      std::move(swatch_el),
      text(pad_right_utf8(spec.metric, kMetricW)) | color(Color::GrayLight),
      text("  "),
      text(std::format("{:>{}}", spec.value, kValueW)) | bold | color(spec.value_color),
      text("  "),
      text(std::format("{:>{}}", spec.share, kShareW)) | dim,
      text("  "),
      std::move(spec.detail) | flex,
      text(" "),
  });
}

ftxui::Element build_summary_table_header(const SystemMemory& sys) {
  using namespace ftxui;

  std::string total_note = std::format("{} total", humanize_bytes(sys.total));
  if (sys.free_bytes > 0) {
    total_note += std::format("  ({} free)", humanize_bytes(sys.free_bytes));
  }
  return hbox({
      text(" "),
      text("  "),
      text(pad_right_utf8("METRIC", 11)) | bold | color(Color::GrayLight),
      text("  "),
      text(std::format("{:>9}", "VALUE")) | bold | color(Color::White),
      text("  "),
      text(std::format("{:>7}", "SHARE")) | bold | color(Color::GrayLight),
      text("  "),
      text("DETAILS  ") | bold | color(Color::GrayLight),
      text(total_note) | dim,
      text(" "),
  });
}

void append_memory_rows(const SystemMemory& sys, ftxui::Elements& rows) {
  using namespace ftxui;

  constexpr const char* kSwatch = "\xe2\x96\xa0";  // ■

  rows.push_back(build_metric_row({
      kSwatch, Color::Magenta, "Wired", humanize_bytes(sys.wired), Color::Magenta,
      format_pct(sys.wired, sys.total), text("kernel & pinned") | dim,
  }));
  rows.push_back(build_metric_row({
      kSwatch, Color::Yellow, "Anonymous", humanize_bytes(sys.anonymous), Color::Yellow,
      format_pct(sys.anonymous, sys.total), text("app memory") | dim,
  }));

  std::string comp_detail = "compressed pages";
  if (const double ratio = sys.compression_ratio(); ratio > 0.0) {
    comp_detail = std::format("{:.1f}x ratio ({} uncompressed)",
                              ratio,
                              humanize_bytes(sys.compressor_uncompressed));
  }
  rows.push_back(build_metric_row({
      kSwatch, Color::Cyan, "Compressor", humanize_bytes(sys.compressor), Color::Cyan,
      format_pct(sys.compressor, sys.total), text(comp_detail) | dim,
  }));
  rows.push_back(build_metric_row({
      kSwatch, Color::Green, "File cache", humanize_bytes(sys.file_cache), Color::Green,
      format_pct(sys.file_cache, sys.total), text("reclaimable") | dim,
  }));
}

void append_swap_and_pressure_rows(const SystemMemory& sys, ftxui::Elements& rows) {
  using namespace ftxui;

  if (sys.swap.valid) {
    const Color sw_col = sys.swap.used >= 4ULL * kGiB
                             ? Color::RedLight
                             : (sys.swap.used > 0 ? Color::Yellow : Color::GrayLight);
    std::string detail = std::format("of {} total ({} free)",
                                     humanize_bytes(sys.swap.total),
                                     humanize_bytes(sys.swap.free_bytes));
    rows.push_back(build_metric_row({
        "", Color::Default, "Swap", humanize_bytes(sys.swap.used), sw_col,
        format_pct(sys.swap.used, sys.swap.total), text(detail) | dim,
    }));
  }

  if (sys.pressure != MemoryPressureLevel::Unknown) {
    const Color p_col = pressure_color(sys.pressure);
    const std::string free_pct =
        sys.memory_free_pct ? std::format("{}%", *sys.memory_free_pct) : "";
    const std::string detail = sys.memory_free_pct ? "free headroom" : "";
    rows.push_back(build_metric_row({
        "", Color::Default, "Pressure", std::string(pressure_label(sys.pressure)), p_col,
        free_pct, text(detail) | dim,
    }));
  }
}

void append_gpu_row(const GpuInfo& gpu, ftxui::Elements& rows) {
  using namespace ftxui;

  if (!gpu.valid && gpu.effective_ceiling() == 0) {
    return;
  }

  const auto ceiling = gpu.effective_ceiling();
  const std::string value = gpu.valid ? humanize_bytes(gpu.allocated) : "-";
  const std::string share =
      (gpu.valid && ceiling > 0) ? format_pct(gpu.allocated, ceiling) : "";

  Elements parts;
  if (gpu.valid) {
    parts.push_back(text(std::format("{} in use  ", humanize_bytes(gpu.in_use))) |
                    color(Color::BlueLight));
  }
  if (ceiling > 0) {
    std::string lim = std::format("limit {}", humanize_bytes(ceiling));
    if (auto head = gpu.headroom_bytes()) {
      lim += std::format(" ({} headroom)", humanize_bytes(*head));
    }
    parts.push_back(text(lim + "  ") | dim);
  }
  if (gpu.device_util_pct) {
    std::string util = std::format("{:.0f}% util", *gpu.device_util_pct);
    if (gpu.freq_mhz) util += std::format(" @ {:.0f}MHz", *gpu.freq_mhz);
    if (gpu.power_watts) util += std::format(" ({:.1f}W)", *gpu.power_watts);
    parts.push_back(text(util) | color(Color::Cyan));
  }

  rows.push_back(build_metric_row({
      "", Color::Default, "GPU", value, Color::BlueLight, share,
      flexbox(std::move(parts), FlexboxConfig().Set(FlexboxConfig::Wrap::Wrap)),
  }));
}

void append_cpu_and_disk_rows(const SystemSnapshot& snap,
                              const Config& cfg,
                              ftxui::Elements& rows) {
  using namespace ftxui;

  if (cfg.show_cpu && snap.cpu.valid) {
    const std::string watts =
        snap.cpu.power_watts ? std::format("{:.1f}W", *snap.cpu.power_watts) : "";
    Elements parts;
    for (const auto& t : snap.cpu.tiers) {
      std::string s = std::format("{} ({}c): {:.1f}%", t.name, t.core_count, t.util_pct);
      if (t.freq_mhz) s += std::format(" @ {:.0f}MHz", *t.freq_mhz);
      parts.push_back(text(s + "  ") | color(Color::Green));
    }
    if (snap.telemetry.dram_bandwidth_gbps) {
      parts.push_back(text(std::format("DRAM {:.1f} GB/s", *snap.telemetry.dram_bandwidth_gbps)) |
                      color(Color::Cyan));
    }
    rows.push_back(build_metric_row({
        "", Color::Default, "CPU", std::format("{:.1f}%", snap.cpu.total_util_pct),
        Color::GreenLight, watts,
        flexbox(std::move(parts), FlexboxConfig().Set(FlexboxConfig::Wrap::Wrap)),
    }));
  }

  if (cfg.show_disk && snap.disk.valid) {
    std::string val;
    std::string detail;
    if (snap.disk.read_bps || snap.disk.write_bps) {
      const double rb = snap.disk.read_bps.value_or(0.0);
      const double wb = snap.disk.write_bps.value_or(0.0);
      val = format_rate(rb + wb);
      detail = std::format("read {}  ·  write {}", format_rate(rb), format_rate(wb));
    } else {
      val = humanize_bytes(snap.disk.total_read_bytes + snap.disk.total_write_bytes);
      detail = std::format("read {}  ·  write {} (lifetime)",
                           humanize_bytes(snap.disk.total_read_bytes),
                           humanize_bytes(snap.disk.total_write_bytes));
    }
    rows.push_back(build_metric_row({
        "", Color::Default, "Disk", val, Color::MagentaLight, "", text(detail) | dim,
    }));
  }
}

}  // namespace

ftxui::Element build_memory_stacked_bar(const SystemMemory& sys, int bar_width) {
  using namespace ftxui;

  if (sys.total == 0 || bar_width < 10) {
    return text("");
  }

  const double denom = static_cast<double>(sys.total);
  const std::size_t cap = static_cast<std::size_t>(bar_width);
  auto cols_for = [&](std::uint64_t bytes) -> std::size_t {
    return static_cast<std::size_t>(
        std::lround((static_cast<double>(bytes) / denom) * static_cast<double>(bar_width)));
  };
  auto clamp_add = [&](std::size_t& seg, std::size_t& used) {
    seg = std::min(seg, cap - used);
    used += seg;
  };

  std::size_t w_wired = cols_for(sys.wired);
  std::size_t w_anon = cols_for(sys.anonymous);
  std::size_t w_comp = cols_for(sys.compressor);
  std::size_t w_cache = cols_for(sys.file_cache);
  std::size_t used = 0;
  clamp_add(w_wired, used);
  clamp_add(w_anon, used);
  clamp_add(w_comp, used);
  clamp_add(w_cache, used);
  const std::size_t w_free = cap - used;

  constexpr std::string_view kFull = "\xe2\x96\x88";  // █
  constexpr std::string_view kFree = "\xe2\x96\x91";  // ░
  return hbox({
      text(repeat_str(kFull, w_wired)) | color(Color::Magenta),
      text(repeat_str(kFull, w_anon)) | color(Color::Yellow),
      text(repeat_str(kFull, w_comp)) | color(Color::Cyan),
      text(repeat_str(kFull, w_cache)) | color(Color::Green),
      text(repeat_str(kFree, w_free)) | color(Color::GrayDark),
  });
}

ftxui::Element build_system_summary_element(const SystemSnapshot& snap,
                                            const Config& cfg,
                                            int width) {
  using namespace ftxui;

  const SystemMemory& sys = snap.memory;
  const int card_width = std::min(std::max(60, width), 104);

  Elements table_rows;
  table_rows.push_back(build_summary_table_header(sys));
  table_rows.push_back(separatorLight() | color(Color::GrayDark));
  append_memory_rows(sys, table_rows);

  Elements extra_rows;
  append_swap_and_pressure_rows(sys, extra_rows);
  append_gpu_row(sys.gpu, extra_rows);
  append_cpu_and_disk_rows(snap, cfg, extra_rows);
  if (!extra_rows.empty()) {
    table_rows.push_back(separatorLight() | color(Color::GrayDark));
    for (auto& row : extra_rows) {
      table_rows.push_back(std::move(row));
    }
  }

  Element card = vbox(std::move(table_rows)) | borderStyled(LIGHT, Color::GrayDark) |
                 size(WIDTH, EQUAL, card_width);

  if (cfg.short_mode) {
    return card;
  }

  return vbox({
      build_memory_stacked_bar(sys, card_width),
      std::move(card),
  });
}

std::string TableRenderer::render(const QuerySummary& summary,
                                  const SystemSnapshot& snap,
                                  const Config& cfg,
                                  const RenderContext& ctx) const {
  using namespace ftxui;

  const TableLayout layout = compute_layout(summary, snap.memory, cfg, ctx.width);
  Elements sections;

  if (cfg.short_mode) {
    if (snap.memory.valid) {
      sections.push_back(build_system_summary_element(snap, cfg, layout.width));
    }
  } else {
    if (!cfg.sum_only) {
      std::uint64_t max_row_fp = 1;
      for (const auto& row : summary.rows) {
        max_row_fp = std::max(max_row_fp, row.effective_footprint());
      }

      Elements table_rows;
      table_rows.push_back(build_header(layout, cfg.group));
      table_rows.push_back(separatorLight() | color(Color::GrayDark));
      for (const auto& row : summary.rows) {
        table_rows.push_back(build_row(row, max_row_fp, snap.memory, layout, cfg));
      }
      sections.push_back(vbox(std::move(table_rows)));
      sections.push_back(text(""));
    }

    sections.push_back(paragraph(format_summary_line(summary, cfg)) | bold);

    if (!cfg.sum_only && cfg.show_system && snap.memory.valid) {
      sections.push_back(text(""));
      sections.push_back(build_system_summary_element(snap, cfg, layout.width));
    }
  }

  Element doc = vbox(std::move(sections));
  Screen screen = Screen::Create(Dimension::Fixed(layout.width), Dimension::Fit(doc, true));
  Render(screen, doc);

  std::string out = trim_trailing_line_spaces(screen.ToString());
  if (!out.empty() && out.back() != '\n') {
    out.push_back('\n');
  }
  return out;
}

}  // namespace simon
