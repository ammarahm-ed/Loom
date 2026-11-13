/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fbjni/fbjni.h>
#include <atomic>
#include <memory>

#include <react/renderer/uimanager/LoomHost.h>
#include <react/renderer/uimanager/LoomTextLayoutManagerServices.h>

namespace facebook::react {

class FabricUIManagerBinding;

/**
 * Android's text engine: Java `StaticLayout` through `TextLayoutManager`, with
 * a prepared layout the view draws from. Attribute parsing is React Native's
 * own (`LoomTextLayoutManagerServices`).
 */
class LoomAndroidTextServices final : public LoomTextLayoutManagerServices {
 public:
  LoomAndroidTextServices(
      FabricUIManagerBinding* binding,
      std::shared_ptr<const ContextContainer> contextContainer);

  void invalidate();

  std::optional<Size> measureText(
      SurfaceId,
      const AttributedString&,
      const ParagraphAttributes&,
      const Size& minimumSize,
      const Size& maximumSize) override;
  bool canMeasureOnWorker() override;
  LoomTextMeasurement prepareText(
      SurfaceId,
      const AttributedString&,
      const ParagraphAttributes&,
      const Size& minimumSize,
      const Size& maximumSize) override;
  LoomTextMeasurement prepareTextForMount(
      SurfaceId,
      const AttributedString&,
      const ParagraphAttributes&,
      const Size& minimumSize,
      const Size& maximumSize) override;

 private:
  std::atomic<FabricUIManagerBinding*> binding_;
};

/**
 * The Android `LoomHost`. It forwards to the Fabric binding, which owns the
 * mounting manager, the event manager and the JNI state; after `invalidate()`
 * (the binding was uninstalled) every call is a no-op.
 */
class LoomAndroidHost final : public LoomHost {
 public:
  LoomAndroidHost(
      FabricUIManagerBinding* binding,
      std::shared_ptr<const ContextContainer> contextContainer);

  void invalidate();

  /** What the core reads about Android. Also installed at library load. */
  static LoomPlatformTraits platformTraits();
  /**
   * Resolves the runtime settings (see LoomSettings.h) before any surface
   * starts: defaults, manifest meta-data read through `javaUIManager`, then the
   * `debug.loom.*` system properties.
   */
  static void loadSettings(facebook::jni::alias_ref<jobject> javaUIManager);
  /** Brings up the text measurement threads before the first surface measures. */
  static void warmUpTextPool();

  LoomPlatformTraits traits() const override {
    return platformTraits();
  }
  std::function<void(std::function<void()>)> uiExecutor() override;
  void applyBatch(SurfaceId, LoomMountBatch) override;
  void failSurface(SurfaceId, const std::string& reason) override;
  void updateViewProps(Tag, const folly::dynamic& props) override;
  void registerEventTarget(SurfaceId, Tag, InstanceHandle::Shared) override;
  void unregisterEventTarget(SurfaceId, Tag) override;
  void dispatchEvent(
      SurfaceId,
      Tag,
      const std::string& eventName,
      folly::dynamic payload,
      RawEvent::Category,
      bool unique) override;
  void updateComponentState(
      SurfaceId,
      Tag,
      const std::shared_ptr<const State>&) override;
  void updateTextState(const LoomTextStateUpdate&) override;
  void dispatchCommand(
      SurfaceId,
      Tag,
      const std::string& commandName,
      const folly::dynamic& args) override;
  void sendAccessibilityEvent(
      SurfaceId,
      Tag,
      const std::string& eventType) override;
  void setIsJSResponder(SurfaceId, Tag, bool, bool) override;
  void reportUnsupportedInlineAttachment(SurfaceId, Tag) override;
  std::pair<float, float> getScrollOffset(SurfaceId, Tag) override;
  std::optional<std::pair<float, float>> getViewLocationInSurface(
      SurfaceId,
      Tag) override;
  LoomTextServices* textServices() override {
    return &text_;
  }

 private:
  std::atomic<FabricUIManagerBinding*> binding_;
  LoomAndroidTextServices text_;
};

} // namespace facebook::react
