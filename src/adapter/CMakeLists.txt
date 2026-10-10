# 普通实现归接入层；共享运行时作为组合根单独编译。
file(GLOB_RECURSE EDGEFLOW_ADAPTER_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
list(REMOVE_ITEM EDGEFLOW_ADAPTER_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/shared_algorithm_runtime.cpp")
list(FILTER EDGEFLOW_ADAPTER_SOURCES EXCLUDE REGEX "/operator/mock/")
target_sources(edgeflow_integration_objects PRIVATE
  ${EDGEFLOW_ADAPTER_SOURCES})

# 当前开发构建选择 mock 协议壳；接入其他框架时替换这一组实现。
file(GLOB EDGEFLOW_MOCK_OPERATOR_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/operator/mock/*.cpp")
target_sources(edgeflow_integration_objects PRIVATE
  ${EDGEFLOW_MOCK_OPERATOR_SOURCES})

target_sources(edgeflow_composition_objects PRIVATE
  shared_algorithm_runtime.cpp)
