cmake_minimum_required(VERSION 3.13)
project(loomexample)

set(CMAKE_CXX_STANDARD 20)

find_package(ReactAndroid REQUIRED CONFIG)
find_package(fbjni REQUIRED CONFIG)

add_library(loomexample SHARED
  OnLoad.cpp
  ../../../../cpp/LoomExample.cpp)

target_include_directories(loomexample PRIVATE ../../../../cpp)

target_link_libraries(loomexample
  ReactAndroid::reactnative
  ReactAndroid::jsi
  fbjni::fbjni
  android
  log)
