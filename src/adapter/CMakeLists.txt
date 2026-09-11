target_sources(edgeflow_integration_objects PRIVATE
  biz_adapter_registry.cpp
  biz/audio_asr_intent_adapter.cpp
  biz/compliance_audit_adapter.cpp
  biz/cross_rerank_adapter.cpp
  biz/doc_qa_adapter.cpp
  biz/entity_extract_adapter.cpp
  biz/translate_adapter.cpp
  biz/keyword_match_adapter.cpp
  biz/ocr_doc_qa_adapter.cpp
  c_api_adapter.cpp
  deployment_model_resolver.cpp
  biz/audio_asr_intent_operator_bridge.cpp
  biz/compliance_audit_operator_bridge.cpp
  biz/cross_rerank_operator_bridge.cpp
  biz/doc_qa_operator_bridge.cpp
  biz/entity_extract_operator_bridge.cpp
  biz/translate_operator_bridge.cpp
  biz/keyword_match_operator_bridge.cpp
  biz/ocr_doc_qa_operator_bridge.cpp
  operator/json_output_config_reader.cpp
  operator/operator_config_resolver.cpp
  operator/operator_adapter.cpp
  operator/operator_biz_bridge_registry.cpp
  operator/operator_control_registry.cpp
  operator/operator_output_pool.cpp
  operator/operator_process_binding.cpp
  operator/operator_value_type_registry.cpp
  operator/operator_builtin_value_types.cpp)

target_sources(edgeflow_composition_objects PRIVATE
  shared_algorithm_runtime.cpp)
