/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "../binding/LoomBinding.h"
#include "LoomRendererInternals.h"

#include <algorithm>
#include <cmath>

#include <jsi/JSIDynamic.h>
#include <react/renderer/core/EventTarget.h>
#include <react/renderer/loom/binding/LoomEventManager.h>

#if LOOM_RENDERER
namespace facebook::react {
namespace {

using loom::propsFromValue;
using loom::requireArguments;

using loom::findProp;
using loom::numberProp;
using loom::propsFromValue;
using loom::requireArguments;
using loom::stringProp;

/** NativeDOM's direct-path argument check, kept local to this façade. */


} // namespace

jsi::Value LoomBinding::getDirectDOM(
    jsi::Runtime& rt,
    const std::string& method,
    const jsi::Value* arguments,
    size_t count) {
  // The readers below (getBoundingClientRect, measureLayout, getChildNodes and
  // the rest) hand their result straight back to JavaScript, so they must
  // observe a renderer that includes every submitted commit. `synchronize()`
  // computes *and applies* that work, which is why it stays for them.
  //
  // These three only need ordering against mounts, and the pointer-capture
  // writes do not read the tree at all, so they run as ordered UI effects
  // rather than draining the worker queue on the calling thread.
  if (method != "setNativeProps" && method != "setPointerCapture" &&
      method != "releasePointerCapture") {
    worker_->synchronize();
  }
  auto documentSurface = [&]() -> SurfaceId {
    if (!arguments[0].isNumber()) {
      return 0;
    }
    return static_cast<SurfaceId>(arguments[0].asNumber());
  };
  auto refAt = [&](size_t index) {
    if (index >= count) {
      return std::shared_ptr<NodeRef>{};
    }
    auto ref = nodeRef(rt, arguments[index]);
    return ref != nullptr && isLiveNodeRef(*ref)
        ? ref
        : std::shared_ptr<NodeRef>{};
  };
  auto tagFor = [&](const std::shared_ptr<NodeRef>& ref) -> Tag {
    if (ref == nullptr) {
      return 0;
    }
    if (ref->tag != 0) {
      return ref->tag;
    }
    auto iterator = renderers_.find(ref->surfaceId);
    return iterator == renderers_.end() ? 0 : iterator->second->rootTag();
  };
  auto requireNativeMetrics = [&](LoomRenderer* renderer, Tag tag) {
    if (renderer != nullptr &&
        renderer->componentRoles(tag).isRawText()) {
      throw jsi::JSError(
          rt,
          "NativeDOM measurement is unsupported for viewless raw text nodes");
    }
  };

  if (method == "getChildNodes") {
    requireArguments(rt, method, 1, count);
    SurfaceId surface = 0;
    Tag tag = 0;
    const auto documentRef =
        !arguments[0].isNumber() ? refAt(0) : nullptr;
    if (documentRef != nullptr && documentRef->tag == 0) {
      surface = documentRef->surfaceId;
      auto documentHandle = documentInstanceHandles_.find(surface);
      if (documentHandle == documentInstanceHandles_.end()) {
        return jsi::Array(rt, 0);
      }
      jsi::Array result(rt, 1);
      result.setValueAtIndex(
          rt,
          0,
          documentHandle->second->getInstanceHandle(rt));
      return result;
    }
    if (arguments[0].isNumber()) {
      surface = documentSurface();
      auto documentHandle = documentInstanceHandles_.find(surface);
      if (documentHandle != documentInstanceHandles_.end()) {
        jsi::Array result(rt, 1);
        result.setValueAtIndex(
            rt, 0, documentHandle->second->getInstanceHandle(rt));
        return result;
      }
      return jsi::Array(rt, 0);
    } else if (auto ref = refAt(0)) {
      surface = ref->surfaceId;
      tag = tagFor(ref);
    }
    auto owner = renderers_.find(surface);
    if (owner == renderers_.end() || tag == 0) {
      return jsi::Array(rt, 0);
    }
    auto children = owner->second->childrenOf(tag);
    jsi::Array result(rt, children.size());
    for (size_t i = 0; i < children.size(); ++i) {
      auto handle = owner->second->instanceHandle(children[i]);
      if (handle == nullptr) {
        result.setValueAtIndex(rt, i, jsi::Value::undefined());
        continue;
      }
      EventTarget target(handle, surface);
      target.setEnabled(true);
      target.retain(rt);
      result.setValueAtIndex(rt, i, target.getInstanceHandle(rt));
      target.release(rt);
    }
    return result;
  }

  if (method == "getElementById") {
    requireArguments(rt, method, 2, count);
    const auto surface = documentSurface();
    auto owner = renderers_.find(surface);
    if (owner == renderers_.end() || !arguments[1].isString()) {
      return jsi::Value::undefined();
    }
    const auto tag = owner->second->findByNativeId(arguments[1].asString(rt).utf8(rt));
    auto handle = owner->second->instanceHandle(tag);
    if (handle == nullptr) {
      return jsi::Value::undefined();
    }
    EventTarget target(handle, surface);
    target.setEnabled(true);
    target.retain(rt);
    auto result = target.getInstanceHandle(rt);
    target.release(rt);
    return result;
  }

  if (method == "getParentNode") {
    requireArguments(rt, method, 1, count);
    if (arguments[0].isNumber()) {
      return jsi::Value::undefined();
    }
    auto ref = refAt(0);
    auto tag = tagFor(ref);
    auto owner = ref == nullptr ? nullptr : rendererOwning(tag);
    if (ref == nullptr || owner == nullptr) {
      return jsi::Value::undefined();
    }
    const auto parent = owner->parentOf(tag);
    if (parent == 0 || parent == owner->rootTag()) {
      return jsi::Value{rt, static_cast<double>(ref->surfaceId)};
    }
    auto handle = owner->instanceHandle(parent);
    if (handle == nullptr) {
      return jsi::Value::undefined();
    }
    EventTarget target(handle, ref->surfaceId);
    target.setEnabled(true);
    target.retain(rt);
    auto result = target.getInstanceHandle(rt);
    target.release(rt);
    return result;
  }

  if (method == "isConnected") {
    requireArguments(rt, method, 1, count);
    if (arguments[0].isNumber()) {
      const auto surface = static_cast<SurfaceId>(arguments[0].asNumber());
      return jsi::Value{documentInstanceHandles_.count(surface) != 0};
    }
    auto ref = refAt(0);
    auto tag = tagFor(ref);
    auto owner = ref == nullptr ? nullptr : rendererOwning(tag);
    return jsi::Value{owner != nullptr && owner->isConnected(tag)};
  }

  if (method == "compareDocumentPosition") {
    requireArguments(rt, method, 2, count);
    auto first = refAt(0);
    auto second = refAt(1);
    if (arguments[0].isNumber() || arguments[1].isNumber()) {
      if (arguments[0].isNumber() && arguments[1].isNumber()) {
        return jsi::Value{rt, 1.0};
      }
      const auto surface = arguments[0].isNumber()
          ? static_cast<SurfaceId>(arguments[0].asNumber())
          : static_cast<SurfaceId>(arguments[1].asNumber());
      const auto node = arguments[0].isNumber() ? second : first;
      if (node == nullptr || node->surfaceId != surface) {
        return jsi::Value{rt, 1.0};
      }
      return jsi::Value{rt, arguments[0].isNumber() ? 20.0 : 10.0};
    }
    auto firstTag = tagFor(first);
    auto secondTag = tagFor(second);
    auto owner = first == nullptr ? nullptr : rendererOwning(firstTag);
    if (owner == nullptr || second == nullptr || first->surfaceId != second->surfaceId) {
      return jsi::Value{rt, 1.0};
    }
    return jsi::Value{rt, static_cast<double>(owner->compareDocumentPosition(firstTag, secondTag))};
  }

  if (method == "getBoundingClientRect" || method == "getInnerSize" ||
      method == "getScrollSize" || method == "getScrollPosition" ||
      method == "getBorderWidth" || method == "getOffset" ||
      method == "getTagName" || method == "getTextContent" ||
      method == "measureInstance") {
    requireArguments(rt, method, 1, count);
    auto ref = refAt(0);
    auto tag = tagFor(ref);
    auto owner = ref == nullptr ? nullptr : rendererOwning(tag);
    if (owner == nullptr) {
      return jsi::Value::undefined();
    }
    if (method != "getTagName" && method != "getTextContent") {
      requireNativeMetrics(owner, tag);
    }
    LoomRenderer::Metrics metrics;
    const bool includeTransform =
        method == "getBoundingClientRect" && count > 1 && arguments[1].isBool()
        ? arguments[1].getBool()
        : method == "getBoundingClientRect";
    const bool hasMetrics = method == "getBoundingClientRect"
        ? windowMetrics(tag, metrics, includeTransform)
        : owner->metricsRelativeTo(tag, 0, metrics, false);
    if (method == "getBoundingClientRect") {
      jsi::Array result(rt, 4);
      result.setValueAtIndex(rt, 0, hasMetrics ? metrics.x : 0);
      result.setValueAtIndex(rt, 1, hasMetrics ? metrics.y : 0);
      result.setValueAtIndex(rt, 2, hasMetrics ? metrics.width : 0);
      result.setValueAtIndex(rt, 3, hasMetrics ? metrics.height : 0);
      return result;
    }
    if (method == "getInnerSize") {
      const auto border = owner->borderWidths(tag);
      const auto width = hasMetrics
          ? std::max(0.0f, metrics.width - static_cast<float>(border[0] + border[2]))
          : 0.0f;
      const auto height = hasMetrics
          ? std::max(0.0f, metrics.height - static_cast<float>(border[1] + border[3]))
          : 0.0f;
      return jsi::Array::createWithElements(rt, width, height);
    }
    if (method == "getScrollSize") {
      const auto content = owner->scrollContentSize(tag);
      return jsi::Array::createWithElements(
          rt, std::round(content.first), std::round(content.second));
    }
    if (method == "getScrollPosition") {
      const auto offset = owner->scrollOffset(tag);
      return jsi::Array::createWithElements(rt, offset.first, offset.second);
    }
    if (method == "getBorderWidth") {
      const auto border = owner->borderWidths(tag);
      return jsi::Array::createWithElements(
          rt, border[1], border[2], border[3], border[0]);
    }
    if (method == "getTagName") {
      const auto name = LoomPlatformTraits::current().components.tagName(
          owner->componentName(tag));
      return jsi::String::createFromUtf8(rt, "RN:" + name);
    }
    if (method == "getTextContent") {
      return jsi::String::createFromUtf8(rt, owner->textContent(tag));
    }
    if (method == "getOffset") {
      auto parent = owner->parentOf(tag);
      while (parent != 0 && parent != owner->rootTag() &&
             !owner->isPositioned(parent)) {
        parent = owner->parentOf(parent);
      }
      LoomRenderer::Metrics offsetMetrics;
      const bool hasOffsetMetrics = owner->metricsRelativeTo(
          tag,
          parent == owner->rootTag() ? 0 : parent,
          offsetMetrics);
      auto result = jsi::Array(rt, 3);
      if (parent == 0) {
        result.setValueAtIndex(rt, 0, jsi::Value::null());
      } else {
        auto handle = owner->instanceHandle(parent);
        if (handle == nullptr) {
          result.setValueAtIndex(rt, 0, jsi::Value::null());
        } else {
          EventTarget target(handle, ref->surfaceId);
          target.setEnabled(true);
          target.retain(rt);
          result.setValueAtIndex(rt, 0, target.getInstanceHandle(rt));
          target.release(rt);
        }
      }
      result.setValueAtIndex(rt, 1, hasOffsetMetrics ? offsetMetrics.y : 0);
      result.setValueAtIndex(rt, 2, hasOffsetMetrics ? offsetMetrics.x : 0);
      return result;
    }
    auto result = jsi::Object(rt);
    result.setProperty(rt, "x", hasMetrics ? metrics.x : 0);
    result.setProperty(rt, "y", hasMetrics ? metrics.y : 0);
    result.setProperty(rt, "width", hasMetrics ? metrics.width : 0);
    result.setProperty(rt, "height", hasMetrics ? metrics.height : 0);
    return result;
  }

  if (method == "linkRootNode") {
    requireArguments(rt, method, 2, count);
    const auto surface = static_cast<SurfaceId>(arguments[0].asNumber());
    if (!arguments[1].isObject()) {
      return jsi::Value::undefined();
    }
    pendingDocumentInstanceHandles_[surface] =
        std::make_shared<InstanceHandle>(rt, arguments[1], surface);
    documentInstanceHandles_[surface] = pendingDocumentInstanceHandles_[surface];
    return jsi::Object::createFromHostObject(
        rt, makeNodeRef(surface, 0));
  }

  if (method == "measure" || method == "measureInWindow" || method == "measureLayout") {
    requireArguments(rt, method, method == "measureLayout" ? 4 : 2, count);
    auto ref = refAt(0);
    auto owner = ref == nullptr ? nullptr : rendererOwning(ref->tag);
    auto relativeRef = method == "measureLayout" ? refAt(1) : nullptr;
    const bool relativeTargetIsSameSurfaceDocument =
        method == "measureLayout" && arguments[1].isNumber() && ref != nullptr &&
        static_cast<SurfaceId>(arguments[1].asNumber()) == ref->surfaceId;
    const bool hasValidRelativeTarget = method != "measureLayout" ||
        relativeTargetIsSameSurfaceDocument ||
        (relativeRef != nullptr && ref != nullptr &&
         relativeRef->surfaceId == ref->surfaceId);
    if (ref != nullptr && owner != nullptr) {
      requireNativeMetrics(owner, ref->tag);
    }
    LoomRenderer::Metrics metrics;
    LoomRenderer::Metrics pageMetrics;
    const Tag relativeTag = relativeRef == nullptr ? 0 : tagFor(relativeRef);
    const bool measured = owner != nullptr && hasValidRelativeTarget &&
        owner->metricsRelativeTo(ref->tag, relativeTag, metrics);
    if (method == "measureLayout") {
      auto fail = arguments[2].asObject(rt).asFunction(rt);
      auto success = arguments[3].asObject(rt).asFunction(rt);
      if (!measured) fail.call(rt); else success.call(rt, {metrics.x, metrics.y, metrics.width, metrics.height});
    } else if (method == "measureInWindow") {
      auto callback = arguments[1].asObject(rt).asFunction(rt);
      const bool hasPageMetrics = owner != nullptr && windowMetrics(ref->tag, pageMetrics);
      callback.call(rt, {hasPageMetrics ? pageMetrics.x : 0,
                         hasPageMetrics ? pageMetrics.y : 0,
                         hasPageMetrics ? pageMetrics.width : 0,
                         hasPageMetrics ? pageMetrics.height : 0});
    } else {
      auto callback = arguments[1].asObject(rt).asFunction(rt);
      const bool hasPageMetrics = owner != nullptr && windowMetrics(ref->tag, pageMetrics);
      // `measure()` reports its origin relative to the parent, exactly like the
      // shadow-tree path; `measureLayout` keeps the caller's relative target and
      // `measureInWindow` reports window coordinates.
      LoomRenderer::Metrics localMetrics;
      const bool hasLocalMetrics =
          owner != nullptr && parentMetrics(ref->tag, localMetrics);
      callback.call(rt, {hasLocalMetrics ? localMetrics.x : 0,
                         hasLocalMetrics ? localMetrics.y : 0,
                         measured ? metrics.width : 0,
                         measured ? metrics.height : 0,
                         hasPageMetrics ? pageMetrics.x : 0,
                         hasPageMetrics ? pageMetrics.y : 0});
    }
    return jsi::Value::undefined();
  }

  if (method == "setNativeProps") {
    requireArguments(rt, method, 2, count);
    if (auto ref = refAt(0)) {
      setWorkerNativeProps(*ref, propsFromValue(rt, arguments[1]));
    }
    return jsi::Value::undefined();
  }

  if (method == "hasPointerCapture") {
    // Answers JavaScript, so it reads the tree on this thread.
    requireArguments(rt, method, 2, count);
    auto ref = refAt(0);
    if (ref == nullptr) return jsi::Value{false};
    auto owner = renderers_.find(ref->surfaceId);
    if (owner == renderers_.end() || !isLiveNodeRef(*ref)) {
      return jsi::Value{false};
    }
    const auto pointerId = static_cast<int32_t>(arguments[1].asNumber());
    return jsi::Value{
        LoomEventManager::hasPointerCapture(
            ref->surfaceId, ref->tag, pointerId)};
  }

  if (method == "setPointerCapture" || method == "releasePointerCapture") {
    requireArguments(rt, method, 2, count);
    auto ref = nodeRef(rt, arguments[0]);
    if (ref != nullptr) {
      const auto pointerId = static_cast<int32_t>(arguments[1].asNumber());
      const auto capture = method == "setPointerCapture";
      // Capture must be recorded before any later pointer event is dispatched,
      // which the ordered effect preserves: events queue through the same
      // publication path.
      submitOrderedUIEffect(ref->surfaceId, [this, ref, pointerId, capture] {
        if (!isLiveNodeRef(*ref)) {
          return;
        }
        if (capture) {
          LoomEventManager::setPointerCapture(
              ref->surfaceId, ref->tag, pointerId);
        } else {
          LoomEventManager::releasePointerCapture(
              ref->surfaceId, ref->tag, pointerId);
        }
      });
    }
    return jsi::Value::undefined();
  }

  throw jsi::JSError(rt, "NativeDOM method is not supported by the Loom renderer: " + method);
}

} // namespace facebook::react
#endif // LOOM_RENDERER
