# 保留常规的 include 写法，同时不把其他层的头文件放进编译器搜索路径。
# LayerGuard 仍会检查相对/绝对路径 include。
function(edgeflow_header_view view_name)
  set(view_dir "${PROJECT_BINARY_DIR}/layer_includes/${view_name}")
  # 删除头文件后重新配置时，不得残留过期的可见文件。
  file(REMOVE_RECURSE "${view_dir}")
  file(MAKE_DIRECTORY "${view_dir}")
  foreach(header IN LISTS ARGN)
    string(REGEX REPLACE "^(include|src)/" "" include_path "${header}")
    get_filename_component(parent "${view_dir}/${include_path}" DIRECTORY)
    file(MAKE_DIRECTORY "${parent}")
    if(EXISTS "${view_dir}/${include_path}" OR IS_SYMLINK "${view_dir}/${include_path}")
      message(FATAL_ERROR "Duplicate header spelling in ${view_name}: ${include_path}")
    endif()
    file(CREATE_LINK "${PROJECT_SOURCE_DIR}/${header}"
         "${view_dir}/${include_path}" SYMBOLIC RESULT link_result)
    if(NOT link_result STREQUAL "0")
      # 不支持符号链接的平台把副本登记为配置依赖；
      # 编辑后会在下次增量编译前触发 CMake。
      configure_file("${PROJECT_SOURCE_DIR}/${header}"
                     "${view_dir}/${include_path}" COPYONLY)
    endif()
  endforeach()
  set(edgeflow_${view_name}_include_dir "${view_dir}" PARENT_SCOPE)
endfunction()

function(edgeflow_collect_headers output_var)
  set(patterns)
  foreach(directory IN LISTS ARGN)
    list(APPEND patterns "${directory}/*.h" "${directory}/*.hpp")
  endforeach()
  file(GLOB_RECURSE headers CONFIGURE_DEPENDS
       RELATIVE "${PROJECT_SOURCE_DIR}" ${patterns})
  set(${output_var} "${headers}" PARENT_SCOPE)
endfunction()

set(platform_mock_headers
    include/platform_mock/error_codes.h
    include/platform_mock/operator_data_types.h include/platform_mock/operator_types.h)

set(public_headers
    include/edgeflow/export.h include/edgeflow/log.h
    include/edgeflow/operator/interface.h include/edgeflow/operator/types.h
    ${platform_mock_headers})
edgeflow_header_view(public ${public_headers})

edgeflow_collect_headers(contract_headers "${PROJECT_SOURCE_DIR}/include/contracts")
list(APPEND contract_headers include/edgeflow/log.h include/edgeflow/export.h)
edgeflow_header_view(contracts ${contract_headers})

edgeflow_collect_headers(model_api_headers "${PROJECT_SOURCE_DIR}/include/engine")
edgeflow_collect_headers(model_private_headers "${PROJECT_SOURCE_DIR}/src/engine")
edgeflow_header_view(model_execution ${model_api_headers} ${model_private_headers})

# 共享的 Node 编写契约只有一份清单，供 CMake 和 LayerGuard 共用。
set(node_contract_manifest "${PROJECT_SOURCE_DIR}/cmake_ext/node_core_contracts.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${node_contract_manifest}")
file(STRINGS "${node_contract_manifest}" node_core_contracts)
set(node_core_headers)
foreach(header IN LISTS node_core_contracts)
  list(APPEND node_core_headers "include/core/${header}")
endforeach()
edgeflow_collect_headers(node_headers "${PROJECT_SOURCE_DIR}/include/nodes"
    "${PROJECT_SOURCE_DIR}/src/common_nodes" "${PROJECT_SOURCE_DIR}/src/custom_nodes")
edgeflow_header_view(capability_nodes ${node_headers} ${node_core_headers}
    ${model_api_headers} src/engine/text/utf8.h)

edgeflow_collect_headers(core_headers "${PROJECT_SOURCE_DIR}/include/core"
    "${PROJECT_SOURCE_DIR}/src/core")
edgeflow_header_view(orchestration ${core_headers} ${model_api_headers})

edgeflow_collect_headers(integration_headers "${PROJECT_SOURCE_DIR}/include/adapter"
    "${PROJECT_SOURCE_DIR}/include/edgeflow/operator" "${PROJECT_SOURCE_DIR}/src/adapter")
edgeflow_header_view(integration ${integration_headers} ${core_headers}
    ${model_api_headers} ${platform_mock_headers})

# 记录目标实际求值后的 include 路径 (含传递的使用要求)，
# 以便现有 LayerGuard 门禁发现意外放宽。
function(edgeflow_generate_layer_compile_manifest)
  set(content "set(layer_cxx [==[${CMAKE_CXX_COMPILER}]==])\n")
  set(layer_cxx_flags "")
  foreach(directory IN LISTS edgeflow_cxx_system_include_dirs)
    list(APPEND layer_cxx_flags -isystem "${directory}")
  endforeach()
  string(APPEND content "set(layer_cxx_flags [==[${layer_cxx_flags}]==])\n")
  string(APPEND content "set(layer_source_dir [==[${PROJECT_SOURCE_DIR}]==])\n")
  string(APPEND content "set(layer_generator [==[${CMAKE_GENERATOR}]==])\n")
  string(APPEND content "set(layer_test_root [==[${PROJECT_BINARY_DIR}/layer_compile_checks/$<CONFIG>]==])\n")
  foreach(layer model_execution model_execution_backends capability_nodes orchestration integration composition)
    string(APPEND content
      "set(${layer}_includes [==[$<TARGET_PROPERTY:edgeflow_${layer}_objects,INCLUDE_DIRECTORIES>]==])\n")
    string(APPEND content
      "set(${layer}_definitions [==[$<TARGET_PROPERTY:edgeflow_${layer}_objects,COMPILE_DEFINITIONS>]==])\n")
  endforeach()
  foreach(target alg_sdk alg_pipeline_tool)
    string(APPEND content
      "set(${target}_includes [==[$<TARGET_PROPERTY:${target},INCLUDE_DIRECTORIES>]==])\n")
    string(APPEND content
      "set(${target}_definitions [==[$<TARGET_PROPERTY:${target},COMPILE_DEFINITIONS>]==])\n")
  endforeach()
  foreach(scope public extension)
    string(APPEND content
        "set(${scope}_includes [==[$<TARGET_PROPERTY:edgeflow_${scope}_headers,INTERFACE_INCLUDE_DIRECTORIES>]==])\n")
  endforeach()
  file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/layer_includes/compile_checks_$<CONFIG>.cmake"
       CONTENT "${content}")
endfunction()
