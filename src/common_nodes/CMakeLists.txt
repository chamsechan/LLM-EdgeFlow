# Includes support/ and any operation subdirectories.
file(GLOB_RECURSE EDGEFLOW_COMMON_NODE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(edgeflow_capability_nodes_objects PRIVATE
  ${EDGEFLOW_COMMON_NODE_SOURCES})
