if(NOT DEFINED EDGEFLOW_SOURCE_DIR)
  message(FATAL_ERROR "EDGEFLOW_SOURCE_DIR is required")
endif()

include("${EDGEFLOW_SOURCE_DIR}/cmake/CompilerCache.cmake")

edgeflow_paths_resolve_to_same_file(
  "${CMAKE_COMMAND}" "${CMAKE_COMMAND}" same_path_result)
if(NOT same_path_result)
  message(FATAL_ERROR "An executable must resolve to the same file as itself")
endif()

edgeflow_paths_resolve_to_same_file(
  "${CMAKE_COMMAND}" "${CMAKE_CTEST_COMMAND}" different_path_result)
if(different_path_result)
  message(FATAL_ERROR "cmake and ctest must not resolve to the same file")
endif()

edgeflow_paths_resolve_to_same_file(
  "${EDGEFLOW_SOURCE_DIR}/does-not-exist"
  "${CMAKE_COMMAND}"
  missing_path_result)
if(missing_path_result)
  message(FATAL_ERROR "A missing compiler path must fail closed")
endif()

# Exercise the launcher policy itself without depending on ccache being installed.
# Treat cmake as the discovered cache executable: C is a wrapper (same real path),
# CXX is a normal compiler (different path).
set(LLM_EDGEFLOW_USE_CCACHE ON)
set(LLM_EDGEFLOW_CCACHE_EXECUTABLE "${CMAKE_COMMAND}")
set(CMAKE_C_COMPILER "${CMAKE_COMMAND}")
set(CMAKE_CXX_COMPILER "${CMAKE_CTEST_COMMAND}")
edgeflow_enable_compiler_cache()

if(CMAKE_C_COMPILER_LAUNCHER)
  message(FATAL_ERROR "A compiler wrapper must not receive another launcher")
endif()
if(NOT CMAKE_CXX_COMPILER_LAUNCHER STREQUAL "${CMAKE_COMMAND}")
  message(FATAL_ERROR "A normal compiler must receive the discovered launcher")
endif()

set(CMAKE_CXX_COMPILER_LAUNCHER "existing-launcher")
edgeflow_enable_compiler_cache()
if(NOT CMAKE_CXX_COMPILER_LAUNCHER STREQUAL "existing-launcher")
  message(FATAL_ERROR "An existing compiler launcher must be preserved")
endif()

message(STATUS "Compiler cache path detection contract passed")
