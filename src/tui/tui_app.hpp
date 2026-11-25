#pragma once

#include "config/config.hpp"
#include "platform/monitor.hpp"

namespace simon {

// Read-only interactive FTXUI live monitor (`simon -w`).
class TuiApp {
 public:
  TuiApp(SystemMonitor& monitor, const Config& initial_cfg);

  int run();

 private:
  SystemMonitor& monitor_;
  Config initial_cfg_;
};

}  // namespace simon
