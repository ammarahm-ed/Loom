# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.13)
set(CMAKE_VERBOSE_MAKEFILE on)

include(${REACT_COMMON_DIR}/cmake-utils/react-native-flags.cmake)

# Loom's renderer module: binding/, core/, mounting/, compat/ (the tests/ folder is
# built by the test targets, not into the library).
file(GLOB_RECURSE react_renderer_loom_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
list(FILTER react_renderer_loom_SRC EXCLUDE REGEX "/tests/")
add_library(react_renderer_loom OBJECT ${react_renderer_loom_SRC})

target_include_directories(react_renderer_loom PUBLIC ${REACT_COMMON_DIR})

target_link_libraries(react_renderer_loom
        callinvoker
        glog
        folly_runtime
        jsi
        react_cxxreact
        react_cxxstableapi
        react_debug
        react_featureflags
        react_renderer_componentregistry
        react_renderer_consistency
        react_renderer_uimanager
        react_renderer_uimanager_consistency
        react_renderer_bridging
        react_renderer_core
        react_renderer_debug
        react_renderer_dom
        react_renderer_graphics
        react_renderer_leakchecker
        react_renderer_runtimescheduler
        react_renderer_mounting
        react_timing
        react_utils
        rrc_root
        rrc_view
        runtimeexecutor
)
target_compile_reactnative_options(react_renderer_loom PRIVATE)
target_compile_options(react_renderer_loom PRIVATE -Wno-unused-local-typedef)
target_compile_options(react_renderer_loom PRIVATE -Wpedantic)
