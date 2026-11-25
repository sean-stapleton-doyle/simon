#pragma once

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>

namespace realmem {

class MachPortGuard {
 public:
  explicit MachPortGuard(mach_port_t port = MACH_PORT_NULL) : port_(port) {}
  ~MachPortGuard() { reset(); }

  MachPortGuard(const MachPortGuard&) = delete;
  MachPortGuard& operator=(const MachPortGuard&) = delete;

  mach_port_t get() const { return port_; }
  mach_port_t* out() {
    reset();
    return &port_;
  }

  void reset(mach_port_t next = MACH_PORT_NULL) {
    if (port_ != MACH_PORT_NULL && port_ != mach_task_self()) {
      mach_port_deallocate(mach_task_self(), port_);
    }
    port_ = next;
  }

 private:
  mach_port_t port_ = MACH_PORT_NULL;
};

class IoObjectGuard {
 public:
  explicit IoObjectGuard(io_object_t obj = MACH_PORT_NULL) : obj_(obj) {}
  ~IoObjectGuard() { reset(); }

  IoObjectGuard(const IoObjectGuard&) = delete;
  IoObjectGuard& operator=(const IoObjectGuard&) = delete;

  io_object_t get() const { return obj_; }

  void reset(io_object_t next = MACH_PORT_NULL) {
    if (obj_ != MACH_PORT_NULL) {
      IOObjectRelease(obj_);
    }
    obj_ = next;
  }

 private:
  io_object_t obj_ = MACH_PORT_NULL;
};

class CfTypeGuard {
 public:
  explicit CfTypeGuard(CFTypeRef ref = nullptr) : ref_(ref) {}
  ~CfTypeGuard() { reset(); }

  CfTypeGuard(const CfTypeGuard&) = delete;
  CfTypeGuard& operator=(const CfTypeGuard&) = delete;

  CfTypeGuard(CfTypeGuard&& other) noexcept : ref_(other.ref_) { other.ref_ = nullptr; }
  CfTypeGuard& operator=(CfTypeGuard&& other) noexcept {
    if (this != &other) {
      reset();
      ref_ = other.ref_;
      other.ref_ = nullptr;
    }
    return *this;
  }

  CFTypeRef get() const { return ref_; }
  explicit operator bool() const { return ref_ != nullptr; }

  void reset(CFTypeRef next = nullptr) {
    if (ref_ != nullptr) {
      CFRelease(ref_);
    }
    ref_ = next;
  }

 private:
  CFTypeRef ref_ = nullptr;
};

}  // namespace realmem
