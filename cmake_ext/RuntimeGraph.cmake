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
      COMPANY_ALG_BUILDING_SDK=1)
  target_compile_definitions(${target_name} INTERFACE
      COMPANY_ALG_STATIC_LINK=1)
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

# 仓库工具和测试继续链接一个聚合 INTERFACE target，不依赖公共共享库的
# 内部 C++ 动态符号。该 target 自身不拥有层内源码。
add_library(edgeflow_runtime_objects INTERFACE)
add_library(llm_edgeflow::internal_runtime ALIAS edgeflow_runtime_objects)
target_link_libraries(edgeflow_runtime_objects INTERFACE
    edgeflow_runtime_dependencies)
target_compile_definitions(edgeflow_runtime_objects INTERFACE
    COMPANY_ALG_STATIC_LINK=1)

if(LLM_EDGEFLOW_HAS_ONNXRUNTIME)
  target_include_directories(edgeflow_model_execution_backends_objects SYSTEM PRIVATE
      "${ONNXRUNTIME_INCLUDE_DIR}")
  target_compile_definitions(edgeflow_model_execution_backends_objects PRIVATE
      HAVE_ONNXRUNTIME=1)
  target_link_libraries(edgeflow_runtime_dependencies INTERFACE
      "$<LINK_ONLY:${ONNXRUNTIME_LIB}>")
endif()
if(LLM_EDGEFLOW_HAS_LLAMACPP)
  target_include_directories(edgeflow_model_execution_backends_objects SYSTEM PRIVATE
      ${LLAMACPP_INCLUDE_DIRS})
  target_compile_definitions(edgeflow_model_execution_backends_objects PRIVATE
      HAVE_LLAMACPP=1)
endif()
if(LLM_EDGEFLOW_HAS_WHISPERCPP)
  target_include_directories(edgeflow_model_execution_backends_objects SYSTEM PRIVATE
      ${WHISPERCPP_INCLUDE_DIRS})
  target_compile_definitions(edgeflow_model_execution_backends_objects PRIVATE
      HAVE_WHISPERCPP=1)
endif()
target_compile_definitions(edgeflow_model_execution_backends_objects PRIVATE
    ${LLM_EDGEFLOW_BACKEND_COMPILE_DEFINITIONS})
if(THIRD_PARTY_ENGINE_LIBS)
  target_link_libraries(edgeflow_model_execution_backends_objects PRIVATE
      ${THIRD_PARTY_ENGINE_LIBS})
  foreach(runtime_library IN LISTS THIRD_PARTY_ENGINE_LIBS)
    target_link_libraries(edgeflow_runtime_dependencies INTERFACE
        "$<LINK_ONLY:${runtime_library}>")
  endforeach()
endif()

add_subdirectory(src)

target_sources(edgeflow_runtime_objects INTERFACE
    $<TARGET_OBJECTS:edgeflow_model_execution_objects>
    $<TARGET_OBJECTS:edgeflow_model_execution_backends_objects>
    $<TARGET_OBJECTS:edgeflow_capability_nodes_objects>
    $<TARGET_OBJECTS:edgeflow_orchestration_objects>
    $<TARGET_OBJECTS:edgeflow_integration_objects>
    $<TARGET_OBJECTS:edgeflow_composition_objects>)
