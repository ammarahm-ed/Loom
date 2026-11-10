/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "HermesInstance.h"

#include <algorithm>
#include <map>
#include <variant>
#include <vector>
#include <hermes/inspector-modern/chrome/HermesRuntimeTargetDelegate.h>
#include <jsi/jsilib.h>
#include <react/renderer/runtimescheduler/JankReporter.h>
#include <jsinspector-modern/InspectorFlags.h>
#include <react/featureflags/ReactNativeFeatureFlags.h>

using namespace facebook::hermes;
using namespace facebook::jsi;

namespace facebook::react {

class HermesJSRuntime : public JSRuntime {
 public:
  HermesJSRuntime(
      std::shared_ptr<jsi::Runtime> runtime,
      HermesRuntime& hermesRuntime)
      : runtime_(std::move(runtime)), hermesRuntime_(hermesRuntime) {}

  jsi::Runtime& getRuntime() noexcept override {
    return *runtime_;
  }

  jsinspector_modern::RuntimeTargetDelegate& getRuntimeTargetDelegate()
      override {
    if (!targetDelegate_) {
      targetDelegate_.emplace(runtime_, hermesRuntime_);
    }
    return *targetDelegate_;
  }

  void unstable_initializeOnJsThread() override {
    hermesRuntime_.registerForProfiling();
#if RN_JANK_REPORTER
    installJankSampling();
#endif
  }

#if RN_JANK_REPORTER
  /**
   * React Native X (debug): lets the jank watchdog sample the JS stack, but only
   * while a task is over budget. The profiler is off otherwise, so a task that
   * finishes in time pays nothing for it.
   */
  void installJankSampling() {
    auto& reporter = jank::Reporter::instance();
    reporter.installJsBindings(*runtime_);
    auto* hermes = &hermesRuntime_;
    jank::JsSamplingHooks hooks;
    hooks.enable = [hermes](double hz) {
      auto* root = castInterface<IHermesRootAPI>(hermes->getHermesRootAPI());
      if (root != nullptr) {
        root->enableSamplingProfiler(hz);
      }
    };
    hooks.disable = [hermes]() {
      auto* root = castInterface<IHermesRootAPI>(hermes->getHermesRootAPI());
      if (root != nullptr) {
        root->disableSamplingProfiler();
      }
    };
    hooks.summarize = [hermes]() -> std::string {
      auto profile = hermes->dumpSampledTraceToProfile();
      // Count which JS function each sample was inside (the leaf-most JS
      // frame), which is what was running when the sample hit.
      std::map<std::string, int> self;
      int total = 0;
      for (const auto& sample : profile.getSamplesRange()) {
        const auto frames = sample.getCallStackFramesRange();
        std::string key;
        for (auto it = frames.begin(); it != frames.end(); ++it) {
          if (auto* js = std::get_if<
                  sampling_profiler::ProfileSampleCallStackJSFunctionFrame>(
                  &*it)) {
            key = std::string(js->getFunctionName());
            if (key.empty()) {
              key = "(anonymous)";
            }
            if (js->hasScriptUrl()) {
              // Metro's URL carries a long query string; keep the bundle name.
              auto url = std::string(js->getScriptUrl());
              url = url.substr(0, url.find_first_of("?&"));
              while (!url.empty() && url.back() == '/') {
                url.pop_back();
              }
              const auto slash = url.rfind('/');
              if (slash != std::string::npos) {
                url = url.substr(slash + 1);
              }
              key += " (" + url;
              if (js->hasFunctionLineNumber()) {
                key += ":" + std::to_string(js->getFunctionLineNumber());
                if (js->hasFunctionColumnNumber()) {
                  key += ":" + std::to_string(js->getFunctionColumnNumber());
                }
              }
              key += ")";
            }
            break;
          }
        }
        if (!key.empty()) {
          ++self[key];
          ++total;
        }
      }
      std::vector<std::pair<int, std::string>> ranked;
      for (auto& [name, count] : self) {
        ranked.emplace_back(count, name);
      }
      std::sort(ranked.rbegin(), ranked.rend());
      std::string out;
      for (size_t i = 0; i < ranked.size() && i < 4; ++i) {
        out += "    " + std::to_string(ranked[i].first * 100 / std::max(total, 1)) +
            "% " + ranked[i].second + "\n";
      }
      if (!out.empty()) {
        out.pop_back();
      }
      return out;
    };
    reporter.setJsSamplingHooks(std::move(hooks));
  }
#endif

 private:
  std::shared_ptr<jsi::Runtime> runtime_;
  HermesRuntime& hermesRuntime_;
  std::optional<jsinspector_modern::HermesRuntimeTargetDelegate>
      targetDelegate_;
};

std::unique_ptr<JSRuntime> HermesInstance::createJSRuntime(
    std::shared_ptr<::hermes::vm::CrashManager> crashManager,
    std::shared_ptr<MessageQueueThread> msgQueueThread,
    bool allocInOldGenBeforeTTI) noexcept {
  assert(msgQueueThread != nullptr);

  auto gcConfig = ::hermes::vm::GCConfig::Builder()
                      // Default to 3GB
                      .withMaxHeapSize(3072 << 20)
                      .withName("RNBridgeless");

  if (allocInOldGenBeforeTTI) {
    // For the next two arguments: avoid GC before TTI
    // by initializing the runtime to allocate directly
    // in the old generation, but revert to normal
    // operation when we reach the (first) TTI point.
    gcConfig.withAllocInYoung(false).withRevertToYGAtTTI(true);
  }

  ::hermes::vm::RuntimeConfig::Builder runtimeConfigBuilder =
      ::hermes::vm::RuntimeConfig::Builder()
          .withGCConfig(gcConfig.build())
          .withEnableSampleProfiling(true)
          .withMicrotaskQueue(
              ReactNativeFeatureFlags::enableBridgelessArchitecture());

  if (crashManager) {
    runtimeConfigBuilder.withCrashMgr(crashManager);
  }

  std::unique_ptr<HermesRuntime> hermesRuntime =
      hermes::makeHermesRuntime(runtimeConfigBuilder.build());

  auto errorPrototype = hermesRuntime->global()
                            .getPropertyAsObject(*hermesRuntime, "Error")
                            .getPropertyAsObject(*hermesRuntime, "prototype");
  errorPrototype.setProperty(*hermesRuntime, "jsEngine", "hermes");

  (void)msgQueueThread;

  HermesRuntime& hermesRuntimeRef = *hermesRuntime;
  return std::make_unique<HermesJSRuntime>(
      std::move(hermesRuntime), hermesRuntimeRef);
}

} // namespace facebook::react
