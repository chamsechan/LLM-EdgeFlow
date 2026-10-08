# 框架机制显式列出；业务 Converter 登记递归收集，下次构建时编译。
file(GLOB_RECURSE EDGEFLOW_ADAPTER_CONVERTER_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/input/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/output/*.cpp")
target_sources(edgeflow_integration_objects PRIVATE
  deployment_model_resolver.cpp
  deployment_preparation.cpp
  io_structure.cpp
  io_converter_registry.cpp
  deployment_io_config.cpp
  pipeline_document.cpp
  io_plan_resolver.cpp
  io_catalog.cpp
  ${EDGEFLOW_ADAPTER_CONVERTER_SOURCES}
  operator/operator_config_resolver.cpp
  operator/operator_adapter.cpp
  operator/operator_control_registry.cpp
  operator/operator_error_mapping.cpp
  operator/operator_output_pool.cpp
  operator/operator_process_binding.cpp
  operator/operator_value_type.cpp
  operator/operator_value_type_registry.cpp
  operator/operator_builtin_value_types.cpp)

target_sources(edgeflow_composition_objects PRIVATE
  shared_algorithm_runtime.cpp)
