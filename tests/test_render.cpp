#include "config/config.hpp"
#include "domain/query.hpp"
#include "render/format_utils.hpp"
#include "render/renderer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <string>

using json = nlohmann::json;
using namespace simon;

namespace {

SystemSnapshot make_sample_snapshot() {
  SystemSnapshot snap;
  snap.procs.total_pids = 10;

  ProcessRecord p1;
  p1.pid = 42;
  p1.ppid = 1;
  p1.footprint = 600ULL << 20;
  p1.lifetime_max = 900ULL << 20;
  p1.gpu_footprint = 256ULL << 20;
  p1.cpu_pct = 35.5;
  p1.p_cpu_pct = 20.0;
  p1.disk_read_bytes = 50ULL << 20;
  p1.disk_write_bytes = 10ULL << 20;
  p1.disk_read_bps = 12.0 * 1024.0 * 1024.0;
  p1.disk_write_bps = 2.0 * 1024.0 * 1024.0;
  p1.name = "ghostty";
  p1.path = "/Applications/Ghostty.app/Contents/MacOS/ghostty";
  p1.cmdline = "ghostty --title demo";

  ProcessRecord p2;
  p2.pid = 99;
  p2.ppid = 1;
  p2.footprint = 120ULL << 20;
  p2.lifetime_max = 150ULL << 20;
  p2.gpu_footprint = std::nullopt;
  p2.name = "WindowServer";

  snap.procs.processes = {p1, p2};
  snap.procs.total_footprint = p1.footprint + p2.footprint;

  snap.memory.valid = true;
  snap.memory.total = 48ULL << 30;
  snap.memory.wired = 6ULL << 30;
  snap.memory.anonymous = 18ULL << 30;
  snap.memory.compressor = 4ULL << 30;
  snap.memory.compressor_uncompressed = 12ULL << 30;
  snap.memory.file_cache = 12ULL << 30;
  snap.memory.free_bytes = 8ULL << 30;
  snap.memory.swap = {8ULL << 30, 2ULL << 30, 6ULL << 30, true, true};
  snap.memory.pressure = MemoryPressureLevel::Warn;
  snap.memory.memory_free_pct = 62;
  snap.memory.gpu.valid = true;
  snap.memory.gpu.model = "Apple M5 Pro";
  snap.memory.gpu.core_count = 16;
  snap.memory.gpu.allocated = 10ULL << 30;
  snap.memory.gpu.in_use = 512ULL << 20;
  snap.memory.gpu.recommended_max_working_set = 36ULL << 30;
  snap.memory.gpu.device_util_pct = 42.0;
  snap.memory.gpu.power_watts = 3.2;

  snap.cpu.valid = true;
  snap.cpu.total_util_pct = 18.5;
  snap.cpu.power_watts = 5.4;
  snap.cpu.tiers = {{"Super", 5, 30.0, std::nullopt}, {"Performance", 10, 12.7, std::nullopt}};

  snap.disk.valid = true;
  snap.disk.total_read_bytes = 500ULL << 20;
  snap.disk.total_write_bytes = 200ULL << 20;
  snap.disk.read_bps = 15.0 * 1024.0 * 1024.0;
  snap.disk.write_bps = 3.0 * 1024.0 * 1024.0;
  return snap;
}

}  // namespace

TEST_CASE("format_utils byte, rate, and UTF-8 helpers", "[render]") {
  SECTION("humanize_bytes boundaries") {
    REQUIRE(humanize_bytes(0) == "0.0B");
    REQUIRE(humanize_bytes(1023) == "1023.0B");
    REQUIRE(humanize_bytes(1024) == "1.0K");
    REQUIRE(humanize_bytes(1536 * 1024) == "1.5M");
    REQUIRE(humanize_bytes(2ULL << 30) == "2.0G");
    REQUIRE(humanize_bytes(3ULL << 40) == "3.0T");
  }

  SECTION("format_rate handles zero, negative, and positive rates") {
    REQUIRE(format_rate(0.0) == "0.0B/s");
    REQUIRE(format_rate(-10.0) == "0.0B/s");
    REQUIRE(format_rate(10.0 * 1024.0 * 1024.0) == "10.0M/s");
  }

  SECTION("truncate_utf8 and pad_right_utf8 preserve multi-byte sequences") {
    REQUIRE(truncate_utf8("hello", 0).empty());
    REQUIRE(truncate_utf8("hello", 1) == "\xe2\x80\xa6");
    REQUIRE(truncate_utf8("hello", 5) == "hello");
    REQUIRE(truncate_utf8("hello world", 5) == "hell\xe2\x80\xa6");
    // 2-byte UTF-8: café!
    REQUIRE(truncate_utf8("caf\xc3\xa9!", 5) == "caf\xc3\xa9!");
    REQUIRE(truncate_utf8("caf\xc3\xa9!extra", 5) == "caf\xc3\xa9\xe2\x80\xa6");
    // 4-byte UTF-8 emoji
    const std::string emoji = "\xf0\x9f\x9a\x80" "fast";
    REQUIRE(utf8_length(emoji) == 5);
    REQUIRE(truncate_utf8(emoji, 4) == "\xf0\x9f\x9a\x80" "fa\xe2\x80\xa6");
    REQUIRE(utf8_length(pad_right_utf8("caf\xc3\xa9", 8)) == 8);
  }

  SECTION("pressure_label covers every enum value") {
    REQUIRE(pressure_label(MemoryPressureLevel::Normal) == "normal");
    REQUIRE(pressure_label(MemoryPressureLevel::Warn) == "warn");
    REQUIRE(pressure_label(MemoryPressureLevel::Critical) == "critical");
    REQUIRE(pressure_label(MemoryPressureLevel::Unknown) == "unknown");
  }
}

TEST_CASE("PlainRenderer outputs clean aligned text with zero ANSI escapes", "[render]") {
  const SystemSnapshot snap = make_sample_snapshot();

  SECTION("full mode with GPU, CPU, Disk, and long cmdline") {
    Config cfg;
    cfg.show_gpu = true;
    cfg.show_cpu = true;
    cfg.show_disk = true;
    cfg.long_format = true;
    const QuerySummary summary = QueryEngine::run(snap.procs, cfg);
    const std::string out = render_output(summary, snap, cfg, RenderContext{120, false});

    REQUIRE(out.find("\x1b[") == std::string::npos);
    REQUIRE(out.find("PID") != std::string::npos);
    REQUIRE(out.find("GPU") != std::string::npos);
    REQUIRE(out.find("CPU") != std::string::npos);
    REQUIRE(out.find("DISK") != std::string::npos);
    REQUIRE(out.find("COMMAND LINE") != std::string::npos);
    REQUIRE(out.find("ghostty --title demo") != std::string::npos);
    REQUIRE(out.find("3.0x ratio") != std::string::npos);
    REQUIRE(out.find("limit 36.0G (26.0G headroom)") != std::string::npos);
    REQUIRE(out.find("Super (5c): 30.0%") != std::string::npos);
    REQUIRE(out.find("read 15.0M/s") != std::string::npos);
  }

  SECTION("--sum mode prints concise aggregate lines for 0, 1, and N matches") {
    Config cfg;
    cfg.sum_only = true;

    cfg.pattern = "nomatch_xyz";
    const std::string none =
        render_output(QueryEngine::run(snap.procs, cfg), snap, cfg, RenderContext{100, false});
    REQUIRE(none.find("no processes match /nomatch_xyz/") != std::string::npos);
    REQUIRE(none.find("PID") == std::string::npos);

    cfg.pattern = "ghostty";
    const std::string one =
        render_output(QueryEngine::run(snap.procs, cfg), snap, cfg, RenderContext{100, false});
    REQUIRE(one.find("1 process matching /ghostty/") != std::string::npos);
    REQUIRE(one.find("largest") == std::string::npos);

    cfg.pattern = "ghostty|WindowServer";
    const std::string many =
        render_output(QueryEngine::run(snap.procs, cfg), snap, cfg, RenderContext{100, false});
    REQUIRE(many.find("2 processes matching") != std::string::npos);
    REQUIRE(many.find("largest 600.0M, pid 42") != std::string::npos);
  }

  SECTION("unprivileged --gpu hint appears when no visible row has GPU data") {
    SystemSnapshot no_gpu = snap;
    no_gpu.procs.processes[0].gpu_footprint = std::nullopt;
    Config cfg;
    cfg.show_gpu = true;
    const std::string out =
        render_output(QueryEngine::run(no_gpu.procs, cfg), no_gpu, cfg, RenderContext{100, false});
    REQUIRE(out.find("per-process GPU requires sudo") != std::string::npos);
  }
}

TEST_CASE("TableRenderer (FTXUI) handles full, short, grouped, and narrow 80-col layouts",
          "[render]") {
  const SystemSnapshot snap = make_sample_snapshot();

  SECTION("full mode includes stacked bar, bordered summary table, and ANSI styling") {
    Config cfg;
    cfg.color = ColorMode::Always;
    cfg.show_gpu = true;
    cfg.show_cpu = true;
    cfg.show_disk = true;
    const std::string out =
        render_output(QueryEngine::run(snap.procs, cfg), snap, cfg, RenderContext{110, true});
    REQUIRE(out.find("\x1b[") != std::string::npos);
    REQUIRE(out.find('\r') == std::string::npos);
    REQUIRE(out.find("\xe2\x96\x88") != std::string::npos);  // stacked bar present
    REQUIRE(out.find("METRIC") != std::string::npos);
    REQUIRE(out.find("26.0G headroom") != std::string::npos);
  }

  SECTION("short mode omits process table and stacked bar") {
    Config cfg;
    cfg.short_mode = true;
    cfg.color = ColorMode::Always;
    const std::string out = render_output(QuerySummary{}, snap, cfg, RenderContext{100, true});
    REQUIRE(out.find("PID") == std::string::npos);
    REQUIRE(out.find("\xe2\x96\x88") == std::string::npos);
    REQUIRE(out.find("METRIC") != std::string::npos);
    REQUIRE(out.find("48.0G total") != std::string::npos);
    REQUIRE(out.find("26.0G headroom") != std::string::npos);
  }

  SECTION("narrow 80-column layout with long cmdline and all metric columns never breaks headers") {
    Config cfg;
    cfg.color = ColorMode::Always;
    cfg.long_format = true;
    cfg.show_gpu = true;
    cfg.show_cpu = true;
    cfg.show_disk = true;
    const std::string out =
        render_output(QueryEngine::run(snap.procs, cfg), snap, cfg, RenderContext{80, true});
    REQUIRE(out.find("FOOTPRINT") != std::string::npos);
    REQUIRE(out.find("GPU") != std::string::npos);
    REQUIRE(out.find("PEAK") != std::string::npos);
    REQUIRE(out.find("reclaimable") != std::string::npos);
  }
}

TEST_CASE("JsonRenderer emits valid schema across processes, grouped, tree, short, and --sum",
          "[render]") {
  const SystemSnapshot snap = make_sample_snapshot();

  SECTION("processes mode with --gpu --cpu --disk") {
    Config cfg;
    cfg.output = OutputFormat::Json;
    cfg.show_gpu = true;
    cfg.show_cpu = true;
    cfg.show_disk = true;
    const json doc = json::parse(
        render_output(QueryEngine::run(snap.procs, cfg), snap, cfg, RenderContext{80, true}));

    REQUIRE(doc["mode"] == "processes");
    REQUIRE(doc["summary"]["readable_processes"] == 2);
    REQUIRE(doc["summary"]["gpu_readable_processes"] == 1);
    REQUIRE(doc["rows"].size() == 2);
    REQUIRE(doc["rows"][0]["pid"] == 42);
    REQUIRE(doc["rows"][0]["gpu_bytes"] == (256ULL << 20));
    REQUIRE(doc["rows"][0]["cmdline"] == "ghostty --title demo");
    REQUIRE(doc["rows"][1]["gpu_bytes"].is_null());
    REQUIRE_FALSE(doc["rows"][1].contains("cmdline"));
    REQUIRE(doc["system"]["swap"]["used_bytes"] == (2ULL << 30));
    REQUIRE(doc["system"]["pressure"] == "warn");
    REQUIRE(doc["system"]["gpu"]["headroom_bytes"] == (26ULL << 30));
    REQUIRE(doc["system"]["cpu"]["tiers"].size() == 2);
    REQUIRE(doc["system"]["disk"].contains("read_bps"));
  }

  SECTION("short mode omits rows and summary") {
    Config cfg;
    cfg.output = OutputFormat::Json;
    cfg.short_mode = true;
    const json doc = json::parse(render_output(QuerySummary{}, snap, cfg, RenderContext{80, false}));
    REQUIRE(doc["mode"] == "short");
    REQUIRE_FALSE(doc.contains("rows"));
    REQUIRE_FALSE(doc.contains("summary"));
    REQUIRE(doc["system"]["total_bytes"] == (48ULL << 30));
  }

  SECTION("tree mode includes ppid, depth, and subtree_footprint_bytes") {
    Config cfg;
    cfg.output = OutputFormat::Json;
    cfg.tree = true;
    const json doc = json::parse(
        render_output(QueryEngine::run(snap.procs, cfg), snap, cfg, RenderContext{80, false}));
    REQUIRE(doc["mode"] == "tree");
    REQUIRE(doc["rows"][0]["ppid"] == 1);
    REQUIRE(doc["rows"][0]["depth"] == 0);
    REQUIRE(doc["rows"][0]["subtree_footprint_bytes"] == (600ULL << 20));
  }

  SECTION("--sum grouped mode omits rows and system") {
    Config cfg;
    cfg.output = OutputFormat::Json;
    cfg.group = true;
    cfg.pattern = "ghostty";
    cfg.sum_only = true;
    const json doc = json::parse(
        render_output(QueryEngine::run(snap.procs, cfg), snap, cfg, RenderContext{80, false}));
    REQUIRE(doc["mode"] == "grouped");
    REQUIRE(doc["summary"]["matched_processes"] == 1);
    REQUIRE_FALSE(doc.contains("rows"));
    REQUIRE_FALSE(doc.contains("system"));
  }
}
