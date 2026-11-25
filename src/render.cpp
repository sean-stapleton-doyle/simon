#include "render.hpp"

#include "json_render.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/screen.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <sstream>

namespace realmem {

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kGiB = 1024ULL * kMiB;

struct TableLayout {
  int width = 80;
  bool show_cmdline = false;
  bool show_gpu = false;
  bool show_pct = false;
  bool show_bars = false;
  std::size_t fixed_cols = 0;
  std::size_t name_width = 0;
  std::size_t cmd_width = 0;
};

bool use_rich_output(const Config& cfg, const RenderContext& ctx) {
  if (cfg.plain) {
    return false;
  }
  switch (cfg.color) {
    case ColorMode::Always:
      return true;
    case ColorMode::Never:
      return false;
    case ColorMode::Auto:
      return ctx.is_tty;
  }
  return false;
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

ftxui::Color footprint_color(std::uint64_t bytes) {
  if (bytes >= 4ULL * kGiB) {
    return ftxui::Color::RedLight;
  }
  if (bytes >= 1ULL * kGiB) {
    return ftxui::Color::Yellow;
  }
  if (bytes >= 256ULL * kMiB) {
    return ftxui::Color::Cyan;
  }
  return ftxui::Color::Default;
}

std::string repeat_str(std::string_view unit, std::size_t n) {
  std::string out;
  out.reserve(unit.size() * n);
  for (std::size_t i = 0; i < n; ++i) {
    out.append(unit);
  }
  return out;
}

std::size_t measure_name_width(const QuerySummary& summary, bool grouped, std::size_t cap) {
  std::size_t width =
      grouped ? std::string_view("APPLICATION").size() : std::string_view("COMMAND").size();
  for (const auto& row : summary.rows) {
    width = std::max(width, utf8_length(row.name));
  }
  return std::min(width, cap);
}

TableLayout compute_plain_layout(const QuerySummary& summary, const Config& cfg, int width) {
  TableLayout layout;
  layout.width = width;
  layout.show_cmdline = cfg.long_format && !cfg.group;
  layout.show_gpu = cfg.show_gpu;
  // PID/PROCS(7) + FP(10) + [GPU(10)] + PEAK(10) + gaps
  layout.fixed_cols = 7 + 2 + 10 + 2 + (layout.show_gpu ? 12 : 0) + 10 + 2;
  layout.name_width = measure_name_width(summary, cfg.group, 24);
  const int rem = width - static_cast<int>(layout.fixed_cols) - static_cast<int>(layout.name_width) - 2;
  layout.cmd_width = static_cast<std::size_t>(std::max(20, rem));
  return layout;
}

TableLayout compute_ftxui_layout(const QuerySummary& summary,
                                 const SystemMemory& sys,
                                 const Config& cfg,
                                 int raw_width) {
  TableLayout layout;
  layout.width = std::max(60, raw_width);
  layout.show_cmdline = cfg.long_format && !cfg.group;
  layout.show_gpu = cfg.show_gpu;

  const int gpu_extra = layout.show_gpu ? 12 : 0;
  layout.show_bars =
      cfg.show_bars && (layout.show_cmdline ? layout.width >= 105 + gpu_extra
                                            : layout.width >= 74 + gpu_extra);
  layout.show_pct =
      sys.valid && sys.total > 0 &&
      (layout.show_cmdline ? layout.width >= 88 + gpu_extra : layout.width >= 60 + gpu_extra);

  layout.fixed_cols = 7 + 2 + 10 + 2 + 10 + 2;
  if (layout.show_pct) {
    layout.fixed_cols += 8;
  }
  if (layout.show_bars) {
    layout.fixed_cols += 12;
  }
  if (layout.show_gpu) {
    layout.fixed_cols += 12;
  }

  layout.name_width = measure_name_width(summary, cfg.group, layout.show_cmdline ? 24 : 36);
  if (layout.show_cmdline) {
    const std::size_t avail = (static_cast<std::size_t>(layout.width) > layout.fixed_cols + 2)
                                  ? (static_cast<std::size_t>(layout.width) - layout.fixed_cols - 2)
                                  : 24;
    const std::size_t max_name = (avail > 20) ? std::min<std::size_t>(24, avail - 16) : 8;
    layout.name_width = std::min(layout.name_width, std::max<std::size_t>(8, max_name));
    layout.cmd_width = (avail > layout.name_width) ? (avail - layout.name_width) : 8;
  }
  return layout;
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

std::string format_system_plain(const SystemMemory& sys) {
  std::string out = std::format(
      "system: {} total | {} wired | {} anonymous | {} compressor | {} file cache (reclaimable)",
      humanize_bytes(sys.total),
      humanize_bytes(sys.wired),
      humanize_bytes(sys.anonymous),
      humanize_bytes(sys.compressor),
      humanize_bytes(sys.file_cache));
  if (sys.gpu.valid) {
    out += std::format("\ngpu: {} alloc ({} in use)",
                       humanize_bytes(sys.gpu.allocated),
                       humanize_bytes(sys.gpu.in_use));
  }
  return out;
}

std::string format_plain_header(const TableLayout& layout, bool grouped) {
  const char* first_col = grouped ? "PROCS" : "PID";
  const char* name_col = grouped ? "APPLICATION" : "COMMAND";
  std::string line = std::format("{:>7}  {:>10}", first_col, "FOOTPRINT");
  if (layout.show_gpu) {
    line += std::format("  {:>10}", "GPU");
  }
  line += std::format("  {:>10}  ", "PEAK");
  if (layout.show_cmdline) {
    line += std::format("{}  COMMAND LINE", pad_right_utf8(name_col, layout.name_width));
  } else {
    line += name_col;
  }
  return line;
}

std::string format_plain_row(const DisplayRow& row, const TableLayout& layout, bool grouped) {
  const std::int64_t id_or_cnt =
      grouped ? static_cast<std::int64_t>(row.proc_count) : static_cast<std::int64_t>(row.pid);
  const std::string pk = row.peak ? humanize_bytes(row.peak) : "-";

  std::string line = std::format("{:>7}  {:>10}", id_or_cnt, humanize_bytes(row.footprint));
  if (layout.show_gpu) {
    line += std::format("  {:>10}", format_gpu_value(row.gpu_footprint));
  }
  line += std::format("  {:>10}  ", pk);

  if (layout.show_cmdline) {
    const std::string cmd =
        row.cmdline.empty() ? "-" : truncate_utf8(row.cmdline, layout.cmd_width);
    line += std::format("{}  {}", pad_right_utf8(row.name, layout.name_width), cmd);
  } else {
    line += row.name;
  }
  return line;
}

std::string render_plain(const QuerySummary& summary,
                         const SystemMemory& sys,
                         const Config& cfg,
                         const RenderContext& ctx) {
  std::ostringstream out;
  if (!cfg.sum_only) {
    const TableLayout layout = compute_plain_layout(summary, cfg, ctx.width);
    out << format_plain_header(layout, cfg.group) << '\n';
    for (const auto& row : summary.rows) {
      out << format_plain_row(row, layout, cfg.group) << '\n';
    }
    out << '\n';
  }

  out << format_summary_line(summary, cfg) << '\n';
  if (!cfg.sum_only && cfg.show_system && sys.valid) {
    out << '\n' << format_system_plain(sys) << '\n';
  }
  return out.str();
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

ftxui::Element build_ftxui_header(const TableLayout& layout, bool grouped) {
  using namespace ftxui;

  const char* first_col = grouped ? "PROCS" : "PID";
  const char* name_col = grouped ? "APPLICATION" : "COMMAND";

  Elements cells;
  cells.push_back(text(std::format("{:>7}", first_col)) | bold | color(Color::GrayLight));
  cells.push_back(text("  "));
  cells.push_back(text(std::format("{:>10}", "FOOTPRINT")) | bold | color(Color::White));
  if (layout.show_pct) {
    cells.push_back(text("  "));
    cells.push_back(text(std::format("{:>6}", "%RAM")) | bold | color(Color::GrayLight));
  }
  if (layout.show_bars) {
    cells.push_back(text("  "));
    cells.push_back(text(std::format("{:<10}", "USAGE")) | bold | color(Color::GrayLight));
  }
  if (layout.show_gpu) {
    cells.push_back(text("  "));
    cells.push_back(text(std::format("{:>10}", "GPU")) | bold | color(Color::BlueLight));
  }
  cells.push_back(text("  "));
  cells.push_back(text(std::format("{:>10}", "PEAK")) | bold | color(Color::GrayLight));
  cells.push_back(text("  "));
  if (layout.show_cmdline) {
    cells.push_back(text(pad_right_utf8(name_col, layout.name_width)) | bold | color(Color::White));
    cells.push_back(text("  "));
    cells.push_back(text(truncate_utf8("COMMAND LINE", layout.cmd_width)) | bold |
                    color(Color::GrayLight));
  } else {
    cells.push_back(text(name_col) | bold | color(Color::White));
  }
  return hbox(std::move(cells));
}

ftxui::Element build_ftxui_row(const DisplayRow& row,
                               std::uint64_t max_row_fp,
                               const SystemMemory& sys,
                               const TableLayout& layout,
                               bool grouped) {
  using namespace ftxui;

  const std::int64_t id_or_cnt =
      grouped ? static_cast<std::int64_t>(row.proc_count) : static_cast<std::int64_t>(row.pid);
  const Color fp_col = footprint_color(row.footprint);
  const std::string pk_str = row.peak ? humanize_bytes(row.peak) : "-";

  Elements cells;
  cells.push_back(text(std::format("{:>7}", id_or_cnt)) | dim);
  cells.push_back(text("  "));
  cells.push_back(text(std::format("{:>10}", humanize_bytes(row.footprint))) | color(fp_col) |
                  bold);

  if (layout.show_pct) {
    cells.push_back(text("  "));
    cells.push_back(text(std::format("{:>6}", format_pct(row.footprint, sys.total))) | dim);
  }
  if (layout.show_bars) {
    const float ratio = static_cast<float>(std::clamp(
        static_cast<double>(row.footprint) / static_cast<double>(max_row_fp), 0.0, 1.0));
    cells.push_back(text("  "));
    cells.push_back(gauge(ratio) | size(WIDTH, EQUAL, 10) | color(fp_col));
  }
  if (layout.show_gpu) {
    cells.push_back(text("  "));
    auto gpu_el = text(std::format("{:>10}", format_gpu_value(row.gpu_footprint)));
    const bool active_gpu = row.gpu_footprint.has_value() && *row.gpu_footprint > 0;
    cells.push_back(active_gpu ? (gpu_el | color(Color::BlueLight)) : (gpu_el | dim));
  }

  cells.push_back(text("  "));
  const bool high_peak = row.peak > 0 && row.peak >= row.footprint * 2 && row.peak >= 256 * kMiB;
  auto peak_el = text(std::format("{:>10}", pk_str));
  cells.push_back(high_peak ? (peak_el | color(Color::YellowLight)) : (peak_el | dim));

  cells.push_back(text("  "));
  if (layout.show_cmdline) {
    cells.push_back(text(pad_right_utf8(row.name, layout.name_width)));
    cells.push_back(text("  "));
    const std::string cmd =
        row.cmdline.empty() ? "-" : truncate_utf8(row.cmdline, layout.cmd_width);
    cells.push_back(text(cmd) | dim);
  } else {
    const int avail_name = layout.width - static_cast<int>(layout.fixed_cols);
    const std::size_t max_name = static_cast<std::size_t>(std::max(8, avail_name));
    cells.push_back(text(truncate_utf8(row.name, max_name)));
  }

  return hbox(std::move(cells));
}

ftxui::Element build_system_bar(const SystemMemory& sys, int bar_width) {
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

ftxui::Element build_system_section(const SystemMemory& sys, int width) {
  using namespace ftxui;

  Elements rows;
  rows.push_back(build_system_bar(sys, std::min(width, 96)));

  Elements chips = {
      hbox({text("system: ") | bold,
            text(humanize_bytes(sys.total)) | bold | color(Color::White),
            text(" total  ")}),
      hbox({text("\xe2\x96\xa0 ") | color(Color::Magenta),
            text(humanize_bytes(sys.wired)) | color(Color::Magenta),
            text(" wired  ")}),
      hbox({text("\xe2\x96\xa0 ") | color(Color::Yellow),
            text(humanize_bytes(sys.anonymous)) | color(Color::Yellow),
            text(" anonymous  ")}),
      hbox({text("\xe2\x96\xa0 ") | color(Color::Cyan),
            text(humanize_bytes(sys.compressor)) | color(Color::Cyan),
            text(" compressor  ")}),
      hbox({text("\xe2\x96\xa0 ") | color(Color::Green),
            text(humanize_bytes(sys.file_cache)) | color(Color::Green),
            text(" file cache (reclaimable)")}),
  };
  rows.push_back(flexbox(std::move(chips), FlexboxConfig().Set(FlexboxConfig::Wrap::Wrap)));

  if (sys.gpu.valid) {
    rows.push_back(hbox({
        text("gpu: ") | bold,
        text(humanize_bytes(sys.gpu.allocated)) | bold | color(Color::BlueLight),
        text(" alloc ("),
        text(humanize_bytes(sys.gpu.in_use)) | color(Color::BlueLight),
        text(" in use)"),
    }));
  }

  return vbox(std::move(rows));
}

std::string render_ftxui(const QuerySummary& summary,
                         const SystemMemory& sys,
                         const Config& cfg,
                         const RenderContext& ctx) {
  using namespace ftxui;

  const TableLayout layout = compute_ftxui_layout(summary, sys, cfg, ctx.width);
  Elements sections;

  if (!cfg.sum_only) {
    std::uint64_t max_row_fp = 1;
    for (const auto& row : summary.rows) {
      max_row_fp = std::max(max_row_fp, row.footprint);
    }

    Elements table_rows;
    table_rows.push_back(build_ftxui_header(layout, cfg.group));
    table_rows.push_back(separatorLight() | color(Color::GrayDark));
    for (const auto& row : summary.rows) {
      table_rows.push_back(build_ftxui_row(row, max_row_fp, sys, layout, cfg.group));
    }
    sections.push_back(vbox(std::move(table_rows)));
    sections.push_back(text(""));
  }

  sections.push_back(paragraph(format_summary_line(summary, cfg)) | bold);

  if (!cfg.sum_only && cfg.show_system && sys.valid) {
    sections.push_back(text(""));
    sections.push_back(build_system_section(sys, layout.width));
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

}  // namespace

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

std::string render_output(const QuerySummary& summary,
                          const SystemMemory& sys,
                          const Config& cfg,
                          const RenderContext& ctx) {
  switch (cfg.output) {
    case OutputFormat::Json:
      return render_json(summary, sys, cfg);
    case OutputFormat::Plain:
      return render_plain(summary, sys, cfg, ctx);
    case OutputFormat::Table:
      return render_ftxui(summary, sys, cfg, ctx);
    case OutputFormat::Auto:
      return use_rich_output(cfg, ctx) ? render_ftxui(summary, sys, cfg, ctx)
                                       : render_plain(summary, sys, cfg, ctx);
  }
  return render_plain(summary, sys, cfg, ctx);
}

}  // namespace realmem
