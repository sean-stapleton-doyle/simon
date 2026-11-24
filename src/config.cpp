#include "config.hpp"

#include <CLI/CLI.hpp>
#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace realmem {

namespace {

std::string to_lower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

bool is_all_digits(std::string_view s) {
  if (s.empty()) {
    return false;
  }
  return std::all_of(s.begin(), s.end(), [](unsigned char c) {
    return std::isdigit(c) != 0;
  });
}

void apply_toml_table(const toml::table& tbl, Config& cfg) {
  auto read_int = [&](std::string_view key, int& target, bool non_negative) {
    if (const auto* node = tbl.get(key)) {
      auto val = node->value<std::int64_t>();
      if (!val) {
        throw std::runtime_error(std::format("config key '{}' must be an integer", key));
      }
      if (non_negative && *val < 0) {
        throw std::runtime_error(std::format("config key '{}' must be >= 0", key));
      }
      target = static_cast<int>(*val);
    }
  };

  auto read_bool = [&](std::string_view key, bool& target) {
    if (const auto* node = tbl.get(key)) {
      auto val = node->value<bool>();
      if (!val) {
        throw std::runtime_error(std::format("config key '{}' must be a boolean", key));
      }
      target = *val;
    }
  };

  auto read_string = [&](std::string_view key) -> std::optional<std::string> {
    if (const auto* node = tbl.get(key)) {
      auto val = node->value<std::string>();
      if (!val) {
        throw std::runtime_error(std::format("config key '{}' must be a string", key));
      }
      return *val;
    }
    return std::nullopt;
  };

  read_int("count", cfg.count, /*non_negative=*/true);
  read_bool("reverse", cfg.reverse);
  read_bool("group", cfg.group);
  read_bool("long", cfg.long_format);
  read_bool("case_sensitive", cfg.case_sensitive);
  read_bool("bars", cfg.show_bars);
  read_bool("system", cfg.show_system);
  read_bool("plain", cfg.plain);

  if (auto sort_str = read_string("sort")) {
    auto parsed = parse_sort_field(*sort_str);
    if (!parsed) {
      throw std::runtime_error(
          std::format("invalid sort value '{}' (expected footprint, peak, pid, name, or count)",
                      *sort_str));
    }
    cfg.sort = *parsed;
  }

  if (auto color_str = read_string("color")) {
    auto parsed = parse_color_mode(*color_str);
    if (!parsed) {
      throw std::runtime_error(
          std::format("invalid color value '{}' (expected auto, always, or never)", *color_str));
    }
    cfg.color = *parsed;
  }

  // Optional [defaults] section maps to the same keys for users who prefer grouped TOML.
  if (const auto* defaults = tbl.get_as<toml::table>("defaults")) {
    apply_toml_table(*defaults, cfg);
  }

  // Optional [[grouping.rules]] array of { pattern = "...", name = "..." }.
  if (const auto* grouping = tbl.get_as<toml::table>("grouping")) {
    if (const auto* rules = grouping->get_as<toml::array>("rules")) {
      for (const auto& elem : *rules) {
        const auto* rule_tbl = elem.as_table();
        if (!rule_tbl) {
          throw std::runtime_error("[[grouping.rules]] entries must be tables");
        }
        auto pat = rule_tbl->get("pattern") ? rule_tbl->get("pattern")->value<std::string>()
                                            : std::nullopt;
        auto name =
            rule_tbl->get("name") ? rule_tbl->get("name")->value<std::string>() : std::nullopt;
        if (!pat || pat->empty() || !name || name->empty()) {
          throw std::runtime_error(
              "each [[grouping.rules]] entry requires non-empty 'pattern' and 'name' strings");
        }
        cfg.group_rules.push_back(GroupRule{*pat, *name});
      }
    }
  }
}

}  // namespace

std::optional<SortField> parse_sort_field(std::string_view value) {
  const std::string lower = to_lower(value);
  if (lower == "footprint" || lower == "mem" || lower == "memory") {
    return SortField::Footprint;
  }
  if (lower == "peak" || lower == "max") {
    return SortField::Peak;
  }
  if (lower == "pid") {
    return SortField::Pid;
  }
  if (lower == "name" || lower == "command" || lower == "app") {
    return SortField::Name;
  }
  if (lower == "count" || lower == "procs") {
    return SortField::Count;
  }
  return std::nullopt;
}

std::string_view to_string(SortField field) {
  switch (field) {
    case SortField::Footprint:
      return "footprint";
    case SortField::Peak:
      return "peak";
    case SortField::Pid:
      return "pid";
    case SortField::Name:
      return "name";
    case SortField::Count:
      return "count";
  }
  return "footprint";
}

std::optional<ColorMode> parse_color_mode(std::string_view value) {
  const std::string lower = to_lower(value);
  if (lower == "auto") {
    return ColorMode::Auto;
  }
  if (lower == "always" || lower == "on" || lower == "true") {
    return ColorMode::Always;
  }
  if (lower == "never" || lower == "off" || lower == "false") {
    return ColorMode::Never;
  }
  return std::nullopt;
}

std::string_view to_string(ColorMode mode) {
  switch (mode) {
    case ColorMode::Auto:
      return "auto";
    case ColorMode::Always:
      return "always";
    case ColorMode::Never:
      return "never";
  }
  return "auto";
}

std::optional<std::filesystem::path> discover_config_path(const char* xdg_config_home,
                                                          const char* home) {
  namespace fs = std::filesystem;

  const char* xdg = xdg_config_home ? xdg_config_home : std::getenv("XDG_CONFIG_HOME");
  if (xdg && xdg[0] != '\0') {
    fs::path p = fs::path(xdg) / "realmem" / "config.toml";
    std::error_code ec;
    if (fs::is_regular_file(p, ec)) {
      return p;
    }
  }

  const char* home_dir = home ? home : std::getenv("HOME");
  if (home_dir && home_dir[0] != '\0') {
    std::error_code ec;
    fs::path xdg_default = fs::path(home_dir) / ".config" / "realmem" / "config.toml";
    if (fs::is_regular_file(xdg_default, ec)) {
      return xdg_default;
    }
    fs::path dotfile = fs::path(home_dir) / ".realmem.toml";
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
    throw std::runtime_error(
        std::format("TOML parse error at line {}, col {}: {}",
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

ParseResult parse_args_and_config(int argc, const char* const* argv) {
  CLI::App app{
      "Rank macOS processes by phys_footprint (the metric Activity Monitor reports).\n"
      "Unlike RSS, phys_footprint excludes the reclaimable OS file cache."};
  app.footer(
      "\nA bare positional argument is a row count if all digits, otherwise an extended regex.\n"
      "Defaults can be configured in ~/.config/realmem/config.toml or ~/.realmem.toml.\n\n"
      "Examples:\n"
      "  realmem                 top 20 processes\n"
      "  realmem 40              top 40 processes\n"
      "  realmem -g              group helpers by application\n"
      "  realmem python          processes matching /python/\n"
      "  realmem python --sum    total footprint of python processes\n"
      "  realmem -s peak -l      sort by lifetime peak with full command lines");

  int cli_count = 20;
  std::string cli_match;
  std::string cli_sort;
  std::string cli_color;
  std::string cli_config_path;
  bool cli_reverse = false;
  bool cli_group = false;
  bool cli_no_group = false;
  bool cli_long = false;
  bool cli_no_long = false;
  bool cli_case = false;
  bool cli_sum = false;
  bool cli_bars = true;
  bool cli_no_bars = false;
  bool cli_no_system = false;
  bool cli_plain = false;
  bool cli_no_config = false;
  std::vector<std::string> positional;

  auto* opt_count = app.add_option("-n,--count", cli_count, "Maximum rows to print (0 for all)")
                        ->check(CLI::NonNegativeNumber);
  auto* opt_match = app.add_option("-m,--match", cli_match, "Filter by extended regex");
  auto* opt_sort =
      app.add_option("-s,--sort", cli_sort, "Sort by: footprint, peak, pid, name, count");
  auto* opt_reverse = app.add_flag("-r,--reverse", cli_reverse, "Reverse sort order");
  auto* opt_group = app.add_flag("-g,--group", cli_group, "Group processes by application");
  auto* opt_no_group =
      app.add_flag("--no-group", cli_no_group, "Disable grouping (overrides config)");
  auto* opt_long =
      app.add_flag("-l,--long", cli_long, "Add a column with the full command line");
  auto* opt_no_long =
      app.add_flag("--no-long", cli_no_long, "Hide command line column (overrides config)");
  auto* opt_case = app.add_flag("-c,--case", cli_case, "Match regex case-sensitively");
  auto* opt_sum = app.add_flag("--sum", cli_sum, "Print only the aggregate total for matches");
  auto* opt_bars = app.add_flag("--bars", cli_bars, "Show inline memory usage bars");
  auto* opt_no_bars = app.add_flag("--no-bars", cli_no_bars, "Hide inline memory usage bars");
  auto* opt_no_system =
      app.add_flag("--no-system", cli_no_system, "Hide the system memory summary");
  auto* opt_color =
      app.add_option("--color", cli_color, "Color output: auto, always, never");
  auto* opt_plain =
      app.add_flag("--plain", cli_plain, "Plain text output without FTXUI styling");
  auto* opt_config = app.add_option("--config", cli_config_path, "Path to TOML config file");
  app.add_flag("--no-config", cli_no_config, "Ignore any TOML config file");

  app.add_option("args", positional, "Optional row count and/or extended regex")->expected(0, 2);

  ParseResult result;
  try {
    app.parse(argc, argv);
  } catch (const CLI::CallForHelp& e) {
    result.should_exit = true;
    result.exit_code = 0;
    result.message = app.help();
    return result;
  } catch (const CLI::ParseError& e) {
    result.should_exit = true;
    result.exit_code = 2;
    result.message = std::format("realmem: {}\nRun 'realmem --help' for usage.\n", e.what());
    return result;
  }

  Config cfg;
  cfg.no_config = cli_no_config;

  // 1. Load TOML config first so CLI flags always take precedence.
  if (!cli_no_config) {
    try {
      if (opt_config->count() > 0) {
        cfg.config_path = std::filesystem::path(cli_config_path);
        load_toml_file(*cfg.config_path, cfg);
      } else if (auto discovered = discover_config_path()) {
        cfg.config_path = *discovered;
        load_toml_file(*cfg.config_path, cfg);
      }
    } catch (const std::exception& err) {
      result.should_exit = true;
      result.exit_code = 2;
      result.message = std::format("realmem: {}\n", err.what());
      return result;
    }
  }

  // 2. Apply explicit CLI overrides.
  if (opt_count->count() > 0) {
    cfg.count = cli_count;
  }
  if (opt_match->count() > 0) {
    cfg.pattern = cli_match;
  }
  if (opt_sort->count() > 0) {
    auto parsed = parse_sort_field(cli_sort);
    if (!parsed) {
      result.should_exit = true;
      result.exit_code = 2;
      result.message = std::format(
          "realmem: unknown sort field '{}' (expected footprint, peak, pid, name, or count)\n",
          cli_sort);
      return result;
    }
    cfg.sort = *parsed;
  }
  if (opt_reverse->count() > 0) {
    cfg.reverse = true;
  }
  if (opt_group->count() > 0) {
    cfg.group = true;
  }
  if (opt_no_group->count() > 0) {
    cfg.group = false;
  }
  if (opt_long->count() > 0) {
    cfg.long_format = true;
  }
  if (opt_no_long->count() > 0) {
    cfg.long_format = false;
  }
  if (opt_case->count() > 0) {
    cfg.case_sensitive = true;
  }
  if (opt_sum->count() > 0) {
    cfg.sum_only = true;
  }
  if (opt_bars->count() > 0) {
    cfg.show_bars = true;
  }
  if (opt_no_bars->count() > 0) {
    cfg.show_bars = false;
  }
  if (opt_no_system->count() > 0) {
    cfg.show_system = false;
  }
  if (opt_plain->count() > 0) {
    cfg.plain = true;
  }
  if (opt_color->count() > 0) {
    auto parsed = parse_color_mode(cli_color);
    if (!parsed) {
      result.should_exit = true;
      result.exit_code = 2;
      result.message = std::format(
          "realmem: unknown color mode '{}' (expected auto, always, or never)\n", cli_color);
      return result;
    }
    cfg.color = *parsed;
  }

  // 3. Interpret bare positional args (digits -> count, otherwise -> regex).
  for (const auto& arg : positional) {
    if (is_all_digits(arg)) {
      try {
        cfg.count = std::stoi(arg);
      } catch (const std::exception&) {
        result.should_exit = true;
        result.exit_code = 2;
        result.message = std::format("realmem: invalid row count '{}'\n", arg);
        return result;
      }
    } else {
      cfg.pattern = arg;
    }
  }

  // 4. Semantic validation.
  if (cfg.sum_only && !cfg.pattern) {
    result.should_exit = true;
    result.exit_code = 2;
    result.message = "realmem: --sum needs a regex to sum over\n";
    return result;
  }
  if (cfg.sort == SortField::Count && !cfg.group) {
    result.should_exit = true;
    result.exit_code = 2;
    result.message = "realmem: --sort count requires --group (-g)\n";
    return result;
  }
  if (cfg.sort == SortField::Pid && cfg.group) {
    result.should_exit = true;
    result.exit_code = 2;
    result.message = "realmem: --sort pid cannot be used with --group (-g)\n";
    return result;
  }

  result.config = std::move(cfg);
  return result;
}

}  // namespace realmem
