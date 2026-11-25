#include "platform/macos/cpu_sampler.hpp"

#include "platform/macos/raii_handles.hpp"

#include <mach/mach.h>
#include <mach/processor_info.h>
#include <sys/sysctl.h>

#include <algorithm>
#include <format>
#include <numeric>

namespace simon {

namespace {

std::string query_sysctl_string(const char* name) {
  std::size_t len = 0;
  if (sysctlbyname(name, nullptr, &len, nullptr, 0) != 0 || len == 0) {
    return {};
  }
  std::string out(len, '\0');
  if (sysctlbyname(name, out.data(), &len, nullptr, 0) != 0) {
    return {};
  }
  while (!out.empty() && out.back() == '\0') {
    out.pop_back();
  }
  return out;
}

std::uint32_t query_sysctl_u32(const char* name) {
  std::uint32_t val = 0;
  std::size_t len = sizeof(val);
  return sysctlbyname(name, &val, &len, nullptr, 0) == 0 ? val : 0;
}

std::vector<PerfLevelTopology> discover_perflevels() {
  std::vector<PerfLevelTopology> levels;
  const std::uint32_t n = query_sysctl_u32("hw.nperflevels");
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::string name_key = std::format("hw.perflevel{}.name", i);
    const std::string cnt_key = std::format("hw.perflevel{}.logicalcpu", i);
    const std::uint32_t cores = query_sysctl_u32(cnt_key.c_str());
    if (cores == 0) {
      continue;
    }
    std::string name = query_sysctl_string(name_key.c_str());
    if (name.empty()) {
      name = std::format("Tier {}", i);
    }
    levels.push_back(PerfLevelTopology{std::move(name), cores});
  }
  if (levels.empty()) {
    const std::uint32_t ncpu = query_sysctl_u32("hw.logicalcpu");
    if (ncpu > 0) {
      levels.push_back(PerfLevelTopology{"CPU", ncpu});
    }
  }
  return levels;
}

}  // namespace

MacCpuSampler::MacCpuSampler()
    : brand_(query_sysctl_string("machdep.cpu.brand_string")),
      topology_(discover_perflevels()) {}

std::vector<MacCpuSampler::CoreTicks> MacCpuSampler::read_core_ticks() const {
  MachPortGuard host(mach_host_self());
  natural_t num_cpus = 0;
  processor_info_array_t raw_info = nullptr;
  mach_msg_type_number_t info_count = 0;

  const kern_return_t kr = host_processor_info(
      host.get(), PROCESSOR_CPU_LOAD_INFO, &num_cpus, &raw_info, &info_count);
  if (kr != KERN_SUCCESS || raw_info == nullptr) {
    return {};
  }

  std::vector<CoreTicks> ticks;
  ticks.reserve(num_cpus);
  const auto* cpu_load = reinterpret_cast<const processor_cpu_load_info_data_t*>(raw_info);
  for (natural_t i = 0; i < num_cpus; ++i) {
    ticks.push_back(CoreTicks{
        .user = cpu_load[i].cpu_ticks[CPU_STATE_USER],
        .system = cpu_load[i].cpu_ticks[CPU_STATE_SYSTEM],
        .idle = cpu_load[i].cpu_ticks[CPU_STATE_IDLE],
        .nice = cpu_load[i].cpu_ticks[CPU_STATE_NICE],
    });
  }

  vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(raw_info),
                static_cast<vm_size_t>(info_count) * sizeof(integer_t));
  return ticks;
}

void MacCpuSampler::compute_tier_utils(const std::vector<double>& core_utils, CpuInfo& out) const {
  // On Apple Silicon, host_processor_info orders lower-tier cores (perflevel N-1)
  // first and highest-tier cores (perflevel 0) last.
  std::size_t total_expected = 0;
  for (const auto& t : topology_) {
    total_expected += t.core_count;
  }

  out.tiers.clear();
  out.tiers.reserve(topology_.size());

  if (total_expected != core_utils.size() || topology_.empty()) {
    out.tiers.push_back(CpuTierInfo{"CPU", static_cast<std::uint32_t>(core_utils.size()),
                                    out.total_util_pct, std::nullopt});
    return;
  }

  std::size_t cursor = core_utils.size();
  for (const auto& level : topology_) {
    const std::size_t count = level.core_count;
    const std::size_t start = cursor >= count ? cursor - count : 0;
    double sum = 0.0;
    for (std::size_t i = start; i < cursor; ++i) {
      sum += core_utils[i];
    }
    const double avg = count > 0 ? sum / static_cast<double>(count) : 0.0;
    out.tiers.push_back(CpuTierInfo{level.name, level.core_count, avg, std::nullopt});
    cursor = start;
  }
}

CpuInfo MacCpuSampler::sample() {
  CpuInfo info;
  info.brand = brand_;

  const std::vector<CoreTicks> curr = read_core_ticks();
  if (curr.empty()) {
    return info;
  }

  if (prev_ticks_.size() != curr.size()) {
    prev_ticks_ = curr;
    // Populate topology with 0% on the very first baseline tick.
    for (const auto& level : topology_) {
      info.tiers.push_back(CpuTierInfo{level.name, level.core_count, 0.0, std::nullopt});
    }
    info.valid = true;
    return info;
  }

  std::vector<double> core_utils;
  core_utils.reserve(curr.size());
  double total_sum = 0.0;

  for (std::size_t i = 0; i < curr.size(); ++i) {
    const std::uint64_t du = curr[i].user - prev_ticks_[i].user;
    const std::uint64_t ds = curr[i].system - prev_ticks_[i].system;
    const std::uint64_t dn = curr[i].nice - prev_ticks_[i].nice;
    const std::uint64_t di = curr[i].idle - prev_ticks_[i].idle;
    const std::uint64_t active = du + ds + dn;
    const std::uint64_t total = active + di;
    const double pct =
        total > 0 ? std::clamp((static_cast<double>(active) * 100.0) / static_cast<double>(total),
                               0.0, 100.0)
                  : 0.0;
    core_utils.push_back(pct);
    total_sum += pct;
  }

  prev_ticks_ = curr;
  info.total_util_pct = total_sum / static_cast<double>(core_utils.size());
  compute_tier_utils(core_utils, info);
  info.valid = true;
  return info;
}

}  // namespace simon
