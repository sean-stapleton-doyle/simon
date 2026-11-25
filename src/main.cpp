#include "config/config.hpp"
#include "domain/query.hpp"
#include "platform/macos/process_sampler.hpp"
#include "platform/monitor.hpp"
#include "render/renderer.hpp"
#include "tui/tui_app.hpp"

#include <unistd.h>

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
  using namespace simon;

  const ParseResult parsed = parse_args_and_config(argc, argv);
  if (parsed.should_exit) {
    if (!parsed.message.empty()) {
      (parsed.exit_code == 0 ? std::cout : std::cerr) << parsed.message;
    }
    return parsed.exit_code;
  }

  const Config& cfg = parsed.config;

  try {
    SystemMonitor monitor = create_macos_monitor();

    if (cfg.watch) {
      return TuiApp(monitor, cfg).run();
    }

    const SampleRequest req = build_sample_request(cfg, getpid());
    const bool need_rates = req.need_cpu || req.need_disk;
    const SystemSnapshot snap =
        need_rates ? monitor.sample_with_rates(req, cfg.sample_ms) : monitor.sample(req);
    const QuerySummary summary = QueryEngine::run(snap.procs, cfg);

    const RenderContext ctx{
        .width = terminal_width(160),
        .is_tty = stdout_is_tty(),
    };

    std::cout << render_output(summary, snap, cfg, ctx);
    return 0;
  } catch (const std::exception& err) {
    std::cerr << "simon: " << err.what() << '\n';
    return 1;
  }
}
