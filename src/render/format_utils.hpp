#pragma once

#include "config/config.hpp"
#include "domain/query.hpp"
#include "domain/snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace realmem {

// Formats byte counts in binary units (B, K, M, G, T) with one decimal place.
std::string humanize_bytes(std::uint64_t bytes);

// Formats byte-per-second rates (e.g. "412.5M/s", "0.0B/s").
std::string format_rate(double bytes_per_sec);

// Counts UTF-8 code points.
std::size_t utf8_length(std::string_view text);

// Truncates UTF-8 text to at most `max_cols` code points, appending "…" if truncated.
std::string truncate_utf8(std::string_view text, std::size_t max_cols);

// Truncates and pads with spaces on the right to `width` columns.
std::string pad_right_utf8(std::string_view text, std::size_t width);

std::string format_pct(std::uint64_t part, std::uint64_t total);
std::string format_gpu_value(const std::optional<std::uint64_t>& gpu);
std::string format_cpu_value(const std::optional<double>& cpu_pct);
std::string format_disk_value(const DisplayRow& row);

std::string_view pressure_label(MemoryPressureLevel level);
std::string format_row_label(const DisplayRow& row, const Config& cfg);
std::string format_summary_line(const QuerySummary& summary, const Config& cfg);

}  // namespace realmem
