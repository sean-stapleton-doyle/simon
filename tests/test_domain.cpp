#include "domain/delta.hpp"
#include "domain/grouping.hpp"
#include "domain/query.hpp"
#include "domain/snapshot.hpp"
#include "domain/tree.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <stdexcept>

using Catch::Approx;
using namespace simon;

TEST_CASE("Snapshot helper calculations handle zero and underflow safely", "[domain]") {
  SECTION("SystemMemory::compression_ratio") {
    SystemMemory mem;
    REQUIRE(mem.compression_ratio() == 0.0);
    mem.compressor = 4000;
    mem.compressor_uncompressed = 12000;
    REQUIRE(mem.compression_ratio() == Approx(3.0));
  }

  SECTION("GpuInfo effective ceiling and headroom clamp") {
    GpuInfo gpu;
    REQUIRE(gpu.effective_ceiling() == 0);
    REQUIRE_FALSE(gpu.headroom_bytes().has_value());

    gpu.valid = true;
    gpu.wired_limit_override = 20ULL << 30;
    gpu.allocated = 12ULL << 30;
    REQUIRE(gpu.effective_ceiling() == (20ULL << 30));
    REQUIRE(gpu.headroom_bytes() == (8ULL << 30));

    // Metal recommendedMaxWorkingSetSize takes priority over wired_limit_override.
    gpu.recommended_max_working_set = 36ULL << 30;
    REQUIRE(gpu.effective_ceiling() == (36ULL << 30));
    REQUIRE(gpu.headroom_bytes() == (24ULL << 30));

    // Allocated exceeding ceiling clamps to 0 without unsigned underflow.
    gpu.allocated = 40ULL << 30;
    REQUIRE(gpu.headroom_bytes() == 0ULL);
  }
}

TEST_CASE("PosixRegex matching, case sensitivity, move semantics, and errors", "[domain]") {
  PosixRegex icase("python|node", false);
  REQUIRE(icase.matches("/usr/bin/PYTHON3"));
  REQUIRE(icase.matches("node server.js"));
  REQUIRE_FALSE(icase.matches("rust-analyzer"));

  PosixRegex moved(std::move(icase));
  REQUIRE(moved.matches("python3"));
  REQUIRE_FALSE(icase.matches("python3"));

  PosixRegex cased("Python", true);
  REQUIRE(cased.matches("Python3"));
  REQUIRE_FALSE(cased.matches("python3"));

  REQUIRE_THROWS_AS(PosixRegex("[unclosed", false), std::runtime_error);
}

TEST_CASE("GroupAggregator classifies .app bundles, heuristics, and custom rules", "[domain]") {
  ProcessRecord nested_app;
  nested_app.pid = 1;
  nested_app.footprint = 500;
  nested_app.lifetime_max = 900;
  nested_app.gpu_footprint = 200;
  nested_app.cpu_pct = 12.5;
  nested_app.disk_read_bytes = 100;
  nested_app.name = "Google Chrome Helper (Renderer)";
  nested_app.path =
      "/Applications/Google Chrome.app/Contents/Frameworks/Google Chrome Helper.app/Contents/MacOS/"
      "Google Chrome Helper";

  ProcessRecord main_app;
  main_app.pid = 2;
  main_app.footprint = 300;
  main_app.lifetime_max = 400;
  main_app.gpu_footprint = std::nullopt;  // partial GPU readability
  main_app.cpu_pct = 7.5;
  main_app.disk_read_bytes = 250;
  main_app.name = "Google Chrome";
  main_app.path = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";

  ProcessRecord webkit;
  webkit.pid = 3;
  webkit.footprint = 200;
  webkit.name = "com.apple.WebKit.WebContent";

  ProcessRecord helper;
  helper.pid = 4;
  helper.footprint = 150;
  helper.name = "Slack Helper (GPU)";

  ProcessRecord trainer;
  trainer.pid = 5;
  trainer.footprint = 700;
  trainer.name = "python3.12";
  trainer.cmdline = "python3 train.py --epochs 10";

  const std::vector<GroupRule> rules = {{"train\\.py", "ML Trainer"}};
  REQUIRE(derive_group_name(nested_app, rules) == "Google Chrome");
  REQUIRE(derive_group_name(webkit, rules) == "WebKit");
  REQUIRE(derive_group_name(helper, rules) == "Slack");
  REQUIRE(derive_group_name(trainer, rules) == "ML Trainer");

  ProcessSnapshot snap;
  snap.total_pids = 5;
  snap.processes = {nested_app, main_app, webkit, helper, trainer};
  snap.total_footprint = 1850;

  Config cfg;
  cfg.group = true;
  cfg.show_gpu = true;
  cfg.group_rules = rules;

  const QuerySummary res = QueryEngine::run(snap, cfg);
  REQUIRE(res.total_groups == 4);
  REQUIRE(res.rows[0].name == "Google Chrome");
  REQUIRE(res.rows[0].proc_count == 2);
  REQUIRE(res.rows[0].footprint == 800);
  REQUIRE(res.rows[0].peak == 1300);
  REQUIRE(res.rows[0].gpu_footprint == 200);
  REQUIRE(res.rows[0].cpu_pct == Approx(20.0));
  REQUIRE(res.rows[0].disk_read_bytes == 350);
  REQUIRE(res.rows[1].name == "ML Trainer");
  REQUIRE(res.rows[2].name == "WebKit");
  REQUIRE(res.rows[3].name == "Slack");
}

TEST_CASE("ProcessTreeBuilder rolls up subtrees, formats connectors, and guards cycles",
          "[domain]") {
  SECTION("multi-level rollup and sibling sorting") {
    ProcessRecord uv;
    uv.pid = 10; uv.ppid = 1; uv.footprint = 100; uv.lifetime_max = 100; uv.name = "uv";
    ProcessRecord py;
    py.pid = 20; py.ppid = 10; py.footprint = 500; py.lifetime_max = 600;
    py.gpu_footprint = 300; py.cpu_pct = 20.0; py.name = "python3";
    ProcessRecord w1;
    w1.pid = 30; w1.ppid = 20; w1.footprint = 400; w1.lifetime_max = 400;
    w1.gpu_footprint = 200; w1.cpu_pct = 30.0; w1.name = "worker1";
    ProcessRecord w2;
    w2.pid = 31; w2.ppid = 20; w2.footprint = 200; w2.lifetime_max = 200;
    w2.cpu_pct = 10.0; w2.name = "worker2";
    ProcessRecord solo;
    solo.pid = 40; solo.ppid = 9999;  // orphan parent not in snapshot -> root
    solo.footprint = 900; solo.lifetime_max = 900; solo.name = "solo";

    ProcessSnapshot snap;
    snap.total_pids = 5;
    snap.processes = {solo, w2, py, w1, uv};
    snap.total_footprint = 2100;

    Config cfg;
    cfg.tree = true;
    cfg.show_gpu = true;
    const QuerySummary res = QueryEngine::run(snap, cfg);

    REQUIRE(res.rows.size() == 5);
    REQUIRE(res.rows[0].pid == 10);
    REQUIRE(res.rows[0].tree_depth == 0);
    REQUIRE(res.rows[0].proc_count == 4);
    REQUIRE(res.rows[0].footprint == 100);
    REQUIRE(res.rows[0].subtree_footprint == 1200);
    REQUIRE(res.rows[0].gpu_footprint == 500);
    REQUIRE(res.rows[0].cpu_pct == Approx(60.0));

    REQUIRE(res.rows[1].pid == 20);
    REQUIRE(res.rows[1].tree_depth == 1);
    REQUIRE(res.rows[1].tree_prefix == "\xe2\x94\x94\xe2\x94\x80 ");  // └─

    REQUIRE(res.rows[2].pid == 30);
    REQUIRE(res.rows[2].tree_depth == 2);
    REQUIRE(res.rows[2].tree_prefix == "   \xe2\x94\x9c\xe2\x94\x80 ");  // ├─

    REQUIRE(res.rows[3].pid == 31);
    REQUIRE(res.rows[3].tree_depth == 2);
    REQUIRE(res.rows[3].tree_prefix == "   \xe2\x94\x94\xe2\x94\x80 ");  // └─

    REQUIRE(res.rows[4].pid == 40);
    REQUIRE(res.rows[4].tree_depth == 0);
  }

  SECTION("self-parent does not infinite-loop") {
    ProcessRecord self_loop;
    self_loop.pid = 55;
    self_loop.ppid = 55;
    self_loop.footprint = 100;
    self_loop.name = "loop";

    const auto rows = ProcessTreeBuilder::build({&self_loop}, SortField::Footprint, false);
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].pid == 55);
    REQUIRE(rows[0].subtree_footprint == 100);
  }
}

TEST_CASE("QueryEngine filtering, count limits, and sorting across all SortFields", "[domain]") {
  ProcessRecord a;
  a.pid = 20; a.footprint = 500; a.lifetime_max = 1000; a.gpu_footprint = 100;
  a.cpu_pct = 15.0; a.disk_read_bytes = 5000; a.name = "beta"; a.cmdline = "beta --run";

  ProcessRecord b;
  b.pid = 10; b.footprint = 800; b.lifetime_max = 800; b.gpu_footprint = 300;
  b.cpu_pct = 50.0; b.disk_read_bytes = 1000; b.name = "Alpha"; b.cmdline = "alpha --serve";

  ProcessRecord c;
  c.pid = 30; c.footprint = 200; c.lifetime_max = 400; c.gpu_footprint = std::nullopt;
  c.cpu_pct = std::nullopt; c.disk_read_bytes = 9000; c.name = "gamma";

  ProcessSnapshot snap;
  snap.total_pids = 3;
  snap.processes = {a, b, c};
  snap.total_footprint = 1500;

  SECTION("sort by Peak, Pid, Name, Disk, Cpu, Gpu (nullopt always last)") {
    Config cfg;
    cfg.count = 0;

    cfg.sort = SortField::Peak;
    REQUIRE(QueryEngine::run(snap, cfg).rows[0].pid == 20);

    cfg.sort = SortField::Pid;
    REQUIRE(QueryEngine::run(snap, cfg).rows[0].pid == 10);

    cfg.sort = SortField::Name;
    REQUIRE(QueryEngine::run(snap, cfg).rows[0].name == "Alpha");
    cfg.reverse = true;
    REQUIRE(QueryEngine::run(snap, cfg).rows[0].name == "gamma");
    cfg.reverse = false;

    cfg.sort = SortField::Disk;
    REQUIRE(QueryEngine::run(snap, cfg).rows[0].pid == 30);

    cfg.sort = SortField::Gpu;
    auto gpu_asc = QueryEngine::run(snap, cfg);
    REQUIRE(gpu_asc.rows[0].pid == 10);
    REQUIRE(gpu_asc.rows[2].pid == 30);  // nullopt last
    cfg.reverse = true;
    auto gpu_rev = QueryEngine::run(snap, cfg);
    REQUIRE(gpu_rev.rows[0].pid == 20);
    REQUIRE(gpu_rev.rows[2].pid == 30);  // nullopt STILL last

    cfg.reverse = false;
    cfg.sort = SortField::Cpu;
    REQUIRE(QueryEngine::run(snap, cfg).rows[0].pid == 10);
    REQUIRE(QueryEngine::run(snap, cfg).rows[2].pid == 30);
  }

  SECTION("regex filtering and count truncation") {
    Config cfg;
    cfg.pattern = "beta|alpha";
    cfg.count = 1;
    const QuerySummary res = QueryEngine::run(snap, cfg);
    REQUIRE(res.matched_procs == 2);
    REQUIRE(res.matched_footprint == 1300);
    REQUIRE(res.largest_pid == 10);
    REQUIRE(res.rows.size() == 1);
    REQUIRE(res.rows[0].pid == 10);
  }
}

TEST_CASE("DeltaCalculator computes rates and guards non-monotonic counters", "[domain]") {
  using ms = std::chrono::milliseconds;

  SystemSnapshot s1;
  s1.timestamp = std::chrono::steady_clock::time_point(ms(1000));
  ProcessRecord p1;
  p1.pid = 1;
  p1.cpu_time_ns = 1'000'000'000ULL;
  p1.p_cpu_time_ns = 500'000'000ULL;
  p1.disk_read_bytes = 1000;
  p1.disk_write_bytes = 500;
  s1.procs.processes = {p1};
  s1.disk.valid = true;
  s1.disk.total_read_bytes = 1000;
  s1.disk.total_write_bytes = 2000;
  s1.disk.total_read_ops = 10;
  s1.disk.total_write_ops = 20;
  s1.memory.valid = true;
  s1.memory.swapins = 4096;
  s1.memory.swapouts = 8192;

  SECTION("normal positive delta") {
    SystemSnapshot s2 = s1;
    s2.timestamp = std::chrono::steady_clock::time_point(ms(1500));  // 0.5s
    s2.procs.processes[0].cpu_time_ns += 250'000'000ULL;
    s2.procs.processes[0].p_cpu_time_ns += 200'000'000ULL;
    s2.procs.processes[0].disk_read_bytes += 5'000'000ULL;
    s2.procs.processes[0].disk_write_bytes += 1'000'000ULL;
    ProcessRecord newbie;
    newbie.pid = 99;
    s2.procs.processes.push_back(newbie);

    s2.disk.total_read_bytes += 10'000;
    s2.disk.total_write_bytes += 5'000;
    s2.disk.total_read_ops += 50;
    s2.disk.total_write_ops += 25;
    s2.memory.swapins += 4096;
    s2.memory.swapouts += 16384;

    DeltaCalculator::apply(s1, s2);
    REQUIRE(s2.procs.processes[0].cpu_pct == Approx(50.0));
    REQUIRE(s2.procs.processes[0].p_cpu_pct == Approx(40.0));
    REQUIRE(s2.procs.processes[0].disk_read_bps == Approx(10'000'000.0));
    REQUIRE(s2.procs.processes[0].disk_write_bps == Approx(2'000'000.0));
    REQUIRE_FALSE(s2.procs.processes[1].cpu_pct.has_value());
    REQUIRE(s2.disk.read_bps == Approx(20'000.0));
    REQUIRE(s2.disk.write_bps == Approx(10'000.0));
    REQUIRE(s2.disk.read_iops == Approx(100.0));
    REQUIRE(s2.disk.write_iops == Approx(50.0));
    REQUIRE(s2.memory.swapin_rate_bps == Approx(8192.0));
    REQUIRE(s2.memory.swapout_rate_bps == Approx(32768.0));
  }

  SECTION("zero elapsed time or decreased counters do not produce NaN or underflow") {
    SystemSnapshot same_time = s1;
    DeltaCalculator::apply(s1, same_time);
    REQUIRE_FALSE(same_time.procs.processes[0].cpu_pct.has_value());
    REQUIRE_FALSE(same_time.disk.read_bps.has_value());

    SystemSnapshot wrapped = s1;
    wrapped.timestamp = std::chrono::steady_clock::time_point(ms(2000));
    wrapped.procs.processes[0].cpu_time_ns = 0;  // decreased
    wrapped.disk.total_read_bytes = 0;           // decreased
    DeltaCalculator::apply(s1, wrapped);
    REQUIRE_FALSE(wrapped.procs.processes[0].cpu_pct.has_value());
    REQUIRE_FALSE(wrapped.disk.read_bps.has_value());
  }
}
