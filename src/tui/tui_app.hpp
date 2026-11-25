#pragma once

#include "config/config.hpp"
#include "platform/monitor.hpp"

namespace realmem {

// Read-only interactive FTXUI live monitor (`realmem -w`).
class TuiApp {
 public:
  TuiApp(SystemMonitor& monitor, const Config& initial_cfg);

  int run();

 private:
  SystemMonitor& monitor_;
  Config initial_cfg_;
};

}  // namespace realmem
