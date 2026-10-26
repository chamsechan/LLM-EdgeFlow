file(GLOB_RECURSE EDGEFLOW_DEMO_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
list(REMOVE_ITEM EDGEFLOW_DEMO_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
# 载体构造只取选中 Operator 壳的夹具。
edgeflow_exclude_operator_shells(EDGEFLOW_DEMO_SOURCES)
file(GLOB_RECURSE EDGEFLOW_DEMO_SHELL_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/operator_shell/${EDGEFLOW_OPERATOR_SHELL}/*.cpp")
list(APPEND EDGEFLOW_DEMO_SOURCES ${EDGEFLOW_DEMO_SHELL_SOURCES})
add_library(edgeflow_demo_objects OBJECT ${EDGEFLOW_DEMO_SOURCES})
target_link_libraries(edgeflow_demo_objects PRIVATE
  edgeflow_internal_headers llm_edgeflow::internal_runtime edgeflow_stb_image
  edgeflow_demo_profile_fields)

add_executable(alg_demo
  main.cpp
  $<TARGET_OBJECTS:edgeflow_demo_objects>
  $<TARGET_OBJECTS:edgeflow_demo_fixture_models>)
target_link_libraries(alg_demo PRIVATE
  edgeflow_internal_headers llm_edgeflow::internal_runtime)
