/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Lean release only (LOOM_LEAN_RELEASE). The inspector, DevTools and tracing code is not compiled; every
// symbol it exported is still exported as a zero-returning stub (tools/prebuild/gen-export-stubs.py).
// The few entry points whose callers dereference the result need a real object:

#if defined(LOOM_LEAN_RELEASE)

#include <jsinspector-modern/tracing/PerformanceTracer.h>

namespace facebook::react::jsinspector_modern::tracing {

// Libraries (and React Native's own timeline and networking code) call
// PerformanceTracer::getInstance().isTracing() and report into it. With no tracing code, the instance is
// zero-filled storage: isTracing() reads a false atomic, and every report is a no-op stub.
PerformanceTracer &PerformanceTracer::getInstance()
{
  alignas(PerformanceTracer) static unsigned char storage[sizeof(PerformanceTracer)];
  return *reinterpret_cast<PerformanceTracer *>(storage);
}

} // namespace facebook::react::jsinspector_modern::tracing

#endif // LOOM_LEAN_RELEASE
