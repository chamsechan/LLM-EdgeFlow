# Model、运行时与公共实现自动收集；Backend 保持独立编译目标。
file(GLOB_RECURSE EDGEFLOW_ENGINE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE EDGEFLOW_BACKEND_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/backends/*.cpp")
list(REMOVE_ITEM EDGEFLOW_ENGINE_SOURCES ${EDGEFLOW_BACKEND_SOURCES})

target_sources(edgeflow_model_execution_objects PRIVATE
  ${EDGEFLOW_ENGINE_SOURCES})

# Backend 单独编译，vendor 编译要求只提供给该目标。
target_sources(edgeflow_model_execution_backends_objects PRIVATE
  ${EDGEFLOW_BACKEND_SOURCES})
