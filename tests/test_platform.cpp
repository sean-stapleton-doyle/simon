#include "config/config.hpp"
#include "platform/macos/cpu_sampler.hpp"
#include "platform/macos/gpu_sampler.hpp"
#include "platform/macos/ioreport_sampler.hpp"
#include "platform/macos/memory_sampler.hpp"
#include "platform/macos/process_sampler.hpp"
#include "platform/monitor.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <thread>

using Catch::Approx;
using namespace simon;

TEST_CASE("macOS low-level math and parsing helpers", "[platform]") {
  SECTION("combine_graphics_ledger clamps negative ledger deltas") {
    REQUIRE(combine_graphics_ledger(100, 50) == 150);
    REQUIRE(combine_graphics_ledger(0, 0) == 0);
    REQUIRE(combine_graphics_ledger(-42, 100) == 100);
    REQUIRE(combine_graphics_ledger(100, -10) == 100);
    REQUIRE(combine_graphics_ledger(-5, -5) == 0);
  }

  SECTION("parse_pressure_level maps kernel values") {
    REQUIRE(parse_pressure_level(1) == MemoryPressureLevel::Normal);
    REQUIRE(parse_pressure_level(2) == MemoryPressureLevel::Warn);
    REQUIRE(parse_pressure_level(4) == MemoryPressureLevel::Critical);
    REQUIRE(parse_pressure_level(0) == MemoryPressureLevel::Unknown);
    REQUIRE(parse_pressure_level(99) == MemoryPressureLevel::Unknown);
  }

  SECTION("energy_delta_to_watts handles all SI units and invalid inputs") {
    REQUIRE(energy_delta_to_watts(10, "J", 2.0) == Approx(5.0));
    REQUIRE(energy_delta_to_watts(500, "mJ", 0.5) == Approx(1.0));
    REQUIRE(energy_delta_to_watts(2'000'000, "uJ", 1.0) == Approx(2.0));
    REQUIRE(energy_delta_to_watts(1'000'000'000LL, "nJ", 0.25) == Approx(4.0));
    REQUIRE(energy_delta_to_watts(-5, "mJ", 1.0) == 0.0);
    REQUIRE(energy_delta_to_watts(100, "mJ", 0.0) == 0.0);
  }

  SECTION("sanitize_cmdline replaces ASCII control chars while preserving UTF-8") {
    REQUIRE(sanitize_cmdline("python\t-c\n'print(1)'\r\n") == "python -c 'print(1)'  ");
    REQUIRE(sanitize_cmdline(std::string_view("a\x00\x1f\x7f" "b", 5)) == "a   b");
    REQUIRE(sanitize_cmdline("caf\xc3\xa9") == "caf\xc3\xa9");
  }
}

TEST_CASE("build_sample_request only enables samplers needed by Config", "[platform]") {
  SECTION("default one-shot requests only minimal process + system memory") {
    Config cfg;
    const SampleRequest req = build_sample_request(cfg, 1234);
    REQUIRE(req.need_processes);
    REQUIRE_FALSE(req.need_cmdline);
    REQUIRE_FALSE(req.need_path);
    REQUIRE_FALSE(req.need_ppid);
    REQUIRE_FALSE(req.need_process_gpu);
    REQUIRE(req.need_system_memory);
    REQUIRE_FALSE(req.need_cpu);
    REQUIRE_FALSE(req.need_disk);
    REQUIRE_FALSE(req.need_telemetry);
    REQUIRE_FALSE(req.exclude_pid.has_value());
  }

  SECTION("short mode disables process enumeration completely") {
    Config cfg;
    cfg.short_mode = true;
    cfg.group = true;
    cfg.long_format = true;
    cfg.show_gpu = true;
    const SampleRequest req = build_sample_request(cfg, 1234);
    REQUIRE_FALSE(req.need_processes);
    REQUIRE_FALSE(req.need_cmdline);
    REQUIRE_FALSE(req.need_path);
    REQUIRE_FALSE(req.need_ppid);
    REQUIRE_FALSE(req.need_process_gpu);
    REQUIRE(req.need_system_memory);
  }

  SECTION("regex excludes self PID and enables cmdline") {
    Config cfg;
    cfg.pattern = "python";
    const SampleRequest req = build_sample_request(cfg, 4321);
    REQUIRE(req.need_cmdline);
    REQUIRE(req.exclude_pid == 4321);
  }

  SECTION("tree, group, gpu, cpu, disk, and watch enable corresponding flags") {
    Config cfg;
    cfg.tree = true;
    cfg.show_gpu = true;
    cfg.show_cpu = true;
    cfg.show_disk = true;
    const SampleRequest req = build_sample_request(cfg);
    REQUIRE(req.need_ppid);
    REQUIRE(req.need_process_gpu);
    REQUIRE(req.need_cpu);
    REQUIRE(req.need_disk);
    REQUIRE(req.need_telemetry);
  }
}

TEST_CASE("SystemMonitor coordinates samplers, deltas, short mode, and optional telemetry",
          "[platform]") {
  int proc_calls = 0;

  struct FakeProc final : IProcessSampler {
    int* calls;
    explicit FakeProc(int* c) : calls(c) {}
    ProcessSnapshot sample(const SampleRequest&) override {
      ++(*calls);
      ProcessRecord p;
      p.pid = 7;
      p.name = "train";
      p.footprint = 500ULL << 20;
      p.cpu_time_ns = static_cast<std::uint64_t>(*calls) * 100'000'000ULL;
      p.disk_read_bytes = static_cast<std::uint64_t>(*calls) * (1ULL << 20);
      ProcessSnapshot s;
      s.total_pids = 1;
      s.total_footprint = p.footprint;
      s.processes = {p};
      return s;
    }
  };

  struct FakeMem final : IMemorySampler {
    SystemMemory sample() override {
      SystemMemory m;
      m.valid = true;
      m.total = 48ULL << 30;
      return m;
    }
  };

  struct FakeGpu final : IGpuSampler {
    GpuInfo sample_system() override {
      GpuInfo g;
      g.valid = true;
      g.allocated = 4ULL << 30;
      g.recommended_max_working_set = 36ULL << 30;
      return g;
    }
    std::optional<std::uint64_t> sample_process(pid_t) override { return std::nullopt; }
  };

  struct FakeCpu final : ICpuSampler {
    CpuInfo sample() override {
      CpuInfo c;
      c.valid = true;
      c.total_util_pct = 25.0;
      c.tiers = {{"Super", 5, 40.0, std::nullopt}, {"Performance", 10, 17.5, std::nullopt}};
      return c;
    }
  };

  struct FakeDisk final : IDiskSampler {
    int calls = 0;
    DiskInfo sample() override {
      ++calls;
      DiskInfo d;
      d.valid = true;
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

  SECTION("short mode never calls IProcessSampler") {
    SystemMonitor mon(std::make_unique<FakeProc>(&proc_calls),
                      std::make_unique<FakeMem>(),
                      std::make_unique<FakeGpu>());
    Config cfg;
    cfg.short_mode = true;
    const SystemSnapshot snap = mon.sample(build_sample_request(cfg));
    REQUIRE(proc_calls == 0);
    REQUIRE(snap.memory.valid);
    REQUIRE(snap.memory.gpu.valid);
  }

  SECTION("sample_with_rates populates deltas and IOReport enrichment") {
    SystemMonitor mon(std::make_unique<FakeProc>(&proc_calls),
                      std::make_unique<FakeMem>(),
                      std::make_unique<FakeGpu>(),
                      std::make_unique<FakeCpu>(),
                      std::make_unique<FakeDisk>(),
                      std::make_unique<FakeTelemetry>());
    Config cfg;
    cfg.show_cpu = true;
    cfg.show_disk = true;
    const SystemSnapshot snap = mon.sample_with_rates(build_sample_request(cfg), 10);
    REQUIRE(proc_calls == 2);
    REQUIRE(snap.telemetry.from_ioreport);
    REQUIRE(snap.cpu.power_watts == Approx(14.5));
    REQUIRE(snap.memory.gpu.power_watts == Approx(28.0));
    REQUIRE(snap.procs.processes[0].cpu_pct.has_value());
    REQUIRE(snap.disk.read_bps.has_value());
  }

  SECTION("works safely when optional samplers are null") {
    SystemMonitor mon(std::make_unique<FakeProc>(&proc_calls),
                      std::make_unique<FakeMem>(),
                      nullptr, nullptr, nullptr, nullptr);
    Config cfg;
    cfg.show_cpu = true;
    cfg.show_disk = true;
    const SystemSnapshot snap = mon.sample(build_sample_request(cfg));
    REQUIRE(snap.memory.valid);
    REQUIRE_FALSE(snap.cpu.valid);
    REQUIRE_FALSE(snap.disk.valid);
  }
}

TEST_CASE("Live macOS samplers smoke tests", "[platform][live]") {
  SECTION("MacProcessSampler finds current process with non-zero footprint") {
    MacProcessSampler sampler;
    SampleRequest req;
    req.need_ppid = true;
    req.need_cmdline = true;
    const ProcessSnapshot snap = sampler.sample(req);
    REQUIRE(snap.total_pids > 0);
    REQUIRE_FALSE(snap.processes.empty());

    const pid_t self = getpid();
    const auto it = std::find_if(snap.processes.begin(), snap.processes.end(),
                                 [self](const ProcessRecord& p) { return p.pid == self; });
    REQUIRE(it != snap.processes.end());
    REQUIRE(it->footprint > 0);
    REQUIRE(it->ppid > 0);
    REQUIRE_FALSE(it->name.empty());
  }

  SECTION("MacMemorySampler returns valid host RAM, swap, and pressure") {
    MacMemorySampler sampler;
    const SystemMemory mem = sampler.sample();
    REQUIRE(mem.valid);
    REQUIRE(mem.total > 0);
    REQUIRE(mem.wired > 0);
    REQUIRE(mem.pressure != MemoryPressureLevel::Unknown);
  }

  SECTION("MacCpuSampler discovers Apple Silicon perflevels and computes bounded util %") {
    MacCpuSampler sampler;
    const CpuInfo first = sampler.sample();
    REQUIRE(first.valid);
    REQUIRE_FALSE(first.tiers.empty());

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const CpuInfo second = sampler.sample();
    REQUIRE(second.valid);
    REQUIRE(second.total_util_pct >= 0.0);
    REQUIRE(second.total_util_pct <= 100.0);
    for (const auto& tier : second.tiers) {
      REQUIRE(tier.core_count > 0);
      REQUIRE(tier.util_pct >= 0.0);
      REQUIRE(tier.util_pct <= 100.0);
    }
  }
}
