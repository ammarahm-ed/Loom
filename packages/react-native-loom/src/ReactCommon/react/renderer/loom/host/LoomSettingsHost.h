/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <jsi/jsi.h>

#include <react/renderer/runtimescheduler/LoomSettings.h>

#include <string>
#include <vector>

namespace facebook::react {

/**
 * The JavaScript face of `LoomSettings`: `get(name)` and `set(name, value)`
 * over the registry, reachable as `nativeFabricUIManager.loomSettings`.
 *
 * - An unknown name throws.
 * - Writing a read-only setting (build info, `renderer`) throws a TypeError.
 * - Writing a setting another platform owns does nothing; reading it returns
 *   its default.
 *
 * `Loom` in `react-native-loom` wraps this.
 */
class LoomSettingsHostObject : public jsi::HostObject {
 public:
  LoomSettingsHostObject(bool jsOnMainThread, bool worker) {
    LoomSettings::setBuildInfo(jsOnMainThread, worker);
  }

  jsi::Value get(jsi::Runtime& runtime, const jsi::PropNameID& name) override {
    const auto key = name.utf8(runtime);
    if (key == "renderer") {
      return jsi::String::createFromAscii(runtime, "loom");
    }
    const auto id = LoomSettings::find(key.c_str());
    if (id == LoomSettingId::Count) {
      // Symbols and `then` probes from tooling are not settings; JS wrappers
      // only ask for known names, so an unknown one is a bug.
      throw jsi::JSError(runtime, "Loom: unknown setting '" + key + "'");
    }
    const int value = LoomSettings::valueForJS(id);
    if (LoomSettings::info(id).type == LoomSettingType::Bool) {
      return jsi::Value(value != 0);
    }
    return jsi::Value(value);
  }

  void set(
      jsi::Runtime& runtime,
      const jsi::PropNameID& name,
      const jsi::Value& value) override {
    const auto key = name.utf8(runtime);
    if (key == "renderer") {
      throwTypeError(runtime, "Loom: 'renderer' is read-only");
    }
    const auto id = LoomSettings::find(key.c_str());
    if (id == LoomSettingId::Count) {
      throw jsi::JSError(runtime, "Loom: unknown setting '" + key + "'");
    }
    const auto& info = LoomSettings::info(id);
    if (info.readOnly) {
      throwTypeError(runtime, "Loom: '" + key + "' is read-only");
    }
    int number = 0;
    if (info.type == LoomSettingType::Bool) {
      if (!value.isBool()) {
        throwTypeError(runtime, "Loom: '" + key + "' expects a boolean");
      }
      number = value.getBool() ? 1 : 0;
    } else {
      if (!value.isNumber()) {
        throwTypeError(runtime, "Loom: '" + key + "' expects a number");
      }
      const double raw = value.getNumber();
      number = raw > 2147483647.0 ? 2147483647 : raw < -2147483647.0 ? -2147483647 : static_cast<int>(raw);
    }
    LoomSettings::write(id, number);
  }

  std::vector<jsi::PropNameID> getPropertyNames(jsi::Runtime& runtime) override {
    std::vector<jsi::PropNameID> names;
    names.push_back(jsi::PropNameID::forAscii(runtime, "renderer"));
    for (size_t i = 0; i < LoomSettings::kCount; ++i) {
      names.push_back(jsi::PropNameID::forAscii(runtime, LoomSettings::kTable[i].name));
    }
    return names;
  }

 private:
  [[noreturn]] static void throwTypeError(jsi::Runtime& runtime, const std::string& message) {
    auto error = runtime.global()
                     .getPropertyAsFunction(runtime, "TypeError")
                     .callAsConstructor(runtime, jsi::String::createFromUtf8(runtime, message));
    throw jsi::JSError(runtime, std::move(error));
  }
};

} // namespace facebook::react
