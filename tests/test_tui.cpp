#include "render/format_utils.hpp"
#include "tui/tui_state.hpp"

#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <string>

using namespace simon;

namespace {

SystemSnapshot make_tui_snapshot(std::uint64_t anon_bytes, double cpu_pct) {
  SystemSnapshot snap;
  snap.memory.valid = true;
  snap.memory.total = 1000;
  snap.memory.anonymous = anon_bytes;
  snap.memory.swap = {500, 100, 400, true, true};
  snap.memory.gpu.valid = true;
  snap.memory.gpu.allocated = 300;
  snap.cpu.valid = true;
  snap.cpu.total_util_pct = cpu_pct;

  ProcessRecord p1;
  p1.pid = 10; p1.ppid = 1; p1.footprint = 400; p1.name = "uv"; p1.cmdline = "uv run train.py";
  ProcessRecord p2;
  p2.pid = 20; p2.ppid = 10; p2.footprint = 800; p2.name = "python3"; p2.cmdline = "python3 train.py";
  ProcessRecord p3;
  p3.pid = 30; p3.ppid = 1; p3.footprint = 200; p3.name = "node"; p3.cmdline = "node server.js";
  snap.procs.processes = {p1, p2, p3};
  snap.procs.total_pids = 3;
  snap.procs.total_footprint = 1400;
  return snap;
}

}  // namespace

TEST_CASE("render_sparkline handles empty, zero, auto-scaled, and clipped series", "[tui]") {
  REQUIRE(render_sparkline({}, 10, 100.0).empty());
  REQUIRE(render_sparkline({1.0, 2.0}, 0, 10.0).empty());

  const std::string zeros = render_sparkline({0.0, 0.0, 0.0}, 5, 0.0);
  REQUIRE(utf8_length(zeros) == 3);

  const std::string scaled = render_sparkline({0.0, 50.0, 100.0}, 3, 100.0);
  REQUIRE(utf8_length(scaled) == 3);
  REQUIRE(scaled.find("\xe2\x96\x88") != std::string::npos);  // █ for max

  // Clips to the most recent `width` entries.
  const std::string clipped = render_sparkline({10.0, 20.0, 30.0, 40.0, 50.0}, 2, 50.0);
  REQUIRE(utf8_length(clipped) == 2);
}

TEST_CASE("TuiState ring buffer eviction and pause behavior", "[tui]") {
  Config init_cfg;
  TuiState state(init_cfg, /*history_cap=*/3);

  for (int i = 1; i <= 5; ++i) {
    state.apply_snapshot(make_tui_snapshot(static_cast<std::uint64_t>(i) * 100, i * 10.0));
  }
  REQUIRE(state.mem_history().size() == 3);
  REQUIRE(state.cpu_history().size() == 3);
  REQUIRE(state.cpu_history().front() == 30.0);
  REQUIRE(state.cpu_history().back() == 50.0);

  state.toggle_pause();
  REQUIRE(state.paused());
  state.apply_snapshot(make_tui_snapshot(900, 99.0));
  REQUIRE(state.cpu_history().back() == 50.0);  // unchanged while paused
}

TEST_CASE("TuiState live search handles incremental typing, backspace, invalid regex, and escape",
          "[tui]") {
  Config init_cfg;
  TuiState state(init_cfg);
  state.apply_snapshot(make_tui_snapshot(600, 25.0));
  REQUIRE(state.summary().rows.size() == 3);

  state.begin_search();
  REQUIRE(state.searching());
  for (char c : std::string("train")) {
    state.append_search_char(c);
  }
  REQUIRE(state.filter_text() == "train");
  REQUIRE(state.summary().rows.size() == 2);
  REQUIRE_FALSE(state.filter_error().has_value());

  // Incomplete regex keeps previous valid rows visible and records filter_error.
  state.append_search_char('[');
  REQUIRE(state.filter_error().has_value());
  REQUIRE(state.summary().rows.size() == 2);

  state.backspace_search();
  REQUIRE_FALSE(state.filter_error().has_value());
  REQUIRE(state.filter_text() == "train");

  // Enter keeps filter active; Esc clears it.
  state.end_search(/*clear_filter=*/false);
  REQUIRE_FALSE(state.searching());
  REQUIRE(state.filter_text() == "train");
  REQUIRE(state.summary().rows.size() == 2);

  state.end_search(/*clear_filter=*/true);
  REQUIRE(state.filter_text().empty());
  REQUIRE(state.summary().rows.size() == 3);

  // Backspace on empty filter is a safe no-op.
  state.backspace_search();
  REQUIRE(state.filter_text().empty());
}

TEST_CASE("TuiState mode toggles, sort cycling, and viewport scroll clamping", "[tui]") {
  Config init_cfg;
  TuiState state(init_cfg);
  state.apply_snapshot(make_tui_snapshot(600, 25.0));

  SECTION("mode transitions keep sort field valid") {
    state.toggle_tree();
    REQUIRE(state.view_mode() == ViewMode::Tree);
    REQUIRE(state.summary().rows[0].pid == 10);
    REQUIRE(state.summary().rows[0].subtree_footprint == 1200);

    // Switching from Flat+Pid to Grouped resets sort from Pid to Footprint.
    state.toggle_tree();  // back to Flat
    REQUIRE(state.view_mode() == ViewMode::Flat);
    // Cycle sort all the way around to Pid.
    while (state.sort_field() != SortField::Pid) {
      state.cycle_sort();
    }
    state.toggle_group();
    REQUIRE(state.view_mode() == ViewMode::Grouped);
    REQUIRE(state.sort_field() == SortField::Footprint);

    // Cycle sort in Grouped mode reaches Count instead of Pid.
    while (state.sort_field() != SortField::Count) {
      state.cycle_sort();
    }
    state.toggle_group();  // back to Flat resets Count -> Footprint
    REQUIRE(state.sort_field() == SortField::Footprint);
  }

  SECTION("scroll clamping and jump_top / jump_bottom") {
    state.move_selection(2, 2);
    REQUIRE(state.selected_index() == 2);
    REQUIRE(state.scroll_offset() == 1);

    state.move_selection(100, 2);  // clamped to last row
    REQUIRE(state.selected_index() == 2);

    state.move_selection(-100, 2);  // clamped to 0
    REQUIRE(state.selected_index() == 0);
    REQUIRE(state.scroll_offset() == 0);

    state.jump_bottom(2);
    REQUIRE(state.selected_index() == 2);
    REQUIRE(state.scroll_offset() == 1);

    state.jump_top();
    REQUIRE(state.selected_index() == 0);
    REQUIRE(state.scroll_offset() == 0);
  }

  SECTION("reverse toggle flips row ordering") {
    const pid_t top_before = state.summary().rows.front().pid;
    state.toggle_reverse();
    REQUIRE(state.reverse());
    REQUIRE(state.summary().rows.back().pid == top_before);
  }
}
