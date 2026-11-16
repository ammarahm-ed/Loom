# Loom adapter for react-native-reanimated (see ../README.md).
#
# Passed to Reanimated's build as CMAKE_PROJECT_Reanimated_INCLUDE, so it runs
# inside Reanimated's own CMakeLists.txt, right after its project() call. It
# defers itself to the end of that file, when the `reanimated` library exists,
# and puts the adapter's ReanimatedModuleProxy.cpp in the original's place
# (plus the adapter's own sources). Reanimated's CMakeLists.txt is unchanged.
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL loom_reanimated_adapter)

function(loom_reanimated_adapter)
  get_target_property(sources reanimated SOURCES)
  list(FILTER sources EXCLUDE REGEX "/NativeModules/ReanimatedModuleProxy\\.cpp$")
  set_property(TARGET reanimated PROPERTY SOURCES ${sources})
  target_sources(reanimated PRIVATE
    "${LOOM_REANIMATED_ADAPTER_DIR}/patched/${LOOM_REANIMATED_VERSION}/ReanimatedModuleProxy.cpp"
    "${LOOM_REANIMATED_ADAPTER_DIR}/cpp/LoomLayoutAnimationsProxy.cpp")
  target_include_directories(reanimated PRIVATE "${LOOM_REANIMATED_ADAPTER_DIR}/cpp")
endfunction()
