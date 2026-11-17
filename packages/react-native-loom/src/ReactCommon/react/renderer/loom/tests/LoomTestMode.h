/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the MIT license in the LICENSE file in the root directory.
 */

#pragma once

#include <cstdlib>
#include <cstring>

#include <react/renderer/runtimescheduler/LoomJavaScriptThread.h>

namespace facebook::react {

/**
 * Where JavaScript runs in this run of the test binary: the environment variable
 * `LOOM_TEST_JS_THREAD` is `main` (the default) or `background`. The suites run once per
 * mode (see the headless CMakeLists), against one build. Applied before `main`, as the
 * platform glue does before the JS thread exists.
 */
inline const bool kLoomTestJavaScriptOnUIThread = [] {
  const char* mode = std::getenv("LOOM_TEST_JS_THREAD");
  const bool onUIThread = mode == nullptr || std::strcmp(mode, "background") != 0;
#if defined(LOOM_TEST_HOOKS) && LOOM_TEST_HOOKS
  LoomJavaScriptThread::resetForTesting(onUIThread);
#else
  LoomJavaScriptThread::set(onUIThread);
#endif
  return onUIThread;
}();

} // namespace facebook::react
