# 轻量运行时契约位于所有实现层下方；各层依赖 interface 只向下传播。
add_library(edgeflow_runtime_contracts INTERFACE)
target_include_directories(edgeflow_runtime_contracts INTERFACE
    "${edgeflow_contracts_include_dir}")
target_link_libraries(edgeflow_runtime_contracts INTERFACE
    nlohmann_json::nlohmann_json)

add_library(edgeflow_model_execution_dependencies INTERFACE)
target_link_libraries(edgeflow_model_execution_dependencies INTERFACE
    edgeflow_runtime_contracts)

add_library(edgeflow_capability_nodes_dependencies INTERFACE)
target_link_libraries(edgeflow_capability_nodes_dependencies INTERFACE
    edgeflow_model_execution_dependencies
    edgeflow_pcre2)

add_library(edgeflow_orchestration_dependencies INTERFACE)
target_link_libraries(edgeflow_orchestration_dependencies INTERFACE
    edgeflow_capability_nodes_dependencies)

add_library(edgeflow_integration_dependencies INTERFACE)
target_link_libraries(edgeflow_integration_dependencies INTERFACE
    edgeflow_orchestration_dependencies)

add_library(edgeflow_runtime_dependencies INTERFACE)
target_link_libraries(edgeflow_runtime_dependencies INTERFACE
    edgeflow_integration_dependencies)

function(edgeflow_add_runtime_object_target target_name dependency_target)
  add_library(${target_name} OBJECT)
  target_link_libraries(${target_name} PUBLIC ${dependency_target})
  target_compile_definitions(${target_name} PRIVATE
      EDGEFLOW_BUILDING_SDK=1)
  set_target_properties(${target_name} PROPERTIES
      C_VISIBILITY_PRESET hidden
      CXX_VISIBILITY_PRESET hidden
      VISIBILITY_INLINES_HIDDEN YES)
endfunction()

edgeflow_add_runtime_object_target(edgeflow_model_execution_objects
    edgeflow_model_execution_dependencies)
# 厂商编译只发生在 Backend 实现内部，
# 包括导入运行时目标携带的头文件和编译定义。
edgeflow_add_runtime_object_target(edgeflow_model_execution_backends_objects
    edgeflow_model_execution_dependencies)
target_include_directories(edgeflow_model_execution_backends_objects PRIVATE
    "${edgeflow_model_execution_include_dir}")
edgeflow_add_runtime_object_target(edgeflow_capability_nodes_objects
    edgeflow_capability_nodes_dependencies)
edgeflow_add_runtime_object_target(edgeflow_orchestration_objects
    edgeflow_orchestration_dependencies)
edgeflow_add_runtime_object_target(edgeflow_integration_objects
    edgeflow_integration_dependencies)
edgeflow_add_runtime_object_target(edgeflow_composition_objects
    edgeflow_runtime_dependencies)

foreach(layer model_execution capability_nodes orchestration integration)
  target_include_directories(edgeflow_${layer}_objects PRIVATE
      "${edgeflow_${layer}_include_dir}")
endforeach()
target_include_directories(edgeflow_composition_objects PRIVATE
    "${edgeflow_integration_include_dir}")
edgeflow_generate_layer_compile_manifest()

# SDK 与仓库工具共用同一组层对象。
set(edgeflow_runtime_object_files
    $<TARGET_OBJECTS:edgeflow_model_execution_objects>
    $<TARGET_OBJECTS:edgeflow_model_execution_backends_objects>
    $<TARGET_OBJECTS:edgeflow_capability_nodes_objects>
    $<TARGET_OBJECTS:edgeflow_orchestration_objects>
    $<TARGET_OBJECTS:edgeflow_integration_objects>
    $<TARGET_OBJECTS:edgeflow_composition_objects>)

# 仓库工具和测试继续链接一个聚合 INTERFACE target，不依赖公共共享库的
# 内部 C++ 动态符号。该 target 自身不拥有层内源码。
add_library(edgeflow_runtime_objects INTERFACE)
add_library(llm_edgeflow::internal_runtime ALIAS edgeflow_runtime_objects)
target_link_libraries(edgeflow_runtime_objects INTERFACE
    edgeflow_runtime_dependencies)
target_compile_definitions(edgeflow_runtime_objects INTERFACE
    EDGEFLOW_STATIC_LINK=1)

# 引擎模块只追加 Backend 的 SYSTEM 头目录、编译定义和链接目标；
# 新增 Backend 不修改此处，厂商编译要求只进入 Backend 实现。
target_include_directories(edgeflow_model_execution_backends_objects SYSTEM PRIVATE
    ${LLM_EDGEFLOW_BACKEND_INCLUDE_DIRS})
target_compile_definitions(edgeflow_model_execution_backends_objects PRIVATE
    ${LLM_EDGEFLOW_BACKEND_COMPILE_DEFINITIONS})
foreach(backend_library IN LISTS LLM_EDGEFLOW_BACKEND_LIBRARIES)
  target_link_libraries(edgeflow_model_execution_backends_objects PRIVATE
      ${backend_library})
  target_link_libraries(edgeflow_runtime_dependencies INTERFACE
      "$<LINK_ONLY:${backend_library}>")
endforeach()

add_subdirectory(src)

target_sources(edgeflow_runtime_objects INTERFACE ${edgeflow_runtime_object_files})
