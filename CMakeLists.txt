cmake_minimum_required(VERSION 3.19)

if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_BINARY_DIR}")
  message(FATAL_ERROR "In-source builds are forbidden")
endif()

project(LLMEdgeFlow VERSION 11.0.0 LANGUAGES C CXX)

include(cmake_ext/ProjectSetup.cmake)
if(ENABLE_INFERLET AND ENABLE_KITELLM)
  message(FATAL_ERROR "Inferlet and kiteLLM require separate builds to avoid bundled runtime symbol collisions")
endif()
include(cmake_ext/Dependencies.cmake)
include(cmake_ext/company/KiteLlm.cmake)
include(cmake_ext/RuntimeGraph.cmake)
include(cmake_ext/ProductTargets.cmake)
