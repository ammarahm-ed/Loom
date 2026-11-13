/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#pragma once

#include <cstddef>
#include <string>

#include <folly/dynamic.h>
#include <jsi/JSIDynamic.h>
#include <jsi/jsi.h>

#if LOOM_RENDERER
namespace facebook::react {
namespace loom {

/**
 * Helpers shared by the Loom renderer's translation units. They used to be
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

/**
 * The named member of `props` when it is a string, otherwise empty. Returned by
 * reference into `props` (no copy: most values are longer than the 32-bit
 * small-string buffer, so a copy was a heap allocation per lookup), so it is
 * valid only while `props` is not modified.
 */
inline const std::string& stringProp(
    const folly::dynamic& props,
    const char* name) {
  static const std::string empty;
  const auto* value = findProp(props, name);
  if (value == nullptr || !value->isString()) {
    return empty;
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
/**
 * `jsi::dynamicFromValue` with the same output, minus its overhead on a host
 * call's hot path: no explicit work stack, no temporary (name, value) vector
 * per object, one hash insert per key (it inserts then looks the key up again)
 * and the key string moved, not copied. Semantics match: `undefined` members
 * of an object are dropped, a function member becomes null, `undefined` in an
 * array becomes null, a function in an array throws.
 */
inline void dynamicFromJsi(
    jsi::Runtime& runtime,
    const jsi::Value& value,
    folly::dynamic& output,
    bool inArray);

inline void dynamicFromJsiObject(
    jsi::Runtime& runtime,
    const jsi::Object& object,
    folly::dynamic& output) {
  if (object.isArray(runtime)) {
    auto array = object.getArray(runtime);
    const auto size = array.size(runtime);
    output = folly::dynamic::array();
    output.reserve(size);
    for (size_t i = 0; i < size; ++i) {
      output.push_back(nullptr);
      dynamicFromJsi(
          runtime, array.getValueAtIndex(runtime, i), output[i], true);
    }
    return;
  }
  output = folly::dynamic::object();
  auto names = object.getPropertyNames(runtime);
  const auto count = names.size(runtime);
  // [LoomPerf item 4] One allocation for the whole table instead of rehashing
  // as it grows, and scalar members go straight into the map without the
  // temporary dynamic and the recursive call.
  output.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    auto name = names.getValueAtIndex(runtime, i).getString(runtime);
    auto member = object.getProperty(runtime, name);
    if (member.isUndefined()) {
      continue;
    }
    if (member.isNumber()) {
      output.insert(name.utf8(runtime), member.getNumber());
    } else if (member.isString()) {
      output.insert(
          name.utf8(runtime), member.getString(runtime).utf8(runtime));
    } else if (member.isBool()) {
      output.insert(name.utf8(runtime), member.getBool());
    } else {
      folly::dynamic converted;
      dynamicFromJsi(runtime, member, converted, false);
      output.insert(name.utf8(runtime), std::move(converted));
    }
  }
}

inline void dynamicFromJsi(
    jsi::Runtime& runtime,
    const jsi::Value& value,
    folly::dynamic& output,
    bool inArray) {
  if (value.isNumber()) {
    output = value.getNumber();
  } else if (value.isString()) {
    output = value.getString(runtime).utf8(runtime);
  } else if (value.isBool()) {
    output = value.getBool();
  } else if (value.isUndefined() || value.isNull()) {
    output = nullptr;
  } else if (value.isObject()) {
    auto object = value.getObject(runtime);
    if (object.isFunction(runtime)) {
      if (inArray) {
        throw jsi::JSError(
            runtime, "JS Functions are not convertible to dynamic");
      }
      output = nullptr;
      return;
    }
    dynamicFromJsiObject(runtime, object, output);
  } else {
    // BigInt / Symbol: let the reference implementation raise its error.
    output = jsi::dynamicFromValue(runtime, value);
  }
}

inline folly::dynamic propsFromValue(
    jsi::Runtime& runtime,
    const jsi::Value& value) {
  if (!value.isObject()) {
    return folly::dynamic::object();
  }
  auto object = value.getObject(runtime);
  if (object.isFunction(runtime)) {
    return folly::dynamic::object();
  }
  folly::dynamic props;
  dynamicFromJsiObject(runtime, object, props);
  return props.isObject() ? std::move(props) : folly::dynamic::object();
}

/**
 * Literal-name overload: a host call must not build a `std::string` (a heap
 * allocation for the longer method names) just to pass the check.
 */
inline void requireArguments(
    jsi::Runtime& runtime,
    const char* methodName,
    size_t expected,
    size_t actual) {
  if (actual < expected) {
    throw jsi::JSError(
        runtime,
        std::string(methodName) + " requires " + std::to_string(expected) +
            " arguments, but only " + std::to_string(actual) + " were passed");
  }
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

} // namespace loom
} // namespace facebook::react

#endif // LOOM_RENDERER