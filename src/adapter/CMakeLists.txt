target_sources(edgeflow_layer1_adapter_objects PRIVATE
  adapters/audio_asr_intent_adapter.cpp
  adapters/compliance_audit_adapter.cpp
  adapters/cross_rerank_adapter.cpp
  adapters/doc_qa_adapter.cpp
  adapters/entity_extract_adapter.cpp
  adapters/keyword_match_adapter.cpp
  adapters/ocr_doc_qa_adapter.cpp
  company_c_adapter.cpp
  deployment_model_resolver.cpp
  operator/biz_bridges/audio_asr_intent_bridge.cpp
  operator/biz_bridges/compliance_audit_bridge.cpp
  operator/biz_bridges/cross_rerank_bridge.cpp
  operator/biz_bridges/doc_qa_bridge.cpp
  operator/biz_bridges/entity_extract_bridge.cpp
  operator/biz_bridges/keyword_match_bridge.cpp
  operator/biz_bridges/ocr_doc_qa_bridge.cpp
  operator/company_conf_resolver.cpp
  operator/operator_adapter.cpp
  operator/operator_biz_bridge_registry.cpp
  operator/operator_control_registry.cpp
  operator/operator_output_pool.cpp
  operator/operator_process_binding.cpp
  operator/operator_value_type_registry.cpp)

target_sources(edgeflow_composition_objects PRIVATE
  shared_algorithm_runtime.cpp)
