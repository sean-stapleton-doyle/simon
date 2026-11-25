#include "platform/macos/gpu_sampler.hpp"

#include "platform/macos/raii_handles.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <sys/sysctl.h>

#include <algorithm>

namespace realmem {

namespace {

std::optional<std::uint64_t> read_cf_uint64(CFDictionaryRef dict, CFStringRef key) {
  const void* raw = CFDictionaryGetValue(dict, key);
  if (raw == nullptr || CFGetTypeID(raw) != CFNumberGetTypeID()) {
    return std::nullopt;
  }
  std::int64_t val = 0;
  if (!CFNumberGetValue(static_cast<CFNumberRef>(raw), kCFNumberSInt64Type, &val)) {
    return std::nullopt;
  }
  return val > 0 ? static_cast<std::uint64_t>(val) : 0ULL;
}

std::optional<double> read_cf_double(CFDictionaryRef dict, CFStringRef key) {
  const void* raw = CFDictionaryGetValue(dict, key);
  if (raw == nullptr || CFGetTypeID(raw) != CFNumberGetTypeID()) {
    return std::nullopt;
  }
  double val = 0.0;
  if (!CFNumberGetValue(static_cast<CFNumberRef>(raw), kCFNumberDoubleType, &val)) {
    return std::nullopt;
  }
  return val;
}

std::string read_cf_string_prop(io_registry_entry_t service, CFStringRef key) {
  CfTypeGuard ref(IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0));
  if (!ref || CFGetTypeID(ref.get()) != CFStringGetTypeID()) {
    return {};
  }
  char buf[256];
  if (CFStringGetCString(static_cast<CFStringRef>(ref.get()), buf, sizeof(buf),
                         kCFStringEncodingUTF8)) {
    return std::string(buf);
  }
  return {};
}

std::uint32_t read_cf_uint32_prop(io_registry_entry_t service, CFStringRef key) {
  CfTypeGuard ref(IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0));
  if (!ref || CFGetTypeID(ref.get()) != CFNumberGetTypeID()) {
    return 0;
  }
  std::int32_t val = 0;
  if (CFNumberGetValue(static_cast<CFNumberRef>(ref.get()), kCFNumberSInt32Type, &val) && val > 0) {
    return static_cast<std::uint32_t>(val);
  }
  return 0;
}

std::uint64_t query_wired_limit_override() {
  std::uint64_t limit_mb = 0;
  std::size_t len = sizeof(limit_mb);
  if (sysctlbyname("iogpu.wired_limit_mb", &limit_mb, &len, nullptr, 0) != 0 || limit_mb == 0) {
    return 0;
  }
  return limit_mb * 1024ULL * 1024ULL;
}

bool accumulate_accelerator_stats(io_registry_entry_t service, GpuInfo& out) {
  CfTypeGuard stats_ref(IORegistryEntryCreateCFProperty(
      service, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0));
  if (!stats_ref || CFGetTypeID(stats_ref.get()) != CFDictionaryGetTypeID()) {
    return false;
  }

  const auto dict = static_cast<CFDictionaryRef>(stats_ref.get());
  const auto alloc = read_cf_uint64(dict, CFSTR("Alloc system memory"));
  const auto in_use = read_cf_uint64(dict, CFSTR("In use system memory"));
  const auto dev_util = read_cf_double(dict, CFSTR("Device Utilization %"));
  const auto ren_util = read_cf_double(dict, CFSTR("Renderer Utilization %"));
  const auto til_util = read_cf_double(dict, CFSTR("Tiler Utilization %"));
  const auto recov = read_cf_uint64(dict, CFSTR("recoveryCount"));

  if (!alloc && !in_use && !dev_util) {
    return false;
  }

  out.allocated += alloc.value_or(0);
  out.in_use += in_use.value_or(0);
  if (dev_util) out.device_util_pct = *dev_util;
  if (ren_util) out.renderer_util_pct = *ren_util;
  if (til_util) out.tiler_util_pct = *til_util;
  out.recovery_count += recov.value_or(0);
  return true;
}

}  // namespace

std::uint64_t combine_graphics_ledger(std::int64_t footprint, std::int64_t compressed) {
  const std::int64_t base = std::max<std::int64_t>(0, footprint);
  const std::int64_t comp = std::max<std::int64_t>(0, compressed);
  return static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(comp);
}

MacGpuSampler::MacGpuSampler() : metal_(query_metal_device_info()) {
  CFMutableDictionaryRef matching = IOServiceMatching("IOAccelerator");
  if (!matching) {
    return;
  }
  io_iterator_t raw_iter = MACH_PORT_NULL;
  if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &raw_iter) != KERN_SUCCESS) {
    return;
  }
  IoObjectGuard iter(raw_iter);
  for (io_registry_entry_t raw_svc = IOIteratorNext(iter.get()); raw_svc != MACH_PORT_NULL;
       raw_svc = IOIteratorNext(iter.get())) {
    IoObjectGuard service(raw_svc);
    if (cached_model_.empty()) {
      cached_model_ = read_cf_string_prop(service.get(), CFSTR("model"));
    }
    if (cached_cores_ == 0) {
      cached_cores_ = read_cf_uint32_prop(service.get(), CFSTR("gpu-core-count"));
    }
  }
}

GpuInfo MacGpuSampler::sample_system() {
  GpuInfo gpu;
  gpu.model = cached_model_;
  gpu.core_count = cached_cores_;
  gpu.recommended_max_working_set = metal_.recommended_max_working_set;
  gpu.wired_limit_override = query_wired_limit_override();

  CFMutableDictionaryRef matching = IOServiceMatching("IOAccelerator");
  if (!matching) {
    return gpu;
  }
  io_iterator_t raw_iter = MACH_PORT_NULL;
  if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &raw_iter) != KERN_SUCCESS) {
    return gpu;
  }

  IoObjectGuard iter(raw_iter);
  for (io_registry_entry_t raw_svc = IOIteratorNext(iter.get()); raw_svc != MACH_PORT_NULL;
       raw_svc = IOIteratorNext(iter.get())) {
    IoObjectGuard service(raw_svc);
    if (accumulate_accelerator_stats(service.get(), gpu)) {
      gpu.valid = true;
    }
  }
  return gpu;
}

std::optional<std::uint64_t> MacGpuSampler::sample_process(pid_t pid) {
  MachPortGuard task;
  if (task_for_pid(mach_task_self(), pid, task.out()) != KERN_SUCCESS) {
    return std::nullopt;
  }

  task_vm_info_data_t vm{};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(task.get(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &count) !=
      KERN_SUCCESS) {
    return std::nullopt;
  }
  if (count < TASK_VM_INFO_REV3_COUNT) {
    return std::nullopt;
  }

  return combine_graphics_ledger(vm.ledger_tag_graphics_footprint,
                                 vm.ledger_tag_graphics_footprint_compressed);
}

}  // namespace realmem
