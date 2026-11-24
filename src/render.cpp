#include "render.hpp"

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
  if (len >= width) {
    return truncated;
  }
  return truncated + std::string(width - len, ' ');
}

std::string format_pct(std::uint64_t part, std::uint64_t total) {
  if (total == 0) {
    return "-";
  }
  const double pct = (static_cast<double>(part) * 100.0) / static_cast<double>(total);
  return std::format("{:.1f}%", pct);
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

std::string format_summary_line(const QuerySummary& summary, const Config& cfg) {
  if (cfg.pattern) {
    if (summary.matched_procs == 0) {
      return std::format("no processes match /{}/", *cfg.pattern);
    }
    std::string line = std::format("{} process{} matching /{}/",
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
    return line;
  }

  std::string line = std::format("{} of {} processes readable",
                                 summary.total_readable,
                                 summary.total_pids);
  if (cfg.group) {
    line += std::format(" across {} apps", summary.total_groups);
  }
  line += std::format(", {} total footprint", humanize_bytes(summary.total_footprint));
  return line;
}

std::string format_system_plain(const SystemMemory& sys) {
  return std::format(
      "system: {} total | {} wired | {} anonymous | {} compressor | {} file cache (reclaimable)",
      humanize_bytes(sys.total),
      humanize_bytes(sys.wired),
      humanize_bytes(sys.anonymous),
      humanize_bytes(sys.compressor),
      humanize_bytes(sys.file_cache));
}

std::string render_plain(const QuerySummary& summary,
                         const SystemMemory& sys,
                         const Config& cfg,
                         const RenderContext& ctx) {
  std::ostringstream out;
  const bool show_cmdline = cfg.long_format && !cfg.group;

  if (!cfg.sum_only) {
    std::size_t namew = cfg.group ? std::string_view("APPLICATION").size()
                                  : std::string_view("COMMAND").size();
    if (show_cmdline) {
      for (const auto& row : summary.rows) {
        namew = std::max(namew, utf8_length(row.name));
      }
      namew = std::min<std::size_t>(namew, 24);
    }

    const int numeric_cols = 7 + 2 + 10 + 2 + 10 + 2;  // PID/PROCS + FOOTPRINT + PEAK + gaps
    const int cmdw = std::max(20, ctx.width - numeric_cols - static_cast<int>(namew) - 2);

    const char* first_col = cfg.group ? "PROCS" : "PID";
    const char* name_col = cfg.group ? "APPLICATION" : "COMMAND";

    if (show_cmdline) {
      out << std::format("{:>7}  {:>10}  {:>10}  {}  {}\n",
                         first_col,
                         "FOOTPRINT",
                         "PEAK",
                         pad_right_utf8(name_col, namew),
                         "COMMAND LINE");
    } else {
      out << std::format("{:>7}  {:>10}  {:>10}  {}\n", first_col, "FOOTPRINT", "PEAK", name_col);
    }

    for (const auto& row : summary.rows) {
      const std::string fp = humanize_bytes(row.footprint);
      const std::string pk = row.peak ? humanize_bytes(row.peak) : "-";
      const std::int64_t id_or_cnt =
          cfg.group ? static_cast<std::int64_t>(row.proc_count) : static_cast<std::int64_t>(row.pid);

      if (show_cmdline) {
        const std::string cmd =
            row.cmdline.empty() ? "-" : truncate_utf8(row.cmdline, static_cast<std::size_t>(cmdw));
        out << std::format("{:>7}  {:>10}  {:>10}  {}  {}\n",
                           id_or_cnt,
                           fp,
                           pk,
                           pad_right_utf8(row.name, namew),
                           cmd);
      } else {
        out << std::format("{:>7}  {:>10}  {:>10}  {}\n", id_or_cnt, fp, pk, row.name);
      }
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

ftxui::Element build_system_bar(const SystemMemory& sys, int bar_width) {
  using namespace ftxui;

  if (sys.total == 0 || bar_width < 10) {
    return text("");
  }

  const double denom = static_cast<double>(sys.total);
  auto cols_for = [&](std::uint64_t bytes) -> std::size_t {
    return static_cast<std::size_t>(
        std::lround((static_cast<double>(bytes) / denom) * static_cast<double>(bar_width)));
  };

  std::size_t w_wired = cols_for(sys.wired);
  std::size_t w_anon = cols_for(sys.anonymous);
  std::size_t w_comp = cols_for(sys.compressor);
  std::size_t w_cache = cols_for(sys.file_cache);

  const std::size_t cap = static_cast<std::size_t>(bar_width);
  auto clamp_add = [&](std::size_t& seg, std::size_t& used) {
    if (used + seg > cap) {
      seg = cap - used;
    }
    used += seg;
  };

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

std::string render_ftxui(const QuerySummary& summary,
                         const SystemMemory& sys,
                         const Config& cfg,
                         const RenderContext& ctx) {
  using namespace ftxui;

  const int width = std::max(60, ctx.width);
  const bool show_cmdline = cfg.long_format && !cfg.group;
  const bool show_bars = cfg.show_bars && (show_cmdline ? width >= 105 : width >= 68);
  const bool show_pct =
      sys.valid && sys.total > 0 && (show_cmdline ? width >= 88 : width >= 60);

  Elements sections;

  if (!cfg.sum_only) {
    const std::size_t bar_w = show_bars ? 10 : 0;
    const std::size_t pct_w = show_pct ? 6 : 0;

    // Fixed column widths: PID/PROCS(7) + FP(10) + [PCT(6)] + [BAR(10)] + PEAK(10) + gaps
    std::size_t fixed_cols = 7 + 2 + 10 + 2 + 10 + 2;
    if (show_pct) {
      fixed_cols += pct_w + 2;
    }
    if (show_bars) {
      fixed_cols += bar_w + 2;
    }

    std::size_t namew = cfg.group ? std::string_view("APPLICATION").size()
                                  : std::string_view("COMMAND").size();
    for (const auto& row : summary.rows) {
      namew = std::max(namew, utf8_length(row.name));
    }
    namew = std::min<std::size_t>(namew, show_cmdline ? 24 : 36);

    std::size_t cmdw = 0;
    if (show_cmdline) {
      const std::size_t avail =
          (static_cast<std::size_t>(width) > fixed_cols + 2)
              ? (static_cast<std::size_t>(width) - fixed_cols - 2)
              : 24;
      const std::size_t max_namew = (avail > 20) ? std::min<std::size_t>(24, avail - 16) : 8;
      namew = std::min(namew, std::max<std::size_t>(8, max_namew));
      cmdw = (avail > namew) ? (avail - namew) : 8;
    }

    const char* first_col = cfg.group ? "PROCS" : "PID";
    const char* name_col = cfg.group ? "APPLICATION" : "COMMAND";

    // Header row
    Elements header_cells;
    header_cells.push_back(text(std::format("{:>7}", first_col)) | bold | color(Color::GrayLight));
    header_cells.push_back(text("  "));
    header_cells.push_back(text(std::format("{:>10}", "FOOTPRINT")) | bold | color(Color::White));
    if (show_pct) {
      header_cells.push_back(text("  "));
      header_cells.push_back(text(std::format("{:>6}", "%RAM")) | bold | color(Color::GrayLight));
    }
    if (show_bars) {
      header_cells.push_back(text("  "));
      header_cells.push_back(text(std::format("{:<10}", "USAGE")) | bold | color(Color::GrayLight));
    }
    header_cells.push_back(text("  "));
    header_cells.push_back(text(std::format("{:>10}", "PEAK")) | bold | color(Color::GrayLight));
    header_cells.push_back(text("  "));
    if (show_cmdline) {
      header_cells.push_back(text(pad_right_utf8(name_col, namew)) | bold | color(Color::White));
      header_cells.push_back(text("  "));
      header_cells.push_back(text(truncate_utf8("COMMAND LINE", cmdw)) | bold |
                             color(Color::GrayLight));
    } else {
      header_cells.push_back(text(name_col) | bold | color(Color::White));
    }

    Elements table_rows;
    table_rows.push_back(hbox(std::move(header_cells)));
    table_rows.push_back(separatorLight() | color(Color::GrayDark));

    std::uint64_t max_row_fp = 1;
    for (const auto& row : summary.rows) {
      max_row_fp = std::max(max_row_fp, row.footprint);
    }

    for (const auto& row : summary.rows) {
      const std::int64_t id_or_cnt =
          cfg.group ? static_cast<std::int64_t>(row.proc_count) : static_cast<std::int64_t>(row.pid);
      const std::string fp_str = humanize_bytes(row.footprint);
      const std::string pk_str = row.peak ? humanize_bytes(row.peak) : "-";
      const Color fp_col = footprint_color(row.footprint);

      Elements cells;
      cells.push_back(text(std::format("{:>7}", id_or_cnt)) | dim);
      cells.push_back(text("  "));
      cells.push_back(text(std::format("{:>10}", fp_str)) | color(fp_col) | bold);

      if (show_pct) {
        cells.push_back(text("  "));
        cells.push_back(text(std::format("{:>6}", format_pct(row.footprint, sys.total))) | dim);
      }

      if (show_bars) {
        const float ratio = static_cast<float>(
            std::clamp(static_cast<double>(row.footprint) / static_cast<double>(max_row_fp),
                       0.0,
                       1.0));
        cells.push_back(text("  "));
        cells.push_back(gauge(ratio) | size(WIDTH, EQUAL, static_cast<int>(bar_w)) | color(fp_col));
      }

      cells.push_back(text("  "));
      const bool high_peak = row.peak > 0 && row.peak >= row.footprint * 2 && row.peak >= 256 * kMiB;
      auto peak_el = text(std::format("{:>10}", pk_str));
      cells.push_back(high_peak ? (peak_el | color(Color::YellowLight)) : (peak_el | dim));

      cells.push_back(text("  "));
      if (show_cmdline) {
        cells.push_back(text(pad_right_utf8(row.name, namew)));
        cells.push_back(text("  "));
        const std::string cmd = row.cmdline.empty() ? "-" : truncate_utf8(row.cmdline, cmdw);
        cells.push_back(text(cmd) | dim);
      } else {
        const std::size_t max_name =
            static_cast<std::size_t>(std::max(16, width - static_cast<int>(fixed_cols)));
        cells.push_back(text(truncate_utf8(row.name, max_name)));
      }

      table_rows.push_back(hbox(std::move(cells)));
    }

    sections.push_back(vbox(std::move(table_rows)));
    sections.push_back(text(""));
  }

  // Summary line
  sections.push_back(text(format_summary_line(summary, cfg)) | bold);

  // System memory breakdown with stacked visual bar
  if (!cfg.sum_only && cfg.show_system && sys.valid) {
    sections.push_back(text(""));
    const int bar_width = std::min(width, 96);
    sections.push_back(build_system_bar(sys, bar_width));

    Elements legend_chips = {
        hbox({
            text("system: ") | bold,
            text(humanize_bytes(sys.total)) | bold | color(Color::White),
            text(" total  "),
        }),
        hbox({
            text("\xe2\x96\xa0 ") | color(Color::Magenta),
            text(humanize_bytes(sys.wired)) | color(Color::Magenta),
            text(" wired  "),
        }),
        hbox({
            text("\xe2\x96\xa0 ") | color(Color::Yellow),
            text(humanize_bytes(sys.anonymous)) | color(Color::Yellow),
            text(" anonymous  "),
        }),
        hbox({
            text("\xe2\x96\xa0 ") | color(Color::Cyan),
            text(humanize_bytes(sys.compressor)) | color(Color::Cyan),
            text(" compressor  "),
        }),
        hbox({
            text("\xe2\x96\xa0 ") | color(Color::Green),
            text(humanize_bytes(sys.file_cache)) | color(Color::Green),
            text(" file cache (reclaimable)"),
        }),
    };
    sections.push_back(
        flexbox(std::move(legend_chips), FlexboxConfig().Set(FlexboxConfig::Wrap::Wrap)));
  }

  Element doc = vbox(std::move(sections));
  Screen screen = Screen::Create(Dimension::Fixed(width), Dimension::Fit(doc, true));
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
    if (i + seq_len > text.size()) {
      seq_len = text.size() - i;
    }

    ++cols;
    if (cols == max_cols) {
      cut_bytes = i;  // position right before the max_cols-th code point
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
  if (use_rich_output(cfg, ctx)) {
    return render_ftxui(summary, sys, cfg, ctx);
  }
  return render_plain(summary, sys, cfg, ctx);
}

}  // namespace realmem
