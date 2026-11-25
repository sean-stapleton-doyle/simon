#include "config/config.hpp"

#include "skill_doc.hpp"

#include <CLI/CLI.hpp>
#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace simon {

namespace {

struct CliValues {
  std::optional<int> count;
  std::optional<int> interval_ms;
  std::optional<int> sample_ms;
  std::optional<std::string> match;
  std::optional<std::string> sort;
  std::optional<std::string> output;
  std::optional<std::string> color;
  std::optional<std::string> config_path;
  std::optional<bool> reverse;
  std::optional<bool> group;
  std::optional<bool> tree;
  std::optional<bool> long_format;
  std::optional<bool> show_gpu;
  std::optional<bool> show_cpu;
  std::optional<bool> show_disk;
  std::optional<bool> watch;
  std::optional<bool> case_sensitive;
  std::optional<bool> sum_only;
  std::optional<bool> show_bars;
  std::optional<bool> show_system;
  std::optional<bool> plain;
  bool print_skill = false;
  bool no_config = false;
  std::vector<std::string> positional;
};

std::string to_lower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

bool is_all_digits(std::string_view s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
    return std::isdigit(c) != 0;
  });
}

void read_toml_int(const toml::table& tbl, std::string_view key, int& target, int min_value) {
  const auto* node = tbl.get(key);
  if (!node) {
    return;
  }
  const auto val = node->value<std::int64_t>();
  if (!val) {
    throw std::runtime_error(std::format("config key '{}' must be an integer", key));
  }
  if (*val < min_value) {
    throw std::runtime_error(std::format("config key '{}' must be >= {}", key, min_value));
  }
  target = static_cast<int>(*val);
}

void read_toml_bool(const toml::table& tbl, std::string_view key, bool& target) {
  const auto* node = tbl.get(key);
  if (!node) {
    return;
  }
  const auto val = node->value<bool>();
  if (!val) {
    throw std::runtime_error(std::format("config key '{}' must be a boolean", key));
  }
  target = *val;
}

std::optional<std::string> read_toml_string(const toml::table& tbl, std::string_view key) {
  const auto* node = tbl.get(key);
  if (!node) {
    return std::nullopt;
  }
  const auto val = node->value<std::string>();
  if (!val) {
    throw std::runtime_error(std::format("config key '{}' must be a string", key));
  }
  return *val;
}

void parse_toml_group_rules(const toml::table& tbl, std::vector<GroupRule>& rules_out) {
  const auto* grouping = tbl.get_as<toml::table>("grouping");
  if (!grouping) {
    return;
  }
  const auto* rules = grouping->get_as<toml::array>("rules");
  if (!rules) {
    return;
  }

  for (const auto& elem : *rules) {
    const auto* rule_tbl = elem.as_table();
    if (!rule_tbl) {
      throw std::runtime_error("[[grouping.rules]] entries must be tables");
    }
    const auto pat = read_toml_string(*rule_tbl, "pattern");
    const auto name = read_toml_string(*rule_tbl, "name");
    if (!pat || pat->empty() || !name || name->empty()) {
      throw std::runtime_error(
          "each [[grouping.rules]] entry requires non-empty 'pattern' and 'name' strings");
    }
    rules_out.push_back(GroupRule{*pat, *name});
  }
}

void apply_toml_table(const toml::table& tbl, Config& cfg) {
  read_toml_int(tbl, "count", cfg.count, 0);
  read_toml_int(tbl, "interval_ms", cfg.interval_ms, 100);
  read_toml_int(tbl, "sample_ms", cfg.sample_ms, 50);
  read_toml_bool(tbl, "reverse", cfg.reverse);
  read_toml_bool(tbl, "group", cfg.group);
  read_toml_bool(tbl, "tree", cfg.tree);
  read_toml_bool(tbl, "long", cfg.long_format);
  read_toml_bool(tbl, "gpu", cfg.show_gpu);
  read_toml_bool(tbl, "cpu", cfg.show_cpu);
  read_toml_bool(tbl, "disk", cfg.show_disk);
  read_toml_bool(tbl, "case_sensitive", cfg.case_sensitive);
  read_toml_bool(tbl, "bars", cfg.show_bars);
  read_toml_bool(tbl, "system", cfg.show_system);
  read_toml_bool(tbl, "plain", cfg.plain);

  if (auto sort_str = read_toml_string(tbl, "sort")) {
    auto parsed = parse_sort_field(*sort_str);
    if (!parsed) {
      throw std::runtime_error(std::format(
          "invalid sort value '{}' (expected footprint, peak, gpu, cpu, disk, pid, name, or count)",
          *sort_str));
    }
    cfg.sort = *parsed;
  }
  if (auto color_str = read_toml_string(tbl, "color")) {
    auto parsed = parse_color_mode(*color_str);
    if (!parsed) {
      throw std::runtime_error(
          std::format("invalid color value '{}' (expected auto, always, or never)", *color_str));
    }
    cfg.color = *parsed;
  }
  if (auto out_str = read_toml_string(tbl, "output")) {
    auto parsed = parse_output_format(*out_str);
    if (!parsed) {
      throw std::runtime_error(std::format(
          "invalid output value '{}' (expected auto, table, plain, or json)", *out_str));
    }
    cfg.output = *parsed;
  }

  if (const auto* defaults = tbl.get_as<toml::table>("defaults")) {
    apply_toml_table(*defaults, cfg);
  }
  parse_toml_group_rules(tbl, cfg.group_rules);
}

void register_cli_options(CLI::App& app, CliValues& cli) {
  auto set_flag = [&](const std::string& names, const std::string& desc,
                      std::optional<bool>& target, bool value) {
    app.add_flag_callback(names, [&target, value]() { target = value; }, desc);
  };

  app.add_option("-n,--count", cli.count, "Maximum rows to print (0 for all)")
      ->check(CLI::NonNegativeNumber);
  app.add_option("-m,--match", cli.match, "Filter by extended regex");
  app.add_option("-s,--sort", cli.sort, "Sort by: footprint, peak, gpu, cpu, disk, pid, name, count");
  app.add_option("-o,--output", cli.output, "Output format: auto, table, plain, json");
  app.add_option("--color", cli.color, "Color output: auto, always, never");
  app.add_option("--interval-ms", cli.interval_ms, "Refresh interval in ms for --watch (min 100)")
      ->check(CLI::Range(100, 60000));
  app.add_option("--sample-ms", cli.sample_ms, "Delta sample window in ms for one-shot --cpu/--disk")
      ->check(CLI::Range(50, 10000));
  app.add_option("--config", cli.config_path, "Path to TOML config file");

  set_flag("-r,--reverse", "Reverse sort order", cli.reverse, true);
  set_flag("-g,--group", "Group processes by application", cli.group, true);
  set_flag("--no-group", "Disable grouping (overrides config)", cli.group, false);
  set_flag("-t,--tree", "Show parent/child process tree with rolled-up footprints", cli.tree, true);
  set_flag("--no-tree", "Disable tree view (overrides config)", cli.tree, false);
  set_flag("-l,--long", "Add a column with the full command line", cli.long_format, true);
  set_flag("--no-long", "Hide command line column (overrides config)", cli.long_format, false);
  set_flag("--gpu", "Show per-process GPU memory column (requires sudo)", cli.show_gpu, true);
  set_flag("--no-gpu", "Hide per-process GPU column (overrides config)", cli.show_gpu, false);
  set_flag("--cpu", "Include Apple Silicon P/E-core CPU utilization", cli.show_cpu, true);
  set_flag("--no-cpu", "Disable CPU telemetry (overrides config)", cli.show_cpu, false);
  set_flag("--disk", "Include system and per-process disk I/O", cli.show_disk, true);
  set_flag("--no-disk", "Disable disk I/O telemetry (overrides config)", cli.show_disk, false);
  set_flag("-w,--watch", "Launch read-only live TUI monitor", cli.watch, true);
  set_flag("-c,--case", "Match regex case-sensitively", cli.case_sensitive, true);
  set_flag("--sum", "Print only the aggregate total for matches", cli.sum_only, true);
  set_flag("--bars", "Show inline memory usage bars", cli.show_bars, true);
  set_flag("--no-bars", "Hide inline memory usage bars", cli.show_bars, false);
  set_flag("--no-system", "Hide the system memory summary", cli.show_system, false);
  set_flag("--plain", "Plain text output without FTXUI styling", cli.plain, true);
  app.add_flag("--skill", cli.print_skill, "Print the agent SKILL.md document and exit");
  app.add_flag("--no-config", cli.no_config, "Ignore any TOML config file");

  app.add_option("args", cli.positional, "Optional row count and/or extended regex")
      ->expected(0, 2);
}

void load_config_source(const CliValues& cli, Config& cfg) {
  cfg.no_config = cli.no_config;
  if (cli.no_config) {
    return;
  }
  if (cli.config_path) {
    cfg.config_path = std::filesystem::path(*cli.config_path);
    load_toml_file(*cfg.config_path, cfg);
  } else if (auto discovered = discover_config_path()) {
    cfg.config_path = *discovered;
    load_toml_file(*cfg.config_path, cfg);
  }
}

void apply_cli_overrides(const CliValues& cli, Config& cfg) {
  if (cli.count) cfg.count = *cli.count;
  if (cli.interval_ms) cfg.interval_ms = *cli.interval_ms;
  if (cli.sample_ms) cfg.sample_ms = *cli.sample_ms;
  if (cli.match) cfg.pattern = *cli.match;
  if (cli.reverse) cfg.reverse = *cli.reverse;
  if (cli.group) cfg.group = *cli.group;
  if (cli.tree) cfg.tree = *cli.tree;
  if (cli.long_format) cfg.long_format = *cli.long_format;
  if (cli.show_gpu) cfg.show_gpu = *cli.show_gpu;
  if (cli.show_cpu) cfg.show_cpu = *cli.show_cpu;
  if (cli.show_disk) cfg.show_disk = *cli.show_disk;
  if (cli.watch) cfg.watch = *cli.watch;
  if (cli.case_sensitive) cfg.case_sensitive = *cli.case_sensitive;
  if (cli.sum_only) cfg.sum_only = *cli.sum_only;
  if (cli.show_bars) cfg.show_bars = *cli.show_bars;
  if (cli.show_system) cfg.show_system = *cli.show_system;
  if (cli.plain) cfg.plain = *cli.plain;

  if (cli.sort) {
    auto parsed = parse_sort_field(*cli.sort);
    if (!parsed) {
      throw std::runtime_error(std::format(
          "unknown sort field '{}' (expected footprint, peak, gpu, cpu, disk, pid, name, or count)",
          *cli.sort));
    }
    cfg.sort = *parsed;
  }
  if (cli.color) {
    auto parsed = parse_color_mode(*cli.color);
    if (!parsed) {
      throw std::runtime_error(
          std::format("unknown color mode '{}' (expected auto, always, or never)", *cli.color));
    }
    cfg.color = *parsed;
  }
  if (cli.output) {
    auto parsed = parse_output_format(*cli.output);
    if (!parsed) {
      throw std::runtime_error(std::format(
          "unknown output format '{}' (expected auto, table, plain, or json)", *cli.output));
    }
    cfg.output = *parsed;
    if (*parsed != OutputFormat::Plain) {
      cfg.plain = false;
    }
  }

  // CLI -t overrides config group=true and vice versa if only one was passed on CLI.
  if (cli.tree.value_or(false) && !cli.group.has_value()) {
    cfg.group = false;
  }
  if (cli.group.value_or(false) && !cli.tree.has_value()) {
    cfg.tree = false;
  }
}

void apply_positional_args(const std::vector<std::string>& positional, Config& cfg) {
  for (const auto& arg : positional) {
    if (is_all_digits(arg)) {
      try {
        cfg.count = std::stoi(arg);
      } catch (const std::exception&) {
        throw std::runtime_error(std::format("invalid row count '{}'", arg));
      }
    } else {
      cfg.pattern = arg;
    }
  }
}

void apply_implicit_columns(const CliValues& cli, Config& cfg) {
  if (cfg.sort == SortField::Gpu && !cli.show_gpu.has_value()) {
    cfg.show_gpu = true;
  }
  if (cfg.sort == SortField::Cpu && !cli.show_cpu.has_value()) {
    cfg.show_cpu = true;
  }
  if (cfg.sort == SortField::Disk && !cli.show_disk.has_value()) {
    cfg.show_disk = true;
  }
}

ParseResult make_error(std::string_view msg, int code = 2) {
  return ParseResult{{}, code, true, std::format("simon: {}\n", msg)};
}

}  // namespace

std::optional<SortField> parse_sort_field(std::string_view value) {
  const std::string lower = to_lower(value);
  if (lower == "footprint" || lower == "mem" || lower == "memory") return SortField::Footprint;
  if (lower == "peak" || lower == "max") return SortField::Peak;
  if (lower == "gpu" || lower == "graphics") return SortField::Gpu;
  if (lower == "cpu") return SortField::Cpu;
  if (lower == "disk" || lower == "io") return SortField::Disk;
  if (lower == "pid") return SortField::Pid;
  if (lower == "name" || lower == "command" || lower == "app") return SortField::Name;
  if (lower == "count" || lower == "procs") return SortField::Count;
  return std::nullopt;
}

std::string_view to_string(SortField field) {
  switch (field) {
    case SortField::Footprint: return "footprint";
    case SortField::Peak: return "peak";
    case SortField::Gpu: return "gpu";
    case SortField::Cpu: return "cpu";
    case SortField::Disk: return "disk";
    case SortField::Pid: return "pid";
    case SortField::Name: return "name";
    case SortField::Count: return "count";
  }
  return "footprint";
}

std::optional<ColorMode> parse_color_mode(std::string_view value) {
  const std::string lower = to_lower(value);
  if (lower == "auto") return ColorMode::Auto;
  if (lower == "always" || lower == "on" || lower == "true") return ColorMode::Always;
  if (lower == "never" || lower == "off" || lower == "false") return ColorMode::Never;
  return std::nullopt;
}

std::string_view to_string(ColorMode mode) {
  switch (mode) {
    case ColorMode::Auto: return "auto";
    case ColorMode::Always: return "always";
    case ColorMode::Never: return "never";
  }
  return "auto";
}

std::optional<OutputFormat> parse_output_format(std::string_view value) {
  const std::string lower = to_lower(value);
  if (lower == "auto") return OutputFormat::Auto;
  if (lower == "table" || lower == "rich" || lower == "ftxui") return OutputFormat::Table;
  if (lower == "plain" || lower == "text") return OutputFormat::Plain;
  if (lower == "json") return OutputFormat::Json;
  return std::nullopt;
}

std::string_view to_string(OutputFormat format) {
  switch (format) {
    case OutputFormat::Auto: return "auto";
    case OutputFormat::Table: return "table";
    case OutputFormat::Plain: return "plain";
    case OutputFormat::Json: return "json";
  }
  return "auto";
}

std::optional<std::filesystem::path> discover_config_path(const char* xdg_config_home,
                                                          const char* home) {
  namespace fs = std::filesystem;

  const char* xdg = xdg_config_home ? xdg_config_home : std::getenv("XDG_CONFIG_HOME");
  if (xdg && xdg[0] != '\0') {
    fs::path p = fs::path(xdg) / "simon" / "config.toml";
    std::error_code ec;
    if (fs::is_regular_file(p, ec)) {
      return p;
    }
  }

  const char* home_dir = home ? home : std::getenv("HOME");
  if (home_dir && home_dir[0] != '\0') {
    std::error_code ec;
    fs::path xdg_default = fs::path(home_dir) / ".config" / "simon" / "config.toml";
    if (fs::is_regular_file(xdg_default, ec)) {
      return xdg_default;
    }
    fs::path dotfile = fs::path(home_dir) / ".simon.toml";
    if (fs::is_regular_file(dotfile, ec)) {
      return dotfile;
    }
  }

  return std::nullopt;
}

void load_toml_string(std::string_view content, Config& cfg) {
  try {
    toml::table tbl = toml::parse(content);
    apply_toml_table(tbl, cfg);
  } catch (const toml::parse_error& err) {
    throw std::runtime_error(std::format("TOML parse error at line {}, col {}: {}",
                                         err.source().begin.line,
                                         err.source().begin.column,
                                         err.description()));
  }
}

void load_toml_file(const std::filesystem::path& path, Config& cfg) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error(std::format("unable to open config file '{}'", path.string()));
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  try {
    load_toml_string(ss.str(), cfg);
  } catch (const std::exception& err) {
    throw std::runtime_error(std::format("{}: {}", path.string(), err.what()));
  }
}

std::optional<std::string> validate_config(const Config& cfg) {
  if (cfg.group && cfg.tree) {
    return "--group (-g) and --tree (-t) cannot be used together";
  }
  if (cfg.sum_only && !cfg.pattern) {
    return "--sum needs a regex to sum over";
  }
  if (cfg.sort == SortField::Count && !cfg.group) {
    return "--sort count requires --group (-g)";
  }
  if (cfg.sort == SortField::Pid && cfg.group) {
    return "--sort pid cannot be used with --group (-g)";
  }
  if (cfg.watch && (cfg.output == OutputFormat::Json || cfg.sum_only)) {
    return "--watch (-w) cannot be combined with --output json or --sum";
  }
  return std::nullopt;
}

ParseResult parse_args_and_config(int argc, const char* const* argv) {
  CLI::App app{
      "Rank macOS processes by phys_footprint (the metric Activity Monitor reports).\n"
      "Unlike RSS, phys_footprint excludes the reclaimable OS file cache."};
  app.footer(
      "\nA bare positional argument is a row count if all digits, otherwise an extended regex.\n"
      "Defaults can be configured in ~/.config/simon/config.toml or ~/.simon.toml.\n\n"
      "Examples:\n"
      "  simon                 top 20 processes\n"
      "  simon -g              group helpers by application\n"
      "  simon -t              parent/child process tree with rolled-up footprints\n"
      "  simon -w              live read-only TUI monitor\n"
      "  simon -o json         machine-readable JSON output\n"
      "  sudo simon --gpu      include per-process GPU memory column");

  CliValues cli;
  register_cli_options(app, cli);

  try {
    app.parse(argc, argv);
  } catch (const CLI::CallForHelp&) {
    return ParseResult{{}, 0, true, app.help()};
  } catch (const CLI::ParseError& e) {
    return ParseResult{
        {}, 2, true, std::format("simon: {}\nRun 'simon --help' for usage.\n", e.what())};
  }

  if (cli.print_skill) {
    return ParseResult{{}, 0, true, std::string(kSkillDocument)};
  }

  Config cfg;
  try {
    load_config_source(cli, cfg);
    apply_cli_overrides(cli, cfg);
    apply_positional_args(cli.positional, cfg);
    apply_implicit_columns(cli, cfg);
  } catch (const std::exception& err) {
    return make_error(err.what());
  }

  if (auto err = validate_config(cfg)) {
    return make_error(*err);
  }

  return ParseResult{std::move(cfg), 0, false, {}};
}

}  // namespace simon
