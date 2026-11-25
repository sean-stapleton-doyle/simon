#include "platform/macos/memory_sampler.hpp"

#include "platform/macos/raii_handles.hpp"

#include <mach/mach.h>
#include <sys/sysctl.h>

namespace realmem {

namespace {

SwapInfo query_swap_usage() {
  SwapInfo swap;
  struct xsw_usage raw {};
  std::size_t len = sizeof(raw);
  if (sysctlbyname("vm.swapusage", &raw, &len, nullptr, 0) != 0) {
    return swap;
  }
  swap.total = raw.xsu_total;
  swap.used = raw.xsu_used;
  swap.free_bytes = raw.xsu_avail;
  swap.encrypted = raw.xsu_encrypted != 0;
  swap.valid = true;
  return swap;
}

std::optional<int> query_sysctl_int(const char* name) {
  int val = 0;
  std::size_t len = sizeof(val);
  if (sysctlbyname(name, &val, &len, nullptr, 0) != 0) {
    return std::nullopt;
  }
  return val;
}

std::uint64_t query_memsize() {
  std::uint64_t memsize = 0;
  std::size_t len = sizeof(memsize);
  sysctlbyname("hw.memsize", &memsize, &len, nullptr, 0);
  return memsize;
}

}  // namespace

MemoryPressureLevel parse_pressure_level(int raw) {
  switch (raw) {
    case 1:
      return MemoryPressureLevel::Normal;
    case 2:
      return MemoryPressureLevel::Warn;
    case 4:
      return MemoryPressureLevel::Critical;
    default:
      return MemoryPressureLevel::Unknown;
  }
}

SystemMemory MacMemorySampler::sample() {
  SystemMemory sys;
  MachPortGuard host(mach_host_self());
  vm_size_t page = 0;
  vm_statistics64_data_t vm{};
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;

  if (host_page_size(host.get(), &page) != KERN_SUCCESS) {
    return sys;
  }
  if (host_statistics64(
          host.get(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) !=
      KERN_SUCCESS) {
    return sys;
  }

  const auto to_bytes = [page](std::uint64_t pages) { return pages * static_cast<std::uint64_t>(page); };

  sys.total = query_memsize();
  sys.wired = to_bytes(vm.wire_count);
  sys.anonymous = to_bytes(vm.internal_page_count);
  sys.compressor = to_bytes(vm.compressor_page_count);
  sys.compressor_uncompressed = to_bytes(vm.total_uncompressed_pages_in_compressor);
  sys.file_cache = to_bytes(vm.external_page_count);
  sys.purgeable = to_bytes(vm.purgeable_count);
  sys.free_bytes = to_bytes(vm.free_count);
  sys.swapped_pages_bytes = to_bytes(vm.swapped_count);
  sys.swapins = to_bytes(vm.swapins);
  sys.swapouts = to_bytes(vm.swapouts);
  sys.compressions = vm.compressions;
  sys.decompressions = vm.decompressions;
  sys.pageins = vm.pageins;
  sys.pageouts = vm.pageouts;
  sys.swap = query_swap_usage();
  if (auto level = query_sysctl_int("kern.memorystatus_vm_pressure_level")) {
    sys.pressure = parse_pressure_level(*level);
  }
  sys.memory_free_pct = query_sysctl_int("kern.memorystatus_level");
  sys.valid = true;
  return sys;
}

}  // namespace realmem
