# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.13)
set(CMAKE_VERBOSE_MAKEFILE on)

if(NOT DEFINED LOOM_PROFILE)
    set(LOOM_PROFILE OFF)
endif()
# The worker is part of the Loom renderer: on exactly when the renderer is.
if(DEFINED LOOM_RENDERER AND LOOM_RENDERER)
    set(LOOM_WORKER ON)
else()
    set(LOOM_WORKER OFF)
endif()

# This CMake file exposes the React Native Flags that all the libraries should use when
# compiling a module that will end up inside libreactnative.so

SET(reactnative_FLAGS
        -Wall
        -Werror
        -fexceptions
        -frtti
        -std=c++20
        -DFOLLY_NO_CONFIG=1
        -DLOG_TAG=\"ReactNative\"
)

# Loom compile-time switches.
#
# Each is a CMake cache variable supplied by the build (on Android, ReactAndroid's Gradle
# properties of the same name) and is mirrored here as a preprocessor macro of the same name, so
# that code can branch on it with `#if`. They are always defined, as `1` or `0`, rather than only
# defined when set, so that `#if LOOM_RENDERER` is unambiguous and cannot silently take the wrong
# branch under -Wundef.
#
# Where JavaScript runs (UI thread or its own thread) is not a build switch: it is a runtime setting
# fixed at startup (LoomJavaScriptThread.h), so one build serves both.
#
#   LOOM_RENDERER Selects the independent direct JS-host-to-Yoga-to-Android-view renderer.
#                      Legacy Fabric remains on its normal path and synchronization model.
#   LOOM_WORKER Loom's worker thread (commit preparation off the JS thread; mounting stays on
#                      the UI thread). Derived: always equal to LOOM_RENDERER.
#
# All default to 0: without any of them the build is upstream React Native.
function(loom_compile_switch target_name scope name)
  if(${name})
    target_compile_definitions(${target_name} ${scope} ${name}=1)
  else()
    target_compile_definitions(${target_name} ${scope} ${name}=0)
  endif()
endfunction()

# This function can be used to configure the reactnative flags for a specific target in
# a convenient way. The usage is:
#
# target_compile_reactnative_options(target_name scope)
#
# scope is either PUBLIC, PRIVATE or INTERFACE

function(target_compile_reactnative_options target_name scope)
  target_compile_options(${target_name} ${scope} ${reactnative_FLAGS})
  # TODO T228344694 improve this so that it works for all platforms
  if(ANDROID)
    target_compile_definitions(${target_name} ${scope} RN_SERIALIZABLE_STATE)
  endif()
  target_compile_definitions(${target_name} ${scope} HERMES_V1_ENABLED=1)

  # The main-thread jank reporter (debug builds only; see JankReporter.h). Compiled in for debug
  # builds in either mode; it only reports while JavaScript runs on the UI thread.
  if(ANDROID)
    target_compile_definitions(${target_name} ${scope} RN_JANK_REPORTER=1)
  endif()
  loom_compile_switch(${target_name} ${scope} LOOM_RENDERER)
  loom_compile_switch(${target_name} ${scope} LOOM_WORKER)
  target_compile_definitions(
          ${target_name} ${scope}
          LOOM_PROFILE=$<BOOL:${LOOM_PROFILE}>
  )
endfunction()
