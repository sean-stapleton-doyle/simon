#include "tui/tui_state.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace realmem {

namespace {

constexpr std::array<std::string_view, 8> kSparkBlocks = {
    "\xe2\x96\x81",  // ▁
    "\xe2\x96\x82",  // ▂
    "\xe2\x96\x83",  // ▃
    "\xe2\x96\x84",  // ▄
    "\xe2\x96\x85",  // ▅
    "\xe2\x96\x86",  // ▆
    "\xe2\x96\x87",  // ▇
    "\xe2\x96\x88",  // █
};

SortField next_sort_field(SortField current, ViewMode mode) {
  switch (current) {
    case SortField::Footprint: return SortField::Peak;
    case SortField::Peak: return SortField::Cpu;
    case SortField::Cpu: return SortField::Disk;
    case SortField::Disk: return SortField::Gpu;
    case SortField::Gpu: return SortField::Name;
    case SortField::Name: return mode == ViewMode::Grouped ? SortField::Count : SortField::Pid;
    case SortField::Pid:
    case SortField::Count: return SortField::Footprint;
  }
  return SortField::Footprint;
}

}  // namespace

std::string_view to_string(ViewMode mode) {
  switch (mode) {
    case ViewMode::Flat: return "flat";
    case ViewMode::Grouped: return "grouped";
    case ViewMode::Tree: return "tree";
  }
  return "flat";
}

std::string render_sparkline(const std::deque<double>& values,
                             std::size_t width,
                             double max_value) {
  if (values.empty() || width == 0) {
    return {};
  }

  const std::size_t start = values.size() > width ? values.size() - width : 0;
  double scale_max = max_value;
  if (scale_max <= 0.0) {
    for (std::size_t i = start; i < values.size(); ++i) {
      scale_max = std::max(scale_max, values[i]);
    }
  }
  if (scale_max <= 0.0) {
    scale_max = 1.0;
  }

  std::string out;
  out.reserve((values.size() - start) * 3);
  for (std::size_t i = start; i < values.size(); ++i) {
    const double ratio = std::clamp(values[i] / scale_max, 0.0, 1.0);
    const std::size_t idx =
        std::min<std::size_t>(kSparkBlocks.size() - 1, static_cast<std::size_t>(std::lround(ratio * 7.0)));
    out.append(kSparkBlocks[idx]);
  }
  return out;
}

TuiState::TuiState(const Config& initial_cfg, std::size_t history_cap)
    : base_cfg_(initial_cfg),
      sort_(initial_cfg.sort),
      reverse_(initial_cfg.reverse),
      history_cap_(std::max<std::size_t>(1, history_cap)) {
  if (initial_cfg.tree) {
    mode_ = ViewMode::Tree;
  } else if (initial_cfg.group) {
    mode_ = ViewMode::Grouped;
  }
  if (initial_cfg.pattern) {
    filter_text_ = *initial_cfg.pattern;
  }
}

void TuiState::push_history(std::deque<double>& buf, double val) {
  buf.push_back(std::max(0.0, val));
  while (buf.size() > history_cap_) {
    buf.pop_front();
  }
}

void TuiState::apply_snapshot(SystemSnapshot snap) {
  if (paused_) {
    return;
  }

  const auto& mem = snap.memory;
  if (mem.valid && mem.total > 0) {
    const std::uint64_t used = mem.wired + mem.anonymous + mem.compressor;
    const double pct = (static_cast<double>(used) * 100.0) / static_cast<double>(mem.total);
    push_history(mem_history_, pct);
  }
  if (mem.swap.valid) {
    push_history(swap_history_, static_cast<double>(mem.swap.used));
  }
  if (mem.gpu.valid) {
    push_history(gpu_history_, static_cast<double>(mem.gpu.allocated));
  }
  if (snap.cpu.valid) {
    push_history(cpu_history_, snap.cpu.total_util_pct);
  }

  snapshot_ = std::move(snap);
  recompute_view();
}

Config TuiState::active_config() const {
  Config cfg = base_cfg_;
  cfg.count = 0;  // TUI renders all rows through its scrollable viewport.
  cfg.group = (mode_ == ViewMode::Grouped);
  cfg.tree = (mode_ == ViewMode::Tree);
  cfg.sort = sort_;
  cfg.reverse = reverse_;
  cfg.show_cpu = true;
  cfg.show_disk = true;
  cfg.watch = true;
  if (!filter_text_.empty()) {
    cfg.pattern = filter_text_;
  } else {
    cfg.pattern.reset();
  }
  return cfg;
}

void TuiState::recompute_view() {
  const Config cfg = active_config();
  try {
    summary_ = QueryEngine::run(snapshot_.procs, cfg);
    filter_error_.reset();
  } catch (const std::runtime_error& err) {
    // Keep prior rows visible while user is mid-typing an incomplete regex.
    filter_error_ = err.what();
  }
  clamp_selection(20);
}

void TuiState::toggle_group() {
  mode_ = (mode_ == ViewMode::Grouped) ? ViewMode::Flat : ViewMode::Grouped;
  if (mode_ == ViewMode::Grouped && sort_ == SortField::Pid) {
    sort_ = SortField::Footprint;
  } else if (mode_ != ViewMode::Grouped && sort_ == SortField::Count) {
    sort_ = SortField::Footprint;
  }
  selected_ = 0;
  scroll_offset_ = 0;
  recompute_view();
}

void TuiState::toggle_tree() {
  mode_ = (mode_ == ViewMode::Tree) ? ViewMode::Flat : ViewMode::Tree;
  if (sort_ == SortField::Count) {
    sort_ = SortField::Footprint;
  }
  selected_ = 0;
  scroll_offset_ = 0;
  recompute_view();
}

void TuiState::cycle_sort() {
  sort_ = next_sort_field(sort_, mode_);
  recompute_view();
}

void TuiState::toggle_reverse() {
  reverse_ = !reverse_;
  recompute_view();
}

void TuiState::toggle_pause() {
  paused_ = !paused_;
}

void TuiState::begin_search() {
  searching_ = true;
}

void TuiState::end_search(bool clear_filter) {
  searching_ = false;
  if (clear_filter && !filter_text_.empty()) {
    filter_text_.clear();
    filter_error_.reset();
    selected_ = 0;
    scroll_offset_ = 0;
    recompute_view();
  }
}

void TuiState::append_search_char(char ch) {
  filter_text_.push_back(ch);
  selected_ = 0;
  scroll_offset_ = 0;
  recompute_view();
}

void TuiState::backspace_search() {
  if (!filter_text_.empty()) {
    filter_text_.pop_back();
    selected_ = 0;
    scroll_offset_ = 0;
    recompute_view();
  }
}

void TuiState::set_filter(std::string pattern) {
  filter_text_ = std::move(pattern);
  selected_ = 0;
  scroll_offset_ = 0;
  recompute_view();
}

void TuiState::clamp_selection(std::size_t viewport_rows) {
  const std::size_t total = summary_.rows.size();
  if (total == 0) {
    selected_ = 0;
    scroll_offset_ = 0;
    return;
  }
  if (selected_ >= total) {
    selected_ = total - 1;
  }
  const std::size_t vp = std::max<std::size_t>(1, viewport_rows);
  if (selected_ < scroll_offset_) {
    scroll_offset_ = selected_;
  } else if (selected_ >= scroll_offset_ + vp) {
    scroll_offset_ = selected_ - vp + 1;
  }
  if (scroll_offset_ + vp > total && total >= vp) {
    scroll_offset_ = total - vp;
  } else if (total < vp) {
    scroll_offset_ = 0;
  }
}

void TuiState::move_selection(int delta, std::size_t viewport_rows) {
  if (summary_.rows.empty()) {
    return;
  }
  const int next = static_cast<int>(selected_) + delta;
  selected_ = static_cast<std::size_t>(
      std::clamp(next, 0, static_cast<int>(summary_.rows.size()) - 1));
  clamp_selection(viewport_rows);
}

void TuiState::jump_top() {
  selected_ = 0;
  scroll_offset_ = 0;
}

void TuiState::jump_bottom(std::size_t viewport_rows) {
  if (summary_.rows.empty()) {
    return;
  }
  selected_ = summary_.rows.size() - 1;
  clamp_selection(viewport_rows);
}

}  // namespace realmem
