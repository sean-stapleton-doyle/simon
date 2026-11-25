#include "config.hpp"
#include "gpu.hpp"
#include "model.hpp"
#include "process.hpp"
#include "render.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool cond, const char* expr, int line) {
  if (!cond) {
    std::cerr << "FAIL line " << line << ": " << expr << '\n';
    ++g_failures;
  }
}

#define EXPECT(expr) expect((expr), #expr, __LINE__)

void test_humanize_and_utf8() {
  using namespace realmem;
  EXPECT(humanize_bytes(0) == "0.0B");
  EXPECT(humanize_bytes(1024) == "1.0K");
  EXPECT(humanize_bytes(1536 * 1024) == "1.5M");
  EXPECT(humanize_bytes(2ULL * 1024 * 1024 * 1024) == "2.0G");

  EXPECT(truncate_utf8("hello", 5) == "hello");
  EXPECT(truncate_utf8("hello world", 5) == "hell\xe2\x80\xa6");
  // Multi-byte UTF-8: "café!" (5 code points)
  EXPECT(truncate_utf8("caf\xc3\xa9!", 5) == "caf\xc3\xa9!");
  EXPECT(truncate_utf8("caf\xc3\xa9!extra", 5) == "caf\xc3\xa9\xe2\x80\xa6");

  EXPECT(sanitize_cmdline("python\t-c\n'print(1)'\r\n") == "python -c 'print(1)'  ");
}

void test_graphics_ledger_math() {
  using namespace realmem;
  EXPECT(combine_graphics_ledger(100, 50) == 150);
  EXPECT(combine_graphics_ledger(0, 0) == 0);
  EXPECT(combine_graphics_ledger(-42, 100) == 100);
  EXPECT(combine_graphics_ledger(100, -10) == 100);
}

void test_toml_loading_and_precedence() {
  using namespace realmem;

  Config cfg;
  load_toml_string(R"(
    count = 35
    sort = "peak"
    reverse = true
    group = true
    gpu = true
    bars = false
    color = "never"

    [[grouping.rules]]
    pattern = "python3.*train"
    name = "ML Trainer"
  )",
                   cfg);

  EXPECT(cfg.count == 35);
  EXPECT(cfg.sort == SortField::Peak);
  EXPECT(cfg.reverse == true);
  EXPECT(cfg.group == true);
  EXPECT(cfg.show_gpu == true);
  EXPECT(cfg.show_bars == false);
  EXPECT(cfg.color == ColorMode::Never);
  EXPECT(cfg.group_rules.size() == 1);
  EXPECT(cfg.group_rules[0].name == "ML Trainer");

  // Invalid config throws.
  bool threw = false;
  try {
    Config bad;
    load_toml_string("sort = \"bogus\"\n", bad);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  EXPECT(threw);

  // CLI overrides TOML file (including --no-gpu).
  namespace fs = std::filesystem;
  const fs::path tmp = fs::temp_directory_path() / "realmem_test_config.toml";
  {
    std::ofstream f(tmp);
    f << "count = 50\nsort = \"peak\"\ngroup = true\ngpu = true\n";
  }

  const std::string cfg_path = tmp.string();
  const char* argv[] = {"realmem",   "--config", cfg_path.c_str(), "-n",  "12",
                        "--no-group", "--no-gpu", "-s",             "pid"};
  const ParseResult res = parse_args_and_config(9, argv);
  fs::remove(tmp);

  EXPECT(!res.should_exit);
  EXPECT(res.config.count == 12);
  EXPECT(res.config.group == false);
  EXPECT(res.config.show_gpu == false);
  EXPECT(res.config.sort == SortField::Pid);
}

void test_bare_positional_and_gpu_cli() {
  using namespace realmem;

  const char* argv1[] = {"realmem", "--no-config", "40"};
  const ParseResult r1 = parse_args_and_config(3, argv1);
  EXPECT(!r1.should_exit);
  EXPECT(r1.config.count == 40);
  EXPECT(!r1.config.pattern.has_value());

  const char* argv2[] = {"realmem", "--no-config", "python", "--sum"};
  const ParseResult r2 = parse_args_and_config(4, argv2);
  EXPECT(!r2.should_exit);
  EXPECT(r2.config.pattern == "python");
  EXPECT(r2.config.sum_only == true);

  // --sort gpu automatically enables the GPU column unless --no-gpu is passed.
  const char* argv_gpu[] = {"realmem", "--no-config", "--sort", "gpu"};
  const ParseResult r_gpu = parse_args_and_config(4, argv_gpu);
  EXPECT(!r_gpu.should_exit);
  EXPECT(r_gpu.config.sort == SortField::Gpu);
  EXPECT(r_gpu.config.show_gpu == true);

  const char* argv_nogpu[] = {"realmem", "--no-config", "--sort", "gpu", "--no-gpu"};
  const ParseResult r_nogpu = parse_args_and_config(5, argv_nogpu);
  EXPECT(!r_nogpu.should_exit);
  EXPECT(r_nogpu.config.sort == SortField::Gpu);
  EXPECT(r_nogpu.config.show_gpu == false);

  // --sum without regex must fail.
  const char* argv3[] = {"realmem", "--no-config", "--sum"};
  const ParseResult r3 = parse_args_and_config(3, argv3);
  EXPECT(r3.should_exit);
  EXPECT(r3.exit_code == 2);

  // --sort count without --group must fail.
  const char* argv4[] = {"realmem", "--no-config", "--sort", "count"};
  const ParseResult r4 = parse_args_and_config(4, argv4);
  EXPECT(r4.should_exit);
  EXPECT(r4.exit_code == 2);
}

void test_grouping_filtering_and_sorting() {
  using namespace realmem;

  Snapshot snap;
  snap.total_pids = 5;
  snap.processes = {
      ProcessInfo{101, 500, 900, 200, "Google Chrome Helper (Renderer)",
                  "/Applications/Google Chrome.app/Contents/Frameworks/Google Chrome Helper.app/Contents/MacOS/Google Chrome Helper",
                  "/Applications/Google Chrome.app/... --type=renderer"},
      ProcessInfo{102, 300, 400, 150, "Google Chrome",
                  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
                  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"},
      ProcessInfo{103, 700, 1200, 600, "python3.12", "/usr/bin/python3", "python3 train.py"},
      ProcessInfo{104, 200, 250, std::nullopt, "com.apple.WebKit.WebContent", "", ""},
  };
  snap.total_footprint = 1700;

  // Test grouping via .app bundle + WebKit fallback + custom rule + GPU sum.
  Config cfg;
  cfg.group = true;
  cfg.show_gpu = true;
  cfg.sort = SortField::Footprint;
  cfg.group_rules.push_back(GroupRule{"train\\.py", "Training"});

  const QuerySummary grouped = build_query_summary(snap, cfg);
  EXPECT(grouped.rows.size() == 3);
  EXPECT(grouped.gpu_readable_procs == 3);
  EXPECT(grouped.rows[0].name == "Google Chrome");
  EXPECT(grouped.rows[0].proc_count == 2);
  EXPECT(grouped.rows[0].footprint == 800);
  EXPECT(grouped.rows[0].peak == 1300);
  EXPECT(grouped.rows[0].gpu_footprint == 350);
  EXPECT(grouped.rows[1].name == "Training");
  EXPECT(grouped.rows[1].gpu_footprint == 600);
  EXPECT(grouped.rows[2].name == "WebKit");
  EXPECT(!grouped.rows[2].gpu_footprint.has_value());

  // Test sorting by GPU (known descending, nullopt always last even when reversed).
  cfg.sort = SortField::Gpu;
  const QuerySummary by_gpu = build_query_summary(snap, cfg);
  EXPECT(by_gpu.rows[0].name == "Training");
  EXPECT(by_gpu.rows[1].name == "Google Chrome");
  EXPECT(by_gpu.rows[2].name == "WebKit");

  cfg.reverse = true;
  const QuerySummary by_gpu_rev = build_query_summary(snap, cfg);
  EXPECT(by_gpu_rev.rows[0].name == "Google Chrome");
  EXPECT(by_gpu_rev.rows[1].name == "Training");
  EXPECT(by_gpu_rev.rows[2].name == "WebKit");

  // Test sorting by count.
  cfg.reverse = false;
  cfg.sort = SortField::Count;
  const QuerySummary by_cnt = build_query_summary(snap, cfg);
  EXPECT(by_cnt.rows[0].name == "Google Chrome");
  EXPECT(by_cnt.rows[0].proc_count == 2);

  // Test regex filtering (case-insensitive by default).
  Config fcfg;
  fcfg.pattern = "CHROME";
  const QuerySummary filtered = build_query_summary(snap, fcfg);
  EXPECT(filtered.matched_procs == 2);
  EXPECT(filtered.matched_footprint == 800);
  EXPECT(filtered.largest_pid == 101);
  EXPECT(filtered.rows.size() == 2);

  // Case-sensitive regex excludes lowercase-mismatched pattern.
  fcfg.case_sensitive = true;
  const QuerySummary case_filtered = build_query_summary(snap, fcfg);
  EXPECT(case_filtered.matched_procs == 0);

  // Reverse sorting by name.
  Config scfg;
  scfg.sort = SortField::Name;
  scfg.reverse = true;
  const QuerySummary rev_name = build_query_summary(snap, scfg);
  EXPECT(rev_name.rows.front().name == "python3.12");

  // Invalid regex throws std::runtime_error.
  bool bad_re_threw = false;
  try {
    Config bcfg;
    bcfg.pattern = "[unclosed";
    (void)build_query_summary(snap, bcfg);
  } catch (const std::runtime_error&) {
    bad_re_threw = true;
  }
  EXPECT(bad_re_threw);
}

void test_config_discovery() {
  using namespace realmem;
  namespace fs = std::filesystem;

  const fs::path base = fs::temp_directory_path() / "realmem_test_home";
  fs::remove_all(base);
  fs::create_directories(base / ".config" / "realmem");

  const fs::path dotfile = base / ".realmem.toml";
  {
    std::ofstream f(dotfile);
    f << "count = 10\n";
  }
  const auto found_dot = discover_config_path("", base.c_str());
  EXPECT(found_dot.has_value() && *found_dot == dotfile);

  // ~/.config/realmem/config.toml takes precedence over ~/.realmem.toml.
  const fs::path xdg_default = base / ".config" / "realmem" / "config.toml";
  {
    std::ofstream f(xdg_default);
    f << "count = 15\n";
  }
  const auto found_xdg = discover_config_path("", base.c_str());
  EXPECT(found_xdg.has_value() && *found_xdg == xdg_default);

  fs::remove_all(base);
}

void test_rendering_plain_and_ftxui() {
  using namespace realmem;

  Snapshot snap;
  snap.total_pids = 10;
  snap.processes = {
      ProcessInfo{42, 1024ULL * 1024 * 600, 1024ULL * 1024 * 900, 1024ULL * 1024 * 256, "ghostty",
                  "", "ghostty --title demo"},
      ProcessInfo{99, 1024ULL * 1024 * 120, 1024ULL * 1024 * 150, std::nullopt, "WindowServer", "",
                  ""},
  };
  snap.total_footprint = snap.processes[0].footprint + snap.processes[1].footprint;

  SystemMemory sys;
  sys.total = 48ULL * 1024 * 1024 * 1024;
  sys.wired = 6ULL * 1024 * 1024 * 1024;
  sys.anonymous = 18ULL * 1024 * 1024 * 1024;
  sys.compressor = 3ULL * 1024 * 1024 * 1024;
  sys.file_cache = 12ULL * 1024 * 1024 * 1024;
  sys.gpu.allocated = 2ULL * 1024 * 1024 * 1024;
  sys.gpu.in_use = 512ULL * 1024 * 1024;
  sys.gpu.valid = true;
  sys.valid = true;

  Config cfg;
  cfg.show_gpu = true;
  const QuerySummary summary = build_query_summary(snap, cfg);

  // Plain rendering with GPU column and system GPU line.
  const std::string plain = render_output(summary, sys, cfg, RenderContext{100, false});
  EXPECT(plain.find("GPU") != std::string::npos);
  EXPECT(plain.find("256.0M") != std::string::npos);
  EXPECT(plain.find("6.0G wired") != std::string::npos);
  EXPECT(plain.find("gpu: 2.0G alloc (512.0M in use)") != std::string::npos);
  EXPECT(plain.find("requires sudo") == std::string::npos);
  EXPECT(plain.find("\x1b[") == std::string::npos);

  // FTXUI rich rendering (wide + narrow 80-col long + GPU mode).
  cfg.color = ColorMode::Always;
  const std::string rich = render_output(summary, sys, cfg, RenderContext{110, true});
  EXPECT(rich.find("ghostty") != std::string::npos);
  EXPECT(rich.find("256.0M") != std::string::npos);
  EXPECT(rich.find("2.0G") != std::string::npos);
  EXPECT(rich.find("512.0M") != std::string::npos);
  EXPECT(rich.find("\x1b[") != std::string::npos);
  EXPECT(rich.find('\r') == std::string::npos);

  cfg.long_format = true;
  const std::string narrow_long = render_output(summary, sys, cfg, RenderContext{80, true});
  EXPECT(narrow_long.find("FOOTPRINT") != std::string::npos);
  EXPECT(narrow_long.find("GPU") != std::string::npos);
  EXPECT(narrow_long.find("PEAK") != std::string::npos);
  EXPECT(narrow_long.find("file cache (reclaimable)") != std::string::npos);

  // Unprivileged --gpu hint appears when no process had readable GPU ledger.
  Snapshot no_gpu_snap;
  no_gpu_snap.total_pids = 1;
  no_gpu_snap.processes = {
      ProcessInfo{7, 1024 * 1024, 1024 * 1024, std::nullopt, "zsh", "", "zsh"},
  };
  no_gpu_snap.total_footprint = 1024 * 1024;
  Config hint_cfg;
  hint_cfg.show_gpu = true;
  const QuerySummary no_gpu_summary = build_query_summary(no_gpu_snap, hint_cfg);
  const std::string hinted = render_output(no_gpu_summary, sys, hint_cfg, RenderContext{100, false});
  EXPECT(hinted.find("per-process GPU requires sudo") != std::string::npos);
}

void test_json_output_and_skill() {
  using namespace realmem;
  using json = nlohmann::json;

  // CLI -o json and TOML output = "json".
  const char* argv_json[] = {"realmem", "--no-config", "-o", "json", "5"};
  const ParseResult r_json = parse_args_and_config(5, argv_json);
  EXPECT(!r_json.should_exit);
  EXPECT(r_json.config.output == OutputFormat::Json);
  EXPECT(r_json.config.count == 5);

  Config toml_cfg;
  load_toml_string("output = \"json\"\n", toml_cfg);
  EXPECT(toml_cfg.output == OutputFormat::Json);

  // --skill exits cleanly and outputs embedded SKILL.md frontmatter.
  const char* argv_skill[] = {"realmem", "--skill"};
  const ParseResult r_skill = parse_args_and_config(2, argv_skill);
  EXPECT(r_skill.should_exit);
  EXPECT(r_skill.exit_code == 0);
  EXPECT(r_skill.message.find("name: realmem") != std::string::npos);
  EXPECT(r_skill.message.find("--output json") != std::string::npos);

  // Build realistic snapshot + system stats and verify parsed JSON schema.
  Snapshot snap;
  snap.total_pids = 10;
  snap.processes = {
      ProcessInfo{42, 600, 900, 256, "ghostty", "/Applications/Ghostty.app/Contents/MacOS/ghostty",
                  "ghostty --title demo"},
      ProcessInfo{99, 120, 150, std::nullopt, "WindowServer", "", ""},
  };
  snap.total_footprint = 720;

  SystemMemory sys;
  sys.total = 48000;
  sys.wired = 6000;
  sys.anonymous = 18000;
  sys.compressor = 3000;
  sys.file_cache = 12000;
  sys.free_bytes = 9000;
  sys.gpu.allocated = 2000;
  sys.gpu.in_use = 500;
  sys.gpu.valid = true;
  sys.valid = true;

  Config cfg;
  cfg.output = OutputFormat::Json;
  cfg.show_gpu = true;
  const QuerySummary summary = build_query_summary(snap, cfg);
  const json parsed = json::parse(render_output(summary, sys, cfg, RenderContext{80, true}));

  EXPECT(parsed["mode"] == "processes");
  EXPECT(parsed["summary"]["readable_processes"] == 2);
  EXPECT(parsed["summary"]["total_processes"] == 10);
  EXPECT(parsed["summary"]["total_footprint_bytes"] == 720);
  EXPECT(parsed["summary"]["gpu_readable_processes"] == 1);
  EXPECT(parsed["rows"].size() == 2);
  EXPECT(parsed["rows"][0]["pid"] == 42);
  EXPECT(parsed["rows"][0]["footprint_bytes"] == 600);
  EXPECT(parsed["rows"][0]["gpu_bytes"] == 256);
  EXPECT(parsed["rows"][0]["cmdline"] == "ghostty --title demo");
  EXPECT(parsed["rows"][1]["gpu_bytes"].is_null());
  EXPECT(parsed["system"]["wired_bytes"] == 6000);
  EXPECT(parsed["system"]["gpu"]["allocated_bytes"] == 2000);
  EXPECT(parsed["system"]["gpu"]["in_use_bytes"] == 500);

  // Grouped + --sum JSON omits rows/system and populates pattern totals.
  Config sum_cfg;
  sum_cfg.output = OutputFormat::Json;
  sum_cfg.group = true;
  sum_cfg.pattern = "ghostty";
  sum_cfg.sum_only = true;
  const QuerySummary sum_summary = build_query_summary(snap, sum_cfg);
  const json sum_json =
      json::parse(render_output(sum_summary, sys, sum_cfg, RenderContext{80, false}));
  EXPECT(sum_json["mode"] == "grouped");
  EXPECT(sum_json["summary"]["matched_processes"] == 1);
  EXPECT(sum_json["summary"]["matched_footprint_bytes"] == 600);
  EXPECT(!sum_json.contains("rows"));
  EXPECT(!sum_json.contains("system"));
}

}  // namespace

int main() {
  test_humanize_and_utf8();
  test_graphics_ledger_math();
  test_toml_loading_and_precedence();
  test_bare_positional_and_gpu_cli();
  test_grouping_filtering_and_sorting();
  test_config_discovery();
  test_rendering_plain_and_ftxui();
  test_json_output_and_skill();

  if (g_failures == 0) {
    std::cout << "All tests passed.\n";
    return 0;
  }
  std::cerr << g_failures << " test failure(s).\n";
  return 1;
}
