#pragma once

#include "config/config.hpp"
#include "domain/query.hpp"
#include "domain/snapshot.hpp"

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

namespace realmem {

enum class ViewMode {
  Flat,
  Grouped,
  Tree,
};

std::string_view to_string(ViewMode mode);

// Renders a Unicode block sparkline (▁▂▃▄▅▆▇█) from a series of non-negative samples.
std::string render_sparkline(const std::deque<double>& values,
                             std::size_t width,
                             double max_value = 0.0);

// Pure, terminal-independent state for the read-only live TUI monitor.
class TuiState {
 public:
  explicit TuiState(const Config& initial_cfg, std::size_t history_cap = 60);

  void apply_snapshot(SystemSnapshot snap);
  void recompute_view();

  // Mode, sort, and pause toggles (strictly read-only).
  void toggle_group();
  void toggle_tree();
  void cycle_sort();
  void toggle_reverse();
  void toggle_pause();

  // Live filter editing.
  void begin_search();
  void end_search(bool clear_filter);
  void append_search_char(char ch);
  void backspace_search();
  void set_filter(std::string pattern);

  // Viewport navigation.
  void move_selection(int delta, std::size_t viewport_rows);
  void jump_top();
  void jump_bottom(std::size_t viewport_rows);

  Config active_config() const;

  const SystemSnapshot& snapshot() const { return snapshot_; }
  const QuerySummary& summary() const { return summary_; }
  ViewMode view_mode() const { return mode_; }
  SortField sort_field() const { return sort_; }
  bool reverse() const { return reverse_; }
  bool paused() const { return paused_; }
  bool searching() const { return searching_; }
  const std::string& filter_text() const { return filter_text_; }
  const std::optional<std::string>& filter_error() const { return filter_error_; }
  std::size_t selected_index() const { return selected_; }
  std::size_t scroll_offset() const { return scroll_offset_; }

  const std::deque<double>& mem_history() const { return mem_history_; }
  const std::deque<double>& swap_history() const { return swap_history_; }
  const std::deque<double>& gpu_history() const { return gpu_history_; }
  const std::deque<double>& cpu_history() const { return cpu_history_; }

 private:
  void push_history(std::deque<double>& buf, double val);
  void clamp_selection(std::size_t viewport_rows);

  Config base_cfg_;
  ViewMode mode_ = ViewMode::Flat;
  SortField sort_ = SortField::Footprint;
  bool reverse_ = false;
  bool paused_ = false;
  bool searching_ = false;
  std::string filter_text_;
  std::optional<std::string> filter_error_;
  std::size_t selected_ = 0;
  std::size_t scroll_offset_ = 0;
  std::size_t history_cap_ = 60;

  SystemSnapshot snapshot_;
  QuerySummary summary_;
  std::deque<double> mem_history_;
  std::deque<double> swap_history_;
  std::deque<double> gpu_history_;
  std::deque<double> cpu_history_;
};

}  // namespace realmem
