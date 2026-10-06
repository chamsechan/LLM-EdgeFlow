# 框架运行时显式列出；Model 与 Backend 递归收集，下次构建时编译。
file(GLOB_RECURSE EDGEFLOW_MODEL_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/models/*.cpp")
file(GLOB_RECURSE EDGEFLOW_BACKEND_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/backends/*.cpp")

target_sources(edgeflow_model_execution_objects PRIVATE
  runtime/backend_registry.cpp
  runtime/model_registry.cpp
  runtime/model_runtime_factory.cpp
  text/utf8.cpp
  text_generation/common_autoregressive_generator.cpp
  ${EDGEFLOW_MODEL_SOURCES})

# Backend 单独编译，vendor 编译要求只提供给该目标。
target_sources(edgeflow_model_execution_backends_objects PRIVATE
  ${EDGEFLOW_BACKEND_SOURCES})

target_link_libraries(edgeflow_model_execution_objects PRIVATE edgeflow_stb_image)
