#pragma once

#include "domain/snapshot.hpp"

#include <sys/types.h>

#include <optional>

namespace realmem {

struct SampleRequest {
  bool need_cmdline = false;
  bool need_path = false;
  bool need_ppid = false;
  bool need_process_gpu = false;
  bool need_system_memory = true;
  bool need_cpu = false;
  bool need_disk = false;
  bool need_telemetry = false;
  std::optional<pid_t> exclude_pid;
};

class IProcessSampler {
 public:
  virtual ~IProcessSampler() = default;
  virtual ProcessSnapshot sample(const SampleRequest& req) = 0;
};

class IMemorySampler {
 public:
  virtual ~IMemorySampler() = default;
  virtual SystemMemory sample() = 0;
};

class IGpuSampler {
 public:
  virtual ~IGpuSampler() = default;
  virtual GpuInfo sample_system() = 0;
  virtual std::optional<std::uint64_t> sample_process(pid_t pid) = 0;
};

class ICpuSampler {
 public:
  virtual ~ICpuSampler() = default;
  virtual CpuInfo sample() = 0;
};

class IDiskSampler {
 public:
  virtual ~IDiskSampler() = default;
  virtual DiskInfo sample() = 0;
};

class ITelemetrySampler {
 public:
  virtual ~ITelemetrySampler() = default;
  // Enriches `cpu`, `gpu`, and `telemetry` with IOReport delta measurements when available.
  virtual void sample_into(CpuInfo& cpu, GpuInfo& gpu, TelemetryInfo& telemetry) = 0;
};

}  // namespace realmem
