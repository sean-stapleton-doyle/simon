#include "config/config.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

using namespace simon;

struct TempDir {
  std::filesystem::path path;
  explicit TempDir(const char* name) : path(std::filesystem::temp_directory_path() / name) {
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
  }
  ~TempDir() { std::filesystem::remove_all(path); }
};

}  // namespace

TEST_CASE("parse_sort_field and to_string round-trips and aliases", "[config]") {
  REQUIRE(parse_sort_field("footprint") == SortField::Footprint);
  REQUIRE(parse_sort_field("MEM") == SortField::Footprint);
  REQUIRE(parse_sort_field("memory") == SortField::Footprint);
  REQUIRE(parse_sort_field("peak") == SortField::Peak);
  REQUIRE(parse_sort_field("max") == SortField::Peak);
  REQUIRE(parse_sort_field("gpu") == SortField::Gpu);
  REQUIRE(parse_sort_field("graphics") == SortField::Gpu);
  REQUIRE(parse_sort_field("cpu") == SortField::Cpu);
  REQUIRE(parse_sort_field("disk") == SortField::Disk);
  REQUIRE(parse_sort_field("io") == SortField::Disk);
  REQUIRE(parse_sort_field("pid") == SortField::Pid);
  REQUIRE(parse_sort_field("name") == SortField::Name);
  REQUIRE(parse_sort_field("command") == SortField::Name);
  REQUIRE(parse_sort_field("app") == SortField::Name);
  REQUIRE(parse_sort_field("count") == SortField::Count);
  REQUIRE(parse_sort_field("procs") == SortField::Count);
  REQUIRE_FALSE(parse_sort_field("unknown").has_value());

  for (SortField f : {SortField::Footprint, SortField::Peak, SortField::Gpu, SortField::Cpu,
                      SortField::Disk, SortField::Pid, SortField::Name, SortField::Count}) {
    REQUIRE(parse_sort_field(to_string(f)) == f);
  }
}

TEST_CASE("parse_color_mode and parse_output_format aliases", "[config]") {
  REQUIRE(parse_color_mode("auto") == ColorMode::Auto);
  REQUIRE(parse_color_mode("always") == ColorMode::Always);
  REQUIRE(parse_color_mode("on") == ColorMode::Always);
  REQUIRE(parse_color_mode("true") == ColorMode::Always);
  REQUIRE(parse_color_mode("never") == ColorMode::Never);
  REQUIRE(parse_color_mode("off") == ColorMode::Never);
  REQUIRE(parse_color_mode("false") == ColorMode::Never);
  REQUIRE_FALSE(parse_color_mode("neon").has_value());

  REQUIRE(parse_output_format("auto") == OutputFormat::Auto);
  REQUIRE(parse_output_format("table") == OutputFormat::Table);
  REQUIRE(parse_output_format("rich") == OutputFormat::Table);
  REQUIRE(parse_output_format("ftxui") == OutputFormat::Table);
  REQUIRE(parse_output_format("plain") == OutputFormat::Plain);
  REQUIRE(parse_output_format("text") == OutputFormat::Plain);
  REQUIRE(parse_output_format("json") == OutputFormat::Json);
  REQUIRE_FALSE(parse_output_format("yaml").has_value());

  for (ColorMode m : {ColorMode::Auto, ColorMode::Always, ColorMode::Never}) {
    REQUIRE(parse_color_mode(to_string(m)) == m);
  }
  for (OutputFormat o :
       {OutputFormat::Auto, OutputFormat::Table, OutputFormat::Plain, OutputFormat::Json}) {
    REQUIRE(parse_output_format(to_string(o)) == o);
  }
}

TEST_CASE("load_toml_string parses full configuration and [defaults] section", "[config]") {
  SECTION("top-level keys and grouping rules") {
    Config cfg;
    load_toml_string(R"(
      count = 35
      sort = "peak"
      reverse = true
      group = true
      gpu = true
      cpu = true
      disk = true
      short = true
      long = true
      case_sensitive = true
      interval_ms = 500
      sample_ms = 300
      bars = false
      system = false
      color = "never"
      output = "json"
      plain = true

      [[grouping.rules]]
      pattern = "python3.*train"
      name = "ML Trainer"
    )",
                     cfg);

    REQUIRE(cfg.count == 35);
    REQUIRE(cfg.sort == SortField::Peak);
    REQUIRE(cfg.reverse);
    REQUIRE(cfg.group);
    REQUIRE(cfg.show_gpu);
    REQUIRE(cfg.show_cpu);
    REQUIRE(cfg.show_disk);
    REQUIRE(cfg.short_mode);
    REQUIRE(cfg.long_format);
    REQUIRE(cfg.case_sensitive);
    REQUIRE(cfg.interval_ms == 500);
    REQUIRE(cfg.sample_ms == 300);
    REQUIRE_FALSE(cfg.show_bars);
    REQUIRE_FALSE(cfg.show_system);
    REQUIRE(cfg.color == ColorMode::Never);
    REQUIRE(cfg.output == OutputFormat::Json);
    REQUIRE(cfg.plain);
    REQUIRE(cfg.group_rules.size() == 1);
    REQUIRE(cfg.group_rules[0].pattern == "python3.*train");
    REQUIRE(cfg.group_rules[0].name == "ML Trainer");
  }

  SECTION("[defaults] section overrides top-level defaults cleanly") {
    Config cfg;
    load_toml_string(R"(
      [defaults]
      count = 12
      sort = "cpu"
      tree = true
    )",
                     cfg);
    REQUIRE(cfg.count == 12);
    REQUIRE(cfg.sort == SortField::Cpu);
    REQUIRE(cfg.tree);
  }
}

TEST_CASE("load_toml_string rejects malformed syntax, wrong types, and out-of-range values",
          "[config]") {
  Config cfg;
  REQUIRE_THROWS_AS(load_toml_string("count = [unclosed\n", cfg), std::runtime_error);
  REQUIRE_THROWS_AS(load_toml_string("count = \"twenty\"\n", cfg), std::runtime_error);
  REQUIRE_THROWS_AS(load_toml_string("count = -1\n", cfg), std::runtime_error);
  REQUIRE_THROWS_AS(load_toml_string("interval_ms = 50\n", cfg), std::runtime_error);
  REQUIRE_THROWS_AS(load_toml_string("sample_ms = 10\n", cfg), std::runtime_error);
  REQUIRE_THROWS_AS(load_toml_string("group = \"yes\"\n", cfg), std::runtime_error);
  REQUIRE_THROWS_AS(load_toml_string("sort = \"bogus\"\n", cfg), std::runtime_error);
  REQUIRE_THROWS_AS(load_toml_string("color = \"rainbow\"\n", cfg), std::runtime_error);
  REQUIRE_THROWS_AS(load_toml_string("output = \"xml\"\n", cfg), std::runtime_error);
  REQUIRE_THROWS_AS(load_toml_string("sort = 123\n", cfg), std::runtime_error);
  REQUIRE_THROWS_AS(load_toml_string("[[grouping.rules]]\npattern = \"foo\"\n", cfg),
                    std::runtime_error);
  REQUIRE_THROWS_AS(load_toml_string("grouping = { rules = [\"not_a_table\"] }\n", cfg),
                    std::runtime_error);
}

TEST_CASE("discover_config_path respects XDG > ~/.config > ~/.simon.toml precedence", "[config]") {
  TempDir tmp("simon_test_discover");
  const auto xdg_root = tmp.path / "xdg";
  const auto home_root = tmp.path / "home";
  std::filesystem::create_directories(xdg_root / "simon");
  std::filesystem::create_directories(home_root / ".config" / "simon");

  REQUIRE_FALSE(discover_config_path(xdg_root.c_str(), home_root.c_str()).has_value());

  const auto dotfile = home_root / ".simon.toml";
  { std::ofstream f(dotfile); f << "count = 10\n"; }
  REQUIRE(discover_config_path("", home_root.c_str()) == dotfile);

  const auto home_xdg = home_root / ".config" / "simon" / "config.toml";
  { std::ofstream f(home_xdg); f << "count = 15\n"; }
  REQUIRE(discover_config_path("", home_root.c_str()) == home_xdg);

  const auto custom_xdg = xdg_root / "simon" / "config.toml";
  { std::ofstream f(custom_xdg); f << "count = 20\n"; }
  REQUIRE(discover_config_path(xdg_root.c_str(), home_root.c_str()) == custom_xdg);
}

TEST_CASE("CLI parsing, TOML override precedence, and positional arguments", "[config]") {
  SECTION("CLI overrides TOML config file") {
    TempDir tmp("simon_test_cli_override");
    const auto cfg_file = tmp.path / "config.toml";
    {
      std::ofstream f(cfg_file);
      f << "count = 50\nsort = \"peak\"\ngroup = true\ngpu = true\nshort = true\nplain = true\n";
    }
    const std::string p = cfg_file.string();
    const char* argv[] = {"simon", "--config", p.c_str(), "-t", "-n", "12",
                          "--no-gpu", "--no-short", "-o", "json"};
    const ParseResult res = parse_args_and_config(10, argv);

    REQUIRE_FALSE(res.should_exit);
    REQUIRE(res.config.tree);
    REQUIRE_FALSE(res.config.group);
    REQUIRE(res.config.count == 12);
    REQUIRE_FALSE(res.config.show_gpu);
    REQUIRE_FALSE(res.config.short_mode);
    REQUIRE(res.config.output == OutputFormat::Json);
    REQUIRE_FALSE(res.config.plain);
  }

  SECTION("nonexistent explicit --config returns error") {
    const char* argv[] = {"simon", "--config", "/tmp/does_not_exist_simon_99999.toml"};
    const ParseResult res = parse_args_and_config(3, argv);
    REQUIRE(res.should_exit);
    REQUIRE(res.exit_code == 2);
  }

  SECTION("two bare positional arguments work in either order") {
    const char* argv1[] = {"simon", "--no-config", "15", "python"};
    const ParseResult r1 = parse_args_and_config(4, argv1);
    REQUIRE_FALSE(r1.should_exit);
    REQUIRE(r1.config.count == 15);
    REQUIRE(r1.config.pattern == "python");

    const char* argv2[] = {"simon", "--no-config", "python", "15"};
    const ParseResult r2 = parse_args_and_config(4, argv2);
    REQUIRE_FALSE(r2.should_exit);
    REQUIRE(r2.config.count == 15);
    REQUIRE(r2.config.pattern == "python");
  }

  SECTION("overflowing numeric positional argument returns exit code 2") {
    const char* argv[] = {"simon", "--no-config", "99999999999999999999999"};
    const ParseResult res = parse_args_and_config(3, argv);
    REQUIRE(res.should_exit);
    REQUIRE(res.exit_code == 2);
  }

  SECTION("implicit columns enabled by --sort gpu/cpu/disk unless disabled") {
    const char* argv_gpu[] = {"simon", "--no-config", "-s", "gpu"};
    REQUIRE(parse_args_and_config(4, argv_gpu).config.show_gpu);

    const char* argv_nogpu[] = {"simon", "--no-config", "-s", "gpu", "--no-gpu"};
    REQUIRE_FALSE(parse_args_and_config(5, argv_nogpu).config.show_gpu);

    const char* argv_cpu[] = {"simon", "--no-config", "-s", "cpu"};
    REQUIRE(parse_args_and_config(4, argv_cpu).config.show_cpu);

    const char* argv_disk[] = {"simon", "--no-config", "-s", "disk"};
    REQUIRE(parse_args_and_config(4, argv_disk).config.show_disk);
  }

  SECTION("--help and --skill exit cleanly with code 0") {
    const char* argv_h[] = {"simon", "--help"};
    const ParseResult rh = parse_args_and_config(2, argv_h);
    REQUIRE(rh.should_exit);
    REQUIRE(rh.exit_code == 0);
    REQUIRE(rh.message.find("--short") != std::string::npos);

    const char* argv_s[] = {"simon", "--skill"};
    const ParseResult rs = parse_args_and_config(2, argv_s);
    REQUIRE(rs.should_exit);
    REQUIRE(rs.exit_code == 0);
    REQUIRE(rs.message.find("name: simon") != std::string::npos);
    REQUIRE(rs.message.find("--output json") != std::string::npos);
  }
}

TEST_CASE("validate_config rejects mutually incompatible option combinations", "[config]") {
  const char* bad_cases[][6] = {
      {"simon", "--no-config", "-g", "-t", nullptr, nullptr},
      {"simon", "--no-config", "--sum", nullptr, nullptr, nullptr},
      {"simon", "--no-config", "-s", "count", nullptr, nullptr},
      {"simon", "--no-config", "-g", "-s", "pid", nullptr},
      {"simon", "--no-config", "-w", "-o", "json", nullptr},
      {"simon", "--no-config", "-w", "-S", nullptr, nullptr},
      {"simon", "--no-config", "-S", "--sum", "py", nullptr},
      {"simon", "--no-config", "-S", "--no-system", nullptr, nullptr},
  };

  for (const auto& row : bad_cases) {
    int argc = 0;
    while (argc < 6 && row[argc] != nullptr) ++argc;
    const ParseResult res = parse_args_and_config(argc, row);
    REQUIRE(res.should_exit);
    REQUIRE(res.exit_code == 2);
  }
}
