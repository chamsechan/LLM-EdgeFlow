# Keep conventional include spellings while withholding other layers' headers
# from compiler search paths. LayerGuard still checks relative/absolute includes.
function(edgeflow_header_view view_name)
  set(view_dir "${PROJECT_BINARY_DIR}/layer_includes/${view_name}")
  # Reconfiguration after a header removal must not retain a stale visible file.
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
      # Platforms without symlink support track copies as configure dependencies;
      # edits then trigger CMake before the next incremental compilation.
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

edgeflow_collect_headers(contract_headers "${PROJECT_SOURCE_DIR}/include/contracts")
list(APPEND contract_headers include/company_alg_log.h include/company_alg_export.h)
edgeflow_header_view(contracts ${contract_headers})

edgeflow_collect_headers(model_api_headers "${PROJECT_SOURCE_DIR}/include/engine")
edgeflow_collect_headers(model_private_headers "${PROJECT_SOURCE_DIR}/src/engine")
edgeflow_header_view(model_execution ${model_api_headers} ${model_private_headers})

# Shared Node authoring contracts have one manifest for CMake and LayerGuard.
set(node_contract_manifest "${PROJECT_SOURCE_DIR}/cmake/node_core_contracts.txt")
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
    "${PROJECT_SOURCE_DIR}/include/operator" "${PROJECT_SOURCE_DIR}/src/adapter")
edgeflow_header_view(integration ${integration_headers} ${core_headers}
    ${model_api_headers} include/company_alg_interface.h include/company_alg_cpp.hpp)

# Capture the actual evaluated target include paths, including transitive usage
# requirements, so the existing LayerGuard gate detects accidental broadening.
function(edgeflow_generate_layer_compile_manifest)
  set(content "set(layer_cxx [==[${CMAKE_CXX_COMPILER}]==])\n")
  string(APPEND content "set(layer_source_dir [==[${PROJECT_SOURCE_DIR}]==])\n")
  string(APPEND content "set(layer_generator [==[${CMAKE_GENERATOR}]==])\n")
  string(APPEND content "set(layer_test_root [==[${PROJECT_BINARY_DIR}/layer_compile_checks/$<CONFIG>]==])\n")
  foreach(layer model_execution capability_nodes orchestration integration)
    string(APPEND content
      "set(${layer}_includes [==[$<TARGET_PROPERTY:edgeflow_${layer}_objects,INCLUDE_DIRECTORIES>]==])\n")
  endforeach()
  file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/layer_includes/compile_checks_$<CONFIG>.cmake"
       CONTENT "${content}")
endfunction()
