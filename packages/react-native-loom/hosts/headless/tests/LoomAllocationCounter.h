/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace facebook::react::loomtest {

/**
 * Counts every `operator new` in the test executable (it replaces the global
 * allocation functions). For the probes: allocations per node of a phase.
 */
uint64_t allocationCount();

/**
 * With LOOM_PROBE_ALLOC_SITES=1 in the environment, records the call stack of
 * every allocation made from `startAllocationSites` until
 * `stopAllocationSites`, and keeps matching frees until the next start. Prints
 * the innermost Loom function of each (and its Loom caller), by count: every
 * allocation made, or with `liveOnly` only those still allocated. Slow; for
 * finding where a probe's allocations come from and what stays behind.
 */
void startAllocationSites();
void stopAllocationSites();
void printAllocationSites(
    const char* label, bool liveOnly = false, size_t top = 25);

}  // namespace facebook::react::loomtest
