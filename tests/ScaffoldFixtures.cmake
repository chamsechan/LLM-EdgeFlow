# 生成的扩展只属于测试，绝不进入 SDK 或生产 Catalog。
set(EDGEFLOW_CONTROL_FIXTURE_SOURCE
  "${CMAKE_CURRENT_BINARY_DIR}/test-fixtures/control/test_control_node.cpp")
get_filename_component(_control_fixture_dir "${EDGEFLOW_CONTROL_FIXTURE_SOURCE}" DIRECTORY)
add_custom_command(
  OUTPUT "${EDGEFLOW_CONTROL_FIXTURE_SOURCE}"
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/scaffold_custom_node.py"
          test_control --control-id 2000000041 --force
          --output-dir "${_control_fixture_dir}"
  DEPENDS
    "${PROJECT_SOURCE_DIR}/tools/scaffold_custom_node.py"
    "${PROJECT_SOURCE_DIR}/dev_support/node_authoring/starter_control_node.cpp"
  COMMENT "Generating isolated Control authoring fixture"
  VERBATIM)
set(EDGEFLOW_SCAFFOLD_FIXTURE_SOURCE
    "${CMAKE_CURRENT_BINARY_DIR}/test-fixtures/scaffold/scaffold_contracts.cpp")
add_custom_command(
  OUTPUT "${EDGEFLOW_SCAFFOLD_FIXTURE_SOURCE}"
  BYPRODUCTS
    "${CMAKE_CURRENT_BINARY_DIR}/test-fixtures/scaffold/control_tutorial/pipeline.json"
    "${CMAKE_CURRENT_BINARY_DIR}/test-fixtures/scaffold/control_tutorial/pipeline.conf"
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/tooling/generate_scaffold_fixtures.py"
          "${EDGEFLOW_SCAFFOLD_FIXTURE_SOURCE}"
  DEPENDS
    "${PROJECT_SOURCE_DIR}/tools/scaffold_custom_node.py"
    "${PROJECT_SOURCE_DIR}/dev_support/node_authoring/starter_llm_node.cpp"
    "${PROJECT_SOURCE_DIR}/dev_support/node_authoring/starter_control_node.cpp"
    "${PROJECT_SOURCE_DIR}/doc/dev_guide/first_custom_node.md"
    "${PROJECT_SOURCE_DIR}/doc/dev_guide/first_control.md"
    "${PROJECT_SOURCE_DIR}/tests/tooling/generate_scaffold_fixtures.py"
  COMMENT "Generating custom Node snippets and standalone behavioral test fixtures"
  VERBATIM)

# 在测试 runner 中编译文档里的原样函数示例，绝不进入 SDK Catalog。
list(APPEND EDGEFLOW_SCAFFOLD_FIXTURE_SOURCE
  "${PROJECT_SOURCE_DIR}/dev_support/node_authoring/starter_batch_node.cpp"
  "${PROJECT_SOURCE_DIR}/dev_support/node_authoring/starter_multi_model_node.cpp"
  "${PROJECT_SOURCE_DIR}/dev_support/node_authoring/starter_batch_join_node.cpp"
  "${PROJECT_SOURCE_DIR}/dev_support/node_authoring/starter_batch_group_node.cpp"
  "${PROJECT_SOURCE_DIR}/dev_support/node_authoring/starter_batch_select_scatter_node.cpp")
