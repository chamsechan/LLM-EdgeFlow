add_executable(alg_show alg_show.cpp)
target_link_libraries(alg_show PRIVATE
  edgeflow_internal_headers nlohmann_json::nlohmann_json)

add_executable(alg_pipeline_tool alg_pipeline_tool.cpp)
target_link_libraries(alg_pipeline_tool PRIVATE
  edgeflow_internal_headers llm_edgeflow::internal_runtime)
