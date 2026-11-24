#include "config.hpp"
#include "model.hpp"
#include "process.hpp"
#include "render.hpp"

#include <unistd.h>

#include <exception>
#include <iostream>
#include <optional>

int main(int argc, char** argv) {
  using namespace realmem;

  const ParseResult parsed = parse_args_and_config(argc, argv);
  if (parsed.should_exit) {
    if (!parsed.message.empty()) {
      (parsed.exit_code == 0 ? std::cout : std::cerr) << parsed.message;
    }
    return parsed.exit_code;
  }

  const Config& cfg = parsed.config;

  try {
    const bool need_cmdline =
        cfg.pattern.has_value() || (cfg.long_format && !cfg.group) ||
        (cfg.group && !cfg.group_rules.empty());
    const bool need_path = cfg.group;
    const std::optional<pid_t> exclude_self =
        cfg.pattern.has_value() ? std::optional<pid_t>(getpid()) : std::nullopt;

    const Snapshot snap = collect_processes(need_cmdline, need_path, exclude_self);
    const SystemMemory sys =
        (!cfg.sum_only && cfg.show_system) ? collect_system_memory() : SystemMemory{};
    const QuerySummary summary = build_query_summary(snap, cfg);

    const RenderContext ctx{
        .width = terminal_width(160),
        .is_tty = stdout_is_tty(),
    };

    std::cout << render_output(summary, sys, cfg, ctx);
    return 0;
  } catch (const std::exception& err) {
    std::cerr << "realmem: " << err.what() << '\n';
    return 1;
  }
}
