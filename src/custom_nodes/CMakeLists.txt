# Every .cpp under this directory (including subdirectories) is compiled into
# the Capability Nodes layer. CONFIGURE_DEPENDS picks up changes on the next build.
file(GLOB_RECURSE EDGEFLOW_CUSTOM_NODE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(edgeflow_capability_nodes_objects PRIVATE
  ${EDGEFLOW_CUSTOM_NODE_SOURCES})
