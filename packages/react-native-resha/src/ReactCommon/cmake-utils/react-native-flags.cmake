# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.13)
set(CMAKE_VERBOSE_MAKEFILE on)

if(NOT DEFINED RN_PROFILE_UI_THREAD_RENDERER)
    set(RN_PROFILE_UI_THREAD_RENDERER OFF)
endif()
if(NOT DEFINED UI_THREAD_RENDERER_WORKER)
    set(UI_THREAD_RENDERER_WORKER OFF)
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

# React Native X compile-time switches.
#
# Each is a CMake cache variable supplied by the build (on Android, ReactAndroid's Gradle
# properties of the same name) and is mirrored here as a preprocessor macro of the same name, so
# that code can branch on it with `#if`. They are always defined, as `1` or `0`, rather than only
# defined when set, so that `#if RN_UI_THREAD` is unambiguous and cannot silently take the wrong
# branch under -Wundef.
#
#   RN_UI_THREAD       React Native's JavaScript runtime and native modules queue run on the
#                      Android UI thread instead of on the background queues upstream uses.
#   UI_THREAD_RENDERER Selects the independent direct JS-host-to-Yoga-to-Android-view renderer.
#                      Legacy Fabric remains on its normal path and synchronization model.
#   UI_THREAD_RENDERER_WORKER Selects worker-owned direct-renderer commit preparation; UI mounting
#                      remains on the platform UI thread.
#
# All default to 0: without any of them the build is upstream React Native.
function(rnx_compile_switch target_name scope name)
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

  rnx_compile_switch(${target_name} ${scope} RN_UI_THREAD)
  rnx_compile_switch(${target_name} ${scope} UI_THREAD_RENDERER)
  rnx_compile_switch(${target_name} ${scope} UI_THREAD_RENDERER_WORKER)
  target_compile_definitions(
          ${target_name} ${scope}
          RN_PROFILE_UI_THREAD_RENDERER=$<BOOL:${RN_PROFILE_UI_THREAD_RENDERER}>
  )
endfunction()
