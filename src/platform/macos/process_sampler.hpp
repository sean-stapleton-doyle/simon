#pragma once

#include "platform/samplers.hpp"

#include <mach/mach_time.h>

#include <string>
#include <string_view>
#include <vector>

namespace simon {

// Replaces ASCII control characters (newlines, tabs, etc.) with spaces.
std::string sanitize_cmdline(std::string_view raw);

bool stdout_is_tty();
int terminal_width(int fallback = 160);

class MacProcessSampler final : public IProcessSampler {
 public:
  explicit MacProcessSampler(IGpuSampler* gpu_sampler = nullptr);

  ProcessSnapshot sample(const SampleRequest& req) override;

 private:
  std::optional<ProcessRecord> sample_one(pid_t pid, const SampleRequest& req);
  std::string cmdline_for(pid_t pid);
  std::uint64_t ticks_to_ns(std::uint64_t ticks) const;

  IGpuSampler* gpu_sampler_ = nullptr;
  mach_timebase_info_data_t timebase_{1, 1};
  std::vector<char> arg_scratch_;
};

}  // namespace simon
