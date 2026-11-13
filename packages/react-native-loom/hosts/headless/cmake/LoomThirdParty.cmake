# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Third-party dependencies for the core, at the versions React Native pins.
#
# React Native's Android build prepares them as `third-party-ndk` (ReactAndroid
# build.gradle.kts: download a release tarball, copy the sources into a fixed
# layout next to RN's CMakeLists under ReactAndroid/src/main/jni/third-party).
# This does the same at configure time with FetchContent, then adds RN's own
# CMakeLists, so folly, glog, fmt, double-conversion and fast_float are built
# from the same sources, with the same flags, as in production. Boost is
# header-only here. Versions come from the materialized tree's
# gradle/libs.versions.toml.
#
# With -DLOOM_USE_SYSTEM_DEPS=ON the system packages (Homebrew and the like) are
# used instead. Their versions may not match what React Native pins.
#
# Defines the targets folly_runtime, glog, glog_init, boost, fast_float, and
# GTest::gtest_main.

include(FetchContent)

option(LOOM_USE_SYSTEM_DEPS "Use system packages instead of React Native's pinned versions" OFF)

if(LOOM_USE_SYSTEM_DEPS)
  find_package(folly CONFIG REQUIRED)
  find_package(glog CONFIG REQUIRED)
  find_package(fmt CONFIG REQUIRED)
  find_package(Boost REQUIRED)
  find_package(GTest REQUIRED)
  find_path(FAST_FLOAT_INCLUDE_DIR fast_float/fast_float.h REQUIRED)

  add_library(folly_runtime INTERFACE)
  target_link_libraries(folly_runtime INTERFACE Folly::folly fmt::fmt)
  add_library(glog_target INTERFACE)
  target_link_libraries(glog_target INTERFACE glog::glog)
  add_library(glog ALIAS glog_target)
  add_library(glog_init INTERFACE)
  add_library(boost INTERFACE)
  target_link_libraries(boost INTERFACE Boost::headers)
  add_library(fast_float INTERFACE)
  target_include_directories(fast_float INTERFACE "${FAST_FLOAT_INCLUDE_DIR}")
  return()
endif()

set(LOOM_TP_RN "${LOOM_REACT_NATIVE_DIR}/ReactAndroid/src/main/jni/third-party")
set(LOOM_TP_OUT "${CMAKE_BINARY_DIR}/third-party-ndk")

function(loom_pinned_version key out)
  file(STRINGS "${LOOM_REACT_NATIVE_DIR}/gradle/libs.versions.toml" line
       REGEX "^${key}[ ]*=")
  if(NOT line)
    message(FATAL_ERROR "No '${key}' version in gradle/libs.versions.toml")
  endif()
  string(REGEX REPLACE "^[^=]*=[ ]*\"([^\"]*)\".*$" "\\1" version "${line}")
  set(${out} "${version}" PARENT_SCOPE)
endfunction()

loom_pinned_version(boost BOOST_VERSION)
loom_pinned_version(doubleconversion DOUBLE_CONVERSION_VERSION)
loom_pinned_version(fastFloat FAST_FLOAT_VERSION)
loom_pinned_version(fmt FMT_VERSION)
loom_pinned_version(folly FOLLY_VERSION)
loom_pinned_version(glog GLOG_VERSION)
string(REPLACE "_" "." BOOST_DOTTED "${BOOST_VERSION}")

# Sources only: SOURCE_SUBDIR points at a directory that has no CMakeLists, so
# MakeAvailable downloads and extracts without adding the project.
# Uses the archive React Native's Gradle build already downloaded, when present.
macro(loom_fetch name url file)
  set(loom_fetch_url "${url}")
  if(EXISTS "${LOOM_REACT_NATIVE_DIR}/ReactAndroid/build/downloads/${file}")
    set(loom_fetch_url "${LOOM_REACT_NATIVE_DIR}/ReactAndroid/build/downloads/${file}")
  endif()
  FetchContent_Declare(loom_${name}
      URL "${loom_fetch_url}"
      SOURCE_SUBDIR loom-no-cmake
      DOWNLOAD_EXTRACT_TIMESTAMP ON)
  FetchContent_MakeAvailable(loom_${name})
endmacro()

# The URLs are the ones ReactAndroid/build.gradle.kts downloads.
loom_fetch(boost "https://archives.boost.io/release/${BOOST_DOTTED}/source/boost_${BOOST_VERSION}.tar.gz" "boost_${BOOST_VERSION}.tar.gz")
loom_fetch(double_conversion "https://github.com/google/double-conversion/archive/v${DOUBLE_CONVERSION_VERSION}.tar.gz" "double-conversion-${DOUBLE_CONVERSION_VERSION}.tar.gz")
loom_fetch(folly "https://github.com/facebook/folly/archive/v${FOLLY_VERSION}.tar.gz" "folly-${FOLLY_VERSION}.tar.gz")
loom_fetch(fast_float "https://github.com/fastfloat/fast_float/archive/v${FAST_FLOAT_VERSION}.tar.gz" "fast_float-${FAST_FLOAT_VERSION}.tar.gz")
loom_fetch(fmt "https://github.com/fmtlib/fmt/archive/${FMT_VERSION}.tar.gz" "fmt-${FMT_VERSION}.tar.gz")
loom_fetch(glog "https://github.com/google/glog/archive/v${GLOG_VERSION}.tar.gz" "glog-${GLOG_VERSION}.tar.gz")

# Lay the sources out as `third-party-ndk` does, next to RN's CMakeLists.
if(NOT EXISTS "${LOOM_TP_OUT}/.prepared-${FOLLY_VERSION}")
  file(REMOVE_RECURSE "${LOOM_TP_OUT}")

  # folly
  file(COPY "${loom_folly_SOURCE_DIR}/folly" DESTINATION "${LOOM_TP_OUT}/folly"
       PATTERN "test" EXCLUDE PATTERN "docs" EXCLUDE)
  file(READ "${LOOM_TP_RN}/folly/CMakeLists.txt" folly_cmake)
  if(APPLE)
    # Android flag with no libc counterpart on Apple platforms.
    string(REPLACE "-DFOLLY_HAVE_RECVMMSG=1" "" folly_cmake "${folly_cmake}")
  endif()
  file(WRITE "${LOOM_TP_OUT}/folly/CMakeLists.txt" "${folly_cmake}")

  # double-conversion: the sources live in src/ and are laid out flat.
  file(GLOB dc_sources "${loom_double_conversion_SOURCE_DIR}/src/*")
  file(COPY ${dc_sources} DESTINATION "${LOOM_TP_OUT}/double-conversion/double-conversion")
  file(COPY "${LOOM_TP_RN}/double-conversion/CMakeLists.txt"
       DESTINATION "${LOOM_TP_OUT}/double-conversion")

  # fast_float
  file(COPY "${loom_fast_float_SOURCE_DIR}/include" DESTINATION "${LOOM_TP_OUT}/fast_float")
  file(COPY "${LOOM_TP_RN}/fast_float/CMakeLists.txt" DESTINATION "${LOOM_TP_OUT}/fast_float")

  # fmt
  file(COPY "${loom_fmt_SOURCE_DIR}/src" "${loom_fmt_SOURCE_DIR}/include"
       DESTINATION "${LOOM_TP_OUT}/fmt")
  file(COPY "${LOOM_TP_RN}/fmt/CMakeLists.txt" DESTINATION "${LOOM_TP_OUT}/fmt")

  # glog: the sources, RN's config.h, and the headers PrepareGlogTask generates
  # from the *.h.in templates (the same token values).
  set(glog_root "${LOOM_TP_OUT}/glog")
  file(COPY "${loom_glog_SOURCE_DIR}/src" DESTINATION "${glog_root}/glog-${GLOG_VERSION}")
  file(COPY "${LOOM_TP_RN}/glog/CMakeLists.txt" "${LOOM_TP_RN}/glog/config.h"
       DESTINATION "${glog_root}")
  set(ac_cv_have_unistd_h 1)
  set(ac_cv_have_stdint_h 1)
  set(ac_cv_have_systypes_h 1)
  set(ac_cv_have_inttypes_h 1)
  set(ac_cv_have_libgflags 0)
  set(ac_google_start_namespace "namespace google {")
  set(ac_cv_have_uint16_t 1)
  set(ac_cv_have_u_int16_t 1)
  set(ac_cv_have___uint16 0)
  set(ac_google_end_namespace "}")
  set(ac_cv_have___builtin_expect 1)
  set(ac_google_namespace "google")
  set(ac_cv___attribute___noinline "__attribute__ ((noinline))")
  set(ac_cv___attribute___noreturn "__attribute__ ((noreturn))")
  set(ac_cv___attribute___printf_4_5 "__attribute__((__format__ (__printf__, 4, 5)))")
  set(glog_headers "${glog_root}/glog-${GLOG_VERSION}/src/glog")
  file(MAKE_DIRECTORY "${glog_root}/exported/glog")
  foreach(header logging raw_logging stl_logging vlog_is_on)
    configure_file("${glog_headers}/${header}.h.in" "${glog_headers}/${header}.h" @ONLY)
    file(COPY "${glog_headers}/${header}.h" DESTINATION "${glog_root}/exported/glog")
  endforeach()
  file(COPY "${glog_headers}/log_severity.h" DESTINATION "${glog_root}/exported/glog")

  file(WRITE "${LOOM_TP_OUT}/.prepared-${FOLLY_VERSION}" "")
endif()

# Boost is header-only for the core.
add_library(boost INTERFACE)
target_include_directories(boost INTERFACE "${loom_boost_SOURCE_DIR}")

# glog_init is an Android-only initializer library.
add_library(glog_init INTERFACE)

foreach(dependency double-conversion fast_float fmt glog folly)
  add_subdirectory("${LOOM_TP_OUT}/${dependency}" "third-party/${dependency}")
endforeach()

# RN builds these with -Werror; a newer host compiler may warn in code that is
# not ours. Scope the relaxation to the third-party targets.
foreach(target double-conversion fmt glog folly_runtime)
  target_compile_options(${target} PRIVATE -Wno-error)
endforeach()

# GoogleTest, at the version the NDK ships (what the Android test target uses).
FetchContent_Declare(googletest
    URL "https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz"
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
