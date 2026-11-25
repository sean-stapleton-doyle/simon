#include "platform/macos/process_sampler.hpp"

#include <errno.h>
#include <libproc.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <cstring>
#include <format>
#include <stdexcept>

namespace realmem {

namespace {

std::size_t query_argmax() {
  int argmax = 0;
  std::size_t sz = sizeof(argmax);
  int mib[2] = {CTL_KERN, KERN_ARGMAX};
  if (sysctl(mib, 2, &argmax, &sz, nullptr, 0) != 0 || argmax <= 0) {
    return 262144;
  }
  return static_cast<std::size_t>(argmax);
}

std::vector<pid_t> list_all_pids() {
  int nbytes = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
  if (nbytes <= 0) {
    throw std::runtime_error(std::format("proc_listpids failed: {}", std::strerror(errno)));
  }

  std::vector<pid_t> pids(static_cast<std::size_t>(nbytes) * 2 / sizeof(pid_t) + 16, 0);
  nbytes = proc_listpids(
      PROC_ALL_PIDS, 0, pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
  if (nbytes <= 0) {
    throw std::runtime_error(std::format("proc_listpids failed: {}", std::strerror(errno)));
  }
  pids.resize(static_cast<std::size_t>(nbytes) / sizeof(pid_t));
  return pids;
}

bool read_rusage(pid_t pid, rusage_info_current& out, int& flavor_out) {
  for (int flavor = RUSAGE_INFO_CURRENT; flavor >= RUSAGE_INFO_V0; --flavor) {
    if (proc_pid_rusage(pid, flavor, reinterpret_cast<rusage_info_t*>(&out)) == 0) {
      flavor_out = flavor;
      return true;
    }
    if (errno != EINVAL) {
      return false;
    }
  }
  return false;
}

pid_t query_ppid(pid_t pid) {
  struct proc_bsdshortinfo info {};
  const int ret = proc_pidinfo(pid, PROC_PIDT_SHORTBSDINFO, 0, &info, sizeof(info));
  return ret == static_cast<int>(sizeof(info)) ? static_cast<pid_t>(info.pbsi_ppid) : 0;
}

std::string query_name(pid_t pid) {
  char buf[2 * MAXCOMLEN + 1];
  const int len = proc_name(pid, buf, static_cast<std::uint32_t>(sizeof(buf)));
  return len > 0 ? std::string(buf) : std::format("<pid {}>", pid);
}

std::string query_path(pid_t pid) {
  char buf[PROC_PIDPATHINFO_MAXSIZE];
  const int len = proc_pidpath(pid, buf, sizeof(buf));
  return len > 0 ? std::string(buf, static_cast<std::size_t>(len)) : std::string{};
}

}  // namespace

std::string sanitize_cmdline(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  for (unsigned char ch : raw) {
    out.push_back((ch < 0x20 || ch == 0x7f) ? ' ' : static_cast<char>(ch));
  }
  return out;
}

bool stdout_is_tty() {
  return isatty(STDOUT_FILENO) != 0;
}

int terminal_width(int fallback) {
  struct winsize ws {};
  if (stdout_is_tty() && ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 40) {
    return ws.ws_col;
  }
  return fallback;
}

MacProcessSampler::MacProcessSampler(IGpuSampler* gpu_sampler) : gpu_sampler_(gpu_sampler) {
  mach_timebase_info(&timebase_);
  if (timebase_.denom == 0) {
    timebase_ = {1, 1};
  }
}

std::uint64_t MacProcessSampler::ticks_to_ns(std::uint64_t ticks) const {
  return static_cast<std::uint64_t>((static_cast<__uint128_t>(ticks) * timebase_.numer) /
                                    timebase_.denom);
}

std::string MacProcessSampler::cmdline_for(pid_t pid) {
  if (arg_scratch_.empty()) {
    arg_scratch_.resize(query_argmax());
  }

  int mib[3] = {CTL_KERN, KERN_PROCARGS2, pid};
  std::size_t len = arg_scratch_.size();
  if (sysctl(mib, 3, arg_scratch_.data(), &len, nullptr, 0) != 0 || len < sizeof(int)) {
    return {};
  }

  int argc = 0;
  std::memcpy(&argc, arg_scratch_.data(), sizeof(int));
  if (argc <= 0) {
    return {};
  }

  const char* p = arg_scratch_.data() + sizeof(int);
  const char* end = arg_scratch_.data() + len;
  while (p < end && *p != '\0') ++p;
  while (p < end && *p == '\0') ++p;

  std::string out;
  for (int i = 0; i < argc && p < end; ++i) {
    const std::size_t n = strnlen(p, static_cast<std::size_t>(end - p));
    if (!out.empty()) {
      out.push_back(' ');
    }
    out.append(p, n);
    if (n >= static_cast<std::size_t>(end - p)) {
      break;
    }
    p += n + 1;
  }
  return sanitize_cmdline(out);
}

std::optional<ProcessRecord> MacProcessSampler::sample_one(pid_t pid, const SampleRequest& req) {
  rusage_info_current ri{};
  int flavor = 0;
  if (!read_rusage(pid, ri, flavor)) {
    return std::nullopt;
  }

  ProcessRecord proc;
  proc.pid = pid;
  proc.footprint = ri.ri_phys_footprint;
  proc.wired = ri.ri_wired_size;
  proc.pageins = ri.ri_pageins;
  proc.cpu_time_ns = ticks_to_ns(ri.ri_user_time + ri.ri_system_time);
  if (flavor >= RUSAGE_INFO_V2) {
    proc.disk_read_bytes = ri.ri_diskio_bytesread;
    proc.disk_write_bytes = ri.ri_diskio_byteswritten;
  }
  if (flavor >= RUSAGE_INFO_V4) {
    proc.lifetime_max = ri.ri_lifetime_max_phys_footprint;
  }
  if (flavor >= RUSAGE_INFO_V6) {
    proc.p_cpu_time_ns = ticks_to_ns(ri.ri_user_ptime + ri.ri_system_ptime);
    proc.neural_footprint = ri.ri_neural_footprint;
  }

  proc.name = query_name(pid);
  if (req.need_ppid) {
    proc.ppid = query_ppid(pid);
  }
  if (req.need_path) {
    proc.path = query_path(pid);
  }
  if (req.need_cmdline) {
    proc.cmdline = cmdline_for(pid);
  }
  if (req.need_process_gpu && gpu_sampler_) {
    proc.gpu_footprint = gpu_sampler_->sample_process(pid);
  }
  return proc;
}

ProcessSnapshot MacProcessSampler::sample(const SampleRequest& req) {
  const std::vector<pid_t> pids = list_all_pids();

  ProcessSnapshot snap;
  snap.processes.reserve(pids.size());

  for (pid_t pid : pids) {
    if (pid == 0) {
      continue;
    }
    ++snap.total_pids;

    if (req.exclude_pid && pid == *req.exclude_pid) {
      continue;
    }
    if (auto proc = sample_one(pid, req)) {
      snap.total_footprint += proc->footprint;
      snap.processes.push_back(std::move(*proc));
    }
  }
  return snap;
}

}  // namespace realmem
