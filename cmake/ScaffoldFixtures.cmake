# Generated extensions belong only to tests, never the SDK or production Catalog.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
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
    "${PROJECT_SOURCE_DIR}/doc/dev_guide/first_custom_node.md"
    "${PROJECT_SOURCE_DIR}/tests/tooling/generate_scaffold_fixtures.py"
  COMMENT "Generating custom Node source and registration test fixtures"
  VERBATIM)
