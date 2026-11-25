#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace simon {

enum class SortField {
  Footprint,
  Peak,
  Gpu,
  Cpu,
  Disk,
  Pid,
  Name,
  Count,
};

enum class ColorMode {
  Auto,
  Always,
  Never,
};

enum class OutputFormat {
  Auto,
  Table,
  Plain,
  Json,
};

struct GroupRule {
  std::string pattern;
  std::string name;
};

struct Config {
  int count = 20;  // 0 means show all rows
  SortField sort = SortField::Footprint;
  bool reverse = false;
  bool group = false;
  bool tree = false;
  bool long_format = false;
  bool show_gpu = false;
  bool show_cpu = false;
  bool show_disk = false;
  bool short_mode = false;
  bool watch = false;
  int interval_ms = 1000;  // TUI refresh interval
  int sample_ms = 250;     // one-shot delta sample window for --cpu/--disk
  bool case_sensitive = false;
  bool sum_only = false;
  bool show_bars = true;
  bool show_system = true;
  bool plain = false;
  OutputFormat output = OutputFormat::Auto;
  ColorMode color = ColorMode::Auto;
  std::optional<std::string> pattern;
  std::optional<std::filesystem::path> config_path;
  bool no_config = false;
  std::vector<GroupRule> group_rules;
};

struct ParseResult {
  Config config;
  int exit_code = 0;
  bool should_exit = false;
  std::string message;
};

std::optional<SortField> parse_sort_field(std::string_view value);
std::string_view to_string(SortField field);

std::optional<ColorMode> parse_color_mode(std::string_view value);
std::string_view to_string(ColorMode mode);

std::optional<OutputFormat> parse_output_format(std::string_view value);
std::string_view to_string(OutputFormat format);

// Discovers the first existing config file in standard locations:
//   1. $XDG_CONFIG_HOME/simon/config.toml
//   2. $HOME/.config/simon/config.toml
//   3. $HOME/.simon.toml
std::optional<std::filesystem::path> discover_config_path(
    const char* xdg_config_home = nullptr,
    const char* home = nullptr);

// Loads TOML settings into `cfg`, throwing std::runtime_error on parse/type errors.
void load_toml_string(std::string_view content, Config& cfg);
void load_toml_file(const std::filesystem::path& path, Config& cfg);

// Validates semantic consistency of a merged Config; returns an error message or std::nullopt.
std::optional<std::string> validate_config(const Config& cfg);

// Parses CLI arguments and merges with TOML config (precedence: CLI > TOML > defaults).
ParseResult parse_args_and_config(int argc, const char* const* argv);

}  // namespace simon
