#pragma once

#include <cstdint>

namespace simon {

struct MetalDeviceInfo {
  std::uint64_t recommended_max_working_set = 0;
  bool has_unified_memory = false;
  bool valid = false;
};

// Queries the default Metal device once for Apple Silicon GPU memory ceiling.
MetalDeviceInfo query_metal_device_info();

}  // namespace simon
