/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "JankReporter.h"

#if RN_JANK_REPORTER

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#endif

namespace facebook::react::jank {

namespace {

int64_t nowNs() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

/** Per-task facts the JS thread collects as the task runs. */
struct TaskFacts {
  int depth{0};
  Cause cause{Cause::Other};
  bool causeSet{false};
  char label[64]{};
  unsigned commits{0};
  double commitMs{0};
  std::string site;
  double siteMs{0};

  void reset() {
    cause = Cause::Other;
    causeSet = false;
    label[0] = 0;
    commits = 0;
    commitMs = 0;
    site.clear();
    siteMs = 0;
  }
};

thread_local TaskFacts tFacts;

/** Reads `debug.resha.<name>`: a debug-only knob set with `adb shell setprop`. */
int readIntProperty(const char* name, int fallback) {
#if defined(__ANDROID__)
  char value[PROP_VALUE_MAX] = {};
  if (__system_property_get(name, value) > 0) {
    return std::atoi(value);
  }
#endif
  return fallback;
}

/** A finished over-budget task, waiting for its stack samples (if any). */
struct Record {
  uint32_t seq{0};
  TaskTimes times;
  Cause cause{Cause::Other};
  std::string label;
  unsigned commits{0};
  double commitMs{0};
  std::string site;
  double siteMs{0};
};

} // namespace

struct Reporter::Impl {
  std::atomic<int64_t> startNs{0};
  std::atomic<uint32_t> seq{0};
  std::atomic<uint32_t> samplingSeq{0};
  std::atomic<bool> watchdogStarted{false};
  std::atomic<bool> hasPending{false};

  std::mutex mutex;
  std::deque<std::string> pending;
  std::vector<Record> ended;
  JsSamplingHooks js;
  std::function<std::string()> nativeSampler;
  int64_t lastAcceptedNs{0};
  unsigned suppressed{0};
  std::thread watchdog;
  Reporter* owner{nullptr};

  void ensureWatchdog() {
    if (watchdogStarted.load(std::memory_order_relaxed)) {
      return;
    }
    if (watchdogStarted.exchange(true)) {
      return;
    }
    watchdog = std::thread([this] { watch(); });
    watchdog.detach();
  }

  void watch();
  std::string compose(const Record& record, const std::string& jsStack, const std::string& nativeStack);
  void accept(std::string message);
};

Reporter& Reporter::instance() {
  static Reporter* reporter = new Reporter();
  return *reporter;
}

Reporter::Reporter() : impl_(new Impl()) {
  impl_->owner = this;
  // Debug-only knobs: `adb shell setprop debug.resha.jank_budget_ms 30` and
  // `debug.resha.jank 0`. Read once; there is deliberately no release switch.
  budgetMs_.store(
      std::max(1, readIntProperty("debug.resha.jank_budget_ms", 50)));
  enabled_.store(readIntProperty("debug.resha.jank", 1) != 0);
}

void Reporter::beginTask() noexcept {
  if (!enabled()) {
    return;
  }
  if (tFacts.depth++ != 0) {
    return;
  }
  tFacts.reset();
  impl_->ensureWatchdog();
  impl_->seq.fetch_add(1, std::memory_order_relaxed);
  impl_->startNs.store(nowNs(), std::memory_order_release);
}

void Reporter::endTask(const TaskTimes& times) {
  if (!enabled() || tFacts.depth == 0) {
    return;
  }
  if (--tFacts.depth != 0) {
    return;
  }
  const bool over = times.totalMs() >= budgetMs();
  if (over) {
    Record record;
    record.seq = impl_->seq.load(std::memory_order_relaxed);
    record.times = times;
    record.cause = tFacts.cause;
    record.label = tFacts.label;
    record.commits = tFacts.commits;
    record.commitMs = tFacts.commitMs;
    record.site = tFacts.site;
    record.siteMs = tFacts.siteMs;
    // Deposited before the task is marked finished, so the watchdog that is
    // still sampling this task finds it as soon as it sees the task end.
    if (impl_->samplingSeq.load(std::memory_order_acquire) == record.seq) {
      std::lock_guard lock(impl_->mutex);
      impl_->ended.push_back(std::move(record));
    } else {
      impl_->accept(impl_->compose(record, {}, {}));
    }
  }
  impl_->startNs.store(0, std::memory_order_release);
}

void Reporter::setCause(Cause cause, const char* label) noexcept {
  if (tFacts.depth == 0 || tFacts.causeSet) {
    return;
  }
  tFacts.cause = cause;
  tFacts.causeSet = true;
  if (label != nullptr) {
    std::snprintf(tFacts.label, sizeof(tFacts.label), "%s", label);
  }
}

void Reporter::addCommit(double ms) noexcept {
  if (tFacts.depth == 0) {
    return;
  }
  ++tFacts.commits;
  tFacts.commitMs += ms;
}

void Reporter::noteScheduleSite(std::string site, double ms) {
  if (tFacts.depth == 0) {
    return;
  }
  // The longest callback wins: it is the one that made the task slow.
  if (ms >= tFacts.siteMs) {
    tFacts.site = std::move(site);
    tFacts.siteMs = ms;
  }
}

void Reporter::setJsSamplingHooks(JsSamplingHooks hooks) {
  std::lock_guard lock(impl_->mutex);
  impl_->js = std::move(hooks);
}

void Reporter::setNativeStackSampler(std::function<std::string()> sampler) {
  std::lock_guard lock(impl_->mutex);
  impl_->nativeSampler = std::move(sampler);
}

void Reporter::Impl::accept(std::string message) {
  std::lock_guard lock(mutex);
  const int64_t now = nowNs();
  // At most one report per second; the rest are counted, not lost silently.
  if (lastAcceptedNs != 0 && now - lastAcceptedNs < 1'000'000'000) {
    ++suppressed;
    return;
  }
  lastAcceptedNs = now;
  if (suppressed > 0) {
    message += "\n  (" + std::to_string(suppressed) +
        " more slow task(s) since the last report were not shown.)";
    suppressed = 0;
  }
  pending.push_back(std::move(message));
  hasPending.store(true, std::memory_order_release);
}

void Reporter::enqueueMessage(std::string message) {
  impl_->accept(std::move(message));
}

std::string Reporter::Impl::compose(
    const Record& r,
    const std::string& jsStack,
    const std::string& nativeStack) {
  Cause cause = r.cause;
  if (cause == Cause::Other && r.commits > 0) {
    cause = Cause::Commit;
  }
  std::string what;
  switch (cause) {
    case Cause::Event:
      what = "a JS event handler" +
          (r.label.empty() ? std::string() : " (" + r.label + ")");
      break;
    case Cause::Timer:
      what = "a " + (r.label.empty() ? std::string("timer") : r.label) +
          " callback";
      break;
    case Cause::AnimationFrame:
      what = "a requestAnimationFrame callback";
      break;
    case Cause::Commit:
      what = "a React render/commit";
      break;
    case Cause::Other:
      what = "a JS task";
      break;
  }
  char head[512];
  std::snprintf(
      head,
      sizeof(head),
      "[Resha] Main thread blocked %.0f ms (debug build; release is usually several times faster) by %s: "
      "%.0f ms JS, %.0f ms microtasks, %.0f ms rendering. "
      "If it is anywhere near this long in release it will drop frames; split the work or move it off the main thread (e.g. a worker).",
      r.times.totalMs(),
      what.c_str(),
      r.times.taskMs,
      r.times.microtaskMs,
      r.times.renderingMs);
  std::string out = head;
  if (r.commits > 0) {
    char commits[160];
    std::snprintf(
        commits,
        sizeof(commits),
        "\n  Included %u native commit(s) taking %.0f ms.",
        r.commits,
        r.commitMs);
    out += commits;
  }
  if (!r.site.empty()) {
    char site[48];
    std::snprintf(site, sizeof(site), " ran %.0f ms", r.siteMs);
    out += "\n  Callback scheduled at: " + r.site + site;
  }
  if (!jsStack.empty()) {
    out += "\n  JS (sampled while blocked):\n" + jsStack;
  }
  if (!nativeStack.empty()) {
    out += "\n  Native (sampled while blocked):\n" + nativeStack;
  }
  return out;
}

void Reporter::Impl::watch() {
  using namespace std::chrono;
  constexpr int kMaxSamples = 10;
  uint32_t lastHandled = 0;
  for (;;) {
    std::this_thread::sleep_for(milliseconds(10));
    const int64_t start = startNs.load(std::memory_order_acquire);
    if (start == 0) {
      continue;
    }
    const uint32_t current = seq.load(std::memory_order_relaxed);
    const int64_t budgetNs = static_cast<int64_t>(owner->budgetMs()) * 1'000'000;
    if (current == lastHandled || nowNs() - start < budgetNs) {
      continue;
    }
    lastHandled = current;
    // The task is over budget and still running. Sampling starts only now:
    // the profiler is off for every task that finishes in time.
    samplingSeq.store(current, std::memory_order_release);
    JsSamplingHooks hooks;
    std::function<std::string()> sampler;
    {
      std::lock_guard lock(mutex);
      hooks = js;
      sampler = nativeSampler;
    }
    bool jsOn = false;
    if (hooks.enable) {
      try {
        hooks.enable(1000.0);
        jsOn = true;
      } catch (...) {
      }
    }
    std::map<std::string, int> nativeVotes;
    for (int i = 0; i < kMaxSamples; ++i) {
      std::this_thread::sleep_for(milliseconds(4));
      if (startNs.load(std::memory_order_acquire) == 0 ||
          seq.load(std::memory_order_relaxed) != current) {
        break;
      }
      if (sampler) {
        auto stack = sampler();
        if (!stack.empty()) {
          ++nativeVotes[stack];
        }
      }
    }
    std::string jsStack;
    if (jsOn) {
      try {
        if (hooks.disable) {
          hooks.disable();
        }
        if (hooks.summarize) {
          jsStack = hooks.summarize();
        }
      } catch (...) {
      }
    }
    std::string nativeStack;
    int best = 0;
    for (const auto& [stack, votes] : nativeVotes) {
      if (votes > best) {
        best = votes;
        nativeStack = stack;
      }
    }
    // The JS thread deposits the record before it marks the task finished.
    // Give it a moment in case the task is still winding down.
    Record record;
    bool found = false;
    for (int attempt = 0; attempt < 200 && !found; ++attempt) {
      {
        std::lock_guard lock(mutex);
        for (auto it = ended.begin(); it != ended.end(); ++it) {
          if (it->seq == current) {
            record = std::move(*it);
            ended.erase(it);
            found = true;
            break;
          }
        }
        // Records of tasks the watchdog never sampled cannot accumulate.
        if (ended.size() > 8) {
          ended.erase(ended.begin());
        }
      }
      if (!found) {
        std::this_thread::sleep_for(milliseconds(1));
        if (startNs.load(std::memory_order_acquire) != 0 &&
            seq.load(std::memory_order_relaxed) != current) {
          break;
        }
      }
    }
    if (found) {
      accept(compose(record, jsStack, nativeStack));
    }
  }
}

void Reporter::flushPending(jsi::Runtime& runtime) {
  if (!impl_->hasPending.load(std::memory_order_acquire)) {
    return;
  }
  std::deque<std::string> messages;
  {
    std::lock_guard lock(impl_->mutex);
    messages.swap(impl_->pending);
    impl_->hasPending.store(false, std::memory_order_release);
  }
  try {
    auto console = runtime.global().getProperty(runtime, "console");
    if (!console.isObject()) {
      return;
    }
    auto warn = console.asObject(runtime).getProperty(runtime, "warn");
    if (!warn.isObject() || !warn.asObject(runtime).isFunction(runtime)) {
      return;
    }
    auto function = warn.asObject(runtime).asFunction(runtime);
    // The dev JS module, when present, maps bundle positions to sources before
    // it warns; without it the raw message goes straight to console.warn.
    auto report = runtime.global().getProperty(runtime, "__reshaJankReport");
    const bool hasReport =
        report.isObject() && report.asObject(runtime).isFunction(runtime);
    for (const auto& message : messages) {
      auto text = jsi::String::createFromUtf8(runtime, message);
      if (hasReport) {
        report.asObject(runtime).asFunction(runtime).call(runtime, text);
      } else {
        function.call(runtime, text);
      }
    }
  } catch (...) {
  }
}

void Reporter::installJsBindings(jsi::Runtime& runtime) {
  using jsi::Function;
  using jsi::PropNameID;
  using jsi::Value;
  auto& global = runtime;
  // __reshaJankBudgetMs(): the budget, so the dev module and the native side agree.
  runtime.global().setProperty(
      runtime,
      "__reshaJankBudgetMs",
      Function::createFromHostFunction(
          runtime,
          PropNameID::forAscii(runtime, "__reshaJankBudgetMs"),
          0,
          [this](jsi::Runtime&, const Value&, const Value*, size_t) -> Value {
            return Value(enabled() ? budgetMs() : 0);
          }));
  // __reshaJankNote(kind, site, ms): a wrapped timer callback ran long; remember
  // where it was scheduled so the report for this task can name the site.
  runtime.global().setProperty(
      runtime,
      "__reshaJankNote",
      Function::createFromHostFunction(
          runtime,
          PropNameID::forAscii(runtime, "__reshaJankNote"),
          3,
          [this](jsi::Runtime& rt, const Value&, const Value* args, size_t count) -> Value {
            if (count >= 3 && args[0].isString() && args[1].isString() &&
                args[2].isNumber()) {
              setCause(Cause::Timer, args[0].asString(rt).utf8(rt).c_str());
              noteScheduleSite(args[1].asString(rt).utf8(rt), args[2].asNumber());
            }
            return Value::undefined();
          }));
  (void)global;
}

} // namespace facebook::react::jank

#endif // RN_JANK_REPORTER
