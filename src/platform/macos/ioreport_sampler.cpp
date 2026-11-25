#include "platform/macos/ioreport_sampler.hpp"

#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <string>
#include <unordered_map>

extern "C" {

using IOReportSubscriptionRef = struct __IOReportSubscription*;

CFDictionaryRef IOReportCopyChannelsInGroup(CFStringRef group,
                                            CFStringRef subgroup,
                                            uint64_t a,
                                            uint64_t b,
                                            uint64_t c);
void IOReportMergeChannels(CFDictionaryRef a, CFDictionaryRef b, CFTypeRef unused);
IOReportSubscriptionRef IOReportCreateSubscription(void* a,
                                                   CFMutableDictionaryRef desired_channels,
                                                   CFMutableDictionaryRef* subbed_channels,
                                                   uint64_t channel_id,
                                                   CFTypeRef b);
CFDictionaryRef IOReportCreateSamples(IOReportSubscriptionRef sub,
                                      CFMutableDictionaryRef subbed_channels,
                                      CFTypeRef a);
CFDictionaryRef IOReportCreateSamplesDelta(CFDictionaryRef prev,
                                           CFDictionaryRef curr,
                                           CFTypeRef a);
CFStringRef IOReportChannelGetGroup(CFDictionaryRef ch);
CFStringRef IOReportChannelGetSubGroup(CFDictionaryRef ch);
CFStringRef IOReportChannelGetChannelName(CFDictionaryRef ch);
CFStringRef IOReportChannelGetUnitLabel(CFDictionaryRef ch);
int64_t IOReportSimpleGetIntegerValue(CFDictionaryRef ch, int32_t idx);
int32_t IOReportStateGetCount(CFDictionaryRef ch);
CFStringRef IOReportStateGetNameForIndex(CFDictionaryRef ch, int32_t idx);
int64_t IOReportStateGetResidency(CFDictionaryRef ch, int32_t idx);
void IOReportIterate(CFDictionaryRef samples, int (^block)(CFDictionaryRef channel));

}  // extern "C"

namespace simon {

namespace {

std::string cf_to_string(CFStringRef ref) {
  if (!ref) {
    return {};
  }
  char buf[256];
  if (CFStringGetCString(ref, buf, sizeof(buf), kCFStringEncodingUTF8)) {
    return std::string(buf);
  }
  return {};
}

bool is_idle_state(std::string_view state) {
  return state == "IDLE" || state == "OFF" || state == "DOWN" || state.starts_with("IDLE");
}

double compute_active_residency_pct(CFDictionaryRef ch) {
  const int32_t count = IOReportStateGetCount(ch);
  std::int64_t total = 0;
  std::int64_t active = 0;
  for (int32_t i = 0; i < count; ++i) {
    const std::int64_t res = IOReportStateGetResidency(ch, i);
    if (res <= 0) {
      continue;
    }
    total += res;
    const std::string name = cf_to_string(IOReportStateGetNameForIndex(ch, i));
    if (!is_idle_state(name)) {
      active += res;
    }
  }
  return total > 0 ? std::clamp((static_cast<double>(active) * 100.0) / static_cast<double>(total),
                                0.0, 100.0)
                   : 0.0;
}

void merge_group(CFMutableDictionaryRef dst, CFStringRef group, CFStringRef subgroup) {
  CfTypeGuard ch(IOReportCopyChannelsInGroup(group, subgroup, 0, 0, 0));
  if (ch) {
    IOReportMergeChannels(dst, static_cast<CFDictionaryRef>(ch.get()), nullptr);
  }
}

struct ClusterAccumulator {
  double sum_pct = 0.0;
  int count = 0;
};

void apply_cluster_residency(const std::unordered_map<std::string, ClusterAccumulator>& clusters,
                             CpuInfo& cpu) {
  if (clusters.empty() || cpu.tiers.empty()) {
    return;
  }

  // Match hardware cluster prefix (e.g. "SCPU"/"PCPU"/"ECPU") onto dynamic sysctl tiers.
  auto match_tier = [&](const std::string& tier_name) -> std::optional<double> {
    const char prefix = tier_name.empty() ? '\0' : static_cast<char>(std::toupper(tier_name[0]));
    for (const auto& [key, acc] : clusters) {
      if (acc.count > 0 && !key.empty() && std::toupper(key[0]) == prefix) {
        return acc.sum_pct / static_cast<double>(acc.count);
      }
    }
    return std::nullopt;
  };

  double weighted_sum = 0.0;
  std::uint32_t weighted_cores = 0;
  for (auto& tier : cpu.tiers) {
    if (auto pct = match_tier(tier.name)) {
      tier.util_pct = *pct;
    }
    weighted_sum += tier.util_pct * static_cast<double>(tier.core_count);
    weighted_cores += tier.core_count;
  }
  if (weighted_cores > 0) {
    cpu.total_util_pct = weighted_sum / static_cast<double>(weighted_cores);
    cpu.valid = true;
  }
}

}  // namespace

double energy_delta_to_watts(std::int64_t energy, std::string_view unit, double dt_sec) {
  if (energy <= 0 || dt_sec <= 0.0) {
    return 0.0;
  }
  double joules = static_cast<double>(energy);
  if (unit == "mJ") {
    joules /= 1e3;
  } else if (unit == "uJ") {
    joules /= 1e6;
  } else if (unit == "nJ") {
    joules /= 1e9;
  }
  return joules / dt_sec;
}

IoReportSampler::IoReportSampler() {
  CfTypeGuard base(IOReportCopyChannelsInGroup(CFSTR("Energy Model"), nullptr, 0, 0, 0));
  if (!base) {
    return;
  }

  CfTypeGuard merged(
      CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, static_cast<CFDictionaryRef>(base.get())));
  if (!merged) {
    return;
  }

  auto* mut_dict = const_cast<CFMutableDictionaryRef>(static_cast<CFDictionaryRef>(merged.get()));
  merge_group(mut_dict, CFSTR("CPU Stats"), CFSTR("CPU Core Performance States"));
  merge_group(mut_dict, CFSTR("GPU Stats"), CFSTR("GPU Performance States"));

  CFMutableDictionaryRef subbed = nullptr;
  IOReportSubscriptionRef sub = IOReportCreateSubscription(nullptr, mut_dict, &subbed, 0, nullptr);
  if (sub == nullptr || subbed == nullptr) {
    if (subbed != nullptr) {
      CFRelease(subbed);
    }
    if (sub != nullptr) {
      CFRelease(sub);
    }
    return;
  }

  subscription_ = sub;
  subscribed_channels_.reset(subbed);
}

IoReportSampler::~IoReportSampler() {
  if (subscription_ != nullptr) {
    CFRelease(subscription_);
    subscription_ = nullptr;
  }
}

void IoReportSampler::sample_into(CpuInfo& cpu, GpuInfo& gpu, TelemetryInfo& telemetry) {
  if (subscription_ == nullptr || !subscribed_channels_) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  auto* sub = static_cast<IOReportSubscriptionRef>(subscription_);
  auto* channels =
      const_cast<CFMutableDictionaryRef>(static_cast<CFDictionaryRef>(subscribed_channels_.get()));

  CfTypeGuard curr(IOReportCreateSamples(sub, channels, nullptr));
  if (!curr) {
    return;
  }

  if (!prev_sample_) {
    prev_sample_ = std::move(curr);
    prev_time_ = now;
    return;
  }

  const double dt_sec =
      static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(now - prev_time_).count()) /
      1e9;
  CfTypeGuard delta(IOReportCreateSamplesDelta(static_cast<CFDictionaryRef>(prev_sample_.get()),
                                               static_cast<CFDictionaryRef>(curr.get()), nullptr));
  prev_sample_ = std::move(curr);
  prev_time_ = now;

  if (!delta || dt_sec <= 0.0) {
    return;
  }

  std::unordered_map<std::string, ClusterAccumulator> cpu_clusters;
  auto* clusters_ptr = &cpu_clusters;
  auto* cpu_ptr = &cpu;
  auto* gpu_ptr = &gpu;
  auto* tel_ptr = &telemetry;

  IOReportIterate(static_cast<CFDictionaryRef>(delta.get()), ^int(CFDictionaryRef ch) {
    const std::string group = cf_to_string(IOReportChannelGetGroup(ch));
    const std::string name = cf_to_string(IOReportChannelGetChannelName(ch));

    if (group == "Energy Model") {
      const std::int64_t val = IOReportSimpleGetIntegerValue(ch, 0);
      const std::string unit = cf_to_string(IOReportChannelGetUnitLabel(ch));
      const double watts = energy_delta_to_watts(val, unit, dt_sec);
      if (name == "CPU Energy") {
        tel_ptr->cpu_watts = watts;
        cpu_ptr->power_watts = watts;
      } else if (name == "GPU Energy") {
        tel_ptr->gpu_watts = watts;
        gpu_ptr->power_watts = watts;
      } else if (name.starts_with("ANE")) {
        tel_ptr->ane_watts = tel_ptr->ane_watts.value_or(0.0) + watts;
      } else if (name.starts_with("DRAM")) {
        tel_ptr->dram_watts = tel_ptr->dram_watts.value_or(0.0) + watts;
      }
      tel_ptr->from_ioreport = true;
    } else if (group == "GPU Stats" && name == "GPUPH") {
      gpu_ptr->device_util_pct = compute_active_residency_pct(ch);
      tel_ptr->from_ioreport = true;
    } else if (group == "CPU Stats" && name.size() >= 4 &&
               (name.starts_with("ECPU") || name.starts_with("PCPU") || name.starts_with("SCPU"))) {
      auto& acc = (*clusters_ptr)[name.substr(0, 4)];
      acc.sum_pct += compute_active_residency_pct(ch);
      ++acc.count;
      tel_ptr->from_ioreport = true;
    }
    return 0;
  });

  apply_cluster_residency(cpu_clusters, cpu);
}

}  // namespace simon
