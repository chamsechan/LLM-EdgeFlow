# 基于 Validator 诊断的开发工具指引；不属于 SDK。
file(GLOB_RECURSE EDGEFLOW_PIPELINE_TOOLING_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
list(REMOVE_ITEM EDGEFLOW_PIPELINE_TOOLING_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/alg_show.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/alg_pipeline_tool.cpp")
add_library(edgeflow_pipeline_tooling STATIC ${EDGEFLOW_PIPELINE_TOOLING_SOURCES})
target_link_libraries(edgeflow_pipeline_tooling PUBLIC
  edgeflow_internal_headers llm_edgeflow::internal_runtime)

add_executable(alg_show alg_show.cpp)
target_link_libraries(alg_show PRIVATE
  edgeflow_internal_headers llm_edgeflow::internal_runtime)

add_executable(alg_pipeline_tool alg_pipeline_tool.cpp)
target_link_libraries(alg_pipeline_tool PRIVATE
  edgeflow_internal_headers llm_edgeflow::internal_runtime
  edgeflow_pipeline_tooling)
target_include_directories(alg_pipeline_tool PRIVATE
  "${PROJECT_BINARY_DIR}/generated/internal")
