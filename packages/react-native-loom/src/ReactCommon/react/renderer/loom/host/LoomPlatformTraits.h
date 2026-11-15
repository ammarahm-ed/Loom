/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>

#include <react/renderer/runtimescheduler/LoomRuntimeHooks.h>
#include <react/renderer/loom/text/LoomTextMeasurement.h>

namespace facebook::react {

/**
 * Whether the JavaScript runtime runs on the thread that owns native views
 * (the `LOOM_JS_ON_MAIN` build switch). When it does not, JS-thread callers may
 * wait for the UI thread but the UI thread must never wait for JS.
 */
#if defined(LOOM_JS_ON_MAIN) && !LOOM_JS_ON_MAIN
inline constexpr bool kLoomJavaScriptOnUIThread = false;
#else
inline constexpr bool kLoomJavaScriptOnUIThread = true;
#endif

enum class LoomLogLevel : uint8_t {
  Info,
  Warn,
  /** Profiling output; only produced while `Loom.profileWorker` is set. */
  Profile,
};

/**
 * What the core needs to know about a host's component vocabulary.
 *
 * The core speaks Fabric component names (`View`, `Paragraph`, `Text`,
 * `RawText`, `ScrollView`, ...) and passes each node's *host name* to
 * `LoomHost::applyBatch`. A host whose view managers use other names maps them
 * in `hostNames`; roles are looked up under the React/Fabric name and under the
 * host name alike, so either spelling is classified.
 */
struct LoomComponentTable {
  enum Role : uint16_t {
    /** A plain box; eligible to be flattened away when it has no native props. */
    Box = 1 << 0,
    /** A text root that owns a host view. */
    Paragraph = 1 << 1,
    /** Nested text: a span inside the enclosing paragraph. */
    VirtualText = 1 << 2,
    /** A text leaf with no host view. */
    RawText = 1 << 3,
    /** The host view needs a generic state wrapper. */
    GenericStateWrapper = 1 << 4,
    /** The host measures it itself; the core needs an explicit size. */
    IntrinsicMeasurement = 1 << 5,
    /** Rendered by the core even though a custom ComponentDescriptor exists. */
    CoreRendered = 1 << 6,
  };

  struct Entry {
    uint16_t roles{0};
    LoomTextStateKind stateKind{LoomTextStateKind::Other};
  };

  /**
   * One component's roles, resolved by name once, when a node or draft is
   * created. Hot paths read this instead of looking a name up.
   */
  struct Roles {
    uint16_t flags{0};
    LoomTextStateKind stateKind{LoomTextStateKind::Other};
    bool textInput{false};

    bool has(Role role) const {
      return (flags & role) != 0;
    }
    bool isParagraph() const {
      return has(Paragraph);
    }
    bool isVirtualText() const {
      return has(VirtualText);
    }
    bool isRawText() const {
      return has(RawText);
    }
    /** A text root or nested text: content that may hold only text. */
    bool isText() const {
      return (flags & (Paragraph | VirtualText)) != 0;
    }
    bool isTextStack() const {
      return (flags & (Paragraph | VirtualText | RawText)) != 0;
    }
    /** Text with a state wrapper of its own: `RCTText`, `Paragraph`, `Text`. */
    bool carriesText() const {
      return stateKind == LoomTextStateKind::Text ||
          stateKind == LoomTextStateKind::Paragraph;
    }
  };

  std::unordered_map<std::string, Entry> entries;
  /** React/Fabric name to host name. Absent means the host name is the same. */
  std::unordered_map<std::string, std::string> hostNames;
  /** Host name to the name `getTagName` reports. Absent means the same. */
  std::unordered_map<std::string, std::string> tagNames;
  /** The host name the core gives text leaves in its own reports. */
  std::string rawTextHostName{"RawText"};

  uint16_t roles(const std::string& name) const {
    const auto it = entries.find(name);
    return it == entries.end() ? uint16_t{0} : it->second.roles;
  }
  bool has(const std::string& name, Role role) const {
    return (roles(name) & role) != 0;
  }
  /** Resolves every role of `name` with one lookup. Not for hot paths. */
  Roles classify(const std::string& name) const {
    Roles roles;
    const auto it = entries.find(name);
    if (it != entries.end()) {
      roles.flags = it->second.roles;
      roles.stateKind = it->second.stateKind;
    }
    roles.textInput = isTextInput(name);
    if (roles.stateKind == LoomTextStateKind::Other && roles.textInput) {
      roles.stateKind = LoomTextStateKind::TextInput;
    }
    return roles;
  }
  /** `TextInput` is matched by name fragment: hosts spell it differently. */
  static bool isTextInput(const std::string& name) {
    return name.find("TextInput") != std::string::npos;
  }
  std::string hostName(const std::string& reactName) const {
    const auto it = hostNames.find(reactName);
    return it == hostNames.end() ? reactName : it->second;
  }
  std::string tagName(const std::string& hostName) const {
    const auto it = tagNames.find(hostName);
    return it == tagNames.end() ? hostName : it->second;
  }

  /** Fabric's own component names, with identity host names. */
  static LoomComponentTable fabric() {
    LoomComponentTable table;
    auto add = [&](const char* name,
                   uint16_t roles,
                   LoomTextStateKind kind = LoomTextStateKind::Other) {
      table.entries[name] = Entry{roles, kind};
    };
    add("View", Box | CoreRendered);
    add("RootView", CoreRendered);
    add("Root", CoreRendered);
    add("Paragraph", Paragraph | CoreRendered, LoomTextStateKind::Paragraph);
    add("SelectableParagraph",
        Paragraph | CoreRendered,
        LoomTextStateKind::SelectableParagraph);
    add("Text", VirtualText | CoreRendered, LoomTextStateKind::Paragraph);
    add("VirtualText", VirtualText);
    add("RawText", RawText | CoreRendered);
    add("Image", CoreRendered);
    add("ScrollView", GenericStateWrapper | CoreRendered);
    add("TextInput", CoreRendered);
    add("LayoutConformance", CoreRendered);
    add("UnimplementedNativeView", CoreRendered);
    return table;
  }
};

/**
 * Everything about a platform that the core reads but does not call. A host
 * fills one in at startup, before the first surface starts, and installs it
 * (`LoomPlatformTraits::install`, or `LoomHost::registerIn`, which does it).
 * The core reads it through `current()`; every field is either data or a plain
 * function pointer, so a read is a load and a predictable branch.
 *
 * A default-constructed instance is the generic host: no prop diffs, no
 * prepared text layout, no parallel measurement, Fabric component names,
 * logging to stderr, the default thread stack.
 */
struct LoomPlatformTraits {
  LoomComponentTable components = LoomComponentTable::fabric();

  /**
   * Whether an update of a component sends only the changed keys
   * (`DirectUpdatePropsOperation::props`) instead of the whole merged set.
   * Receives the host name; evaluated once per node. Null: never. A host may
   * send diffs only for components whose update path keeps the keys the diff
   * leaves out.
   */
  bool (*sendsPropDiffs)(const std::string& hostName){nullptr};

  /**
   * The host draws text in the size of the published state, so a resize must
   * republish the text state even when the text is unchanged.
   */
  bool republishTextStateOnResize{false};

  /**
   * `LoomTextServices::prepareTextForMount` returns a reusable layout the host draws
   * from. Without it the core publishes the final content box with the text
   * state instead.
   */
  bool hasPreparedTextLayout{false};

  /**
   * Runs `body(0..count-1)`, possibly in parallel, and returns when every call
   * has. Null: text is not measured speculatively.
   */
  std::function<void(size_t, const std::function<void(size_t)>&)> parallelFor;

  /**
   * Receives every log and profile line, already formatted. Null: stderr as
   * `tag: message`.
   */
  void (*log)(LoomLogLevel, const char* tag, const char* message){nullptr};

  /** Services the runtime scheduler layer reads (the frame source). */
  LoomRuntimeHooks runtime;

  /** True on the thread that owns native views. Null: always true. */
  bool (*isUIThread)(){nullptr};

  /** 0: the platform's default stack. */
  size_t workerStackSize{0};
  /** Applied on the worker thread through `setCurrentThreadName`. */
  const char* workerThreadName{nullptr};
  void (*setCurrentThreadName)(const char*){nullptr};
  /**
   * Wraps one job's computation on the worker thread, for hosts that must
   * attach the thread to a managed runtime. Null: the job runs as is.
   */
  void (*withWorkerScope)(const std::function<void()>&){nullptr};

  static const LoomPlatformTraits& current() {
    const auto* installed = installed_.load(std::memory_order_acquire);
    return installed != nullptr ? *installed : defaults();
  }

  /**
   * Replaces the process-wide traits. Call before the first surface starts; the
   * previous instance is never freed because readers hold no lock.
   */
  static void install(LoomPlatformTraits traits) {
    LoomRuntimeHooks::install(traits.runtime);
    installed_.store(
        new LoomPlatformTraits(std::move(traits)), std::memory_order_release);
  }

  bool onUIThread() const {
    return isUIThread == nullptr || isUIThread();
  }

  /** True once a host installed traits (false means `current()` is the defaults). */
  static bool isInstalled() {
    return installed_.load(std::memory_order_acquire) != nullptr;
  }

 private:
  // Process-wide state lives in `static inline` data members, never in a function-local
  // `static` of an inline function: with hidden inline functions (Xcode's default) and
  // per-target prelinking (`ld -r`, as SwiftPM builds package targets), each target keeps
  // its own copy of such a static, so a value installed by one target is invisible to
  // another. Inline variables keep default visibility and stay one definition.
  static inline std::atomic<const LoomPlatformTraits*> installed_{nullptr};
  // Constant, so a per-target copy would be harmless.
  static const LoomPlatformTraits& defaults() {
    static const LoomPlatformTraits traits;
    return traits;
  }
};

/** Formats and routes one line through the installed traits' log hook. */
#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
inline void loomLog(
    LoomLogLevel level,
    const char* tag,
    const char* format,
    ...) {
  char message[1024];
  va_list arguments;
  va_start(arguments, format);
  std::vsnprintf(message, sizeof(message), format, arguments);
  va_end(arguments);
  const auto& traits = LoomPlatformTraits::current();
  if (traits.log != nullptr) {
    traits.log(level, tag, message);
  } else {
    std::fprintf(stderr, "%s: %s\n", tag, message);
  }
}

} // namespace facebook::react
