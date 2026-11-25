#pragma once

#include "platform/macos/raii_handles.hpp"
#include "platform/samplers.hpp"

#include <chrono>
#include <cstdint>
#include <string_view>

namespace realmem {

// Converts an IOReport energy delta into Watts given its unit label ("mJ", "uJ", "nJ", "J").
double energy_delta_to_watts(std::int64_t energy, std::string_view unit, double dt_sec);

// Optional private libIOReport telemetry layer (no sudo required).
// Degrades gracefully to a no-op if channels are unavailable.
class IoReportSampler final : public ITelemetrySampler {
 public:
  IoReportSampler();
  ~IoReportSampler() override;

  IoReportSampler(const IoReportSampler&) = delete;
  IoReportSampler& operator=(const IoReportSampler&) = delete;

  bool available() const { return subscription_ != nullptr; }

  void sample_into(CpuInfo& cpu, GpuInfo& gpu, TelemetryInfo& telemetry) override;

 private:
  void* subscription_ = nullptr;  // IOReportSubscriptionRef
  CfTypeGuard subscribed_channels_;
  CfTypeGuard prev_sample_;
  std::chrono::steady_clock::time_point prev_time_{};
};

}  // namespace realmem
