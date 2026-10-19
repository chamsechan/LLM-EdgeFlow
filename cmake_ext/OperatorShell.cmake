# Operator 协议壳选择。每个构建只编译一套壳及其测试驱动、清单和夹具。
#
# 具体壳目录：
#   src/adapter/operator/<shell>/、include/adapter/operator/<shell>/（若有）
# 壳夹具目录（统一命名 operator_shell/<shell>/）：
#   tests/contract/operator_shell/<shell>/  测试驱动与错误码镜像清单
#   demo/operator_shell/<shell>/            Demo 载体构造
#   tests/**/operator_shell/<shell>/        依赖该壳载体的测试
# 各处通配收集先排除全部具体壳及夹具目录，再加入选中项。

set(EDGEFLOW_OPERATOR_SHELL "mock" CACHE STRING
    "Operator protocol shell implementation and its test fixtures")

# 具体壳和壳夹具目录的路径特征，用于从通配结果中整体排除。
# 只对相对源码根的路径匹配，检出目录本身不参与判断。
set(EDGEFLOW_OPERATOR_SHELL_PATH_REGEX
    "/adapter/operator/[^/]+/|/operator_shell/[^/]+/")

function(edgeflow_require_operator_shell root shell)
  if(NOT shell MATCHES "^[a-z][a-z0-9_]*$")
    message(FATAL_ERROR
        "EDGEFLOW_OPERATOR_SHELL must be a lowercase directory name: '${shell}'")
  endif()
  foreach(directory
      "src/adapter/operator/${shell}"
      "tests/contract/operator_shell/${shell}"
      "demo/operator_shell/${shell}")
    if(NOT IS_DIRECTORY "${root}/${directory}")
      message(FATAL_ERROR
          "Operator shell '${shell}' is missing ${directory}")
    endif()
  endforeach()
endfunction()

# 从列表中去除全部具体壳及夹具路径。绝对路径先转为相对 PROJECT_SOURCE_DIR。
function(edgeflow_exclude_operator_shells list_var)
  set(kept)
  foreach(item IN LISTS ${list_var})
    set(relative "${item}")
    if(IS_ABSOLUTE "${item}")
      file(RELATIVE_PATH relative "${PROJECT_SOURCE_DIR}" "${item}")
    endif()
    if(NOT relative MATCHES "${EDGEFLOW_OPERATOR_SHELL_PATH_REGEX}")
      list(APPEND kept "${item}")
    endif()
  endforeach()
  set(${list_var} "${kept}" PARENT_SCOPE)
endfunction()

edgeflow_require_operator_shell("${PROJECT_SOURCE_DIR}" "${EDGEFLOW_OPERATOR_SHELL}")
