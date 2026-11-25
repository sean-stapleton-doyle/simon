#include "gpu.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>

#include <algorithm>

namespace realmem {

namespace {

struct IoObjectGuard {
  io_object_t obj = MACH_PORT_NULL;

  explicit IoObjectGuard(io_object_t o = MACH_PORT_NULL) : obj(o) {}
  ~IoObjectGuard() {
    if (obj != MACH_PORT_NULL) {
      IOObjectRelease(obj);
    }
  }

  IoObjectGuard(const IoObjectGuard&) = delete;
  IoObjectGuard& operator=(const IoObjectGuard&) = delete;
};

struct CfTypeGuard {
  CFTypeRef ref = nullptr;

  explicit CfTypeGuard(CFTypeRef r = nullptr) : ref(r) {}
  ~CfTypeGuard() {
    if (ref != nullptr) {
      CFRelease(ref);
    }
  }

  CfTypeGuard(const CfTypeGuard&) = delete;
  CfTypeGuard& operator=(const CfTypeGuard&) = delete;
};

struct MachTaskPort {
  mach_port_t port = MACH_PORT_NULL;

  ~MachTaskPort() {
    if (port != MACH_PORT_NULL && port != mach_task_self()) {
      mach_port_deallocate(mach_task_self(), port);
    }
  }

  MachTaskPort() = default;
  MachTaskPort(const MachTaskPort&) = delete;
  MachTaskPort& operator=(const MachTaskPort&) = delete;
};

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

bool accumulate_accelerator_stats(io_registry_entry_t service, SystemGpuMemory& out) {
  CfTypeGuard stats_ref(IORegistryEntryCreateCFProperty(
      service, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0));
  if (stats_ref.ref == nullptr || CFGetTypeID(stats_ref.ref) != CFDictionaryGetTypeID()) {
    return false;
  }

  const auto dict = static_cast<CFDictionaryRef>(stats_ref.ref);
  const auto alloc = read_cf_uint64(dict, CFSTR("Alloc system memory"));
  const auto in_use = read_cf_uint64(dict, CFSTR("In use system memory"));
  if (!alloc && !in_use) {
    return false;
  }

  out.allocated += alloc.value_or(0);
  out.in_use += in_use.value_or(0);
  return true;
}

}  // namespace

std::uint64_t combine_graphics_ledger(std::int64_t footprint, std::int64_t compressed) {
  const std::int64_t base = std::max<std::int64_t>(0, footprint);
  const std::int64_t comp = std::max<std::int64_t>(0, compressed);
  return static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(comp);
}

SystemGpuMemory collect_system_gpu_memory() {
  SystemGpuMemory gpu;
  CFMutableDictionaryRef matching = IOServiceMatching("IOAccelerator");
  if (matching == nullptr) {
    return gpu;
  }

  io_iterator_t raw_iter = MACH_PORT_NULL;
  if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &raw_iter) != KERN_SUCCESS) {
    return gpu;
  }

  IoObjectGuard iter(raw_iter);
  for (io_registry_entry_t raw_svc = IOIteratorNext(iter.obj); raw_svc != MACH_PORT_NULL;
       raw_svc = IOIteratorNext(iter.obj)) {
    IoObjectGuard service(raw_svc);
    if (accumulate_accelerator_stats(service.obj, gpu)) {
      gpu.valid = true;
    }
  }
  return gpu;
}

std::optional<std::uint64_t> collect_process_gpu_footprint(pid_t pid) {
  MachTaskPort task;
  if (task_for_pid(mach_task_self(), pid, &task.port) != KERN_SUCCESS) {
    return std::nullopt;
  }

  task_vm_info_data_t vm{};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(task.port, TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &count) !=
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
