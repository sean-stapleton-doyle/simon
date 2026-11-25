#include "platform/macos/disk_sampler.hpp"

#include "platform/macos/raii_handles.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

#include <cstdint>
#include <optional>

namespace realmem {

namespace {

std::uint64_t read_stat_uint64(CFDictionaryRef dict, CFStringRef key) {
  const void* raw = CFDictionaryGetValue(dict, key);
  if (raw == nullptr || CFGetTypeID(raw) != CFNumberGetTypeID()) {
    return 0;
  }
  std::int64_t val = 0;
  if (!CFNumberGetValue(static_cast<CFNumberRef>(raw), kCFNumberSInt64Type, &val) || val < 0) {
    return 0;
  }
  return static_cast<std::uint64_t>(val);
}

bool accumulate_driver_stats(io_registry_entry_t service, DiskInfo& out) {
  CfTypeGuard stats_ref(IORegistryEntryCreateCFProperty(
      service, CFSTR("Statistics"), kCFAllocatorDefault, 0));
  if (!stats_ref || CFGetTypeID(stats_ref.get()) != CFDictionaryGetTypeID()) {
    return false;
  }

  const auto dict = static_cast<CFDictionaryRef>(stats_ref.get());
  const std::uint64_t rb = read_stat_uint64(dict, CFSTR("Bytes (Read)"));
  const std::uint64_t wb = read_stat_uint64(dict, CFSTR("Bytes (Write)"));
  const std::uint64_t ro = read_stat_uint64(dict, CFSTR("Operations (Read)"));
  const std::uint64_t wo = read_stat_uint64(dict, CFSTR("Operations (Write)"));

  out.total_read_bytes += rb;
  out.total_write_bytes += wb;
  out.total_read_ops += ro;
  out.total_write_ops += wo;
  return (rb > 0 || wb > 0 || ro > 0 || wo > 0);
}

}  // namespace

DiskInfo MacDiskSampler::sample() {
  DiskInfo disk;
  CFMutableDictionaryRef matching = IOServiceMatching("IOBlockStorageDriver");
  if (!matching) {
    return disk;
  }

  io_iterator_t raw_iter = MACH_PORT_NULL;
  if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &raw_iter) != KERN_SUCCESS) {
    return disk;
  }

  IoObjectGuard iter(raw_iter);
  for (io_registry_entry_t raw_svc = IOIteratorNext(iter.get()); raw_svc != MACH_PORT_NULL;
       raw_svc = IOIteratorNext(iter.get())) {
    IoObjectGuard service(raw_svc);
    if (accumulate_driver_stats(service.get(), disk)) {
      disk.valid = true;
    }
  }
  return disk;
}

}  // namespace realmem
