cmake_minimum_required(VERSION 3.19)
# Operator 壳选择检查。
#   -DTEST_ROOT=<dir> [-DSOURCE_MANIFEST=<file>]
# 第一部分在临时源码树上检查选择模块：默认 mock、缺失目录配置失败、
# 全部具体壳及夹具被排除。第二部分读取实际构建生成的目标源码清单，
# 确认生产目标不含测试驱动和清单，各目标只含选中壳的源码与夹具。

get_filename_component(repo "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(module "${repo}/cmake_ext/support/OperatorShell.cmake")

# 把 root 下的绝对路径改写为 prefix 加相对路径；其余项原样保留。
function(relative_to_root list_var root prefix)
  set(items)
  foreach(item IN LISTS ${list_var})
    if(IS_ABSOLUTE "${item}")
      file(RELATIVE_PATH relative "${root}" "${item}")
      if(NOT relative MATCHES "^\\.\\./")
        set(item "${prefix}${relative}")
      endif()
    endif()
    list(APPEND items "${item}")
  endforeach()
  set(${list_var} "${items}" PARENT_SCOPE)
endfunction()

if(DEFINED SELECT_ROOT)
  # 子进程：在给定源码树上加载模块，并输出选择及排除结果。
  set(PROJECT_SOURCE_DIR "${SELECT_ROOT}")
  include("${module}")
  # 以绝对路径排除，与构建中的通配结果一致。
  file(GLOB_RECURSE files "${SELECT_ROOT}/*")
  edgeflow_exclude_operator_shells(files)
  relative_to_root(files "${SELECT_ROOT}" "")
  list(SORT files)
  message("SELECTED=${EDGEFLOW_OPERATOR_SHELL}")
  message("KEPT=${files}")
  return()
endif()

if(NOT TEST_ROOT)
  message(FATAL_ERROR "TEST_ROOT is required")
endif()
file(REMOVE_RECURSE "${TEST_ROOT}")
# 检出目录本身含壳目录特征，排除只能依据相对源码根的路径。
set(tree "${TEST_ROOT}/tests/operator_shell/checkout/adapter/operator/tree")
set(shell_files
    src/adapter/operator/SHELL/operator_adapter.cpp
    include/adapter/operator/SHELL/binding.h
    tests/contract/operator_shell/SHELL/driver.cpp
    tests/unit/operator/operator_shell/SHELL/test_carrier.cpp
    tests/integration/demo/operator_shell/SHELL/test_demo.cpp
    demo/operator_shell/SHELL/input/input.cpp)
set(shared_files
    src/adapter/operator/operator_failure_category.cpp
    tests/contract/operator_shell/test_entries.cpp
    demo/common/runner.cpp)
foreach(shell mock other)
  foreach(path IN LISTS shell_files)
    string(REPLACE SHELL "${shell}" path "${path}")
    file(WRITE "${tree}/${path}" "")
  endforeach()
endforeach()
foreach(path IN LISTS shared_files)
  file(WRITE "${tree}/${path}" "")
endforeach()
# 只有源码目录、缺少测试驱动和 Demo 夹具的壳。
file(WRITE "${tree}/src/adapter/operator/partial/operator_adapter.cpp" "")

function(run_selection result_var output_var)
  execute_process(
      COMMAND "${CMAKE_COMMAND}" -DSELECT_ROOT=${tree} ${ARGN}
              -P "${CMAKE_CURRENT_LIST_FILE}"
      RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
  set(${result_var} "${result}" PARENT_SCOPE)
  set(${output_var} "${output}" PARENT_SCOPE)
endfunction()

run_selection(result output)
if(NOT result EQUAL 0 OR NOT output MATCHES "SELECTED=mock\n")
  message(FATAL_ERROR "Default shell must be mock:\n${output}")
endif()
string(REGEX MATCH "KEPT=[^\n]*" kept "${output}")
string(REPLACE "KEPT=" "" kept "${kept}")
list(SORT shared_files)
if(NOT kept STREQUAL "${shared_files}")
  message(FATAL_ERROR "Every concrete shell and fixture must be excluded; kept: ${kept}")
endif()

run_selection(result output -DEDGEFLOW_OPERATOR_SHELL=other)
if(NOT result EQUAL 0 OR NOT output MATCHES "SELECTED=other\n")
  message(FATAL_ERROR "A complete shell must be selectable:\n${output}")
endif()

foreach(case "absent;src/adapter/operator/absent"
             "partial;tests/contract/operator_shell/partial"
             "Mock;lowercase directory name")
  list(GET case 0 shell)
  list(GET case 1 expected)
  run_selection(result output -DEDGEFLOW_OPERATOR_SHELL=${shell})
  string(FIND "${output}" "${expected}" found)
  if(result EQUAL 0 OR found EQUAL -1)
    message(FATAL_ERROR "Shell '${shell}' must fail configuration naming ${expected}:\n${output}")
  endif()
endforeach()
file(REMOVE_RECURSE "${TEST_ROOT}")

if(SOURCE_MANIFEST)
  include("${SOURCE_MANIFEST}")
  set(shell "${operator_shell}")
  set(any_shell "/adapter/operator/[^/]+/|/operator_shell/[^/]+/")
  set(selected "/adapter/operator/${shell}/|/operator_shell/${shell}/")
  # 只按相对仓库根的路径判断，检出目录本身不参与匹配。
  foreach(target IN LISTS production_targets test_targets)
    relative_to_root(${target}_sources "${repo}" "/")
  endforeach()
  foreach(target IN LISTS production_targets)
    foreach(source IN LISTS ${target}_sources)
      if(source MATCHES "^/tests/")
        message(FATAL_ERROR "Production target ${target} contains test source ${source}")
      endif()
      if(source MATCHES "${any_shell}" AND NOT source MATCHES "${selected}")
        message(FATAL_ERROR "Production target ${target} contains unselected shell source ${source}")
      endif()
    endforeach()
  endforeach()
  foreach(target IN LISTS test_targets)
    foreach(source IN LISTS ${target}_sources)
      if(source MATCHES "${any_shell}" AND NOT source MATCHES "${selected}")
        message(FATAL_ERROR "Test target ${target} contains unselected shell source ${source}")
      endif()
    endforeach()
  endforeach()
  foreach(required
      "edgeflow_integration_objects;/src/adapter/operator/${shell}/"
      "edgeflow_demo_objects;/demo/operator_shell/${shell}/"
      "edgeflow_test_adapter_runner;/tests/contract/operator_shell/${shell}/")
    list(GET required 0 target)
    list(GET required 1 pattern)
    string(REPLACE ";" "\n" sources "${${target}_sources}")
    string(FIND "${sources}" "${pattern}" found)
    if(found EQUAL -1)
      message(FATAL_ERROR "${target} does not contain the selected ${pattern}")
    endif()
  endforeach()
endif()
message(STATUS "PASS Operator shell selection and fixture isolation")
