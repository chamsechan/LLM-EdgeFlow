add_executable(alg_show alg_show.cpp)
target_link_libraries(alg_show PRIVATE
  edgeflow_internal_headers nlohmann_json::nlohmann_json)

# Developer-tool guidance on Validator diagnostics; not part of the SDK.
add_library(edgeflow_pipeline_tooling STATIC pipeline_remediation.cpp)
target_link_libraries(edgeflow_pipeline_tooling PUBLIC
  edgeflow_internal_headers llm_edgeflow::internal_runtime)

add_executable(alg_pipeline_tool
  alg_pipeline_tool.cpp
  pipeline_authoring.cpp
  pipeline_json_schema.cpp
  pipeline_document_validation.cpp)
target_link_libraries(alg_pipeline_tool PRIVATE
  edgeflow_internal_headers llm_edgeflow::internal_runtime
  edgeflow_pipeline_tooling)
target_include_directories(alg_pipeline_tool PRIVATE
  "${PROJECT_BINARY_DIR}/generated/internal")
