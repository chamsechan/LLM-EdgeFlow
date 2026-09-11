# Generated extensions belong only to tests, never the SDK or production Catalog.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
get_filename_component(_control_fixture_dir "${EDGEFLOW_CONTROL_FIXTURE_SOURCE}" DIRECTORY)
add_custom_command(
  OUTPUT "${EDGEFLOW_CONTROL_FIXTURE_SOURCE}"
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/scaffold_custom_node.py"
          TestControlNode --control-id 2000000041 --force
          --output-dir "${_control_fixture_dir}"
  DEPENDS
    "${PROJECT_SOURCE_DIR}/scripts/scaffold_custom_node.py"
    "${PROJECT_SOURCE_DIR}/dev_support/node_authoring/starter_control_node.cpp"
  COMMENT "Generating isolated Control authoring fixture"
  VERBATIM)
set(EDGEFLOW_SCAFFOLD_FIXTURE_SOURCE
    "${CMAKE_CURRENT_BINARY_DIR}/test-fixtures/scaffold/scaffold_contracts.cpp")
add_custom_command(
  OUTPUT "${EDGEFLOW_SCAFFOLD_FIXTURE_SOURCE}"
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/tooling/generate_scaffold_fixtures.py"
          "${EDGEFLOW_SCAFFOLD_FIXTURE_SOURCE}"
  DEPENDS
    "${PROJECT_SOURCE_DIR}/scripts/scaffold_custom_node.py"
    "${PROJECT_SOURCE_DIR}/dev_support/node_authoring/starter_llm_node.cpp"
    "${PROJECT_SOURCE_DIR}/dev_support/node_authoring/starter_control_node.cpp"
    "${PROJECT_SOURCE_DIR}/doc/dev_guide/first_custom_node.md"
    "${PROJECT_SOURCE_DIR}/tests/tooling/generate_scaffold_fixtures.py"
    "${PROJECT_SOURCE_DIR}/src/custom_nodes/CMakeLists.txt"
    "${PROJECT_SOURCE_DIR}/cmake_ext/CustomNodeTests.cmake"
  COMMENT "Generating custom Node snippets and standalone behavioral test fixtures"
  VERBATIM)
