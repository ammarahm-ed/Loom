/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#pragma once

#include <cstddef>
#include <string>

#include <folly/dynamic.h>
#include <jsi/jsi.h>

#if UI_THREAD_RENDERER
namespace facebook::react {
namespace uiThreadRenderer {

/**
 * Helpers shared by the direct renderer's translation units. They used to be
 * copied into each one; the bodies here are the ones that were already in place,
 * kept `inline` so every unit still gets a single definition without a
 * translation-unit dependency.
 */

/** The named member of `props`, or null when absent. */
inline const folly::dynamic* findProp(
    const folly::dynamic& props,
    const char* name) {
  if (!props.isObject()) {
    return nullptr;
  }
  const auto item = props.find(folly::StringPiece(name));
  return item == props.items().end() ? nullptr : &item->second;
}

/** The named member of `props` when it is a string, otherwise empty. */
inline std::string stringProp(const folly::dynamic& props, const char* name) {
  const auto* value = findProp(props, name);
  if (value == nullptr || !value->isString()) {
    return {};
  }
  return value->getString();
}

/** The named member of `props` when it is a number, otherwise nothing. */
inline std::optional<float> numberProp(
    const folly::dynamic& props,
    const char* name) {
  const auto* value = findProp(props, name);
  if (value == nullptr || !value->isNumber()) {
    return std::nullopt;
  }
  return static_cast<float>(value->asDouble());
}

/**
 * The props of a NativeDOM/direct-path argument as an object. A non-object
 * argument yields an empty map rather than throwing, which is the full-map
 * semantics `setNativeProps` relies on.
 */
inline folly::dynamic propsFromValue(
    jsi::Runtime& runtime,
    const jsi::Value& value) {
  if (!value.isObject()) {
    return folly::dynamic::object();
  }
  auto props = jsi::dynamicFromValue(runtime, value);
  return props.isObject() ? props : folly::dynamic::object();
}

/** The direct path's argument check for its JSI host functions. */
inline void requireArguments(
    jsi::Runtime& runtime,
    const std::string& methodName,
    size_t expected,
    size_t actual) {
  if (actual < expected) {
    throw jsi::JSError(
        runtime,
        methodName + " requires " + std::to_string(expected) +
            " arguments, but only " + std::to_string(actual) + " were passed");
  }
}

/**
 * The component names that make up the text stack. These checks appeared in
 * several units with the same name lists; they live here so a new alias is
 * added once.
 */
inline bool isParagraphName(const std::string& name) {
  return name == "RCTText" || name == "Paragraph" ||
      name == "RCTSelectableText" || name == "SelectableParagraph";
}

inline bool isVirtualTextName(const std::string& name) {
  return name == "RCTVirtualText" || name == "VirtualText" || name == "Text";
}

inline bool isRawTextName(const std::string& name) {
  return name == "RCTRawText" || name == "RawText";
}

} // namespace uiThreadRenderer
} // namespace facebook::react

#endif // UI_THREAD_RENDERER