# List custom node translation units here. They use the same Capability Nodes target
# and registration path as common nodes, including SDK, CLI and Demo consumers.
target_sources(edgeflow_capability_nodes_objects PRIVATE
  prompt_guided_llm_node.cpp
)
