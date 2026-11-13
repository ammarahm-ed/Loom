/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LoomAndroidHost.h"

#if LOOM_RENDERER

#include "FabricUIManagerBinding.h"

#include <android/log.h>
#include <fbjni/fbjni.h>
#include <react/renderer/runtimescheduler/LoomSettings.h>
#include <react/renderer/uimanager/LoomViewManagerNames.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <climits>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <pthread.h>
#include <thread>

namespace {

// Text is left out of the prop diffs: one of its managers reads the stored
// props in updateState, which would then see only the last delta.
bool loomSendsPropDiffs(const std::string& name) {
  return name == "RCTView" || name == "RCTImageView" ||
      name == "RCTScrollView" || name == "AndroidHorizontalScrollView";
}

void loomLogToLogcat(
    facebook::react::LoomLogLevel level,
    const char* tag,
    const char* message) {
  __android_log_print(
      level == facebook::react::LoomLogLevel::Warn ? ANDROID_LOG_WARN
                                                   : ANDROID_LOG_INFO,
      tag,
      "%s",
      message);
}

// `adb shell setprop debug.loom.<name> <value>`: a legacy override of a setting.
bool loomReadProperty(const char* name, LoomSettingType type, int& out) {
  char value[PROP_VALUE_MAX] = {};
  if (__system_property_get(name, value) <= 0) {
    return false;
  }
  out = type == LoomSettingType::Bool
      ? (value[0] == '1' || value[0] == 't' || value[0] == 'y' ? 1 : 0)
      : std::atoi(value);
  return true;
}

// The Java half of the jank reporter keeps its own copy of the two jank settings.
void loomPushJankSettings(LoomSettingId id, int) {
  if (id != LoomSettingId::JankReporter && id != LoomSettingId::JankBudgetMs) {
    return;
  }
  try {
    facebook::jni::ThreadScope scope;
    static auto cls = facebook::jni::findClassStatic(
        "com/facebook/react/common/JankReporterSupport");
    static auto configure = cls->getStaticMethod<void(jboolean, jint)>("configure");
    configure(
        cls,
        LoomSettings::enabled(LoomSettingId::JankReporter),
        LoomSettings::get(LoomSettingId::JankBudgetMs));
  } catch (...) {
  }
}

bool loomIsUIThread() {
  return static_cast<pid_t>(::syscall(SYS_gettid)) == ::getpid();
}

void loomWithWorkerScope(const std::function<void()>& body) {
  facebook::jni::ThreadScope::WithClassLoader([&body] { body(); });
}

// Loom: threads for the renderer's parallel text measurement
// (LoomPlatformTraits::parallelFor). Text is measured by Java (StaticLayout)
// through JNI, so each thread stays attached to the JVM, with the app's class
// loader, for its whole life. One job at a time; the calling thread works on it
// too.
class LoomTextPool {
 public:
  static LoomTextPool& shared() {
    static auto* pool = new LoomTextPool();
    return *pool;
  }

  void run(size_t count, const std::function<void(size_t)>& body) {
    std::lock_guard<std::mutex> serial(runMutex_);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      body_ = &body;
      count_ = count;
      next_ = 0;
      pending_ = count;
      ++generation_;
    }
    wake_.notify_all();
    work();
    std::unique_lock<std::mutex> lock(mutex_);
    done_.wait(lock, [this] { return pending_ == 0; });
    body_ = nullptr;
  }

 private:
  LoomTextPool() {
    const unsigned cores = std::thread::hardware_concurrency();
    const unsigned threads = cores > 2 ? std::min(cores - 2, 5u) : 1u;
    for (unsigned index = 0; index < threads; ++index) {
      std::thread([this] {
        pthread_setname_np(pthread_self(), "loom.text");
        facebook::jni::ThreadScope::WithClassLoader([this] { loop(); });
      }).detach();
    }
  }

  void loop() {
    uint64_t seen = 0;
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [&] { return generation_ != seen; });
        seen = generation_;
      }
      work();
    }
  }

  void work() {
    for (;;) {
      size_t index;
      const std::function<void(size_t)>* body;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (body_ == nullptr || next_ >= count_) {
          return;
        }
        index = next_++;
        body = body_;
      }
      try {
        (*body)(index);
      } catch (...) {
        // That paragraph stays unmeasured; the real layout pass measures it.
      }
      std::lock_guard<std::mutex> lock(mutex_);
      if (--pending_ == 0) {
        done_.notify_all();
      }
    }
  }

  std::mutex runMutex_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable done_;
  const std::function<void(size_t)>* body_{nullptr};
  size_t count_{0};
  size_t next_{0};
  size_t pending_{0};
  uint64_t generation_{0};
};

} // namespace

namespace facebook::react {

LoomPlatformTraits LoomAndroidHost::platformTraits() {
  LoomPlatformTraits traits;
  traits.components = loomViewManagerComponentTable();
  traits.sendsPropDiffs = loomSendsPropDiffs;
  traits.hasPreparedTextLayout = true;
  traits.log = loomLogToLogcat;
  traits.isUIThread = loomIsUIThread;
  traits.withWorkerScope = loomWithWorkerScope;
  traits.parallelFor = [](size_t count, const std::function<void(size_t)>& body) {
    LoomTextPool::shared().run(count, body);
  };
  return traits;
}

void LoomAndroidHost::loadSettings(facebook::jni::alias_ref<jobject> javaUIManager) {
  // Built-in defaults, then the manifest's `<meta-data android:name="loom.<name>">`,
  // then the legacy `debug.loom.*` system properties. JavaScript writes come later.
  LoomSettings::configurePlatform(kLoomPlatformAndroid);
  LoomSettings::set(LoomSettingId::ProfileWorker, LOOM_PROFILE);
  constexpr int kAbsent = INT_MIN;
  for (size_t i = 0; i < LoomSettings::kCount; ++i) {
    const auto id = static_cast<LoomSettingId>(i);
    const auto& info = LoomSettings::info(id);
    if (info.readOnly || !LoomSettings::applies(id)) {
      continue;
    }
    try {
      static auto metaData = javaUIManager->getClass()->getMethod<jint(jstring, jint)>("loomMetaDataInt");
      const int configured = metaData(
          javaUIManager,
          facebook::jni::make_jstring(std::string("loom.") + info.name).get(),
          kAbsent);
      if (configured != kAbsent) {
        LoomSettings::set(id, configured);
      }
    } catch (...) {
    }
    int value = 0;
    if (info.androidProperty != nullptr && loomReadProperty(info.androidProperty, info.type, value)) {
      LoomSettings::set(id, value);
    }
  }
  LoomSettings::setChangeListener(loomPushJankSettings);
  loomPushJankSettings(LoomSettingId::JankReporter, 0);
}

void LoomAndroidHost::warmUpTextPool() {
  LoomTextPool::shared();
}

LoomAndroidTextServices::LoomAndroidTextServices(
    FabricUIManagerBinding* binding,
    std::shared_ptr<const ContextContainer> contextContainer)
    : LoomTextLayoutManagerServices(std::move(contextContainer), nullptr, false),
      binding_(binding) {}

void LoomAndroidTextServices::invalidate() {
  binding_.store(nullptr);
}

std::optional<Size> LoomAndroidTextServices::measureText(
    SurfaceId surfaceId,
    const AttributedString& attributedString,
    const ParagraphAttributes& paragraphAttributes,
    const Size& minimumSize,
    const Size& maximumSize) {
  auto* binding = binding_.load();
  return binding == nullptr
      ? std::nullopt
      : binding->loomMeasureText(
            surfaceId, attributedString, paragraphAttributes, minimumSize, maximumSize);
}

bool LoomAndroidTextServices::canMeasureOnWorker() {
  auto* binding = binding_.load();
  return binding != nullptr && binding->loomCanMeasureTextOnWorker();
}

LoomTextMeasurement LoomAndroidTextServices::prepareText(
    SurfaceId surfaceId,
    const AttributedString& attributedString,
    const ParagraphAttributes& paragraphAttributes,
    const Size& minimumSize,
    const Size& maximumSize) {
  auto* binding = binding_.load();
  return binding == nullptr
      ? LoomTextMeasurement{}
      : binding->loomPrepareText(
            surfaceId, attributedString, paragraphAttributes, minimumSize, maximumSize);
}

LoomTextMeasurement LoomAndroidTextServices::prepareTextForMount(
    SurfaceId surfaceId,
    const AttributedString& attributedString,
    const ParagraphAttributes& paragraphAttributes,
    const Size& minimumSize,
    const Size& maximumSize) {
  auto* binding = binding_.load();
  return binding == nullptr
      ? LoomTextMeasurement{}
      : binding->loomPrepareTextForMount(
            surfaceId, attributedString, paragraphAttributes, minimumSize, maximumSize);
}

LoomAndroidHost::LoomAndroidHost(
    FabricUIManagerBinding* binding,
    std::shared_ptr<const ContextContainer> contextContainer)
    : binding_(binding), text_(binding, std::move(contextContainer)) {}

void LoomAndroidHost::invalidate() {
  binding_.store(nullptr);
  text_.invalidate();
}

std::function<void(std::function<void()>)> LoomAndroidHost::uiExecutor() {
  auto* binding = binding_.load();
  return binding == nullptr ? nullptr : binding->loomGetUIExecutor();
}

#define LOOM_ANDROID_FORWARD(call)       \
  if (auto* binding = binding_.load()) { \
    binding->call;                       \
  }

void LoomAndroidHost::applyBatch(SurfaceId surfaceId, LoomMountBatch batch) {
  LOOM_ANDROID_FORWARD(loomApplyBatch(surfaceId, std::move(batch)))
}

void LoomAndroidHost::failSurface(SurfaceId surfaceId, const std::string& reason) {
  LOOM_ANDROID_FORWARD(loomFailSurface(surfaceId, reason))
}

void LoomAndroidHost::updateViewProps(Tag tag, const folly::dynamic& props) {
  LOOM_ANDROID_FORWARD(schedulerShouldSynchronouslyUpdateViewOnUIThread(tag, props))
}

void LoomAndroidHost::registerEventTarget(
    SurfaceId surfaceId,
    Tag tag,
    InstanceHandle::Shared instanceHandle) {
  LOOM_ANDROID_FORWARD(loomRegisterEventTarget(surfaceId, tag, std::move(instanceHandle)))
}

void LoomAndroidHost::unregisterEventTarget(SurfaceId surfaceId, Tag tag) {
  LOOM_ANDROID_FORWARD(loomUnregisterEventTarget(surfaceId, tag))
}

void LoomAndroidHost::dispatchEvent(
    SurfaceId surfaceId,
    Tag tag,
    const std::string& eventName,
    folly::dynamic payload,
    RawEvent::Category category,
    bool unique) {
  LOOM_ANDROID_FORWARD(
      loomDispatchEvent(surfaceId, tag, eventName, std::move(payload), category, unique))
}

void LoomAndroidHost::updateComponentState(
    SurfaceId surfaceId,
    Tag tag,
    const std::shared_ptr<const State>& state) {
  LOOM_ANDROID_FORWARD(loomUpdateComponentState(surfaceId, tag, state))
}

void LoomAndroidHost::updateTextState(const LoomTextStateUpdate& update) {
  LOOM_ANDROID_FORWARD(loomUpdateState(update))
}

void LoomAndroidHost::dispatchCommand(
    SurfaceId surfaceId,
    Tag tag,
    const std::string& commandName,
    const folly::dynamic& args) {
  LOOM_ANDROID_FORWARD(loomDispatchCommand(surfaceId, tag, commandName, args))
}

void LoomAndroidHost::sendAccessibilityEvent(
    SurfaceId surfaceId,
    Tag tag,
    const std::string& eventType) {
  LOOM_ANDROID_FORWARD(loomSendAccessibilityEvent(surfaceId, tag, eventType))
}

void LoomAndroidHost::setIsJSResponder(
    SurfaceId surfaceId,
    Tag tag,
    bool isJSResponder,
    bool blockNativeResponder) {
  LOOM_ANDROID_FORWARD(
      loomSetIsJSResponder(surfaceId, tag, isJSResponder, blockNativeResponder))
}

void LoomAndroidHost::reportUnsupportedInlineAttachment(SurfaceId surfaceId, Tag tag) {
  LOOM_ANDROID_FORWARD(loomReportUnsupportedInlineAttachment(surfaceId, tag))
}

#undef LOOM_ANDROID_FORWARD

std::pair<float, float> LoomAndroidHost::getScrollOffset(SurfaceId surfaceId, Tag tag) {
  auto* binding = binding_.load();
  return binding == nullptr ? std::pair<float, float>{0, 0}
                            : binding->loomGetScrollOffset(surfaceId, tag);
}

std::optional<std::pair<float, float>> LoomAndroidHost::getViewLocationInSurface(
    SurfaceId surfaceId,
    Tag tag) {
  auto* binding = binding_.load();
  return binding == nullptr ? std::nullopt
                            : binding->loomGetViewLocationInSurface(surfaceId, tag);
}

} // namespace facebook::react

#endif // LOOM_RENDERER
