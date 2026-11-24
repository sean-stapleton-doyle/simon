#pragma once

#include "config.hpp"
#include "model.hpp"
#include "process.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace realmem {

struct RenderContext {
  int width = 120;
  bool is_tty = false;
};

// Formats byte counts in binary units (B, K, M, G, T) with one decimal place.
std::string humanize_bytes(std::uint64_t bytes);

// Truncates UTF-8 text to at most `max_cols` code points, appending "…" if truncated.
std::string truncate_utf8(std::string_view text, std::size_t max_cols);

// Renders the complete output (either styled via FTXUI or plain text for pipes/--plain).
std::string render_output(const QuerySummary& summary,
                          const SystemMemory& sys,
                          const Config& cfg,
                          const RenderContext& ctx);

}  // namespace realmem
