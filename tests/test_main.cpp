#include "config/config.hpp"
#include "domain/delta.hpp"
#include "domain/grouping.hpp"
#include "domain/query.hpp"
#include "domain/tree.hpp"
#include "platform/macos/gpu_sampler.hpp"
#include "platform/macos/ioreport_sampler.hpp"
#include "platform/macos/memory_sampler.hpp"
#include "platform/macos/process_sampler.hpp"
#include "platform/monitor.hpp"
#include "render/format_utils.hpp"
#include "render/renderer.hpp"
#include "tui/tui_state.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
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
  using namespace simon;
  EXPECT(humanize_bytes(0) == "0.0B");
  EXPECT(humanize_bytes(1024) == "1.0K");
  EXPECT(humanize_bytes(1536 * 1024) == "1.5M");
  EXPECT(humanize_bytes(2ULL * 1024 * 1024 * 1024) == "2.0G");
  EXPECT(format_rate(1024.0 * 1024.0 * 10.0) == "10.0M/s");

  EXPECT(truncate_utf8("hello", 5) == "hello");
  EXPECT(truncate_utf8("hello world", 5) == "hell\xe2\x80\xa6");
  EXPECT(truncate_utf8("caf\xc3\xa9!", 5) == "caf\xc3\xa9!");
  EXPECT(truncate_utf8("caf\xc3\xa9!extra", 5) == "caf\xc3\xa9\xe2\x80\xa6");

  EXPECT(sanitize_cmdline("python\t-c\n'print(1)'\r\n") == "python -c 'print(1)'  ");
}

void test_graphics_ledger_and_pressure() {
  using namespace simon;
  EXPECT(combine_graphics_ledger(100, 50) == 150);
  EXPECT(combine_graphics_ledger(-42, 100) == 100);
  EXPECT(parse_pressure_level(1) == MemoryPressureLevel::Normal);
  EXPECT(parse_pressure_level(2) == MemoryPressureLevel::Warn);
  EXPECT(parse_pressure_level(4) == MemoryPressureLevel::Critical);
  EXPECT(parse_pressure_level(99) == MemoryPressureLevel::Unknown);

  GpuInfo gpu;
  gpu.valid = true;
  gpu.allocated = 10ULL * 1024 * 1024 * 1024;
  gpu.recommended_max_working_set = 36ULL * 1024 * 1024 * 1024;
  EXPECT(gpu.effective_ceiling() == 36ULL * 1024 * 1024 * 1024);
  EXPECT(gpu.headroom_bytes() == 26ULL * 1024 * 1024 * 1024);

  SystemMemory mem;
  mem.compressor = 4ULL * 1024 * 1024 * 1024;
  mem.compressor_uncompressed = 12ULL * 1024 * 1024 * 1024;
  EXPECT(std::abs(mem.compression_ratio() - 3.0) < 0.01);
}

void test_toml_loading_and_precedence() {
  using namespace simon;

  Config cfg;
  load_toml_string(R"(
    count = 35
    sort = "peak"
    reverse = true
    group = true
    gpu = true
    cpu = true
    disk = true
    sample_ms = 300
    bars = false
    color = "never"
    output = "json"

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
  EXPECT(cfg.show_cpu == true);
  EXPECT(cfg.show_disk == true);
  EXPECT(cfg.sample_ms == 300);
  EXPECT(cfg.show_bars == false);
  EXPECT(cfg.color == ColorMode::Never);
  EXPECT(cfg.output == OutputFormat::Json);
  EXPECT(cfg.group_rules.size() == 1);
  EXPECT(cfg.group_rules[0].name == "ML Trainer");

  // CLI -t overrides TOML group=true cleanly.
  namespace fs = std::filesystem;
  const fs::path tmp = fs::temp_directory_path() / "simon_test_config.toml";
  {
    std::ofstream f(tmp);
    f << "count = 50\nsort = \"peak\"\ngroup = true\ngpu = true\n";
  }

  const std::string cfg_path = tmp.string();
  const char* argv[] = {"simon", "--config", cfg_path.c_str(), "-t", "-n", "12", "--no-gpu"};
  const ParseResult res = parse_args_and_config(7, argv);
  fs::remove(tmp);

  EXPECT(!res.should_exit);
  EXPECT(res.config.tree == true);
  EXPECT(res.config.group == false);
  EXPECT(res.config.count == 12);
  EXPECT(res.config.show_gpu == false);
}

void test_cli_validation() {
  using namespace simon;

  // -g and -t together must fail.
  const char* argv_gt[] = {"simon", "--no-config", "-g", "-t"};
  const ParseResult r_gt = parse_args_and_config(4, argv_gt);
  EXPECT(r_gt.should_exit && r_gt.exit_code == 2);

  // --sort gpu enables --gpu automatically.
  const char* argv_gpu[] = {"simon", "--no-config", "--sort", "gpu"};
  const ParseResult r_gpu = parse_args_and_config(4, argv_gpu);
  EXPECT(!r_gpu.should_exit && r_gpu.config.show_gpu);

  // --sort cpu enables --cpu automatically.
  const char* argv_cpu[] = {"simon", "--no-config", "--sort", "cpu"};
  const ParseResult r_cpu = parse_args_and_config(4, argv_cpu);
  EXPECT(!r_cpu.should_exit && r_cpu.config.show_cpu);

  // --sum without regex must fail.
  const char* argv_sum[] = {"simon", "--no-config", "--sum"};
  const ParseResult r_sum = parse_args_and_config(3, argv_sum);
  EXPECT(r_sum.should_exit && r_sum.exit_code == 2);

  // --skill prints embedded SKILL.md.
  const char* argv_skill[] = {"simon", "--skill"};
  const ParseResult r_skill = parse_args_and_config(2, argv_skill);
  EXPECT(r_skill.should_exit && r_skill.exit_code == 0);
  EXPECT(r_skill.message.find("name: simon") != std::string::npos);
}

void test_process_tree_and_rollup() {
  using namespace simon;

  ProcessSnapshot snap;
  snap.total_pids = 4;
  // uv (10) -> python3 (20) -> worker1 (30), worker2 (31); separate standalone app (40)
  ProcessRecord uv;
  uv.pid = 10; uv.ppid = 1; uv.footprint = 100; uv.lifetime_max = 100; uv.name = "uv";
  ProcessRecord py;
  py.pid = 20; py.ppid = 10; py.footprint = 500; py.lifetime_max = 600; py.name = "python3";
  py.gpu_footprint = 300;
  ProcessRecord w1;
  w1.pid = 30; w1.ppid = 20; w1.footprint = 400; w1.lifetime_max = 400; w1.name = "worker";
  w1.gpu_footprint = 200;
  ProcessRecord w2;
  w2.pid = 31; w2.ppid = 20; w2.footprint = 200; w2.lifetime_max = 200; w2.name = "worker";
  ProcessRecord solo;
  solo.pid = 40; solo.ppid = 1; solo.footprint = 900; solo.lifetime_max = 900; solo.name = "solo";

  snap.processes = {solo, w2, py, w1, uv};
  snap.total_footprint = 2100;

  Config cfg;
  cfg.tree = true;
  cfg.show_gpu = true;

  const QuerySummary res = QueryEngine::run(snap, cfg);
  EXPECT(res.rows.size() == 5);
  // uv subtree = 100 + 500 + 400 + 200 = 1200 > solo (900), so uv tree ranks #1!
  EXPECT(res.rows[0].pid == 10);
  EXPECT(res.rows[0].tree_depth == 0);
  EXPECT(res.rows[0].proc_count == 4);
  EXPECT(res.rows[0].footprint == 100);
  EXPECT(res.rows[0].subtree_footprint == 1200);
  EXPECT(res.rows[0].gpu_footprint == 500);

  EXPECT(res.rows[1].pid == 20);
  EXPECT(res.rows[1].tree_depth == 1);
  EXPECT(res.rows[1].subtree_footprint == 1100);
  EXPECT(!res.rows[1].tree_prefix.empty());

  // Children of python3 sorted descending: w1 (400) before w2 (200).
  EXPECT(res.rows[2].pid == 30);
  EXPECT(res.rows[2].tree_depth == 2);
  EXPECT(res.rows[3].pid == 31);
  EXPECT(res.rows[3].tree_depth == 2);

  EXPECT(res.rows[4].pid == 40);
  EXPECT(res.rows[4].tree_depth == 0);
}

void test_grouping_and_delta_calculator() {
  using namespace simon;

  ProcessRecord chrome1;
  chrome1.pid = 101; chrome1.footprint = 500; chrome1.lifetime_max = 900;
  chrome1.gpu_footprint = 200; chrome1.name = "Google Chrome Helper";
  chrome1.path = "/Applications/Google Chrome.app/Contents/Frameworks/Helper.app/Contents/MacOS/Helper";

  ProcessRecord chrome2;
  chrome2.pid = 102; chrome2.footprint = 300; chrome2.lifetime_max = 400;
  chrome2.gpu_footprint = 150; chrome2.name = "Google Chrome";
  chrome2.path = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";

  ProcessSnapshot snap;
  snap.total_pids = 2;
  snap.processes = {chrome1, chrome2};
  snap.total_footprint = 800;

  Config cfg;
  cfg.group = true;
  cfg.show_gpu = true;
  const QuerySummary grouped = QueryEngine::run(snap, cfg);
  EXPECT(grouped.rows.size() == 1);
  EXPECT(grouped.rows[0].name == "Google Chrome");
  EXPECT(grouped.rows[0].proc_count == 2);
  EXPECT(grouped.rows[0].gpu_footprint == 350);

  // DeltaCalculator: 500ms elapsed, 250ms CPU time => 50% CPU; 10MB disk read => 20MB/s.
  SystemSnapshot s1;
  s1.timestamp = std::chrono::steady_clock::time_point(std::chrono::milliseconds(1000));
  ProcessRecord p1;
  p1.pid = 1; p1.cpu_time_ns = 1'000'000'000ULL; p1.p_cpu_time_ns = 500'000'000ULL;
  p1.disk_read_bytes = 1000; p1.disk_write_bytes = 500;
  s1.procs.processes = {p1};
  s1.disk.valid = true; s1.disk.total_read_bytes = 1000; s1.disk.total_write_bytes = 2000;

  SystemSnapshot s2;
  s2.timestamp = std::chrono::steady_clock::time_point(std::chrono::milliseconds(1500));
  ProcessRecord p2 = p1;
  p2.cpu_time_ns += 250'000'000ULL;
  p2.p_cpu_time_ns += 200'000'000ULL;
  p2.disk_read_bytes += 5'000'000ULL;
  s2.procs.processes = {p2};
  s2.disk.valid = true; s2.disk.total_read_bytes = 11'000; s2.disk.total_write_bytes = 7'000;

  DeltaCalculator::apply(s1, s2);
  EXPECT(s2.procs.processes[0].cpu_pct.has_value());
  EXPECT(std::abs(*s2.procs.processes[0].cpu_pct - 50.0) < 0.1);
  EXPECT(std::abs(*s2.procs.processes[0].p_cpu_pct - 40.0) < 0.1);
  EXPECT(std::abs(*s2.procs.processes[0].disk_read_bps - 10'000'000.0) < 1.0);
  EXPECT(std::abs(*s2.disk.read_bps - 20'000.0) < 1.0);
  EXPECT(std::abs(*s2.disk.write_bps - 10'000.0) < 1.0);
}

void test_rendering_all_formats() {
  using namespace simon;
  using json = nlohmann::json;

  SystemSnapshot snap;
  snap.procs.total_pids = 10;
  ProcessRecord p;
  p.pid = 42; p.ppid = 1;
  p.footprint = 1024ULL * 1024 * 600;
  p.lifetime_max = 1024ULL * 1024 * 900;
  p.gpu_footprint = 1024ULL * 1024 * 256;
  p.name = "ghostty";
  p.cmdline = "ghostty --title demo";
  snap.procs.processes = {p};
  snap.procs.total_footprint = p.footprint;

  snap.memory.valid = true;
  snap.memory.total = 48ULL * 1024 * 1024 * 1024;
  snap.memory.wired = 6ULL * 1024 * 1024 * 1024;
  snap.memory.anonymous = 18ULL * 1024 * 1024 * 1024;
  snap.memory.compressor = 4ULL * 1024 * 1024 * 1024;
  snap.memory.compressor_uncompressed = 12ULL * 1024 * 1024 * 1024;
  snap.memory.file_cache = 12ULL * 1024 * 1024 * 1024;
  snap.memory.swap = {8ULL * 1024 * 1024 * 1024, 2ULL * 1024 * 1024 * 1024,
                      6ULL * 1024 * 1024 * 1024, true, true};
  snap.memory.pressure = MemoryPressureLevel::Warn;
  snap.memory.memory_free_pct = 62;
  snap.memory.gpu.valid = true;
  snap.memory.gpu.allocated = 10ULL * 1024 * 1024 * 1024;
  snap.memory.gpu.in_use = 512ULL * 1024 * 1024;
  snap.memory.gpu.recommended_max_working_set = 36ULL * 1024 * 1024 * 1024;
  snap.memory.gpu.device_util_pct = 42.0;

  Config cfg;
  cfg.show_gpu = true;
  const QuerySummary summary = QueryEngine::run(snap.procs, cfg);

  // Plain rendering includes swap, pressure, compression ratio, GPU limit & headroom & util.
  const std::string plain = render_output(summary, snap, cfg, RenderContext{100, false});
  EXPECT(plain.find("3.0x") != std::string::npos);
  EXPECT(plain.find("swap: 2.0G / 8.0G used") != std::string::npos);
  EXPECT(plain.find("pressure: warn (62% free)") != std::string::npos);
  EXPECT(plain.find("limit 36.0G (26.0G headroom)") != std::string::npos);
  EXPECT(plain.find("42% util") != std::string::npos);

  // FTXUI table rendering
  cfg.color = ColorMode::Always;
  const std::string rich = render_output(summary, snap, cfg, RenderContext{100, true});
  EXPECT(rich.find("ghostty") != std::string::npos);
  EXPECT(rich.find("warn (62% free)") != std::string::npos);
  EXPECT(rich.find("26.0G headroom") != std::string::npos);
  EXPECT(rich.find("\x1b[") != std::string::npos);
  EXPECT(rich.find('\r') == std::string::npos);

  // JSON rendering
  cfg.output = OutputFormat::Json;
  cfg.tree = true;
  const QuerySummary tree_summary = QueryEngine::run(snap.procs, cfg);
  const json parsed = json::parse(render_output(tree_summary, snap, cfg, RenderContext{80, false}));
  EXPECT(parsed["mode"] == "tree");
  EXPECT(parsed["rows"][0]["subtree_footprint_bytes"] == p.footprint);
  EXPECT(parsed["system"]["swap"]["used_bytes"] == 2ULL * 1024 * 1024 * 1024);
  EXPECT(parsed["system"]["pressure"] == "warn");
  EXPECT(parsed["system"]["memory_free_pct"] == 62);
  EXPECT(parsed["system"]["gpu"]["ceiling_bytes"] == 36ULL * 1024 * 1024 * 1024);
  EXPECT(parsed["system"]["gpu"]["headroom_bytes"] == 26ULL * 1024 * 1024 * 1024);
  EXPECT(parsed["system"]["gpu"]["device_util_pct"] == 42.0);
}

void test_config_discovery() {
  using namespace simon;
  namespace fs = std::filesystem;

  const fs::path base = fs::temp_directory_path() / "simon_test_home_m1";
  fs::remove_all(base);
  fs::create_directories(base / ".config" / "simon");

  const fs::path dotfile = base / ".simon.toml";
  { std::ofstream f(dotfile); f << "count = 10\n"; }
  EXPECT(discover_config_path("", base.c_str()) == dotfile);

  const fs::path xdg = base / ".config" / "simon" / "config.toml";
  { std::ofstream f(xdg); f << "count = 15\n"; }
  EXPECT(discover_config_path("", base.c_str()) == xdg);
  fs::remove_all(base);
}

void test_ioreport_energy_and_monitor_facade() {
  using namespace simon;
  using json = nlohmann::json;

  EXPECT(std::abs(energy_delta_to_watts(500, "mJ", 0.5) - 1.0) < 0.001);
  EXPECT(std::abs(energy_delta_to_watts(2'000'000, "uJ", 1.0) - 2.0) < 0.001);
  EXPECT(std::abs(energy_delta_to_watts(1'000'000'000LL, "nJ", 0.25) - 4.0) < 0.001);
  EXPECT(energy_delta_to_watts(-5, "mJ", 1.0) == 0.0);

  // Inject fake samplers into SystemMonitor to test end-to-end orchestration & telemetry layering.
  struct FakeProc final : IProcessSampler {
    int calls = 0;
    ProcessSnapshot sample(const SampleRequest&) override {
      ++calls;
      ProcessRecord p;
      p.pid = 7; p.name = "train"; p.footprint = 1024 * 1024 * 500;
      p.cpu_time_ns = static_cast<std::uint64_t>(calls) * 100'000'000ULL;
      p.disk_read_bytes = static_cast<std::uint64_t>(calls) * 1024 * 1024;
      ProcessSnapshot s;
      s.total_pids = 1; s.total_footprint = p.footprint;
      s.processes = {p};
      return s;
    }
  };
  struct FakeMem final : IMemorySampler {
    SystemMemory sample() override {
      SystemMemory m;
      m.valid = true; m.total = 48ULL * 1024 * 1024 * 1024;
      return m;
    }
  };
  struct FakeGpu final : IGpuSampler {
    GpuInfo sample_system() override {
      GpuInfo g; g.valid = true; g.allocated = 4ULL * 1024 * 1024 * 1024;
      g.recommended_max_working_set = 36ULL * 1024 * 1024 * 1024;
      return g;
    }
    std::optional<std::uint64_t> sample_process(pid_t) override { return std::nullopt; }
  };
  struct FakeCpu final : ICpuSampler {
    CpuInfo sample() override {
      CpuInfo c; c.valid = true; c.total_util_pct = 25.0;
      c.tiers = {{"Super", 5, 40.0, std::nullopt}, {"Performance", 10, 17.5, std::nullopt}};
      return c;
    }
  };
  struct FakeDisk final : IDiskSampler {
    int calls = 0;
    DiskInfo sample() override {
      ++calls;
      DiskInfo d; d.valid = true;
      d.total_read_bytes = static_cast<std::uint64_t>(calls) * 10'000'000ULL;
      return d;
    }
  };
  struct FakeTelemetry final : ITelemetrySampler {
    void sample_into(CpuInfo& cpu, GpuInfo& gpu, TelemetryInfo& tel) override {
      cpu.power_watts = 14.5;
      gpu.power_watts = 28.0;
      tel.cpu_watts = 14.5;
      tel.gpu_watts = 28.0;
      tel.from_ioreport = true;
    }
  };

  SystemMonitor mon(std::make_unique<FakeProc>(), std::make_unique<FakeMem>(),
                    std::make_unique<FakeGpu>(), std::make_unique<FakeCpu>(),
                    std::make_unique<FakeDisk>(), std::make_unique<FakeTelemetry>());

  Config cfg;
  cfg.show_cpu = true;
  cfg.show_disk = true;
  cfg.output = OutputFormat::Json;
  const SampleRequest req = build_sample_request(cfg);
  const SystemSnapshot snap = mon.sample_with_rates(req, 10);

  EXPECT(snap.telemetry.from_ioreport);
  EXPECT(snap.cpu.power_watts == 14.5);
  EXPECT(snap.memory.gpu.power_watts == 28.0);
  EXPECT(snap.procs.processes[0].cpu_pct.has_value());
  EXPECT(snap.disk.read_bps.has_value());

  const QuerySummary summary = QueryEngine::run(snap.procs, cfg);
  const json out = json::parse(render_output(summary, snap, cfg, RenderContext{100, false}));
  EXPECT(out["system"]["cpu"]["tiers"].size() == 2);
  EXPECT(out["system"]["cpu"]["tiers"][0]["name"] == "Super");
  EXPECT(out["system"]["cpu"]["power_watts"] == 14.5);
  EXPECT(out["system"]["gpu"]["power_watts"] == 28.0);
  EXPECT(out["system"]["disk"].contains("read_bps"));
}

void test_tui_state_and_sparklines() {
  using namespace simon;

  const std::deque<double> series = {0.0, 50.0, 100.0};
  const std::string spark = render_sparkline(series, 3, 100.0);
  EXPECT(utf8_length(spark) == 3);
  EXPECT(spark.find("\xe2\x96\x88") != std::string::npos);  // max reaches full block █

  Config init_cfg;
  TuiState state(init_cfg, /*history_cap=*/3);

  SystemSnapshot snap;
  snap.memory.valid = true;
  snap.memory.total = 1000;
  snap.memory.anonymous = 600;
  snap.memory.swap = {500, 100, 400, true, true};
  snap.memory.gpu.valid = true;
  snap.memory.gpu.allocated = 300;
  snap.cpu.valid = true;
  snap.cpu.total_util_pct = 45.0;

  ProcessRecord p1;
  p1.pid = 10; p1.ppid = 1; p1.footprint = 400; p1.name = "uv"; p1.cmdline = "uv run train.py";
  ProcessRecord p2;
  p2.pid = 20; p2.ppid = 10; p2.footprint = 800; p2.name = "python3"; p2.cmdline = "python3 train.py";
  ProcessRecord p3;
  p3.pid = 30; p3.ppid = 1; p3.footprint = 200; p3.name = "node"; p3.cmdline = "node server.js";
  snap.procs.processes = {p1, p2, p3};
  snap.procs.total_pids = 3;
  snap.procs.total_footprint = 1400;

  state.apply_snapshot(snap);
  EXPECT(state.summary().rows.size() == 3);
  EXPECT(state.mem_history().size() == 1);

  // Live search filtering + graceful invalid regex handling.
  state.begin_search();
  state.set_filter("train");
  EXPECT(state.summary().rows.size() == 2);
  EXPECT(!state.filter_error().has_value());

  state.set_filter("[invalid");
  EXPECT(state.filter_error().has_value());
  EXPECT(state.summary().rows.size() == 2);  // prior valid rows retained

  state.end_search(/*clear_filter=*/true);
  EXPECT(state.filter_text().empty());
  EXPECT(state.summary().rows.size() == 3);

  // Mode toggle to tree rolls uv(10) + python3(20) together at top.
  state.toggle_tree();
  EXPECT(state.view_mode() == ViewMode::Tree);
  EXPECT(state.summary().rows[0].pid == 10);
  EXPECT(state.summary().rows[0].subtree_footprint == 1200);

  // Scrolling & pause behavior.
  state.move_selection(2, 2);
  EXPECT(state.selected_index() == 2);
  EXPECT(state.scroll_offset() == 1);

  state.toggle_pause();
  state.apply_snapshot(snap);
  EXPECT(state.mem_history().size() == 1);  // paused skips updates
}

void test_short_mode() {
  using namespace simon;
  using json = nlohmann::json;

  // CLI -S / --short, TOML short = true, and --no-short override.
  const char* argv_s[] = {"simon", "--no-config", "-S"};
  const ParseResult rs = parse_args_and_config(3, argv_s);
  EXPECT(!rs.should_exit && rs.config.short_mode);

  Config toml_cfg;
  load_toml_string("short = true\ncpu = true\n", toml_cfg);
  EXPECT(toml_cfg.short_mode && toml_cfg.show_cpu);

  // Validation: --short conflicts with --sum, --watch, and --no-system.
  const char* argv_bad1[] = {"simon", "--no-config", "-S", "--sum", "py"};
  EXPECT(parse_args_and_config(5, argv_bad1).should_exit);
  const char* argv_bad2[] = {"simon", "--no-config", "-S", "--no-system"};
  EXPECT(parse_args_and_config(4, argv_bad2).should_exit);
  const char* argv_bad3[] = {"simon", "--no-config", "-S", "-w"};
  EXPECT(parse_args_and_config(4, argv_bad3).should_exit);

  // Verify SystemMonitor skips process sampling entirely in short mode.
  int proc_calls = 0;
  struct CountingProc final : IProcessSampler {
    int* counter = nullptr;
    explicit CountingProc(int* c) : counter(c) {}
    ProcessSnapshot sample(const SampleRequest&) override {
      ++(*counter);
      return {};
    }
  };
  struct FakeMem final : IMemorySampler {
    SystemMemory sample() override {
      SystemMemory m;
      m.valid = true;
      m.total = 48ULL * 1024 * 1024 * 1024;
      m.wired = 4ULL * 1024 * 1024 * 1024;
      m.anonymous = 20ULL * 1024 * 1024 * 1024;
      m.swap = {8ULL * 1024 * 1024 * 1024, 1ULL * 1024 * 1024 * 1024,
                7ULL * 1024 * 1024 * 1024, true, true};
      m.pressure = MemoryPressureLevel::Normal;
      m.memory_free_pct = 80;
      return m;
    }
  };
  struct FakeGpu final : IGpuSampler {
    GpuInfo sample_system() override {
      GpuInfo g;
      g.valid = true;
      g.allocated = 10ULL * 1024 * 1024 * 1024;
      g.in_use = 512ULL * 1024 * 1024;
      g.recommended_max_working_set = 36ULL * 1024 * 1024 * 1024;
      return g;
    }
    std::optional<std::uint64_t> sample_process(pid_t) override { return std::nullopt; }
  };

  SystemMonitor mon(std::make_unique<CountingProc>(&proc_calls),
                    std::make_unique<FakeMem>(),
                    std::make_unique<FakeGpu>());
  Config cfg;
  cfg.short_mode = true;
  const SampleRequest req = build_sample_request(cfg);
  EXPECT(!req.need_processes);
  const SystemSnapshot snap = mon.sample(req);
  EXPECT(proc_calls == 0);

  // Plain, FTXUI, and JSON short rendering omit table/rows and keep system/swap/gpu.
  const std::string plain = render_output(QuerySummary{}, snap, cfg, RenderContext{100, false});
  EXPECT(plain.find("PID") == std::string::npos);
  EXPECT(plain.find("system:") != std::string::npos);
  EXPECT(plain.find("swap:") != std::string::npos);
  EXPECT(plain.find("gpu:") != std::string::npos);

  cfg.color = ColorMode::Always;
  const std::string rich = render_output(QuerySummary{}, snap, cfg, RenderContext{100, true});
  EXPECT(rich.find("PID") == std::string::npos);
  EXPECT(rich.find("48.0G") != std::string::npos);
  EXPECT(rich.find("26.0G headroom") != std::string::npos);

  cfg.output = OutputFormat::Json;
  const json j = json::parse(render_output(QuerySummary{}, snap, cfg, RenderContext{100, false}));
  EXPECT(j["mode"] == "short");
  EXPECT(!j.contains("rows"));
  EXPECT(!j.contains("summary"));
  EXPECT(j["system"]["gpu"]["headroom_bytes"] == 26ULL * 1024 * 1024 * 1024);
}

}  // namespace

int main() {
  test_humanize_and_utf8();
  test_graphics_ledger_and_pressure();
  test_toml_loading_and_precedence();
  test_cli_validation();
  test_process_tree_and_rollup();
  test_grouping_and_delta_calculator();
  test_rendering_all_formats();
  test_config_discovery();
  test_ioreport_energy_and_monitor_facade();
  test_tui_state_and_sparklines();
  test_short_mode();

  if (g_failures == 0) {
    std::cout << "All tests passed.\n";
    return 0;
  }
  std::cerr << g_failures << " test failure(s).\n";
  return 1;
}
