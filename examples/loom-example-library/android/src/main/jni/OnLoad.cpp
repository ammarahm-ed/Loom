#include <ReactCommon/CxxTurboModuleUtils.h>
#include <fbjni/fbjni.h>

#include "LoomExample.h"

// Loading the library (LoomExamplePackage) registers the module with React Native's global C++ module
// map, which the TurboModule manager consults for any name it has no Java module for.
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
  return facebook::jni::initialize(vm, [] {
    facebook::react::registerCxxModuleToGlobalModuleMap("LoomExample", loomexample::createLoomExampleModule);
  });
}
