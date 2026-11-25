#include "process.hpp"

#include <errno.h>
#include <libproc.h>
#include <mach/mach.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <cstring>
#include <format>
#include <stdexcept>
#include <vector>

namespace realmem {

namespace {

struct MachHostPort {
  mach_port_t port = MACH_PORT_NULL;

  MachHostPort() : port(mach_host_self()) {}
  ~MachHostPort() {
    if (port != MACH_PORT_NULL) {
      mach_port_deallocate(mach_task_self(), port);
    }
  }

  MachHostPort(const MachHostPort&) = delete;
  MachHostPort& operator=(const MachHostPort&) = delete;
};

// phys_footprint lives at the same offset in every rusage_info version >= v0,
// but ask for the newest the kernel will give us and fall back on EINVAL.
bool footprint_for(pid_t pid, std::uint64_t& footprint, std::uint64_t& peak) {
  rusage_info_current ri{};
  for (int flavor = RUSAGE_INFO_CURRENT; flavor >= RUSAGE_INFO_V0; --flavor) {
    if (proc_pid_rusage(pid, flavor, reinterpret_cast<rusage_info_t*>(&ri)) == 0) {
      footprint = ri.ri_phys_footprint;
      peak = (flavor >= RUSAGE_INFO_V4) ? ri.ri_lifetime_max_phys_footprint : 0;
      return true;
    }
    if (errno != EINVAL) {
      return false;  // EPERM/ESRCH: not ours, or it exited
    }
  }
  return false;
}

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

  // Headroom because processes can spawn between the two calls.
  std::vector<pid_t> pids(static_cast<std::size_t>(nbytes) * 2 / sizeof(pid_t) + 16, 0);
  nbytes = proc_listpids(
      PROC_ALL_PIDS, 0, pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
  if (nbytes <= 0) {
    throw std::runtime_error(std::format("proc_listpids failed: {}", std::strerror(errno)));
  }
  pids.resize(static_cast<std::size_t>(nbytes) / sizeof(pid_t));
  return pids;
}

// Full argv via KERN_PROCARGS2. Same-uid only, no root needed.
// Layout: [int argc][exec path][NUL padding][argv[0..argc-1]].
std::string cmdline_for(pid_t pid, std::vector<char>& scratch) {
  int mib[3] = {CTL_KERN, KERN_PROCARGS2, pid};
  std::size_t len = scratch.size();
  if (sysctl(mib, 3, scratch.data(), &len, nullptr, 0) != 0 || len < sizeof(int)) {
    return {};
  }

  int argc = 0;
  std::memcpy(&argc, scratch.data(), sizeof(int));
  if (argc <= 0) {
    return {};
  }

  const char* p = scratch.data() + sizeof(int);
  const char* end = scratch.data() + len;
  while (p < end && *p != '\0') {
    ++p;
  }
  while (p < end && *p == '\0') {
    ++p;
  }

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

std::string path_for(pid_t pid) {
  char buf[PROC_PIDPATHINFO_MAXSIZE];
  const int len = proc_pidpath(pid, buf, sizeof(buf));
  return len > 0 ? std::string(buf, static_cast<std::size_t>(len)) : std::string{};
}

std::string name_for(pid_t pid) {
  char buf[2 * MAXCOMLEN + 1];
  const int len = proc_name(pid, buf, static_cast<std::uint32_t>(sizeof(buf)));
  return len > 0 ? std::string(buf) : std::format("<pid {}>", pid);
}

std::optional<ProcessInfo> sample_process(pid_t pid,
                                          bool need_cmdline,
                                          bool need_path,
                                          bool need_gpu,
                                          std::vector<char>& scratch) {
  std::uint64_t fp = 0;
  std::uint64_t peak = 0;
  if (!footprint_for(pid, fp, peak)) {
    return std::nullopt;
  }

  ProcessInfo proc;
  proc.pid = pid;
  proc.footprint = fp;
  proc.lifetime_max = peak;
  proc.name = name_for(pid);
  if (need_path) {
    proc.path = path_for(pid);
  }
  if (need_cmdline) {
    proc.cmdline = cmdline_for(pid, scratch);
  }
  if (need_gpu) {
    proc.gpu_footprint = collect_process_gpu_footprint(pid);
  }
  return proc;
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

Snapshot collect_processes(bool need_cmdline,
                           bool need_path,
                           bool need_gpu,
                           std::optional<pid_t> exclude_pid) {
  const std::vector<pid_t> pids = list_all_pids();
  std::vector<char> scratch;
  if (need_cmdline) {
    scratch.resize(query_argmax());
  }

  Snapshot snap;
  snap.processes.reserve(pids.size());

  for (pid_t pid : pids) {
    if (pid == 0) {
      continue;
    }
    ++snap.total_pids;

    if (exclude_pid && pid == *exclude_pid) {
      continue;
    }
    if (auto proc = sample_process(pid, need_cmdline, need_path, need_gpu, scratch)) {
      snap.total_footprint += proc->footprint;
      snap.processes.push_back(std::move(*proc));
    }
  }

  return snap;
}

SystemMemory collect_system_memory() {
  SystemMemory sys;
  MachHostPort host;
  vm_size_t page = 0;
  vm_statistics64_data_t vm{};
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;

  if (host_page_size(host.port, &page) != KERN_SUCCESS) {
    return sys;
  }
  if (host_statistics64(
          host.port, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) !=
      KERN_SUCCESS) {
    return sys;
  }

  std::uint64_t memsize = 0;
  std::size_t len = sizeof(memsize);
  sysctlbyname("hw.memsize", &memsize, &len, nullptr, 0);

  sys.total = memsize;
  sys.wired = static_cast<std::uint64_t>(vm.wire_count) * page;
  sys.anonymous = static_cast<std::uint64_t>(vm.internal_page_count) * page;
  sys.compressor = static_cast<std::uint64_t>(vm.compressor_page_count) * page;
  sys.file_cache = static_cast<std::uint64_t>(vm.external_page_count) * page;
  sys.free_bytes = static_cast<std::uint64_t>(vm.free_count) * page;
  sys.gpu = collect_system_gpu_memory();
  sys.valid = true;
  return sys;
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

}  // namespace realmem
