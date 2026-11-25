#include "tui/tui_app.hpp"

#include "render/format_utils.hpp"
#include "render/table_renderer.hpp"
#include "tui/tui_state.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <thread>

namespace realmem {

namespace {

ftxui::Element build_top_bar(const TuiState& state, int interval_ms) {
  using namespace ftxui;

  const auto& gpu = state.snapshot().memory.gpu;
  const std::string chip = !gpu.model.empty() ? gpu.model : state.snapshot().cpu.brand;
  const std::string arrow = state.reverse() ? "\xe2\x96\xb2" : "\xe2\x96\xbc";  // ▲ / ▼

  Elements items;
  items.push_back(text(" realmem ") | bold | inverted);
  if (!chip.empty()) {
    items.push_back(text(" " + chip + " ") | color(Color::Cyan));
  }
  items.push_back(text(std::format(" mode:{} ", to_string(state.view_mode()))) | bold |
                  color(Color::Yellow));
  items.push_back(text(std::format(" sort:{}{} ", to_string(state.sort_field()), arrow)) |
                  color(Color::White));
  items.push_back(text(std::format(" {}ms ", interval_ms)) | dim);
  if (state.paused()) {
    items.push_back(text(" PAUSED ") | bold | color(Color::Black) | bgcolor(Color::Yellow));
  }
  return hbox(std::move(items));
}

ftxui::Element build_sparkline_strip(const TuiState& state) {
  using namespace ftxui;

  const auto& mem = state.snapshot().memory;
  const double gpu_max = static_cast<double>(mem.gpu.effective_ceiling());
  const double swap_max = static_cast<double>(mem.swap.total);

  Elements chips;
  if (!state.mem_history().empty()) {
    chips.push_back(hbox({
        text("RAM ") | dim,
        text(render_sparkline(state.mem_history(), 14, 100.0)) | color(Color::Yellow),
        text("  "),
    }));
  }
  if (!state.swap_history().empty()) {
    chips.push_back(hbox({
        text("SWAP ") | dim,
        text(render_sparkline(state.swap_history(), 14, swap_max)) | color(Color::RedLight),
        text("  "),
    }));
  }
  if (!state.gpu_history().empty()) {
    chips.push_back(hbox({
        text("GPU ") | dim,
        text(render_sparkline(state.gpu_history(), 14, gpu_max)) | color(Color::BlueLight),
        text("  "),
    }));
  }
  if (!state.cpu_history().empty()) {
    chips.push_back(hbox({
        text("CPU ") | dim,
        text(render_sparkline(state.cpu_history(), 14, 100.0)) | color(Color::GreenLight),
    }));
  }
  return flexbox(std::move(chips), FlexboxConfig().Set(FlexboxConfig::Wrap::Wrap));
}

ftxui::Element build_search_bar(const TuiState& state) {
  using namespace ftxui;

  if (!state.searching() && state.filter_text().empty()) {
    return text("");
  }

  Elements items;
  items.push_back(text(" / ") | bold | color(Color::Yellow));
  items.push_back(text(state.filter_text()) | color(Color::White));
  if (state.searching()) {
    items.push_back(text("_") | blink | color(Color::Yellow));
  }
  if (state.filter_error()) {
    items.push_back(text("  (" + *state.filter_error() + ")") | color(Color::RedLight));
  } else if (!state.filter_text().empty()) {
    items.push_back(text(std::format("  [{} rows]", state.summary().rows.size())) | dim);
  }
  return hbox(std::move(items));
}

ftxui::Element build_process_table(const TuiState& state,
                                   int width,
                                   std::size_t viewport_rows) {
  using namespace ftxui;

  const Config cfg = state.active_config();
  const auto& rows = state.summary().rows;
  const bool grouped = cfg.group;
  const bool show_gpu = cfg.show_gpu;

  const char* first_col = grouped ? "PROCS" : "PID";
  const char* name_col = grouped ? "APPLICATION" : "COMMAND";

  std::size_t fixed = 7 + 2 + 10 + 2 + 7 + 2 + 10 + 2 + 10 + 2;
  if (show_gpu) fixed += 12;
  const std::size_t max_name =
      static_cast<std::size_t>(std::max(12, width - static_cast<int>(fixed) - 2));

  Elements header;
  header.push_back(text(std::format("{:>7}", first_col)) | bold | color(Color::GrayLight));
  header.push_back(text("  "));
  header.push_back(text(std::format("{:>10}", "FOOTPRINT")) | bold | color(Color::White));
  if (show_gpu) {
    header.push_back(text("  "));
    header.push_back(text(std::format("{:>10}", "GPU")) | bold | color(Color::BlueLight));
  }
  header.push_back(text("  "));
  header.push_back(text(std::format("{:>7}", "CPU")) | bold | color(Color::GreenLight));
  header.push_back(text("  "));
  header.push_back(text(std::format("{:>10}", "DISK")) | bold | color(Color::MagentaLight));
  header.push_back(text("  "));
  header.push_back(text(std::format("{:>10}", "PEAK")) | bold | color(Color::GrayLight));
  header.push_back(text("  "));
  header.push_back(text(name_col) | bold | color(Color::White));

  Elements lines;
  lines.push_back(hbox(std::move(header)));
  lines.push_back(separatorLight() | color(Color::GrayDark));

  const std::size_t start = state.scroll_offset();
  const std::size_t end = std::min(rows.size(), start + viewport_rows);
  for (std::size_t i = start; i < end; ++i) {
    const DisplayRow& row = rows[i];
    const std::int64_t id_or_cnt =
        grouped ? static_cast<std::int64_t>(row.proc_count) : static_cast<std::int64_t>(row.pid);
    const std::string fp = humanize_bytes(row.effective_footprint());
    const std::string pk = row.peak ? humanize_bytes(row.peak) : "-";
    const std::string label = truncate_utf8(format_row_label(row, cfg), max_name);

    Elements cells;
    cells.push_back(text(std::format("{:>7}", id_or_cnt)) | dim);
    cells.push_back(text("  "));
    cells.push_back(text(std::format("{:>10}", fp)) | bold);
    if (show_gpu) {
      cells.push_back(text("  "));
      cells.push_back(text(std::format("{:>10}", format_gpu_value(row.gpu_footprint))) |
                      color(Color::BlueLight));
    }
    cells.push_back(text("  "));
    cells.push_back(text(std::format("{:>7}", format_cpu_value(row.cpu_pct))) |
                    color(Color::GreenLight));
    cells.push_back(text("  "));
    cells.push_back(text(std::format("{:>10}", format_disk_value(row))) |
                    color(Color::MagentaLight));
    cells.push_back(text("  "));
    cells.push_back(text(std::format("{:>10}", pk)) | dim);
    cells.push_back(text("  "));
    cells.push_back(text(label));

    Element line = hbox(std::move(cells));
    if (i == state.selected_index()) {
      line = line | inverted;
    }
    lines.push_back(std::move(line));
  }

  return vbox(std::move(lines)) | flex;
}

ftxui::Element build_help_bar(const TuiState& state) {
  using namespace ftxui;
  if (state.searching()) {
    return text(" [Enter] apply filter   [Esc] cancel & clear   [Backspace] delete ") | dim;
  }
  return text(
             " [/] filter  [g] group  [t] tree  [s] sort  [r] reverse  [p] pause  [j/k] scroll  "
             "[q] quit  (read-only) ") |
         dim;
}

bool handle_tui_event(const ftxui::Event& event,
                      TuiState& state,
                      std::size_t viewport_rows,
                      ftxui::ScreenInteractive& screen) {
  using namespace ftxui;

  if (state.searching()) {
    if (event == Event::Return) {
      state.end_search(/*clear_filter=*/false);
      return true;
    }
    if (event == Event::Escape) {
      state.end_search(/*clear_filter=*/true);
      return true;
    }
    if (event == Event::Backspace) {
      state.backspace_search();
      return true;
    }
    if (event.is_character()) {
      const std::string& ch = event.character();
      if (ch.size() == 1 && static_cast<unsigned char>(ch[0]) >= 0x20) {
        state.append_search_char(ch[0]);
        return true;
      }
    }
    return false;
  }

  if (event == Event::Character('q') || event == Event::CtrlC) {
    screen.Exit();
    return true;
  }
  if (event == Event::Character('/')) {
    state.begin_search();
    return true;
  }
  if (event == Event::Escape) {
    state.end_search(/*clear_filter=*/true);
    return true;
  }
  if (event == Event::Character('g')) {
    state.toggle_group();
    return true;
  }
  if (event == Event::Character('t')) {
    state.toggle_tree();
    return true;
  }
  if (event == Event::Character('s')) {
    state.cycle_sort();
    return true;
  }
  if (event == Event::Character('r')) {
    state.toggle_reverse();
    return true;
  }
  if (event == Event::Character('p') || event == Event::Character(' ')) {
    state.toggle_pause();
    return true;
  }
  if (event == Event::ArrowDown || event == Event::Character('j')) {
    state.move_selection(1, viewport_rows);
    return true;
  }
  if (event == Event::ArrowUp || event == Event::Character('k')) {
    state.move_selection(-1, viewport_rows);
    return true;
  }
  if (event == Event::PageDown) {
    state.move_selection(static_cast<int>(viewport_rows), viewport_rows);
    return true;
  }
  if (event == Event::PageUp) {
    state.move_selection(-static_cast<int>(viewport_rows), viewport_rows);
    return true;
  }
  if (event == Event::Home) {
    state.jump_top();
    return true;
  }
  if (event == Event::End) {
    state.jump_bottom(viewport_rows);
    return true;
  }
  return false;
}

}  // namespace

TuiApp::TuiApp(SystemMonitor& monitor, const Config& initial_cfg)
    : monitor_(monitor), initial_cfg_(initial_cfg) {}

int TuiApp::run() {
  using namespace ftxui;

  TuiState state(initial_cfg_);
  std::mutex mu;
  std::atomic<bool> running{true};

  // Seed with an immediate baseline snapshot so the first frame is populated.
  {
    SampleRequest req = build_sample_request(state.active_config());
    req.need_cmdline = true;
    req.need_path = true;
    req.need_ppid = true;
    state.apply_snapshot(monitor_.sample(req));
  }

  auto screen = ScreenInteractive::Fullscreen();

  auto compute_viewport = []() -> std::size_t {
    const int h = Terminal::Size().dimy;
    return static_cast<std::size_t>(std::max(5, h - 14));
  };

  auto renderer = Renderer([&]() {
    std::lock_guard<std::mutex> lock(mu);
    const int width = std::max(60, Terminal::Size().dimx);
    const std::size_t vp = compute_viewport();
    const Config cfg = state.active_config();

    Elements sections;
    sections.push_back(build_top_bar(state, initial_cfg_.interval_ms));
    sections.push_back(separatorLight() | color(Color::GrayDark));
    if (state.snapshot().memory.valid) {
      sections.push_back(build_system_summary_element(state.snapshot(), cfg, width));
      sections.push_back(build_sparkline_strip(state));
      sections.push_back(separatorLight() | color(Color::GrayDark));
    }
    if (state.searching() || !state.filter_text().empty()) {
      sections.push_back(build_search_bar(state));
    }
    sections.push_back(build_process_table(state, width, vp));
    sections.push_back(separatorLight() | color(Color::GrayDark));
    sections.push_back(text(format_summary_line(state.summary(), cfg)) | bold);
    sections.push_back(build_help_bar(state));
    return vbox(std::move(sections));
  });

  auto app = CatchEvent(renderer, [&](Event event) {
    if (event == Event::Custom) {
      return true;
    }
    std::lock_guard<std::mutex> lock(mu);
    return handle_tui_event(event, state, compute_viewport(), screen);
  });

  std::thread sampler([&]() {
    const auto sleep_step = std::chrono::milliseconds(50);
    const auto interval = std::chrono::milliseconds(std::max(100, initial_cfg_.interval_ms));
    auto next_tick = std::chrono::steady_clock::now() + interval;

    while (running.load()) {
      if (std::chrono::steady_clock::now() < next_tick) {
        std::this_thread::sleep_for(sleep_step);
        continue;
      }
      next_tick = std::chrono::steady_clock::now() + interval;

      SampleRequest req;
      bool paused = false;
      {
        std::lock_guard<std::mutex> lock(mu);
        paused = state.paused();
        if (!paused) {
          req = build_sample_request(state.active_config());
          // Always sample cmdline/path/ppid in watch mode so mode switches and live `/`
          // searches work instantaneously without waiting for the next tick.
          req.need_cmdline = true;
          req.need_path = true;
          req.need_ppid = true;
        }
      }
      if (paused) {
        continue;
      }

      SystemSnapshot snap = monitor_.sample(req);
      {
        std::lock_guard<std::mutex> lock(mu);
        state.apply_snapshot(std::move(snap));
      }
      screen.PostEvent(Event::Custom);
    }
  });

  screen.Loop(app);
  running.store(false);
  if (sampler.joinable()) {
    sampler.join();
  }
  return 0;
}

}  // namespace realmem
