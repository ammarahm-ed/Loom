#pragma once

#include <ReactCommon/TurboModule.h>
#include <memory>

namespace loomexample {

/**
 * LoomExample: one C++ TurboModule for Android and iOS, registered with React Native's global C++ module map
 * (see android/src/main/jni/OnLoad.cpp and ios/LoomExampleRegistration.mm). It does four things a real
 * library does with the renderer, once for Fabric and once for Loom (see LoomExample.cpp):
 *
 *   renderer()                 which renderer answered: "loom" or "fabric"
 *   measure(tag)               a node's frame in its surface, from the renderer's layout
 *   setOpacity(tag, opacity)   write a prop of a node from native code, on any thread
 *   frames()                   how many frames (Loom) or mounts (Fabric) the renderer reported
 */
std::shared_ptr<facebook::react::TurboModule> createLoomExampleModule(
    std::shared_ptr<facebook::react::CallInvoker> jsInvoker);

} // namespace loomexample
