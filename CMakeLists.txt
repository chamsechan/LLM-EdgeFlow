cmake_minimum_required(VERSION 3.19)

if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_BINARY_DIR}")
  message(FATAL_ERROR "In-source builds are forbidden")
endif()

project(LLMEdgeFlow VERSION 11.0.0 LANGUAGES C CXX)

include(cmake_ext/ProjectSetup.cmake)
include(cmake_ext/Dependencies.cmake)
include(cmake_ext/company/KiteLlm.cmake)
include(cmake_ext/RuntimeGraph.cmake)
include(cmake_ext/ProductTargets.cmake)
