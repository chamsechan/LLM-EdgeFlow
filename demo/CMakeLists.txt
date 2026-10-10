file(GLOB_RECURSE EDGEFLOW_DEMO_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
list(REMOVE_ITEM EDGEFLOW_DEMO_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
add_library(edgeflow_demo_objects OBJECT ${EDGEFLOW_DEMO_SOURCES})
target_link_libraries(edgeflow_demo_objects PRIVATE
  edgeflow_internal_headers llm_edgeflow::internal_runtime)
target_include_directories(edgeflow_demo_objects PRIVATE
  "${PROJECT_BINARY_DIR}/generated/internal")

add_executable(alg_demo
  main.cpp
  $<TARGET_OBJECTS:edgeflow_demo_objects>
  $<TARGET_OBJECTS:edgeflow_test_backend_fixtures>
  $<TARGET_OBJECTS:edgeflow_test_biz_model_fixtures>)
target_link_libraries(alg_demo PRIVATE
  edgeflow_internal_headers llm_edgeflow::internal_runtime)
