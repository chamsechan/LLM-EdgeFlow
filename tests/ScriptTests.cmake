# 按约定发现脚本测试，只为例外情况单独配置参数和属性。
find_program(EDGEFLOW_BASH_EXECUTABLE bash REQUIRED)

set(_edgeflow_guard_flags)
foreach(flag IN LISTS edgeflow_cxx_platform_flags)
  list(APPEND _edgeflow_guard_flags "--cxx-flag=${flag}")
endforeach()
if(ENABLE_SANITIZERS)
  list(APPEND _edgeflow_guard_flags --sanitizers "${LLM_EDGEFLOW_SANITIZERS}")
endif()
set_property(SOURCE contract/architecture/test_quality_gate_contract.py
  PROPERTY EDGEFLOW_TEST_ARGS
    --cxx "${CMAKE_CXX_COMPILER}" ${_edgeflow_guard_flags}
    --gtest-include $<TARGET_PROPERTY:GTest::gtest,INTERFACE_INCLUDE_DIRECTORIES>
    --gtest-library $<TARGET_FILE:GTest::gtest>
    --gtest-main "${CMAKE_CURRENT_SOURCE_DIR}/support/gtest_main.cpp"
    --ctest-policy "${CMAKE_CURRENT_SOURCE_DIR}/SafeGtestDiscovery.cmake")
set_property(SOURCE contract/architecture/test_test_labels_contract.py
  PROPERTY EDGEFLOW_TEST_ARGS --build-dir "${PROJECT_BINARY_DIR}")
set_property(SOURCE contract/architecture/test_operator_shell_selection.cmake
  PROPERTY EDGEFLOW_TEST_ARGS
    "-DSOURCE_MANIFEST=${PROJECT_BINARY_DIR}/operator_shell_sources_$<CONFIG>.cmake")
set_property(SOURCE contract/authoring/test_spec_signature_diagnostics.cmake
  PROPERTY EDGEFLOW_TEST_ARGS
    "-DLAYER_COMPILE_MANIFEST=${PROJECT_BINARY_DIR}/layer_includes/compile_checks_$<CONFIG>.cmake")
set_property(SOURCE tooling/test_pipeline_cli.py PROPERTY EDGEFLOW_TEST_ARGS
  --tool $<TARGET_FILE:alg_pipeline_tool>
  --repo "${PROJECT_SOURCE_DIR}")

edgeflow_collect_test_files(scripts
  "${CMAKE_CURRENT_SOURCE_DIR}/contract/test_*.py"
  "${CMAKE_CURRENT_SOURCE_DIR}/contract/test_*.sh"
  "${CMAKE_CURRENT_SOURCE_DIR}/contract/test_*.cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/tooling/test_*.py"
  "${CMAKE_CURRENT_SOURCE_DIR}/tooling/test_*.sh"
  "${CMAKE_CURRENT_SOURCE_DIR}/tooling/test_*.cmake")
foreach(script IN LISTS scripts)
  file(RELATIVE_PATH name "${CMAKE_CURRENT_SOURCE_DIR}" "${script}")
  if(name MATCHES "^tooling/")
    set(labels "tier4;tooling;dev-fast;sanitizer-compatible")
  else()
    set(labels "tier1;static-gate;dev-fast;sanitizer-compatible")
  endif()
  get_source_file_property(arguments "${script}" EDGEFLOW_TEST_ARGS)
  if(arguments STREQUAL "NOTFOUND")
    set(arguments)
  endif()
  get_filename_component(extension "${script}" LAST_EXT)
  if(extension STREQUAL ".py")
    set(command "${Python3_EXECUTABLE}" "${script}" ${arguments})
  elseif(extension STREQUAL ".sh")
    set(command "${EDGEFLOW_BASH_EXECUTABLE}" "${script}" ${arguments})
  else()
    set(command "${CMAKE_COMMAND}"
      "-DPROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
      "-DPROJECT_BINARY_DIR=${PROJECT_BINARY_DIR}"
      "-DTEST_ROOT=${PROJECT_BINARY_DIR}/script-tests/${name}"
      ${arguments} -P "${script}")
  endif()
  add_test(NAME "${name}" COMMAND ${command})
  set_tests_properties("${name}" PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    LABELS "${labels}"
    TIMEOUT 120)
endforeach()

set_tests_properties(contract/architecture/test_diagram_render_gate.sh PROPERTIES
  LABELS "tier4;tooling;static-gate;slow")
set_tests_properties(tooling/test_json_prompt_demo.py PROPERTIES
  LABELS "tier3;tooling;dev-fast;sanitizer-compatible"
  ENVIRONMENT "LLM_EDGEFLOW_DEMO_BINARY=$<TARGET_FILE:alg_demo>")
set_tests_properties(tooling/test_pipeline_studio.py tooling/test_dev_recipe.py PROPERTIES
  ENVIRONMENT
    "LLM_EDGEFLOW_PIPELINE_TOOL=$<TARGET_FILE:alg_pipeline_tool_test>;LLM_EDGEFLOW_SELECTION_TOOL=$<TARGET_FILE:alg_pipeline_tool>;LLM_EDGEFLOW_DEMO_BINARY=$<TARGET_FILE:alg_demo>;LLM_EDGEFLOW_ALG_SHOW=$<TARGET_FILE:alg_show>")
set_tests_properties(tooling/test_pipeline_cli.py PROPERTIES
  ENVIRONMENT "LLM_EDGEFLOW_ALG_SHOW=$<TARGET_FILE:alg_show>")
