# 本目录 (含子目录) 下的每个 .cpp 都编译进能力节点层。
# CONFIGURE_DEPENDS 会在下次构建时识别变更。
file(GLOB_RECURSE EDGEFLOW_CUSTOM_NODE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(edgeflow_capability_nodes_objects PRIVATE
  ${EDGEFLOW_CUSTOM_NODE_SOURCES})
