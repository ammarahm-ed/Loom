# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# loom-core: the Loom renderer core plus the headless host, built with plain
# CMake on a desktop machine. It contains no iOS or Android code: the React
# Native libraries are built from the materialized react-native tree
# (`loom materialize`, which carries Loom's overrides) through their own
# CMakeLists, which already select `platform/cxx` when ANDROID is not set.
#
#   cmake -S packages/react-native-loom/hosts/headless -B build/loom-core \
#         -DLOOM_REACT_NATIVE_DIR=<app>/node_modules/.loom/react-native
#   cmake --build build/loom-core --target loom-core-tests loom-core-headless-tests
#   ctest --test-dir build/loom-core
#
# Third-party libraries are fetched at the versions React Native pins; pass
# -DLOOM_USE_SYSTEM_DEPS=ON to use system packages instead.

cmake_minimum_required(VERSION 3.19)
project(loom-core LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(LOOM_REACT_NATIVE_DIR "" CACHE PATH "The materialized react-native tree")
if(NOT LOOM_REACT_NATIVE_DIR)
  set(LOOM_REACT_NATIVE_DIR
      "${CMAKE_CURRENT_LIST_DIR}/../../../../apps/loom-test-app/node_modules/.loom/react-native")
endif()
file(REAL_PATH "${LOOM_REACT_NATIVE_DIR}" LOOM_REACT_NATIVE_DIR)
if(NOT EXISTS "${LOOM_REACT_NATIVE_DIR}/ReactCommon/react/renderer/uimanager/LoomHost.h")
  message(FATAL_ERROR
      "LOOM_REACT_NATIVE_DIR=${LOOM_REACT_NATIVE_DIR} is not a materialized Loom tree "
      "(uimanager/LoomHost.h is missing). Run `loom materialize` first.")
endif()

# The switches the Loom sources branch on (see cmake-utils/react-native-flags.cmake).
# The core runs its worker; JavaScript runs on the thread that owns the host.
set(LOOM_RENDERER ON CACHE BOOL "")
set(LOOM_WORKER ON CACHE BOOL "")
set(LOOM_JS_ON_MAIN ON CACHE BOOL "")
set(LOOM_PROFILE OFF CACHE BOOL "")

set(REACT_COMMON_DIR "${LOOM_REACT_NATIVE_DIR}/ReactCommon")
set(REACT_ANDROID_DIR "${LOOM_REACT_NATIVE_DIR}/ReactAndroid")
set(REACT_NATIVE_DEBUG_OPTIMIZED OFF)

# Third-party dependencies at React Native's pinned versions (see
# cmake/LoomThirdParty.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/cmake/LoomThirdParty.cmake")

# The React Native libraries the core links, in-tree. Closure of
# react_renderer_uimanager, react_renderer_componentregistry, rrc_view,
# rrc_root and the other targets the Android test executable names.
set(LOOM_RN_SUBDIRS
    callinvoker
    cxxreact
    jserrorhandler
    jsi
    jsinspector-modern
    jsinspector-modern/cdp
    jsinspector-modern/network
    jsinspector-modern/tracing
    logger
    oscompat
    react/bridging
    react/cxxstableapi
    react/debug
    react/featureflags
    react/performance/timeline
    react/renderer/attributedstring
    react/renderer/bridging
    react/renderer/componentregistry
    react/renderer/components/legacyviewmanagerinterop
    react/renderer/components/root
    react/renderer/components/scrollview
    react/renderer/components/text
    react/renderer/components/view
    react/renderer/consistency
    react/renderer/core
    react/renderer/css
    react/renderer/debug
    react/renderer/dom
    react/renderer/graphics
    react/renderer/leakchecker
    react/renderer/mapbuffer
    react/renderer/mounting
    react/renderer/runtimescheduler
    react/renderer/telemetry
    react/renderer/textlayoutmanager
    react/renderer/uimanager
    react/renderer/uimanager/consistency
    react/timing
    react/utils
    reactperflogger
    runtimeexecutor)

include("${REACT_COMMON_DIR}/cmake-utils/react-native-flags.cmake")
# Some RN CMakeLists call it without including it (Android loads it globally).
include("${REACT_COMMON_DIR}/cmake-utils/internal/react-native-platform-selector.cmake")

foreach(subdir IN LISTS LOOM_RN_SUBDIRS)
  add_subdirectory("${REACT_COMMON_DIR}/${subdir}" "ReactCommon/${subdir}")
endforeach()

# Yoga builds `yogacore`; the React Native CMakeLists link it as `yoga`.
add_subdirectory("${REACT_COMMON_DIR}/yoga" "ReactCommon/yoga")
add_library(yoga ALIAS yogacore)

# Most React Native libraries are OBJECT libraries that the Android build
# merges into one shared library; do the same into one static library.
set(LOOM_CORE_OBJECTS "")
set(LOOM_CORE_LIBRARIES "")
set(LOOM_CORE_INCLUDES "")
foreach(subdir IN LISTS LOOM_RN_SUBDIRS)
  get_property(targets DIRECTORY "${REACT_COMMON_DIR}/${subdir}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(target IN LISTS targets)
    get_target_property(type ${target} TYPE)
    # OBJECT libraries propagate nothing by linking; take their include
    # directories (including the platform/cxx ones) directly.
    list(APPEND LOOM_CORE_INCLUDES "$<TARGET_PROPERTY:${target},INTERFACE_INCLUDE_DIRECTORIES>")
    if(type STREQUAL "OBJECT_LIBRARY")
      list(APPEND LOOM_CORE_OBJECTS $<TARGET_OBJECTS:${target}>)
    elseif(type STREQUAL "STATIC_LIBRARY" OR type STREQUAL "SHARED_LIBRARY")
      list(APPEND LOOM_CORE_LIBRARIES ${target})
    endif()
  endforeach()
endforeach()

add_library(loom-core STATIC
    LoomHeadlessHost.cpp
    ${LOOM_CORE_OBJECTS})
target_include_directories(loom-core PUBLIC
    "${REACT_COMMON_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}"
    ${LOOM_CORE_INCLUDES})
target_link_libraries(loom-core
    PUBLIC
      ${LOOM_CORE_LIBRARIES}
      yogacore
      folly_runtime
      glog
      boost
      fast_float)
target_compile_reactnative_options(loom-core PRIVATE)

# Tests. The renderer and worker suites are the ones the Android build runs;
# they drive the core through TestUIManagerDelegate. The headless suite drives
# it through LoomHeadlessHost.
enable_testing()

add_executable(loom-core-tests
    "${REACT_COMMON_DIR}/react/renderer/uimanager/tests/LoomRendererTest.cpp"
    "${REACT_COMMON_DIR}/react/renderer/uimanager/tests/LoomWorkerTest.cpp")
target_compile_reactnative_options(loom-core-tests PRIVATE)
target_link_libraries(loom-core-tests PRIVATE loom-core GTest::gtest_main)
add_test(NAME loom-core-tests COMMAND loom-core-tests)

add_executable(loom-core-headless-tests
    tests/LoomHeadlessHostTest.cpp
    tests/LoomHeadlessBindingTest.cpp
    tests/LoomSettingsTest.cpp)
target_compile_reactnative_options(loom-core-headless-tests PRIVATE)
target_link_libraries(loom-core-headless-tests PRIVATE loom-core GTest::gtest_main)
add_test(NAME loom-core-headless-tests COMMAND loom-core-headless-tests)
