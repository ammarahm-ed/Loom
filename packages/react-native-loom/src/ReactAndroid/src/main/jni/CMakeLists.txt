# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.13)
set(CMAKE_VERBOSE_MAKEFILE on)

project(ReactAndroid)

include(CheckIPOSupported)
check_ipo_supported(RESULT IPO_SUPPORT)
if (IPO_SUPPORT)
  message(STATUS "LTO support is enabled")
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

include(${REACT_COMMON_DIR}/cmake-utils/react-native-flags.cmake)

# Convert input paths to CMake format (with forward slashes)
file(TO_CMAKE_PATH "${REACT_ANDROID_DIR}" REACT_ANDROID_DIR)
file(TO_CMAKE_PATH "${REACT_BUILD_DIR}" REACT_BUILD_DIR)
file(TO_CMAKE_PATH "${REACT_COMMON_DIR}" REACT_COMMON_DIR)

# If you have ccache installed, we're going to honor it.
find_program(CCACHE_FOUND ccache)
if(CCACHE_FOUND)
  set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ccache)
  set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK ccache)
endif(CCACHE_FOUND)

function(add_react_android_subdir relative_path)
  add_subdirectory(${REACT_ANDROID_DIR}/${relative_path} ReactAndroid/${relative_path})
endfunction()

function(add_react_build_subdir relative_path)
  add_subdirectory(${REACT_BUILD_DIR}/${relative_path} build/${relative_path})
endfunction()

function(add_react_third_party_ndk_subdir relative_path)
  add_react_build_subdir(third-party-ndk/${relative_path})
endfunction()

function(add_react_common_subdir relative_path)
  add_subdirectory(${REACT_COMMON_DIR}/${relative_path} ReactCommon/${relative_path})
endfunction()

# Third-party prefabs
find_package(hermes-engine REQUIRED CONFIG)
# Loom: when hermes-engine is built from source as a sibling Gradle project, AGP 9 hands its prefab
# to this configure step as a header-only package (no libhermesvm.so), and libhermestooling then
# fails to link. Link the library from the hermes-engine build output in that case; Gradle passes
# its build directory and makes these tasks wait for hermes-engine's native build. A prebuilt Hermes
# (react.internal.useHermesStable, RN's release path) is an imported target with its own location:
# nothing to add then.
get_target_property(LOOM_HERMES_LINK_LIBRARIES hermes-engine::hermesvm INTERFACE_LINK_LIBRARIES)
get_target_property(LOOM_HERMES_IMPORTED_LOCATION hermes-engine::hermesvm IMPORTED_LOCATION)
if(NOT LOOM_HERMES_LINK_LIBRARIES AND NOT LOOM_HERMES_IMPORTED_LOCATION AND DEFINED LOOM_HERMES_ENGINE_BUILD_DIR)
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(LOOM_HERMES_VARIANT debug)
  else()
    set(LOOM_HERMES_VARIANT release)
  endif()
  set(LOOM_HERMES_LIB
      "${LOOM_HERMES_ENGINE_BUILD_DIR}/intermediates/cmake/${LOOM_HERMES_VARIANT}/obj/${ANDROID_ABI}/libhermesvm.so")
  set_property(TARGET hermes-engine::hermesvm APPEND PROPERTY INTERFACE_LINK_LIBRARIES "${LOOM_HERMES_LIB}")
endif()
find_package(fbjni REQUIRED CONFIG)
add_library(fbjni ALIAS fbjni::fbjni)

# Third-party downloaded targets
add_react_third_party_ndk_subdir(glog)
add_react_third_party_ndk_subdir(boost)
add_react_third_party_ndk_subdir(double-conversion)
add_react_third_party_ndk_subdir(fast_float)
add_react_third_party_ndk_subdir(fmt)
add_react_third_party_ndk_subdir(folly)

# Marks everything added below as React Native's own build, so the
# react/cxxstableapi guards let internal sources keep including the
# fine-grained headers they fence off from consumers. Deliberately declared
# after the third-party subdirectories so it never reaches them, and this
# project never compiles app or third-party module code (those build against
# the prefab artifacts through ReactNative-application.cmake).
add_compile_definitions(RN_BUILDING)

# Common targets
add_react_common_subdir(yoga)
add_react_common_subdir(runtimeexecutor)
add_react_common_subdir(reactperflogger)
add_react_common_subdir(logger)
add_react_common_subdir(jsiexecutor)
add_react_common_subdir(jsitooling)
add_react_common_subdir(cxxreact)
add_react_common_subdir(jsi)
add_react_common_subdir(callinvoker)
add_react_common_subdir(oscompat)
add_react_common_subdir(jsinspector-modern)
add_react_common_subdir(jsinspector-modern/cdp)
add_react_common_subdir(jsinspector-modern/network)
add_react_common_subdir(jsinspector-modern/tracing)
add_react_common_subdir(hermes/executor)
add_react_common_subdir(hermes/inspector-modern)
add_react_common_subdir(react/cxxstableapi)
add_react_common_subdir(react/renderer/runtimescheduler)
add_react_common_subdir(react/debug)
add_react_common_subdir(react/featureflags)
add_react_common_subdir(react/performance/cdpmetrics)
add_react_common_subdir(react/performance/timeline)
add_react_common_subdir(react/renderer/animated)
add_react_common_subdir(react/renderer/animationbackend)
add_react_common_subdir(react/renderer/animations)
add_react_common_subdir(react/renderer/attributedstring)
add_react_common_subdir(react/renderer/componentregistry)
add_react_common_subdir(react/renderer/mounting)
add_react_common_subdir(react/renderer/scheduler)
add_react_common_subdir(react/renderer/telemetry)
add_react_common_subdir(react/renderer/viewtransition)
add_react_common_subdir(react/renderer/uimanager)
add_react_common_subdir(react/renderer/bridging)
add_react_common_subdir(react/renderer/core)
add_react_common_subdir(react/renderer/consistency)
add_react_common_subdir(react/renderer/css)
add_react_common_subdir(react/renderer/uimanager/consistency)
add_react_common_subdir(react/renderer/dom)
add_react_common_subdir(react/renderer/element)
add_react_common_subdir(react/renderer/debug)
add_react_common_subdir(react/renderer/graphics)
add_react_common_subdir(react/renderer/imagemanager)
add_react_common_subdir(react/renderer/components/view)
add_react_common_subdir(react/renderer/components/switch)
add_react_common_subdir(react/renderer/components/textinput)
add_react_common_subdir(react/renderer/components/progressbar)
add_react_common_subdir(react/renderer/components/root)
add_react_common_subdir(react/renderer/components/image)
add_react_common_subdir(react/renderer/components/legacyviewmanagerinterop)
add_react_common_subdir(react/renderer/componentregistry/native)
add_react_common_subdir(react/renderer/components/text)
add_react_common_subdir(react/renderer/components/unimplementedview)
add_react_common_subdir(react/renderer/components/modal)
add_react_common_subdir(react/renderer/components/scrollview)
add_react_common_subdir(react/renderer/components/safeareaview)
add_react_common_subdir(react/renderer/leakchecker)
add_react_common_subdir(react/renderer/observers/events)
add_react_common_subdir(react/renderer/observers/intersection)
add_react_common_subdir(react/renderer/observers/mutation)
add_react_common_subdir(react/renderer/observers/resize)
add_react_common_subdir(react/renderer/textlayoutmanager)
add_react_common_subdir(react/utils)
add_react_common_subdir(react/bridging)
add_react_common_subdir(react/renderer/mapbuffer)
add_react_common_subdir(react/nativemodule/core)
add_react_common_subdir(react/nativemodule/defaults)
add_react_common_subdir(react/nativemodule/dom)
add_react_common_subdir(react/nativemodule/featureflags)
add_react_common_subdir(react/nativemodule/microtasks)
add_react_common_subdir(react/nativemodule/idlecallbacks)
add_react_common_subdir(react/nativemodule/intersectionobserver)
add_react_common_subdir(react/nativemodule/mutationobserver)
add_react_common_subdir(react/nativemodule/resizeobserver)
add_react_common_subdir(react/nativemodule/viewtransition)
add_react_common_subdir(react/nativemodule/webperformance)
add_react_common_subdir(react/networking)
add_react_common_subdir(jserrorhandler)
add_react_common_subdir(react/runtime)
add_react_common_subdir(react/runtime/hermes)
add_react_common_subdir(react/runtime/nativeviewconfig)
add_react_common_subdir(react/timing)

# ReactAndroid JNI targets
add_react_build_subdir(generated/source/codegen/jni)
add_react_android_subdir(src/main/jni/first-party/fbgloginit)
add_react_android_subdir(src/main/jni/first-party/yogajni)
add_react_android_subdir(src/main/jni/first-party/jni-lib-merge)
add_react_android_subdir(src/main/jni/react/jni)
add_react_android_subdir(src/main/jni/react/reactperflogger)
add_react_android_subdir(src/main/jni/react/turbomodule)
add_react_android_subdir(src/main/jni/react/uimanager)
add_react_android_subdir(src/main/jni/react/mapbuffer)
add_react_android_subdir(src/main/jni/react/reactnativeblob)
add_react_android_subdir(src/main/jni/react/fabric)
add_react_android_subdir(src/main/jni/react/featureflags)
add_react_android_subdir(src/main/jni/react/newarchdefaults)
add_react_android_subdir(src/main/jni/react/hermes/reactexecutor)
add_react_android_subdir(src/main/jni/react/hermes/tooling)
add_react_android_subdir(src/main/jni/react/hermes/instrumentation/)
add_react_android_subdir(src/main/jni/react/runtime/cxxreactpackage)
add_react_android_subdir(src/main/jni/react/runtime/jni)
add_react_android_subdir(src/main/jni/react/runtime/hermes/jni)
add_react_android_subdir(src/main/jni/react/tracing)
add_react_android_subdir(src/main/jni/react/devsupport)

# Optional native test target. It is deliberately outside libreactnative and is disabled unless
# explicitly requested with -P LOOM_BUILD_CPP_TESTS=true. The NDK supplies GoogleTest, so enabling
# this target does not add a repository dependency or change the production graph.
if(LOOM_BUILD_CPP_TESTS)
  # These suites exercise LoomRenderer and its worker, which are only compiled
  # when the Loom renderer is selected. A flag-off build would fail to link
  # with undefined LoomRenderer symbols, so reject it here with a clear
  # message instead.
  if(NOT LOOM_RENDERER)
    message(FATAL_ERROR
        "LOOM_BUILD_CPP_TESTS=true requires LOOM_RENDERER=true: the Loom renderer "
        "test suites do not link against a flag-off build.")
  endif()
  # LoomRendererTest.cpp drives the publication API (publishWorkerCommit,
  # applyPublishedIdentities, the submitted/mounted identity maps) without any
  # guard, and the binding only declares those members under
  # LOOM_WORKER. Without this check the configuration fails with a
  # page of "no member named ..." compile errors instead.
  if(NOT LOOM_WORKER)
    message(FATAL_ERROR
        "LOOM_BUILD_CPP_TESTS=true requires LOOM_WORKER=true: "
        "LoomRendererTest.cpp exercises the worker publication API.")
  endif()

  set(ANDROID_GTEST_DIR "${CMAKE_ANDROID_NDK}/sources/third_party/googletest")
  if(NOT EXISTS "${ANDROID_GTEST_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "LOOM_BUILD_CPP_TESTS=true requires the NDK GoogleTest sources")
  endif()

  set(gtest_build_tests OFF CACHE BOOL "" FORCE)
  set(gtest_build_samples OFF CACHE BOOL "" FORCE)
  # The NDK copy expects the embedding project to provide this value; its
  # CMakeLists uses it for both project() and target VERSION metadata.
  set(GOOGLETEST_VERSION 1.14.0 CACHE STRING "GoogleTest version")
  add_subdirectory("${ANDROID_GTEST_DIR}" "third-party/googletest")

  add_executable(ui_thread_renderer_tests EXCLUDE_FROM_ALL
          ${REACT_COMMON_DIR}/react/renderer/uimanager/tests/LoomRendererTest.cpp
          ${REACT_COMMON_DIR}/react/renderer/uimanager/tests/LoomWorkerTest.cpp
  )
  target_include_directories(ui_thread_renderer_tests PRIVATE ${REACT_COMMON_DIR})
  target_compile_reactnative_options(ui_thread_renderer_tests PRIVATE)
  # `react_renderer_uimanager` is an OBJECT library, so the test executable has
  # to name its dependencies itself: an object library does not propagate them.
  # The publication tests construct a real UIManager, which is what needs core,
  # the descriptor registries and the Root/View descriptors.
  target_link_libraries(ui_thread_renderer_tests
          gtest_main
          react_renderer_attributedstring
          react_renderer_uimanager
          react_renderer_componentregistry
          react_renderer_consistency
          react_renderer_uimanager_consistency
          react_renderer_core
          react_renderer_dom
          react_renderer_graphics
          react_renderer_mounting
          react_renderer_runtimescheduler
          react_featureflags
          logger
          oscompat
          react_renderer_telemetry
          rrc_legacyviewmanagerinterop
          rrc_root
          rrc_view
  )
  add_test(NAME ui_thread_renderer_tests COMMAND ui_thread_renderer_tests)

endif()

# SoMerging Utils
include(${REACT_ANDROID_DIR}/src/main/jni/first-party/jni-lib-merge/SoMerging-utils.cmake)

# Export stubs (loom.exportStubsDir, every flavor under the Loom renderer): Fabric's renderer code is not
# compiled (below), but every name React Native's own libreactnative.so of the same flavor exports stays
# exported: tools/prebuild/gen-export-stubs.py writes a weak, zero-returning stub for each name no real
# code defines, in two passes (pass 1 learns which; the final link adds them), per flavor.
# Lean release (loom.leanRelease, release only) also drops the inspector, DevTools and tracing code;
# debug, debugOptimized and release profiling keep it (React Native's REACT_NATIVE_DEBUGGER_ENABLED condition).
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(LOOM_VARIANT debug)
elseif(REACT_NATIVE_DEBUG_OPTIMIZED AND NOT LOOM_RELEASE_PROFILING)
  set(LOOM_VARIANT debugOptimized)
else()
  set(LOOM_VARIANT release)
endif()
set(LOOM_STUBS OFF)
if(LOOM_RENDERER AND LOOM_EXPORT_STUBS_DIR)
  set(LOOM_STUBS ON)
  set(LOOM_STUBS_VARIANT_DIR "${LOOM_EXPORT_STUBS_DIR}/${LOOM_VARIANT}")
endif()
set(LOOM_LEAN OFF)
if(LOOM_STUBS AND LOOM_LEAN_RELEASE AND LOOM_VARIANT STREQUAL "release")
  set(LOOM_LEAN ON)
endif()
set(LOOM_CUT_FABRIC OFF)
if(LOOM_STUBS)
  set(LOOM_CUT_FABRIC ON)
  enable_language(ASM)
  if(NOT EXISTS "${LOOM_STUBS_VARIANT_DIR}/${ANDROID_ABI}/loom_export_stubs.s")
    message(FATAL_ERROR "loom.exportStubsDir needs ${LOOM_STUBS_VARIANT_DIR}/${ANDROID_ABI}/loom_export_stubs.s")
  endif()
  function(loom_cut_sources target regex)
    get_target_property(srcs ${target} SOURCES)
    list(FILTER srcs EXCLUDE REGEX "${regex}")
    set_property(TARGET ${target} PROPERTY SOURCES ${srcs})
  endfunction()
  # Fabric's renderer under the Loom renderer: Loom mounts, diffs and animates through its own renderer, so
  # the code that does it for Fabric is not compiled (the object libraries of animations and viewtransition
  # are left out of the link below). The callers Loom keeps were rerouted in source (ShadowTree.cpp,
  # MountingCoordinator.cpp, FabricMountingManager.cpp, ...).
  loom_cut_sources(react_renderer_mounting
    "/(Differentiator|ShadowViewMutation|updateMountedFlag|StubView|StubViewTree|stubs|sliceChildShadowNodeViewPairs|CullingContext)\\.cpp$")
  # mode.txt is a configure dependency, so switching passes reconfigures and relinks only.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${LOOM_STUBS_VARIANT_DIR}/mode.txt)
  file(READ ${LOOM_STUBS_VARIANT_DIR}/mode.txt LOOM_STUBS_MODE)
  string(STRIP "${LOOM_STUBS_MODE}" LOOM_STUBS_MODE)
  # The assembler is handed the C++ flags (-Werror among them); none of them apply to it.
  add_library(loom_export_stubs OBJECT ${LOOM_STUBS_VARIANT_DIR}/${ANDROID_ABI}/loom_export_stubs.s)
  target_compile_options(loom_export_stubs PRIVATE -Wno-unused-command-line-argument)
endif()
if(LOOM_LEAN)
  # What stays of the inspector library: the flag singleton and the fallback runtime delegate (released
  # code constructs it), plus LoomLeanStubs.cpp (the zero-filled PerformanceTracer instance).
  get_target_property(LOOM_INSPECTOR_SRCS jsinspector SOURCES)
  list(FILTER LOOM_INSPECTOR_SRCS INCLUDE REGEX "(FallbackRuntimeTargetDelegate|InspectorFlags|LoomLeanStubs)\\.cpp$")
  set_property(TARGET jsinspector PROPERTY SOURCES ${LOOM_INSPECTOR_SRCS})
  target_compile_definitions(jsinspector PRIVATE LOOM_LEAN_RELEASE=1)
endif()

# Release profiling (loom.releaseProfiling): the release flavor with DevTools and tracing, React Native's
# profiling build mode. REACT_NATIVE_DEBUG_OPTIMIZED (set by the build for this flavor) switches the
# inspector code on everywhere; this flag marks the build as a production profiling build.
if(LOOM_RELEASE_PROFILING)
  target_compile_definitions(jsinspector PRIVATE REACT_NATIVE_DEBUGGER_MODE_PROD=1)
  # The Hermes a release app runs has no Chrome DevTools Protocol agent (the debug one does), and
  # REACT_NATIVE_DEBUG_OPTIMIZED turns the CDP bridge on: leave it out. The inspector, tracing and
  # network DevTools stay; the JavaScript debugger domains need a debug Hermes.
  foreach(LOOM_HERMES_TARGET hermes_inspector_modern hermes_executor_common)
    if(TARGET ${LOOM_HERMES_TARGET})
      target_compile_options(${LOOM_HERMES_TARGET} PRIVATE -UHERMES_ENABLE_DEBUGGER)
    endif()
  endforeach()
endif()

# libreactnative.so Dependency
#
# The React Native Android Library (the .aar) should expose headers only via this target.
# This is needed in order to reduce the number of .so files that we ship in the final library
add_library(reactnative
        SHARED
          $<TARGET_OBJECTS:bridgeless>
          $<TARGET_OBJECTS:bridgelessnativeviewconfig>
          $<TARGET_OBJECTS:callinvokerholder>
          $<TARGET_OBJECTS:fabricjni>
          $<TARGET_OBJECTS:glog_init>
          $<TARGET_OBJECTS:jni_lib_merge_glue>
          $<TARGET_OBJECTS:jserrorhandler>
          $<TARGET_OBJECTS:jsinspector>
          $<TARGET_OBJECTS:jsitooling>
          $<$<NOT:$<BOOL:${LOOM_LEAN}>>:$<TARGET_OBJECTS:jsinspector_cdp>>
          $<$<NOT:$<BOOL:${LOOM_LEAN}>>:$<TARGET_OBJECTS:jsinspector_network>>
          $<$<NOT:$<BOOL:${LOOM_LEAN}>>:$<TARGET_OBJECTS:jsinspector_tracing>>
          $<TARGET_OBJECTS:jsireact>
          $<TARGET_OBJECTS:logger>
          $<TARGET_OBJECTS:mapbufferjni>
          $<TARGET_OBJECTS:oscompat>
          $<TARGET_OBJECTS:react_bridging>
          $<TARGET_OBJECTS:react_codegen_rncore>
          $<TARGET_OBJECTS:react_cxxreact>
          $<TARGET_OBJECTS:react_debug>
          $<TARGET_OBJECTS:react_devsupportjni>
          $<TARGET_OBJECTS:react_featureflags>
          $<TARGET_OBJECTS:react_featureflagsjni>
          $<TARGET_OBJECTS:react_nativemodule_core>
          $<TARGET_OBJECTS:react_nativemodule_defaults>
          $<TARGET_OBJECTS:react_nativemodule_dom>
          $<TARGET_OBJECTS:react_nativemodule_featureflags>
          $<TARGET_OBJECTS:react_nativemodule_idlecallbacks>
          $<TARGET_OBJECTS:react_nativemodule_intersectionobserver>
          $<TARGET_OBJECTS:react_nativemodule_microtasks>
          $<TARGET_OBJECTS:react_nativemodule_mutationobserver>
          $<TARGET_OBJECTS:react_nativemodule_resizeobserver>
          $<TARGET_OBJECTS:react_nativemodule_viewtransition>
          $<TARGET_OBJECTS:react_nativemodule_webperformance>
          $<TARGET_OBJECTS:react_networking>
          $<TARGET_OBJECTS:react_newarchdefaults>
          $<TARGET_OBJECTS:react_performance_cdpmetrics>
          $<TARGET_OBJECTS:react_performance_timeline>
          $<TARGET_OBJECTS:react_renderer_animated>
          $<TARGET_OBJECTS:react_renderer_animationbackend>
          $<$<NOT:$<BOOL:${LOOM_CUT_FABRIC}>>:$<TARGET_OBJECTS:react_renderer_animations>>
          $<TARGET_OBJECTS:react_renderer_attributedstring>
          $<TARGET_OBJECTS:react_renderer_componentregistry>
          $<TARGET_OBJECTS:react_renderer_consistency>
          $<TARGET_OBJECTS:react_renderer_core>
          $<TARGET_OBJECTS:react_renderer_debug>
          $<TARGET_OBJECTS:react_renderer_dom>
          $<TARGET_OBJECTS:react_renderer_element>
          $<TARGET_OBJECTS:react_renderer_graphics>
          $<TARGET_OBJECTS:react_renderer_imagemanager>
          $<TARGET_OBJECTS:react_renderer_leakchecker>
          $<TARGET_OBJECTS:react_renderer_mapbuffer>
          $<TARGET_OBJECTS:react_renderer_mounting>
          $<TARGET_OBJECTS:react_renderer_observers_events>
          $<TARGET_OBJECTS:react_renderer_observers_intersection>
          $<TARGET_OBJECTS:react_renderer_observers_mutation>
          $<TARGET_OBJECTS:react_renderer_observers_resize>
          $<TARGET_OBJECTS:react_renderer_runtimescheduler>
          $<TARGET_OBJECTS:react_renderer_scheduler>
          $<TARGET_OBJECTS:react_renderer_telemetry>
          $<TARGET_OBJECTS:react_renderer_textlayoutmanager>
          $<TARGET_OBJECTS:react_renderer_uimanager>
          $<TARGET_OBJECTS:react_renderer_uimanager_consistency>
          $<$<NOT:$<BOOL:${LOOM_CUT_FABRIC}>>:$<TARGET_OBJECTS:react_renderer_viewtransition>>
          $<TARGET_OBJECTS:react_tracingjni>
          $<TARGET_OBJECTS:react_utils>
          $<TARGET_OBJECTS:reactnativeblob>
          $<TARGET_OBJECTS:reactnativejni>
          $<TARGET_OBJECTS:reactnativejni_common>
          $<TARGET_OBJECTS:reactperflogger>
          $<TARGET_OBJECTS:rninstance>
          $<TARGET_OBJECTS:rrc_image>
          $<TARGET_OBJECTS:rrc_legacyviewmanagerinterop>
          $<TARGET_OBJECTS:rrc_modal>
          $<TARGET_OBJECTS:rrc_native>
          $<TARGET_OBJECTS:rrc_progressbar>
          $<TARGET_OBJECTS:rrc_root>
          $<TARGET_OBJECTS:rrc_safeareaview>
          $<TARGET_OBJECTS:rrc_scrollview>
          $<TARGET_OBJECTS:rrc_switch>
          $<TARGET_OBJECTS:rrc_text>
          $<TARGET_OBJECTS:rrc_textinput>
          $<TARGET_OBJECTS:rrc_view>
          $<TARGET_OBJECTS:runtimeexecutor>
          $<TARGET_OBJECTS:turbomodulejsijni>
          $<TARGET_OBJECTS:uimanagerjni>
          $<TARGET_OBJECTS:yoga>
          $<$<BOOL:${LOOM_STUBS}>:$<TARGET_OBJECTS:loom_export_stubs>>
)
target_merge_so(reactnative)
# Size, no behaviour: dynamic relocations in Android's packed format (read by the dynamic linker since
# API 23, our minSdk is 24) and identical-code folding that keeps address-taken functions distinct
# (--icf=safe), neither of which changes what the library exports.
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_link_options(reactnative PRIVATE -Wl,--pack-dyn-relocs=android -Wl,--icf=safe)
endif()
if(LOOM_STUBS AND LOOM_STUBS_MODE STREQUAL "pass1")
  target_link_options(reactnative PRIVATE -Wl,--unresolved-symbols=ignore-all)
endif()

if(LOOM_BUILD_CPP_TESTS)
  add_dependencies(reactnative ui_thread_renderer_tests)
endif()

target_link_libraries(reactnative
          PUBLIC
            android
            double-conversion
            fbjni
            folly_runtime
            glog
            jsi
            log
            yogacore
)

target_compile_reactnative_options(reactnative PRIVATE)

target_include_directories(reactnative
        PUBLIC
        $<TARGET_PROPERTY:bridgeless,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:bridgelessnativeviewconfig,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:callinvokerholder,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:fabricjni,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:glog_init,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:jserrorhandler,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:jsinspector,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:jsinspector_cdp,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:jsinspector_network,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:jsinspector_tracing,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:jsireact,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:mapbufferjni,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_bridging,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_codegen_rncore,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_cxxreact,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_debug,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_devsupportjni,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_featureflags,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_featureflagsjni,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_nativemodule_core,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_nativemodule_defaults,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_nativemodule_dom,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_nativemodule_featureflags,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_nativemodule_idlecallbacks,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_nativemodule_intersectionobserver,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_nativemodule_microtasks,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_nativemodule_mutationobserver,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_nativemodule_resizeobserver,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_nativemodule_viewtransition,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_nativemodule_webperformance,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_networking,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_newarchdefaults,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_performance_cdpmetrics,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_performance_timeline,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_renderer_animated,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_renderer_animationbackend,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_renderer_animations,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_renderer_attributedstring,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_renderer_componentregistry,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_renderer_componentregistry,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_renderer_consistency,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_renderer_core,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_renderer_css,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_renderer_debug,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_renderer_dom,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_renderer_element,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_renderer_graphics,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_renderer_imagemanager,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_renderer_leakchecker,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_renderer_mapbuffer,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_renderer_mounting,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_renderer_observers_events,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_renderer_observers_intersection,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_renderer_observers_mutation,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_renderer_observers_resize,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_renderer_runtimescheduler,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_renderer_scheduler,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_renderer_telemetry,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_renderer_textlayoutmanager,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_renderer_uimanager,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_renderer_uimanager_consistency,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_renderer_viewtransition,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_tracingjni,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:react_utils,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:reactnativeblob,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:reactnativejni,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:reactnativejni_common,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:reactperflogger,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:rninstance,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:rrc_image,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:rrc_legacyviewmanagerinterop,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:rrc_modal,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:rrc_native,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:rrc_progressbar,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:rrc_root,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:rrc_safeareaview,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:rrc_scrollview,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:rrc_switch,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:rrc_text,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:rrc_textinput,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:rrc_view,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:runtimeexecutor,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:turbomodulejsijni,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:uimanagerjni,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:yoga,INTERFACE_INCLUDE_DIRECTORIES>
)

if(${CMAKE_BUILD_TYPE} MATCHES Debug OR REACT_NATIVE_DEBUG_OPTIMIZED)
  target_compile_options(reactnative PRIVATE
          -DREACT_NATIVE_DEBUGGER_ENABLED=1
          -DREACT_NATIVE_DEBUGGER_ENABLED_DEVONLY=1
  )
endif ()
