/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LoomAllocationCounter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#define LOOM_ALLOCATION_SITES 1
#endif

namespace facebook::react::loomtest {
namespace {

std::atomic<uint64_t> gAllocations{0};

#if LOOM_ALLOCATION_SITES
constexpr int kFrames = 24;
using Stack = std::array<void*, kFrames>;
struct StackHash {
  size_t operator()(const Stack& stack) const noexcept {
    size_t h = 0;
    for (auto* frame : stack) {
      h = h * 1000003u ^ reinterpret_cast<uintptr_t>(frame);
    }
    return h;
  }
};
struct Site {
  uint64_t count{0};
  uint64_t bytes{0};
};

std::atomic<bool> gRecording{false};
// Frees are matched while any recorded allocation may still be alive.
std::atomic<bool> gTracking{false};
thread_local bool tInHook = false;
std::mutex& sitesMutex() {
  static auto* mutex = new std::mutex();
  return *mutex;
}
std::unordered_map<Stack, Site, StackHash>& sites() {
  static auto* map = new std::unordered_map<Stack, Site, StackHash>();
  return *map;
}
struct LiveAllocation {
  const Stack* stack;
  size_t size;
};
std::unordered_map<void*, LiveAllocation>& live() {
  static auto* map = new std::unordered_map<void*, LiveAllocation>();
  return *map;
}

void record(void* p, size_t size) {
  if (!gRecording.load(std::memory_order_relaxed) || tInHook) {
    return;
  }
  tInHook = true;
  Stack stack{};
  backtrace(stack.data(), kFrames - 1);
#if defined(__APPLE__)
  // The last slot marks allocations made on the main (UI) thread.
  stack[kFrames - 1] = pthread_main_np() != 0 ? &gRecording : nullptr;
#endif
  {
    std::lock_guard lock(sitesMutex());
    auto entry = sites().try_emplace(stack).first;
    ++entry->second.count;
    entry->second.bytes += size;
    live()[p] = {&entry->first, size};
  }
  tInHook = false;
}

void forget(void* p) {
  if (p == nullptr || !gTracking.load(std::memory_order_relaxed) || tInHook) {
    return;
  }
  tInHook = true;
  {
    std::lock_guard lock(sitesMutex());
    live().erase(p);
  }
  tInHook = false;
}

std::string symbolOf(void* pc) {
  Dl_info info{};
  if (dladdr(pc, &info) == 0 || info.dli_sname == nullptr) {
    return {};
  }
  int status = 0;
  char* demangled =
      abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
  std::string name = status == 0 && demangled ? demangled : info.dli_sname;
  std::free(demangled);
  return name;
}

/** The function name without its arguments and template arguments. */
std::string shortName(const std::string& name) {
  std::string out;
  int depth = 0;
  for (size_t i = 0; i < name.size(); ++i) {
    const char c = name[i];
    if (c == '<') {
      ++depth;
    } else if (c == '>') {
      --depth;
    } else if (c == '(' && depth == 0) {
      break;
    } else if (depth == 0) {
      out += c;
    }
  }
  return out;
}
#endif

void* allocate(size_t size) {
  gAllocations.fetch_add(1, std::memory_order_relaxed);
  void* p = std::malloc(size == 0 ? 1 : size);
  if (p == nullptr) {
    throw std::bad_alloc();
  }
#if LOOM_ALLOCATION_SITES
  record(p, size);
#endif
  return p;
}

void* allocateAligned(size_t size, std::align_val_t alignment) {
  gAllocations.fetch_add(1, std::memory_order_relaxed);
  void* p = nullptr;
  const auto align = std::max(static_cast<size_t>(alignment), sizeof(void*));
  if (posix_memalign(&p, align, size == 0 ? 1 : size) != 0) {
    throw std::bad_alloc();
  }
#if LOOM_ALLOCATION_SITES
  record(p, size);
#endif
  return p;
}

}  // namespace

uint64_t allocationCount() {
  return gAllocations.load(std::memory_order_relaxed);
}

void startAllocationSites() {
#if LOOM_ALLOCATION_SITES
  const char* flag = std::getenv("LOOM_PROBE_ALLOC_SITES");
  if (flag == nullptr || std::strcmp(flag, "1") != 0) {
    return;
  }
  gRecording = false;
  gTracking = false;
  {
    std::lock_guard lock(sitesMutex());
    live().clear();
    sites().clear();
  }
  gTracking = true;
  gRecording = true;
#endif
}

void stopAllocationSites() {
#if LOOM_ALLOCATION_SITES
  gRecording = false;
#endif
}

void printAllocationSites(const char* label, bool liveOnly, size_t top) {
#if LOOM_ALLOCATION_SITES
  if (!gTracking.load()) {
    return;
  }
  const bool wasRecording = gRecording.exchange(false);
  tInHook = true;
  {
    std::unordered_map<void*, std::string> names;
    const auto nameOf = [&](void* pc) -> const std::string& {
      auto found = names.find(pc);
      if (found == names.end()) {
        found = names.emplace(pc, shortName(symbolOf(pc))).first;
      }
      return found->second;
    };
    // Innermost Loom frame, and the Loom frame that called it.
    std::map<std::string, Site> byFunction;
    uint64_t total = 0;
    uint64_t totalBytes = 0;
    std::unordered_map<const Stack*, Site> held;
    std::lock_guard lock(sitesMutex());
    if (liveOnly) {
      for (const auto& [pointer, allocation] : live()) {
        auto& site = held[allocation.stack];
        ++site.count;
        site.bytes += allocation.size;
      }
    } else {
      for (const auto& [stack, site] : sites()) {
        held[&stack] = site;
      }
    }
    {
      for (const auto& [stackPointer, site] : held) {
        const auto& stack = *stackPointer;
        total += site.count;
        totalBytes += site.bytes;
        std::string key;
        for (int i = 2; i < kFrames - 1 && stack[i] != nullptr; ++i) {
          const auto& name = nameOf(stack[i]);
          if (name.find("Loom") == std::string::npos &&
              name.find("loom::") == std::string::npos) {
            continue;
          }
          if (name.find("loomtest") != std::string::npos) {
            continue;
          }
          if (key.empty()) {
            key = name;
            continue;
          }
          if (name != key) {
            key += "  <-  " + name;
            break;
          }
        }
        if (key.empty()) {
          key = "(no Loom frame) " + nameOf(stack[2]);
        }
        // The function that asked for the memory, when it is not the Loom one.
        for (int i = 2; i < kFrames - 1 && stack[i] != nullptr; ++i) {
          const auto& name = nameOf(stack[i]);
          if (name.empty() || name.rfind("operator new", 0) == 0 ||
              name.rfind("std::", 0) == 0 ||
              name.find("loomtest") != std::string::npos ||
              name.rfind("_Znw", 0) == 0) {
            continue;
          }
          if (key.find(name) != 0) {
            key += "  [in " + name + "]";
          }
          break;
        }
        key = (stack[kFrames - 1] != nullptr ? "[ui] " : "     ") + key;
        auto& entry = byFunction[key];
        entry.count += site.count;
        entry.bytes += site.bytes;
      }
    }
    std::vector<std::pair<std::string, Site>> sorted(
        byFunction.begin(), byFunction.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
      return a.second.count > b.second.count;
    });
    printf(
        "[sites] %s (%s): %llu allocations, %llu bytes\n",
        label,
        liveOnly ? "still allocated" : "made",
        static_cast<unsigned long long>(total),
        static_cast<unsigned long long>(totalBytes));
    for (size_t i = 0; i < sorted.size() && i < top; ++i) {
      printf(
          "[sites]   %8llu  %10llu B  %s\n",
          static_cast<unsigned long long>(sorted[i].second.count),
          static_cast<unsigned long long>(sorted[i].second.bytes),
          sorted[i].first.c_str());
    }
  }
  tInHook = false;
  gRecording = wasRecording;
#else
  (void)label;
  (void)liveOnly;
  (void)top;
#endif
}

}  // namespace facebook::react::loomtest

namespace {
void release(void* p) noexcept {
#if LOOM_ALLOCATION_SITES
  facebook::react::loomtest::forget(p);
#endif
  std::free(p);
}
}  // namespace

using facebook::react::loomtest::allocate;
using facebook::react::loomtest::allocateAligned;

void* operator new(size_t size) {
  return allocate(size);
}
void* operator new[](size_t size) {
  return allocate(size);
}
void* operator new(size_t size, const std::nothrow_t&) noexcept {
  try {
    return allocate(size);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](size_t size, const std::nothrow_t&) noexcept {
  try {
    return allocate(size);
  } catch (...) {
    return nullptr;
  }
}
void* operator new(size_t size, std::align_val_t alignment) {
  return allocateAligned(size, alignment);
}
void* operator new[](size_t size, std::align_val_t alignment) {
  return allocateAligned(size, alignment);
}
void operator delete(void* p) noexcept {
  release(p);
}
void operator delete[](void* p) noexcept {
  release(p);
}
void operator delete(void* p, size_t) noexcept {
  release(p);
}
void operator delete[](void* p, size_t) noexcept {
  release(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
  release(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
  release(p);
}
void operator delete(void* p, size_t, std::align_val_t) noexcept {
  release(p);
}
void operator delete[](void* p, size_t, std::align_val_t) noexcept {
  release(p);
}
