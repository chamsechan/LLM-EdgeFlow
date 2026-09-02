add_library(edgeflow_demo_objects OBJECT
  common/demo_options.cpp
  common/demo_registry.cpp
  common/dataset_reader.cpp
  common/result_writer.cpp
  biz/entity_extract_demo.cpp
  biz/keyword_match_demo.cpp
  biz/doc_qa_demo.cpp
  biz/dialogue_audit_demo.cpp
  biz/ocr_doc_qa_demo.cpp
  biz/audio_asr_demo.cpp
  biz/cross_rerank_demo.cpp)
target_link_libraries(edgeflow_demo_objects PRIVATE
  edgeflow_internal_headers llm_edgeflow::internal_runtime)

add_executable(alg_demo
  main.cpp
  $<TARGET_OBJECTS:edgeflow_demo_objects>
  $<TARGET_OBJECTS:edgeflow_test_backend_fixtures>
  $<TARGET_OBJECTS:edgeflow_test_business_model_fixtures>)
target_link_libraries(alg_demo PRIVATE
  edgeflow_internal_headers llm_edgeflow::internal_runtime)
