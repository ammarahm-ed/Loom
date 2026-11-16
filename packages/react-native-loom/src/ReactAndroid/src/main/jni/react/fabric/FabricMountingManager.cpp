/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "FabricMountingManager.h"

#include <chrono>
#include <cmath>
#include <stdexcept>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include "EventEmitterWrapper.h"
#include "MountItem.h"
#include "StateWrapperImpl.h"

#include <cxxreact/TraceSection.h>
#include <react/featureflags/ReactNativeFeatureFlags.h>
#include <react/jni/ReadableNativeMap.h>
#include <react/jni/ReadableNativeArray.h>
#include <react/common/mapbuffer/JReadableMapBuffer.h>
#include <react/renderer/components/scrollview/ScrollViewProps.h>
#include <react/renderer/components/textinput/platform/android/react/renderer/components/androidtextinput/AndroidTextInputState.h>
#include <react/renderer/components/text/ParagraphState.h>
#include <react/renderer/core/ConcreteState.h>
#include <react/renderer/core/DynamicPropsUtilities.h>
#include <react/renderer/core/conversions.h>
#include <react/renderer/mapbuffer/MapBufferBuilder.h>
#include <react/renderer/mounting/MountingTransaction.h>
#include <react/renderer/mounting/ShadowView.h>
#include <react/renderer/mounting/ShadowViewMutation.h>

#include <fbjni/fbjni.h>
#include <glog/logging.h>
#include <android/log.h>

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <unordered_map>
#include <cfenv>
#include <cmath>
#include <vector>

namespace facebook::react {

void logDirectRendererJavaException(
    const char* operation,
    const jni::JniException& error) {
  // The renderer fails a surface with only the exception's message; log the Java stack while it
  // is still available, so a failure can be traced to the Java frame that threw.
  static const auto log = jni::findClassStatic("android/util/Log");
  static const auto getStackTraceString =
      log->getStaticMethod<jstring(jni::JThrowable::javaobject)>("getStackTraceString");
  __android_log_print(
      ANDROID_LOG_ERROR,
      "LoomDirect",
      "%s failed: %s",
      operation,
      getStackTraceString(log, error.getThrowable().get())->toStdString().c_str());
}

#if LOOM_RENDERER
namespace {

/**
 * Generic state used by Android managers whose contract is to retain the
 * StateWrapper and receive later native updates, rather than decode a
 * component-specific map during creation. Component-specific payloads still
 * use their concrete StateData below.
 */
class DirectGenericStateData final {
 public:
  DirectGenericStateData() = default;
  DirectGenericStateData(
      const DirectGenericStateData&,
      folly::dynamic) {}

  folly::dynamic getDynamic() const {
    return folly::dynamic::object();
  }
};

} // namespace

namespace {

/*
 * Loom: props for the Loom renderer's create and update operations,
 * encoded as ints instead of one ReadableNativeMap per view.
 *
 * Java read every ReadableNativeMap back through JNI (key array, then a value
 * array), which cost several microseconds per view and dominated applying a
 * large update on the UI thread. The encoded form crosses JNI once per batch:
 * Java rebuilds plain JavaOnlyMap/JavaOnlyArray objects from it
 * (LoomPropsTable.kt), so view managers still receive a ReadableMap.
 *
 * Layout of one value in `data`:
 *   0 null | 1 false | 2 true | 3 int32 value | 4 double hi lo
 *   5 string <string index> | 6 map <count> (<key string index> <value>)*
 *   7 array <count> <value>*
 * `offsets[objectSlot]` is where the map for that object slot starts, -1 when
 * the slot carries a Java object instead. Strings, keys included, are sent once
 * per batch.
 */
class LoomPropsEncoder {
 public:
  explicit LoomPropsEncoder(size_t objectCount) : offsets_(objectCount, -1) {}

  bool encode(const folly::dynamic& props, size_t objectSlot) {
    const auto start = data_.size();
    offsets_[objectSlot] = static_cast<jint>(start);
    if (props.isObject() && writeValue(props, 0)) {
      return true;
    }
    data_.resize(start);
    offsets_[objectSlot] = -1;
    return false;
  }

  bool empty() const {
    return data_.empty();
  }

  jni::local_ref<jobject> toJava() const {
    static auto tableClass = jni::findClassStatic(
        "com/facebook/react/fabric/mounting/LoomPropsTable");
    static auto constructor = tableClass->getConstructor<jobject(
        jintArray, jintArray, jni::jtypeArray<jstring>)>();
    auto env = jni::Environment::current();
    auto offsets = env->NewIntArray(static_cast<jsize>(offsets_.size()));
    env->SetIntArrayRegion(
        offsets, 0, static_cast<jsize>(offsets_.size()), offsets_.data());
    auto data = env->NewIntArray(static_cast<jsize>(data_.size()));
    env->SetIntArrayRegion(
        data, 0, static_cast<jsize>(data_.size()), data_.data());
    auto strings = jni::JArrayClass<jstring>::newArray(strings_.size());
    for (size_t index = 0; index < strings_.size(); ++index) {
      strings->setElement(index, jni::make_jstring(strings_[index]).get());
    }
    auto table = tableClass->newObject(constructor, offsets, data, strings.get());
    env->DeleteLocalRef(offsets);
    env->DeleteLocalRef(data);
    return table;
  }

 private:
  static constexpr int kMaxDepth = 32;

  jint stringIndex(const std::string& value) {
    auto [it, inserted] =
        stringIds_.try_emplace(value, static_cast<jint>(strings_.size()));
    if (inserted) {
      strings_.push_back(value);
    }
    return it->second;
  }

  bool writeValue(const folly::dynamic& value, int depth) {
    if (depth > kMaxDepth) {
      return false;
    }
    switch (value.type()) {
      case folly::dynamic::NULLT:
        data_.push_back(0);
        return true;
      case folly::dynamic::BOOL:
        data_.push_back(value.getBool() ? 2 : 1);
        return true;
      case folly::dynamic::INT64: {
        const auto number = value.getInt();
        if (number >= std::numeric_limits<jint>::min() &&
            number <= std::numeric_limits<jint>::max()) {
          data_.push_back(3);
          data_.push_back(static_cast<jint>(number));
        } else {
          writeDouble(static_cast<double>(number));
        }
        return true;
      }
      case folly::dynamic::DOUBLE:
        writeDouble(value.getDouble());
        return true;
      case folly::dynamic::STRING:
        data_.push_back(5);
        data_.push_back(stringIndex(value.getString()));
        return true;
      case folly::dynamic::OBJECT:
        data_.push_back(6);
        data_.push_back(static_cast<jint>(value.size()));
        for (const auto& [key, item] : value.items()) {
          if (!key.isString()) {
            return false;
          }
          data_.push_back(stringIndex(key.getString()));
          if (!writeValue(item, depth + 1)) {
            return false;
          }
        }
        return true;
      case folly::dynamic::ARRAY:
        data_.push_back(7);
        data_.push_back(static_cast<jint>(value.size()));
        for (const auto& item : value) {
          if (!writeValue(item, depth + 1)) {
            return false;
          }
        }
        return true;
    }
    return false;
  }

  void writeDouble(double number) {
    const auto bits = std::bit_cast<int64_t>(number);
    data_.push_back(4);
    data_.push_back(static_cast<jint>(bits >> 32));
    data_.push_back(static_cast<jint>(bits & 0xffffffff));
  }

  std::vector<jint> offsets_;
  std::vector<jint> data_;
  std::vector<std::string> strings_;
  std::unordered_map<std::string, jint> stringIds_;
};

} // namespace

void FabricMountingManager::directApplyBatch(
    SurfaceId surfaceId,
    LoomMountBatch batch) {
#if LOOM_PROFILE
  const auto profileStart = std::chrono::steady_clock::now();
#endif
  static auto method = JFabricUIManager::javaClassStatic()->getMethod<void(
      jint,
      jintArray,
      jni::jtypeArray<jobject>,
      jni::alias_ref<JReadableMapBuffer::jhybridobject>,
      jintArray,
      jni::jtypeArray<jobject>,
      jintArray,
      jni::jtypeArray<jobject>,
      jintArray,
      jboolean,
      jni::alias_ref<jobject>)>("loomDirectApplyBatch");

  std::vector<PendingDirectState> states;
  {
    std::lock_guard lock(allocatedViewsMutex_);
    auto pending = pendingDirectStates_.find(surfaceId);
    if (pending != pendingDirectStates_.end()) {
      states = std::move(pending->second);
      pendingDirectStates_.erase(pending);
    }
  }

  std::vector<jint> instructions;
  instructions.reserve(batch.instructionCount);
  auto objects = jni::JArrayClass<jobject>::newArray(batch.objectCount);
  size_t objectIndex = 0;
  LoomPropsEncoder propsEncoder(batch.objectCount);
  const auto opcode = [](LoomOpcode value) {
    return static_cast<jint>(value);
  };
  auto floatBits = [](float value) -> jint {
    return std::bit_cast<jint>(value);
  };
  std::array<jint, LoomMountBatch::PhaseCount + 1> phaseOffsets{};
  for (size_t phase = 0; phase < LoomMountBatch::PhaseCount; ++phase) {
    for (const auto operationIndex : batch.phaseOperationIndexes[phase]) {
      auto& operation = batch.operations[operationIndex];
    std::visit(
        [&](auto& typed) {
          using T = std::decay_t<decltype(typed)>;
          if constexpr (std::is_same_v<T, DirectCreateOperation>) {
            instructions.insert(
                instructions.end(),
                {opcode(LoomOpcode::Create), static_cast<jint>(typed.tag),
                 static_cast<jint>(typed.layoutable)});
            auto name = jni::make_jstring(typed.componentName);
            objects->setElement(objectIndex++, name.get());
            auto props = std::move(typed.props);
            if (!props.isObject()) props = folly::dynamic::object();
            if (propsEncoder.encode(props, objectIndex)) {
              objectIndex++;
            } else {
              auto map = ReadableNativeMap::newObjectCxxArgs(std::move(props));
              objects->setElement(objectIndex++, map.get());
            }
          } else if constexpr (std::is_same_v<T, DirectUpdatePropsOperation>) {
            instructions.insert(
                instructions.end(),
                {opcode(LoomOpcode::UpdateProps),
                 static_cast<jint>(typed.tag)});
            auto props = std::move(typed.props);
            if (!props.isObject()) props = folly::dynamic::object();
            if (propsEncoder.encode(props, objectIndex)) {
              objectIndex++;
            } else {
              auto map = ReadableNativeMap::newObjectCxxArgs(std::move(props));
              objects->setElement(objectIndex++, map.get());
            }
          } else if constexpr (
              std::is_same_v<T, DirectInsertOperation> ||
              std::is_same_v<T, DirectRemoveOperation>) {
            instructions.insert(
                instructions.end(),
                {opcode(std::is_same_v<T, DirectInsertOperation>
                            ? LoomOpcode::Insert
                            : LoomOpcode::Remove),
                 static_cast<jint>(typed.parentTag),
                 static_cast<jint>(typed.tag),
                 static_cast<jint>(typed.index)});
          } else if constexpr (std::is_same_v<T, DirectLayoutOperation>) {
            instructions.insert(
                instructions.end(),
                {opcode(LoomOpcode::Layout), static_cast<jint>(typed.tag),
                 static_cast<jint>(typed.parentTag), typed.displayType,
                 typed.layoutDirection, floatBits(typed.x), floatBits(typed.y),
                 floatBits(typed.width), floatBits(typed.height)});
          } else if constexpr (std::is_same_v<T, DirectDeleteOperation>) {
            instructions.insert(
                instructions.end(),
                {opcode(LoomOpcode::Delete), static_cast<jint>(typed.tag)});
          } else {
            constexpr bool isPadding =
                std::is_same_v<T, DirectPaddingOperation>;
            instructions.push_back(opcode(
                isPadding ? LoomOpcode::Padding
                          : LoomOpcode::OverflowInset));
            instructions.push_back(static_cast<jint>(typed.tag));
            for (const auto inset : typed.insets) {
              instructions.push_back(floatBits(inset));
            }
          }
        },
        operation);
    }
    phaseOffsets[phase + 1] = static_cast<jint>(instructions.size());
  }
  if (instructions.size() != batch.instructionCount ||
      objectIndex != batch.objectCount) {
    throw std::logic_error(
        "LoomMountBatch count disagrees with its encoded operations");
  }

  auto env = jni::Environment::current();
  auto ints = env->NewIntArray(static_cast<jsize>(instructions.size()));
  if (!instructions.empty()) {
    env->SetIntArrayRegion(
        ints, 0, static_cast<jsize>(instructions.size()), instructions.data());
  }
  auto phaseOffsetsArray = env->NewIntArray(phaseOffsets.size());
  env->SetIntArrayRegion(
      phaseOffsetsArray, 0, phaseOffsets.size(), phaseOffsets.data());
  std::vector<int32_t> mapBufferStateTags;
  std::vector<MapBuffer> mapBufferStates;
  std::vector<jint> stateTags;
  std::vector<jni::global_ref<jobject>> stateRefs;
  std::vector<jint> preparedTextTags;
  std::vector<jni::global_ref<jobject>> preparedTextRefs;
  for (auto& state : states) {
    if (state.mapBuffer) {
      mapBufferStateTags.push_back(static_cast<int32_t>(state.tag));
      mapBufferStates.push_back(std::move(*state.mapBuffer));
    } else if (state.preparedTextLayout) {
      preparedTextTags.push_back(static_cast<jint>(state.tag));
      preparedTextRefs.push_back(std::move(state.preparedTextLayout));
    } else if (state.stateWrapper) {
      stateTags.push_back(static_cast<jint>(state.tag));
      stateRefs.push_back(std::move(state.stateWrapper));
    }
  }
  jni::local_ref<JReadableMapBuffer::jhybridobject> mapBufferBatch = nullptr;
  if (!mapBufferStates.empty()) {
    MapBufferBuilder builder(2);
    builder.putIntBuffer(0, mapBufferStateTags);
    builder.putMapBufferList(1, mapBufferStates);
    mapBufferBatch = JReadableMapBuffer::createWithContents(builder.build());
  }
  auto stateWrappers = jni::JArrayClass<jobject>::newArray(stateRefs.size());
  for (size_t index = 0; index < stateRefs.size(); ++index) {
    stateWrappers->setElement(index, stateRefs[index].get());
  }
  auto preparedTextLayouts =
      jni::JArrayClass<jobject>::newArray(preparedTextRefs.size());
  for (size_t index = 0; index < preparedTextRefs.size(); ++index) {
    preparedTextLayouts->setElement(index, preparedTextRefs[index].get());
  }
  auto preparedTextTagsArray =
      env->NewIntArray(static_cast<jsize>(preparedTextTags.size()));
  if (!preparedTextTags.empty()) {
    env->SetIntArrayRegion(
        preparedTextTagsArray,
        0,
        static_cast<jsize>(preparedTextTags.size()),
        preparedTextTags.data());
  }
  auto stateTagsArray = env->NewIntArray(static_cast<jsize>(stateTags.size()));
  if (!stateTags.empty()) {
    env->SetIntArrayRegion(
        stateTagsArray,
        0,
        static_cast<jsize>(stateTags.size()),
        stateTags.data());
  }
  jni::local_ref<jobject> propsTable =
      propsEncoder.empty() ? nullptr : propsEncoder.toJava();
#if LOOM_PROFILE
  const auto profileEncoded = std::chrono::steady_clock::now();
#endif
  try {
    method(
        javaUIManager_,
        static_cast<jint>(surfaceId),
        ints,
        objects.get(),
        mapBufferBatch,
        stateTagsArray,
        stateWrappers.get(),
        preparedTextTagsArray,
        preparedTextLayouts.get(),
        phaseOffsetsArray,
        static_cast<jboolean>(batch.requiresTagReplacement),
        propsTable);
  } catch (const jni::JniException& error) {
    logDirectRendererJavaException("applyBatch", error);
    throw;
  }
#if LOOM_PROFILE
  const auto profileReturned = std::chrono::steady_clock::now();
#endif
#if LOOM_PROFILE && defined(__ANDROID__)
  const auto profileMs = [](auto from, auto to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
  };
  __android_log_print(
      ANDROID_LOG_INFO,
      "LoomMountProfile",
      "surface=%d encode=%.3fms jniJavaCall=%.3fms total=%.3fms "
      "ops=%zu states=%zu preparedText=%zu",
      static_cast<int>(surfaceId),
      profileMs(profileStart, profileEncoded),
      profileMs(profileEncoded, profileReturned),
      profileMs(profileStart, profileReturned),
      batch.operations.size(),
      states.size(),
      preparedTextTags.size());
#endif
}

void FabricMountingManager::directFailSurface(
    SurfaceId surfaceId,
    const std::string& reason) {
  static auto method = JFabricUIManager::javaClassStatic()->getMethod<void(
      jint, jni::alias_ref<jni::JString>)>("loomDirectFailSurface");
  method(
      javaUIManager_,
      static_cast<jint>(surfaceId),
      jni::make_jstring(reason));
}

void FabricMountingManager::directUpdateState(
    const LoomTextStateUpdate& update) {
  const auto surfaceId = update.surfaceId;
  const auto tag = update.tag;
  const auto kind = update.kind;
  const auto& prepared = update.prepared;
  const auto& paragraphAttributes = update.paragraphAttributes;
  // In LOOM_RENDERER mode RCTText is backed by
  // PreparedLayoutTextViewManager. Pass the exact Java PreparedLayout from
  // measurement directly instead of wrapping it in ParagraphState/ConcreteState.
  if (kind == LoomTextStateKind::Text && prepared.valid && prepared.preparedLayout) {
    using PreparedTextLayout = TextLayoutManagerExtended::PreparedTextLayout;
    auto layout = std::static_pointer_cast<PreparedTextLayout>(
        prepared.preparedLayout);
    std::lock_guard lock(allocatedViewsMutex_);
    pendingDirectStates_[surfaceId].push_back(
        {tag, std::nullopt, nullptr, jni::make_global(layout->get())});
    return;
  }
  /*
   * `ParagraphState` carries a weak reference to the text layout manager, but
   * Android never reads it: `ReactTextViewManager.updateState` consumes only
   * the attributed string and the paragraph attributes off the map buffer. The
   * reference is left empty rather than pointing at a manager this layer does
   * not own.
   */
  std::shared_ptr<const State> state;
  const bool isParagraph = kind == LoomTextStateKind::Text ||
      kind == LoomTextStateKind::Paragraph ||
      kind == LoomTextStateKind::SelectableParagraph;
  if (kind == LoomTextStateKind::TextInput) {
    const auto& attributedString = *update.attributedString;
    const auto eventCount = update.mostRecentEventCount;
    state = std::make_shared<const ConcreteState<AndroidTextInputState>>(
        std::make_shared<const AndroidTextInputState>(
            AttributedStringBox(attributedString),
            attributedString,
            paragraphAttributes,
            eventCount),
        ShadowNodeFamily::Weak{});
  } else if (isParagraph) {
    ParagraphState paragraphState{
        *update.attributedString,
        paragraphAttributes,
        std::weak_ptr<const TextLayoutManager>{}};
    if (prepared.valid && prepared.preparedLayout) {
      using PreparedLayout = TextLayoutManagerExtended::PreparedTextLayout;
      auto layout = std::static_pointer_cast<PreparedLayout>(
          prepared.preparedLayout);
      LayoutConstraints constraints;
      constraints.minimumSize = prepared.minimumSize;
      constraints.maximumSize = prepared.maximumSize;
      TextMeasurement measurement;
      measurement.size = prepared.size;
      paragraphState.measuredLayout = MeasuredPreparedTextLayout{
          .layoutConstraints = constraints,
          .measurement = std::move(measurement),
          .preparedTextLayout = *layout};
    }
    state = std::make_shared<const ConcreteState<ParagraphState>>(
        std::make_shared<const ParagraphState>(std::move(paragraphState)),
        ShadowNodeFamily::Weak{});
  } else {
    // Managers such as ScrollView, SafeAreaView, and Modal only need a live
    // StateWrapper so their native callbacks can publish later updates. The
    // Loom renderer cannot synthesize an arbitrary component's private
    // StateData, so keep the generic payload empty and make that boundary
    // explicit in the log instead of pretending it is a concrete state.
    LOG(WARNING) << "LOOM_RENDERER using generic state wrapper for component "
                 << "(kind " << static_cast<int>(kind) << ")"
                 << " on tag " << tag;
    state = std::make_shared<const ConcreteState<DirectGenericStateData>>(
        std::make_shared<const DirectGenericStateData>(), ShadowNodeFamily::Weak{});
  }

  std::lock_guard lock(allocatedViewsMutex_);
  if (isParagraph && !(prepared.valid && prepared.preparedLayout)) {
    pendingDirectStates_[surfaceId].push_back(
        {tag, state->getMapBuffer(), nullptr, nullptr});
  } else {
    auto javaStateWrapper = StateWrapperImpl::newObjectJavaArgs();
    cthis(javaStateWrapper)->setState(state);
    pendingDirectStates_[surfaceId].push_back(
        {tag, std::nullopt, jni::make_global(javaStateWrapper), nullptr});
  }
}

void FabricMountingManager::directUpdateComponentState(
    SurfaceId surfaceId,
    Tag tag,
    const std::shared_ptr<const State>& state) {
  // The component's own state (made by its ComponentDescriptor, with a live
  // family): the view manager's StateWrapper.updateState reaches the Loom
  // renderer through the state pipe, as it reaches the shadow tree in Fabric.
  auto javaStateWrapper = StateWrapperImpl::newObjectJavaArgs();
  cthis(javaStateWrapper)->setState(state);
  std::lock_guard lock(allocatedViewsMutex_);
  pendingDirectStates_[surfaceId].push_back(
      {tag, std::nullopt, jni::make_global(javaStateWrapper), nullptr});
}

std::pair<float, float> FabricMountingManager::directGetScrollOffset(
    SurfaceId surfaceId,
    Tag tag) {
  static auto method = JFabricUIManager::javaClassStatic()->getMethod<jlong(
      jint, jint)>("loomDirectGetScrollOffset");
  const auto packed = method(
      javaUIManager_, static_cast<jint>(surfaceId), static_cast<jint>(tag));
  const auto xBits = static_cast<uint32_t>(static_cast<uint64_t>(packed) >> 32);
  const auto yBits = static_cast<uint32_t>(packed);
  return {std::bit_cast<float>(xBits), std::bit_cast<float>(yBits)};
}

std::optional<std::pair<float, float>>
FabricMountingManager::directGetViewLocationInSurface(
    SurfaceId surfaceId,
    Tag tag) {
  static auto method = JFabricUIManager::javaClassStatic()->getMethod<jlong(
      jint, jint)>("loomDirectGetViewLocationInSurface");
  const auto packed = method(
      javaUIManager_, static_cast<jint>(surfaceId), static_cast<jint>(tag));
  const auto xBits = static_cast<uint32_t>(static_cast<uint64_t>(packed) >> 32);
  const auto yBits = static_cast<uint32_t>(packed);
  const auto x = std::bit_cast<float>(xBits);
  const auto y = std::bit_cast<float>(yBits);
  if (!std::isfinite(x) || !std::isfinite(y)) {
    return std::nullopt;
  }
  return std::pair<float, float>{x, y};
}

void FabricMountingManager::directDispatchCommand(
    SurfaceId surfaceId,
    Tag tag,
    const std::string& commandName,
    const folly::dynamic& args) {
  static auto method = JFabricUIManager::javaClassStatic()->getMethod<void(
      jint, jint, jstring, ReadableArray::javaobject)>(
      "loomDirectDispatchCommand");
  auto command = jni::make_jstring(commandName);
  auto argsArray = jni::adopt_local(
      reinterpret_cast<ReadableArray::javaobject>(
          ReadableNativeArray::newObjectCxxArgs(args).release()));
  method(
      javaUIManager_,
      static_cast<jint>(surfaceId),
      static_cast<jint>(tag),
      command.get(),
      argsArray.get());
}

void FabricMountingManager::directSendAccessibilityEvent(
    SurfaceId surfaceId,
    Tag tag,
    const std::string& eventType) {
  static auto method = JFabricUIManager::javaClassStatic()
      ->getMethod<void(jint, jint, jstring)>(
          "loomDirectSendAccessibilityEvent");
  auto eventTypeString = jni::make_jstring(eventType);
  method(
      javaUIManager_,
      static_cast<jint>(surfaceId),
      static_cast<jint>(tag),
      eventTypeString.get());
}

void FabricMountingManager::directSetIsJSResponder(
    SurfaceId surfaceId,
    Tag tag,
    bool isJSResponder,
    bool blockNativeResponder) {
  static auto method = JFabricUIManager::javaClassStatic()
      ->getMethod<void(jint, jint, jboolean, jboolean)>(
          "loomDirectSetJSResponder");
  method(
      javaUIManager_,
      static_cast<jint>(surfaceId),
      static_cast<jint>(tag),
      static_cast<jboolean>(isJSResponder),
      static_cast<jboolean>(blockNativeResponder));
}

#endif

FabricMountingManager::FabricMountingManager(
    jni::global_ref<JFabricUIManager::javaobject>& javaUIManager)
    : javaUIManager_(javaUIManager) {}

FabricMountingManager::~FabricMountingManager() {
  // Manually reset `javaUIManager_` since FabricMountingManager may be retained
  // from a GC thread (destroyUnmountedShadowNode)
  jni::ThreadScope::WithClassLoader([&]() { javaUIManager_.reset(); });
}

void FabricMountingManager::onSurfaceStart(SurfaceId surfaceId) {
  std::lock_guard lock(allocatedViewsMutex_);
  allocatedViewRegistry_.emplace(
      surfaceId, std::unordered_set<Tag>({surfaceId}));
}

void FabricMountingManager::onSurfaceStop(SurfaceId surfaceId) {
  std::lock_guard lock(allocatedViewsMutex_);
  allocatedViewRegistry_.erase(surfaceId);
#if LOOM_RENDERER
  pendingDirectStates_.erase(surfaceId);
#endif
}

bool FabricMountingManager::isViewAllocated(SurfaceId surfaceId, Tag tag) {
  std::lock_guard lock(allocatedViewsMutex_);
  auto it = allocatedViewRegistry_.find(surfaceId);
  if (it == allocatedViewRegistry_.end()) {
    return false;
  }
  return it->second.count(tag) > 0;
}

namespace {

#ifdef REACT_NATIVE_DEBUG
// List of layout-only props extracted from ViewProps.kt used to filter out
// component props from Props 1.5 to validate the Props 2.0 output
inline bool isLayoutOnlyProp(const std::string& propName) {
  static const std::vector<std::string> layoutOnlyProps = {
      // Flexbox Alignment
      "alignItems",
      "alignSelf",
      "alignContent",

      // Flexbox Properties
      "flex",
      "flexBasis",
      "flexDirection",
      "flexGrow",
      "flexShrink",
      "flexWrap",
      "justifyContent",

      // Gaps
      "rowGap",
      "columnGap",
      "gap",

      // Display & Position
      "display",
      "position",

      // Positioning
      "right",
      "top",
      "bottom",
      "left",
      "start",
      "end",

      // Dimensions
      "width",
      "height",
      "minWidth",
      "maxWidth",
      "minHeight",
      "maxHeight",

      // Margins
      "margin",
      "marginVertical",
      "marginHorizontal",
      "marginLeft",
      "marginRight",
      "marginTop",
      "marginBottom",
      "marginStart",
      "marginEnd",

      // Paddings
      "padding",
      "paddingVertical",
      "paddingHorizontal",
      "paddingLeft",
      "paddingRight",
      "paddingTop",
      "paddingBottom",
      "paddingStart",
      "paddingEnd",

      // Other
      "collapsable",
  };
  return std::find(layoutOnlyProps.begin(), layoutOnlyProps.end(), propName) !=
      layoutOnlyProps.end();
}
#endif
#if !LOOM_RENDERER

inline int getIntBufferSizeForType(CppMountItem::Type mountItemType) {
  switch (mountItemType) {
    case CppMountItem::Type::Create:
      return 2; // tag, isLayoutable
    case CppMountItem::Type::Insert:
    case CppMountItem::Type::Remove:
      return 3; // tag, parentTag, index
    case CppMountItem::Type::Delete:
    case CppMountItem::Type::UpdateProps:
    case CppMountItem::Type::UpdateState:
    case CppMountItem::Type::UpdateEventEmitter:
      return 1; // tag
    case CppMountItem::Type::UpdatePadding:
      return 5; // tag, top, left, bottom, right
    case CppMountItem::Type::UpdateLayout:
      return 8; // tag, parentTag, x, y, w, h, DisplayType, LayoutDirection
    case CppMountItem::Type::UpdateOverflowInset:
      return 5; // tag, left, top, right, bottom
    case CppMountItem::Undefined:
    case CppMountItem::Multiple:
      return -1;
  }
}

inline int getObjectBufferSizeForType(CppMountItem::Type mountItemType) {
  switch (mountItemType) {
    case CppMountItem::Type::Create:
      return 4; // component name, props, state, event emitter
    case CppMountItem::Type::UpdateProps:
      return 1; // props object
    case CppMountItem::Type::UpdateState:
      return 1; // state object
    case CppMountItem::Type::UpdateEventEmitter:
      return 1; // event emitter object
    case CppMountItem::Type::UpdatePadding:
    case CppMountItem::Type::UpdateLayout:
    case CppMountItem::Type::UpdateOverflowInset:
    case CppMountItem::Type::Insert:
    case CppMountItem::Type::Remove:
    case CppMountItem::Type::Delete:
      return 0;
    case CppMountItem::Undefined:
    case CppMountItem::Multiple:
      return -1;
  }
}

inline void updateBufferSizes(
    CppMountItem::Type mountItemType,
    size_t numInstructions,
    int& batchMountItemIntsSize,
    int& batchMountItemObjectsSize) {
  if (numInstructions == 0) {
    return;
  }

  batchMountItemIntsSize +=
      numInstructions == 1 ? 1 : 2; // instructionType[, numInstructions]
  batchMountItemIntsSize +=
      numInstructions * getIntBufferSizeForType(mountItemType);
  batchMountItemObjectsSize +=
      numInstructions * getObjectBufferSizeForType(mountItemType);
}

inline std::pair<int, int> computeBufferSizes(
    std::vector<CppMountItem>& cppCommonMountItems,
    std::vector<CppMountItem>& cppDeleteMountItems,
    std::vector<CppMountItem>& cppUpdatePropsMountItems,
    std::vector<CppMountItem>& cppUpdateStateMountItems,
    std::vector<CppMountItem>& cppUpdatePaddingMountItems,
    std::vector<CppMountItem>& cppUpdateLayoutMountItems,
    std::vector<CppMountItem>& cppUpdateOverflowInsetMountItems,
    std::vector<CppMountItem>& cppUpdateEventEmitterMountItems) {
  int batchMountItemIntsSize = 0;
  int batchMountItemObjectsSize = 0;

  CppMountItem::Type lastType = CppMountItem::Type::Undefined;
  int numSameType = 0;
  for (const auto& mountItem : cppCommonMountItems) {
    const auto& mountItemType = mountItem.type;

    if (lastType == mountItemType) {
      numSameType++;
      if (numSameType == 2) {
        batchMountItemIntsSize += 1; // numInstructions
      }
    } else {
      numSameType = 1;
      lastType = mountItemType;
      batchMountItemIntsSize += 1; // instructionType
    }

    batchMountItemIntsSize += getIntBufferSizeForType(mountItemType);
    batchMountItemObjectsSize += getObjectBufferSizeForType(mountItemType);
  }

  updateBufferSizes(
      CppMountItem::Type::UpdateProps,
      cppUpdatePropsMountItems.size(),
      batchMountItemIntsSize,
      batchMountItemObjectsSize);
  updateBufferSizes(
      CppMountItem::Type::UpdateState,
      cppUpdateStateMountItems.size(),
      batchMountItemIntsSize,
      batchMountItemObjectsSize);
  updateBufferSizes(
      CppMountItem::Type::UpdatePadding,
      cppUpdatePaddingMountItems.size(),
      batchMountItemIntsSize,
      batchMountItemObjectsSize);
  updateBufferSizes(
      CppMountItem::Type::UpdateLayout,
      cppUpdateLayoutMountItems.size(),
      batchMountItemIntsSize,
      batchMountItemObjectsSize);
  updateBufferSizes(
      CppMountItem::Type::UpdateOverflowInset,
      cppUpdateOverflowInsetMountItems.size(),
      batchMountItemIntsSize,
      batchMountItemObjectsSize);
  updateBufferSizes(
      CppMountItem::Type::UpdateEventEmitter,
      cppUpdateEventEmitterMountItems.size(),
      batchMountItemIntsSize,
      batchMountItemObjectsSize);
  updateBufferSizes(
      CppMountItem::Type::Delete,
      cppDeleteMountItems.size(),
      batchMountItemIntsSize,
      batchMountItemObjectsSize);

  return std::make_pair(batchMountItemIntsSize, batchMountItemObjectsSize);
}

#endif // !LOOM_RENDERER
// TODO: this method will be removed when binding for components are code-gen
jni::local_ref<jstring> getPlatformComponentName(const ShadowView& shadowView) {
  constexpr static std::string_view scrollViewComponentName = "ScrollView";

  if (scrollViewComponentName == shadowView.componentName) {
    const auto& newViewProps =
        static_cast<const ScrollViewProps&>(*shadowView.props);
    if (newViewProps.getProbablyMoreHorizontalThanVertical_DEPRECATED()) {
      return jni::make_jstring("AndroidHorizontalScrollView");
    }
  }
  return jni::make_jstring(shadowView.componentName);
}

#if !LOOM_RENDERER
inline float scale(Float value, Float pointScaleFactor) {
  std::feclearexcept(FE_ALL_EXCEPT);
  float result = value * pointScaleFactor;
  if (std::fetestexcept(FE_OVERFLOW) != 0) {
    LOG(ERROR) << "Binding::scale - FE_OVERFLOW - value: " << value
               << " pointScaleFactor: " << pointScaleFactor
               << " result: " << result;
  }
  if (std::fetestexcept(FE_UNDERFLOW) != 0) {
    LOG(ERROR) << "Binding::scale - FE_UNDERFLOW - value: " << value
               << " pointScaleFactor: " << pointScaleFactor
               << " result: " << result;
  }
  return result;
}
#endif

jni::local_ref<jobject> getProps(
    const ShadowView& oldShadowView,
    const ShadowView& newShadowView) {
  // We calculate the diffing between the props of the last mounted ShadowTree
  // and the Props of the latest commited ShadowTree). ONLY for <View>
  // components when the "enablePropsUpdateReconciliationAndroid" feature flag
  // is enabled.
  auto* oldProps = oldShadowView.props.get();
  auto* newProps = newShadowView.props.get();
  if ((ReactNativeFeatureFlags::enablePropsUpdateReconciliationAndroid()) &&
      strcmp(
          newShadowView.componentName,
          newProps->getDiffPropsImplementationTarget()) == 0) {
    auto diff = newProps->getDiffProps(oldProps);

#ifdef REACT_NATIVE_DEBUG
    if (oldProps != nullptr) {
      auto controlDiff =
          diffDynamicProps(oldProps->rawProps, newProps->rawProps);

      for (const auto& [prop, value] : controlDiff.items()) {
        if (diff.count(prop) == 0) {
          // Skip layout-only props since they are not included in Props 2.0
          if (!isLayoutOnlyProp(prop.asString())) {
            LOG(ERROR) << "Props diff validation failed: Props 1.5 has prop '"
                       << prop.asString()
                       << "' = " << (value != nullptr ? value : "NULL")
                       << " that Props 2.0 doesn't have for component "
                       << newShadowView.componentName;
          }
        }
      }
    }
#endif

    return ReadableNativeMap::newObjectCxxArgs(std::move(diff));
  }
  if (ReactNativeFeatureFlags::enableAccumulatedUpdatesInRawPropsAndroid()) {
    if (oldProps == nullptr) {
      return ReadableNativeMap::newObjectCxxArgs(newProps->rawProps);
    } else {
      return ReadableNativeMap::newObjectCxxArgs(
          diffDynamicProps(oldProps->rawProps, newProps->rawProps));
    }
  }
  return ReadableNativeMap::newObjectCxxArgs(newProps->rawProps);
}

#if !LOOM_RENDERER
struct InstructionBuffer {
  JNIEnv* env;
  jintArray ints;
  jni::local_ref<jni::JArrayClass<jobject>> objects;

  int intsPosition = 0;
  int objectsPosition = 0;

  inline void writeInt(int value) {
    env->SetIntArrayRegion(ints, intsPosition, 1, &value);
    intsPosition += 1;
  }

  template <size_t N>
  inline void writeIntArray(const std::array<int, N>& buffer) {
    env->SetIntArrayRegion(ints, intsPosition, N, buffer.data());
    intsPosition += N;
  }

  inline void writeObject(jobject obj) {
    objects->setElement(objectsPosition, obj);
    objectsPosition += 1;
  }

  template <size_t N>
  inline void writeObjectsArray(const std::array<jobject, N>& buffer) {
    for (size_t i = 0; i < N; i++) {
      objects->setElement(objectsPosition + i, buffer[i]);
    }
    objectsPosition += N;
  }
};

inline void writeMountItemPreamble(
    InstructionBuffer& buffer,
    int mountItemType,
    size_t numItems) {
  if (numItems == 1) {
    buffer.writeInt(mountItemType);
  } else {
    buffer.writeIntArray(
        std::array<int, 2>{
            mountItemType | CppMountItem::Type::Multiple,
            static_cast<int>(numItems)});
  }
}

inline void writeCreateMountItem(
    InstructionBuffer& buffer,
    const CppMountItem& mountItem) {
  int isLayoutable =
      mountItem.newChildShadowView.layoutMetrics != EmptyLayoutMetrics ? 1 : 0;
  buffer.writeIntArray(
      std::array<int, 2>{mountItem.newChildShadowView.tag, isLayoutable});

  auto componentName = getPlatformComponentName(mountItem.newChildShadowView);

  jni::local_ref<jobject> props =
      getProps(mountItem.oldChildShadowView, mountItem.newChildShadowView);

  // Do not hold onto Java object from C
  // We DO want to hold onto C object from Java, since we don't know the
  // lifetime of the Java object
  jni::local_ref<StateWrapperImpl::JavaPart> javaStateWrapper = nullptr;
  if (mountItem.newChildShadowView.state != nullptr) {
    javaStateWrapper = StateWrapperImpl::newObjectJavaArgs();
    StateWrapperImpl* cStateWrapper = cthis(javaStateWrapper);
    cStateWrapper->setState(mountItem.newChildShadowView.state);
  }

  // Do not hold a reference to javaEventEmitter from the C++ side.
  auto javaEventEmitter = EventEmitterWrapper::newObjectCxxArgs(
      mountItem.newChildShadowView.eventEmitter);

  buffer.writeObjectsArray(
      std::array<jobject, 4>{
          componentName.get(),
          props.get(),
          javaStateWrapper != nullptr ? javaStateWrapper.get() : nullptr,
          javaEventEmitter.get()});
}

inline void writeDeleteMountItem(
    InstructionBuffer& buffer,
    const CppMountItem& mountItem) {
  buffer.writeInt(mountItem.oldChildShadowView.tag);
}

inline void writeInsertMountItem(
    InstructionBuffer& buffer,
    const CppMountItem& mountItem) {
  buffer.writeIntArray(
      std::array<int, 3>{
          mountItem.newChildShadowView.tag,
          mountItem.parentTag,
          mountItem.index});
}

inline void writeRemoveMountItem(
    InstructionBuffer& buffer,
    const CppMountItem& mountItem) {
  buffer.writeIntArray(
      std::array<int, 3>{
          mountItem.oldChildShadowView.tag,
          mountItem.parentTag,
          mountItem.index});
}

inline void writeUpdatePropsMountItem(
    InstructionBuffer& buffer,
    const CppMountItem& mountItem) {
  buffer.writeInt(mountItem.newChildShadowView.tag);
  buffer.writeObject(
      getProps(mountItem.oldChildShadowView, mountItem.newChildShadowView)
          .get());
}

inline void writeUpdateStateMountItem(
    InstructionBuffer& buffer,
    const CppMountItem& mountItem) {
  buffer.writeInt(mountItem.newChildShadowView.tag);

  auto state = mountItem.newChildShadowView.state;
  // Do not hold onto Java object from C
  // We DO want to hold onto C object from Java, since we don't know the
  // lifetime of the Java object
  jni::local_ref<StateWrapperImpl::JavaPart> javaStateWrapper = nullptr;
  if (state != nullptr) {
    javaStateWrapper = StateWrapperImpl::newObjectJavaArgs();
    StateWrapperImpl* cStateWrapper = cthis(javaStateWrapper);
    cStateWrapper->setState(state);
  }

  buffer.writeObject(
      javaStateWrapper != nullptr ? javaStateWrapper.get() : nullptr);
}

inline void writeUpdateLayoutMountItem(
    InstructionBuffer& buffer,
    const CppMountItem& mountItem) {
  const auto& layoutMetrics = mountItem.newChildShadowView.layoutMetrics;
  auto pointScaleFactor = layoutMetrics.pointScaleFactor;
  auto frame = layoutMetrics.frame;

  int x = round(scale(frame.origin.x, pointScaleFactor));
  int y = round(scale(frame.origin.y, pointScaleFactor));
  int w = round(scale(frame.size.width, pointScaleFactor));
  int h = round(scale(frame.size.height, pointScaleFactor));

  buffer.writeIntArray(
      std::array<int, 8>{
          mountItem.newChildShadowView.tag,
          mountItem.parentTag,
          x,
          y,
          w,
          h,
          toInt(layoutMetrics.displayType),
          toInt(layoutMetrics.layoutDirection)});
}

inline void writeUpdateEventEmitterMountItem(
    InstructionBuffer& buffer,
    const CppMountItem& mountItem) {
  buffer.writeInt(mountItem.newChildShadowView.tag);

  // Do not hold a reference to javaEventEmitter from the C++ side.
  auto javaEventEmitter = EventEmitterWrapper::newObjectCxxArgs(
      mountItem.newChildShadowView.eventEmitter);
  buffer.writeObject(javaEventEmitter.get());
}

inline void writeUpdatePaddingMountItem(
    InstructionBuffer& buffer,
    const CppMountItem& mountItem) {
  auto layoutMetrics = mountItem.newChildShadowView.layoutMetrics;
  auto pointScaleFactor = layoutMetrics.pointScaleFactor;
  auto contentInsets = layoutMetrics.contentInsets;

  int insetLeft = floor(scale(contentInsets.left, pointScaleFactor));
  int insetTop = floor(scale(contentInsets.top, pointScaleFactor));
  int insetRight = floor(scale(contentInsets.right, pointScaleFactor));
  int insetBottom = floor(scale(contentInsets.bottom, pointScaleFactor));

  buffer.writeIntArray(
      std::array<int, 5>{
          mountItem.newChildShadowView.tag,
          insetLeft,
          insetTop,
          insetRight,
          insetBottom});
}

inline void writeUpdateOverflowInsetMountItem(
    InstructionBuffer& buffer,
    const CppMountItem& mountItem) {
  auto layoutMetrics = mountItem.newChildShadowView.layoutMetrics;
  auto pointScaleFactor = layoutMetrics.pointScaleFactor;
  auto overflowInset = layoutMetrics.overflowInset;

  int overflowInsetLeft = round(scale(overflowInset.left, pointScaleFactor));
  int overflowInsetTop = round(scale(overflowInset.top, pointScaleFactor));
  int overflowInsetRight = round(scale(overflowInset.right, pointScaleFactor));
  int overflowInsetBottom =
      round(scale(overflowInset.bottom, pointScaleFactor));

  buffer.writeIntArray(
      std::array<int, 5>{
          mountItem.newChildShadowView.tag,
          overflowInsetLeft,
          overflowInsetTop,
          overflowInsetRight,
          overflowInsetBottom});
}

#endif // !LOOM_RENDERER
} // namespace

#if LOOM_RENDERER
// Loom mounts through its renderer (directApplyBatch); no shadow-tree transaction ever reaches here.
void FabricMountingManager::executeMount(
    const MountingTransaction& /*transaction*/) {}
#else
void FabricMountingManager::executeMount(
    const MountingTransaction& transaction) {
  TraceSection section("FabricMountingManager::executeMount");

  std::scoped_lock lock(commitMutex_);
  auto finishTransactionStartTime = telemetryTimePointNow();

  auto env = jni::Environment::current();

  const auto& telemetry = transaction.getTelemetry();
  auto surfaceId = transaction.getSurfaceId();
  auto& mutations = transaction.getMutations();

  bool maintainMutationOrder =
      ReactNativeFeatureFlags::disableMountItemReorderingAndroid();

  auto revisionNumber = telemetry.getRevisionNumber();

  std::vector<CppMountItem> cppCommonMountItems;
  std::vector<CppMountItem> cppDeleteMountItems;
  std::vector<CppMountItem> cppUpdatePropsMountItems;
  std::vector<CppMountItem> cppUpdateStateMountItems;
  std::vector<CppMountItem> cppUpdatePaddingMountItems;
  std::vector<CppMountItem> cppUpdateLayoutMountItems;
  std::vector<CppMountItem> cppUpdateOverflowInsetMountItems;
  std::vector<CppMountItem> cppUpdateEventEmitterMountItems;

  {
    std::lock_guard allocatedViewsLock(allocatedViewsMutex_);

    auto allocatedViewsIterator = allocatedViewRegistry_.find(surfaceId);
    auto defaultAllocatedViews = std::unordered_set<Tag>{};
    // Do not remove `defaultAllocatedViews` or initialize
    // `std::unordered_set<Tag>{}` inline in below ternary expression - if falsy
    // operand is a value type, the compiler will decide the expression to be a
    // value type, an unnecessary (sometimes expensive) copy will happen as a
    // result.
    auto& allocatedViewTags =
        allocatedViewsIterator != allocatedViewRegistry_.end()
        ? allocatedViewsIterator->second
        : defaultAllocatedViews;
    if (allocatedViewsIterator == allocatedViewRegistry_.end()) {
      LOG(ERROR) << "Executing commit after surface " << surfaceId
                 << " was stopped!";
    }

    for (const auto& mutation : mutations) {
      auto parentTag = mutation.parentTag;
      const auto& oldChildShadowView = mutation.oldChildShadowView;
      const auto& newChildShadowView = mutation.newChildShadowView;
      auto& mutationType = mutation.type;
      auto& index = mutation.index;

      bool isVirtual = mutation.mutatedViewIsVirtual();
      switch (mutationType) {
        case ShadowViewMutation::Create: {
          bool shouldCreateView =
              !allocatedViewTags.contains(newChildShadowView.tag);

          if (shouldCreateView) {
            cppCommonMountItems.push_back(
                CppMountItem::CreateMountItem(newChildShadowView));
            allocatedViewTags.insert(newChildShadowView.tag);
          }
          break;
        }
        case ShadowViewMutation::Remove: {
          if (!isVirtual) {
            cppCommonMountItems.push_back(
                CppMountItem::RemoveMountItem(
                    parentTag, oldChildShadowView, index));
          }
          break;
        }
        case ShadowViewMutation::Delete: {
          (maintainMutationOrder ? cppCommonMountItems : cppDeleteMountItems)
              .push_back(CppMountItem::DeleteMountItem(oldChildShadowView));
          if (allocatedViewTags.erase(oldChildShadowView.tag) != 1) {
            LOG(ERROR) << "Emitting delete for unallocated view "
                       << oldChildShadowView.tag;
          }
          break;
        }
        case ShadowViewMutation::Update: {
          if (!isVirtual) {
            if (!allocatedViewTags.contains(newChildShadowView.tag)) {
              LOG(ERROR) << "Emitting update for unallocated view "
                         << newChildShadowView.tag;
            }

            if (oldChildShadowView.props != newChildShadowView.props) {
              (maintainMutationOrder ? cppCommonMountItems
                                     : cppUpdatePropsMountItems)
                  .push_back(
                      CppMountItem::UpdatePropsMountItem(
                          oldChildShadowView, newChildShadowView));
            }
            if (oldChildShadowView.state != newChildShadowView.state) {
              (maintainMutationOrder ? cppCommonMountItems
                                     : cppUpdateStateMountItems)
                  .push_back(
                      CppMountItem::UpdateStateMountItem(newChildShadowView));
            }

            // Padding: padding mountItems must be executed before layout props
            // are updated in the view. This is necessary to ensure that events
            // (resulting from layout changes) are dispatched with the correct
            // padding information.
            if (oldChildShadowView.layoutMetrics.contentInsets !=
                newChildShadowView.layoutMetrics.contentInsets) {
              (maintainMutationOrder ? cppCommonMountItems
                                     : cppUpdatePaddingMountItems)
                  .push_back(
                      CppMountItem::UpdatePaddingMountItem(newChildShadowView));
            }

            if (oldChildShadowView.layoutMetrics !=
                newChildShadowView.layoutMetrics) {
              (maintainMutationOrder ? cppCommonMountItems
                                     : cppUpdateLayoutMountItems)
                  .push_back(
                      CppMountItem::UpdateLayoutMountItem(
                          mutation.newChildShadowView, parentTag));
            }

            // OverflowInset: This is the values indicating boundaries including
            // children of the current view. The layout of current view may not
            // change, and we separate this part from layout mount items to not
            // pack too much data there.
            if ((oldChildShadowView.layoutMetrics.overflowInset !=
                 newChildShadowView.layoutMetrics.overflowInset)) {
              (maintainMutationOrder ? cppCommonMountItems
                                     : cppUpdateOverflowInsetMountItems)
                  .push_back(
                      CppMountItem::UpdateOverflowInsetMountItem(
                          newChildShadowView));
            }
          }

          if (oldChildShadowView.eventEmitter !=
              newChildShadowView.eventEmitter) {
            (maintainMutationOrder ? cppCommonMountItems
                                   : cppUpdatePropsMountItems)
                .push_back(
                    CppMountItem::UpdateEventEmitterMountItem(
                        mutation.newChildShadowView));
          }
          break;
        }
        case ShadowViewMutation::Insert: {
          if (!isVirtual) {
            // Insert item
            cppCommonMountItems.push_back(
                CppMountItem::InsertMountItem(
                    parentTag, newChildShadowView, index));

            bool shouldCreateView =
                !allocatedViewTags.contains(newChildShadowView.tag);
            if (ReactNativeFeatureFlags::
                    enableAccumulatedUpdatesInRawPropsAndroid()) {
              if (shouldCreateView) {
                LOG(ERROR) << "Emitting insert for unallocated view "
                           << newChildShadowView.tag;
              }
              (maintainMutationOrder ? cppCommonMountItems
                                     : cppUpdatePropsMountItems)
                  .push_back(
                      CppMountItem::UpdatePropsMountItem(
                          {}, newChildShadowView));
            } else {
              if (shouldCreateView) {
                LOG(ERROR) << "Emitting insert for unallocated view "
                           << newChildShadowView.tag;
                (maintainMutationOrder ? cppCommonMountItems
                                       : cppUpdatePropsMountItems)
                    .push_back(
                        CppMountItem::UpdatePropsMountItem(
                            {}, newChildShadowView));
              }
            }

            // State
            if (newChildShadowView.state) {
              (maintainMutationOrder ? cppCommonMountItems
                                     : cppUpdateStateMountItems)
                  .push_back(
                      CppMountItem::UpdateStateMountItem(newChildShadowView));
            }

            // Padding: padding mountItems must be executed before layout props
            // are updated in the view. This is necessary to ensure that events
            // (resulting from layout changes) are dispatched with the correct
            // padding information.
            if (newChildShadowView.layoutMetrics.contentInsets !=
                EdgeInsets::ZERO) {
              (maintainMutationOrder ? cppCommonMountItems
                                     : cppUpdatePaddingMountItems)
                  .push_back(
                      CppMountItem::UpdatePaddingMountItem(newChildShadowView));
            }

            // Layout
            (maintainMutationOrder ? cppCommonMountItems
                                   : cppUpdateLayoutMountItems)
                .push_back(
                    CppMountItem::UpdateLayoutMountItem(
                        newChildShadowView, parentTag));

            // OverflowInset: This is the values indicating boundaries including
            // children of the current view. The layout of current view may not
            // change, and we separate this part from layout mount items to not
            // pack too much data there.
            if (newChildShadowView.layoutMetrics.overflowInset !=
                EdgeInsets::ZERO) {
              (maintainMutationOrder ? cppCommonMountItems
                                     : cppUpdateOverflowInsetMountItems)
                  .push_back(
                      CppMountItem::UpdateOverflowInsetMountItem(
                          newChildShadowView));
            }
          }

          // EventEmitter
          // On insert we always update the event emitter, as we do not pass
          // it in when preallocating views
          (maintainMutationOrder ? cppCommonMountItems
                                 : cppUpdateEventEmitterMountItems)
              .push_back(
                  CppMountItem::UpdateEventEmitterMountItem(
                      mutation.newChildShadowView));

          break;
        }
        default: {
          break;
        }
      }
    }
  }

  // We now have all the information we need, including ordering of mount items,
  // to know exactly how much space must be allocated
  auto [batchMountItemIntsSize, batchMountItemObjectsSize] = computeBufferSizes(
      cppCommonMountItems,
      cppDeleteMountItems,
      cppUpdatePropsMountItems,
      cppUpdateStateMountItems,
      cppUpdatePaddingMountItems,
      cppUpdateLayoutMountItems,
      cppUpdateOverflowInsetMountItems,
      cppUpdateEventEmitterMountItems);

  static auto scheduleMountItem = JFabricUIManager::javaClassStatic()
                                      ->getMethod<void(
                                          JMountItem::javaobject,
                                          jint,
                                          jlong,
                                          jlong,
                                          jlong,
                                          jlong,
                                          jlong,
                                          jlong,
                                          jlong,
                                          jint)>("scheduleMountItem");

  if (batchMountItemIntsSize == 0) {
    auto finishTransactionEndTime = telemetryTimePointNow();
    scheduleMountItem(
        javaUIManager_,
        nullptr,
        telemetry.getRevisionNumber(),
        telemetryTimePointToMilliseconds(telemetry.getCommitStartTime()),
        telemetryTimePointToMilliseconds(telemetry.getDiffStartTime()),
        telemetryTimePointToMilliseconds(telemetry.getDiffEndTime()),
        telemetryTimePointToMilliseconds(telemetry.getLayoutStartTime()),
        telemetryTimePointToMilliseconds(telemetry.getLayoutEndTime()),
        telemetryTimePointToMilliseconds(finishTransactionStartTime),
        telemetryTimePointToMilliseconds(finishTransactionEndTime),
        telemetry.getAffectedLayoutNodesCount());
    return;
  }

  // Allocate the intBuffer and object array, now that we know exact sizes
  // necessary
  InstructionBuffer buffer = {
      .env = env,
      .ints = env->NewIntArray(batchMountItemIntsSize),
      .objects = jni::JArrayClass<jobject>::newArray(batchMountItemObjectsSize),
  };

  // Fill in arrays
  int prevMountItemType = -1;

  // Fill in CREATE instructions.
  for (int i = 0; i < cppCommonMountItems.size(); i++) {
    const auto& mountItem = cppCommonMountItems[i];
    const auto& mountItemType = mountItem.type;

    // Get type here, and count forward how many items of this type are in a
    // row. Write preamble to any common type here.
    if (prevMountItemType != mountItemType) {
      int numSameItemTypes = 1;
      for (int j = i + 1; j < cppCommonMountItems.size() &&
           cppCommonMountItems[j].type == mountItemType;
           j++) {
        numSameItemTypes++;
      }

      writeMountItemPreamble(buffer, mountItemType, numSameItemTypes);
      prevMountItemType = mountItemType;
    }

    switch (mountItemType) {
      case CppMountItem::Type::Create:
        writeCreateMountItem(buffer, mountItem);
        break;
      case CppMountItem::Type::Delete:
        writeDeleteMountItem(buffer, mountItem);
        break;
      case CppMountItem::Type::Insert:
        writeInsertMountItem(buffer, mountItem);
        break;
      case CppMountItem::Type::Remove:
        writeRemoveMountItem(buffer, mountItem);
        break;
      case CppMountItem::Type::UpdateProps:
        writeUpdatePropsMountItem(buffer, mountItem);
        break;
      case CppMountItem::Type::UpdateState:
        writeUpdateStateMountItem(buffer, mountItem);
        break;
      case CppMountItem::Type::UpdateLayout:
        writeUpdateLayoutMountItem(buffer, mountItem);
        break;
      case CppMountItem::Type::UpdateEventEmitter:
        writeUpdateEventEmitterMountItem(buffer, mountItem);
        break;
      case CppMountItem::Type::UpdatePadding:
        writeUpdatePaddingMountItem(buffer, mountItem);
        break;
      case CppMountItem::Type::UpdateOverflowInset:
        writeUpdateOverflowInsetMountItem(buffer, mountItem);
        break;
      default:
        LOG(FATAL) << "Unexpected CppMountItem type: " << mountItemType;
    }
  }

  if (!cppUpdatePropsMountItems.empty()) {
    writeMountItemPreamble(
        buffer,
        CppMountItem::Type::UpdateProps,
        cppUpdatePropsMountItems.size());
    for (const auto& mountItem : cppUpdatePropsMountItems) {
      writeUpdatePropsMountItem(buffer, mountItem);
    }
  }
  if (!cppUpdateStateMountItems.empty()) {
    writeMountItemPreamble(
        buffer,
        CppMountItem::Type::UpdateState,
        cppUpdateStateMountItems.size());
    for (const auto& mountItem : cppUpdateStateMountItems) {
      writeUpdateStateMountItem(buffer, mountItem);
    }
  }
  if (!cppUpdatePaddingMountItems.empty()) {
    writeMountItemPreamble(
        buffer,
        CppMountItem::Type::UpdatePadding,
        cppUpdatePaddingMountItems.size());
    for (const auto& mountItem : cppUpdatePaddingMountItems) {
      writeUpdatePaddingMountItem(buffer, mountItem);
    }
  }
  if (!cppUpdateLayoutMountItems.empty()) {
    writeMountItemPreamble(
        buffer,
        CppMountItem::Type::UpdateLayout,
        cppUpdateLayoutMountItems.size());
    for (const auto& mountItem : cppUpdateLayoutMountItems) {
      writeUpdateLayoutMountItem(buffer, mountItem);
    }
  }
  if (!cppUpdateOverflowInsetMountItems.empty()) {
    writeMountItemPreamble(
        buffer,
        CppMountItem::Type::UpdateOverflowInset,
        cppUpdateOverflowInsetMountItems.size());
    for (const auto& mountItem : cppUpdateOverflowInsetMountItems) {
      writeUpdateOverflowInsetMountItem(buffer, mountItem);
    }
  }
  if (!cppUpdateEventEmitterMountItems.empty()) {
    writeMountItemPreamble(
        buffer,
        CppMountItem::Type::UpdateEventEmitter,
        cppUpdateEventEmitterMountItems.size());
    for (const auto& mountItem : cppUpdateEventEmitterMountItems) {
      writeUpdateEventEmitterMountItem(buffer, mountItem);
    }
  }

  // Write deletes last - so that all prop updates, etc, for the tag in the same
  // batch don't fail. Without additional machinery, moving deletes here
  // requires that the differ never produces "DELETE...CREATE" in that order for
  // the same tag. It's nice to be able to batch all similar operations together
  // for space efficiency.
  // FIXME: this optimization is incorrect when multiple transactions are
  // merged together
  if (!cppDeleteMountItems.empty()) {
    writeMountItemPreamble(
        buffer, CppMountItem::Type::Delete, cppDeleteMountItems.size());
    for (const auto& mountItem : cppDeleteMountItems) {
      writeDeleteMountItem(buffer, mountItem);
    }
  }

  static auto createMountItemsIntBufferBatchContainer =
      JFabricUIManager::javaClassStatic()
          ->getMethod<jni::alias_ref<JMountItem>(
              jint, jintArray, jni::jtypeArray<jobject>, jint)>(
              "createIntBufferBatchMountItem");
  auto batch = createMountItemsIntBufferBatchContainer(
      javaUIManager_,
      surfaceId,
      // If there are no items, we pass a nullptr instead of passing the
      // object through the JNI
      batchMountItemIntsSize > 0 ? buffer.ints : nullptr,
      batchMountItemObjectsSize > 0 ? buffer.objects.get() : nullptr,
      revisionNumber);

  auto finishTransactionEndTime = telemetryTimePointNow();

  scheduleMountItem(
      javaUIManager_,
      batch.get(),
      telemetry.getRevisionNumber(),
      telemetryTimePointToMilliseconds(telemetry.getCommitStartTime()),
      telemetryTimePointToMilliseconds(telemetry.getDiffStartTime()),
      telemetryTimePointToMilliseconds(telemetry.getDiffEndTime()),
      telemetryTimePointToMilliseconds(telemetry.getLayoutStartTime()),
      telemetryTimePointToMilliseconds(telemetry.getLayoutEndTime()),
      telemetryTimePointToMilliseconds(finishTransactionStartTime),
      telemetryTimePointToMilliseconds(finishTransactionEndTime),
      telemetry.getAffectedLayoutNodesCount());

  env->DeleteLocalRef(buffer.ints);
}
#endif // LOOM_RENDERER

void FabricMountingManager::drainPreallocateViewsQueue() {
  std::vector<ShadowView> shadowViews;

  {
    std::lock_guard lock(preallocateMutex_);
    std::swap(shadowViews, preallocatedViewsQueue_);
  }

  for (const auto& shadowView : shadowViews) {
    preallocateShadowView(shadowView);
  }
}

void FabricMountingManager::destroyUnmountedShadowNode(
    const ShadowNodeFamily& family) {
  auto tag = family.getTag();
  auto surfaceId = family.getSurfaceId();

  // Remove from allocatedViewRegistry so that executeMount does not skip
  // the Create mount item for this tag. Without this, if the view was
  // preallocated and then destroyed (e.g. due to a superseded concurrent
  // render), executeMount would skip the Create because allocatedViewTags
  // still contains the tag, but the Java side no longer has the view in
  // tagToViewState (it was deleted by destroyUnmountedView below).
  {
    std::lock_guard allocatedViewsLock(allocatedViewsMutex_);
    auto allocatedViewsIterator = allocatedViewRegistry_.find(surfaceId);
    if (allocatedViewsIterator != allocatedViewRegistry_.end()) {
      allocatedViewsIterator->second.erase(tag);
    }
  }

  // ThreadScope::WithClassLoader is necessary because
  // destroyUnmountedShadowNode is being called from a destructor thread
  jni::ThreadScope::WithClassLoader([&]() {
    static auto destroyUnmountedView =
        JFabricUIManager::javaClassStatic()->getMethod<void(jint, jint)>(
            "destroyUnmountedView");
    destroyUnmountedView(javaUIManager_, surfaceId, tag);
  });
}

bool FabricMountingManager::maybePreallocateShadowNode(
    const ShadowNode& shadowNode) {
  if (!shadowNode.getTraits().check(ShadowNodeTraits::Trait::FormsView)) {
    return false;
  }

  static thread_local bool onMainThread = isOnMainThread();
  if (onMainThread) {
    // View preallocation is not beneficial when rendering on the main thread
    return false;
  }

  auto shadowView = ShadowView(shadowNode);

  {
    std::lock_guard lock(preallocateMutex_);
    preallocatedViewsQueue_.push_back(std::move(shadowView));
  }
  return true;
}

void FabricMountingManager::preallocateShadowView(
    const ShadowView& shadowView) {
  TraceSection section("FabricMountingManager::preallocateShadowView");

  {
    std::lock_guard lock(allocatedViewsMutex_);
    auto allocatedViewsIterator =
        allocatedViewRegistry_.find(shadowView.surfaceId);
    if (allocatedViewsIterator == allocatedViewRegistry_.end()) {
      return;
    }
    const auto [_, inserted] =
        allocatedViewsIterator->second.insert(shadowView.tag);
    if (!inserted) {
      return;
    }
  }

  bool isLayoutableShadowNode = shadowView.layoutMetrics != EmptyLayoutMetrics;

  static auto preallocateView =
      JFabricUIManager::javaClassStatic()
          ->getMethod<void(jint, jint, jstring, jobject, jobject, jboolean)>(
              "preallocateView");

  // Do not hold onto Java object from C
  // We DO want to hold onto C object from Java, since we don't know the
  // lifetime of the Java object
  jni::local_ref<StateWrapperImpl::JavaPart> javaStateWrapper = nullptr;

  // Paragraph only has a dummy state during view preallocation.
  // Updating state on Android side has a cost and doing it unnecessarily for
  // dummy state is wasteful.
  bool preventPassingStateWrapperForText =
      strcmp(shadowView.componentName, "Paragraph") == 0;
  if (shadowView.state != nullptr && !preventPassingStateWrapperForText) {
    javaStateWrapper = StateWrapperImpl::newObjectJavaArgs();
    StateWrapperImpl* cStateWrapper = cthis(javaStateWrapper);
    cStateWrapper->setState(shadowView.state);
  }

  jni::local_ref<jobject> props = getProps({}, shadowView);

  auto component = getPlatformComponentName(shadowView);

  preallocateView(
      javaUIManager_,
      shadowView.surfaceId,
      shadowView.tag,
      component.get(),
      props.get(),
      (javaStateWrapper != nullptr ? javaStateWrapper.get() : nullptr),
      static_cast<unsigned char>(isLayoutableShadowNode));
}

#if LOOM_RENDERER
bool FabricMountingManager::directCanMeasureTextOnWorker() {
  static auto method = JFabricUIManager::javaClassStatic()->getMethod<jboolean()>(
      "loomDirectCanMeasureTextOnWorker");
  return method(javaUIManager_) != 0u;
}
#endif

bool FabricMountingManager::isOnMainThread() {
  static auto isOnMainThread =
      JFabricUIManager::javaClassStatic()->getMethod<jboolean()>(
          "isOnMainThread");
  return isOnMainThread(javaUIManager_) != 0u;
}

void FabricMountingManager::dispatchCommand(
    const ShadowView& shadowView,
    const std::string& commandName,
    const folly::dynamic& args) {
  dispatchCommand(shadowView.surfaceId, shadowView.tag, commandName, args);
}

void FabricMountingManager::sendAccessibilityEvent(
    const ShadowView& shadowView,
    const std::string& eventType) {
  sendAccessibilityEvent(shadowView.surfaceId, shadowView.tag, eventType);
}

void FabricMountingManager::setIsJSResponder(
    const ShadowView& shadowView,
    bool isJSResponder,
    bool blockNativeResponder) {
  setIsJSResponder(
      shadowView.surfaceId, shadowView.tag, isJSResponder, blockNativeResponder);
}

void FabricMountingManager::dispatchCommand(
    SurfaceId surfaceId,
    Tag tag,
    const std::string& commandName,
    const folly::dynamic& args) {
#if LOOM_RENDERER
  directDispatchCommand(surfaceId, tag, commandName, args);
  return;
#else
  static auto dispatchCommand =
      JFabricUIManager::javaClassStatic()
          ->getMethod<void(jint, jint, jstring, ReadableArray::javaobject)>(
              "dispatchCommand");
  auto command = jni::make_jstring(commandName);
  auto argsArray = jni::adopt_local(
      reinterpret_cast<ReadableArray::javaobject>(
          ReadableNativeArray::newObjectCxxArgs(args).release()));
  dispatchCommand(
      javaUIManager_, surfaceId, tag, command.get(), argsArray.get());
#endif
}

void FabricMountingManager::sendAccessibilityEvent(
    SurfaceId surfaceId,
    Tag tag,
    const std::string& eventType) {
#if LOOM_RENDERER
  directSendAccessibilityEvent(surfaceId, tag, eventType);
  return;
#else
  static auto sendAccessibilityEventFromJS =
      JFabricUIManager::javaClassStatic()->getMethod<void(jint, jint, jstring)>(
          "sendAccessibilityEventFromJS");

  auto eventTypeStr = jni::make_jstring(eventType);
  sendAccessibilityEventFromJS(
      javaUIManager_, surfaceId, tag, eventTypeStr.get());
#endif
}

void FabricMountingManager::setIsJSResponder(
    SurfaceId surfaceId,
    Tag tag,
    bool isJSResponder,
    bool blockNativeResponder) {
#if LOOM_RENDERER
  directSetIsJSResponder(
      surfaceId, tag, isJSResponder, blockNativeResponder);
  return;
#else
  static auto setJSResponder =
      JFabricUIManager::javaClassStatic()
          ->getMethod<void(jint, jint, jint, jboolean)>("setJSResponder");

  static auto clearJSResponder =
      JFabricUIManager::javaClassStatic()->getMethod<void()>(
          "clearJSResponder");

  if (isJSResponder) {
    setJSResponder(
        javaUIManager_,
        surfaceId,
        tag,
        // The closest non-flattened ancestor of the same value if the node is
        // not flattened. For now, we don't support the case when the node can
        // be flattened because the only component that uses this feature -
        // ScrollView - cannot be flattened.
        tag,
        (jboolean)blockNativeResponder);
  } else {
    clearJSResponder(javaUIManager_);
  }
#endif
}

void FabricMountingManager::onAnimationStarted() {
  static auto layoutAnimationsStartedJNI =
      JFabricUIManager::javaClassStatic()->getMethod<void()>(
          "onAnimationStarted");

  layoutAnimationsStartedJNI(javaUIManager_);
}

void FabricMountingManager::onAllAnimationsComplete() {
  static auto allAnimationsCompleteJNI =
      JFabricUIManager::javaClassStatic()->getMethod<void()>(
          "onAllAnimationsComplete");

  allAnimationsCompleteJNI(javaUIManager_);
}

void FabricMountingManager::synchronouslyUpdateViewOnUIThread(
    Tag viewTag,
    const folly::dynamic& props) {
  static auto synchronouslyUpdateViewOnUIThreadJNI =
      JFabricUIManager::javaClassStatic()
          ->getMethod<void(jint, ReadableMap::javaobject)>(
              "synchronouslyUpdateViewOnUIThread");
  auto propsMap = reinterpret_cast<ReadableMap::javaobject>(
      ReadableNativeMap::newObjectCxxArgs(props).release());
  synchronouslyUpdateViewOnUIThreadJNI(javaUIManager_, viewTag, propsMap);
}

void FabricMountingManager::captureViewSnapshot(Tag tag, SurfaceId surfaceId) {
  static auto captureViewSnapshotJNI =
      JFabricUIManager::javaClassStatic()->getMethod<void(jint, jint)>(
          "captureViewSnapshot");
  captureViewSnapshotJNI(javaUIManager_, tag, surfaceId);
}

void FabricMountingManager::setViewSnapshot(
    Tag sourceTag,
    Tag targetTag,
    SurfaceId surfaceId) {
  static auto setViewSnapshotJNI =
      JFabricUIManager::javaClassStatic()->getMethod<void(jint, jint, jint)>(
          "setViewSnapshot");
  setViewSnapshotJNI(javaUIManager_, sourceTag, targetTag, surfaceId);
}

void FabricMountingManager::clearPendingSnapshots() {
  static auto clearPendingSnapshotsJNI =
      JFabricUIManager::javaClassStatic()->getMethod<void()>(
          "clearPendingSnapshots");
  clearPendingSnapshotsJNI(javaUIManager_);
}

void FabricMountingManager::scheduleReactRevisionMerge(SurfaceId surfaceId) {
  static const auto scheduleReactRevisionMerge =
      JFabricUIManager::javaClassStatic()->getMethod<void(int32_t)>(
          "scheduleReactRevisionMerge");
  scheduleReactRevisionMerge(javaUIManager_, surfaceId);
}

} // namespace facebook::react
