target_sources(alg_sdk PRIVATE
  asr_transcribe_node.cpp
  llm_generate_node.cpp
  ocr_detect_node.cpp
  structured_json_parse_node.cpp
  support/compiled_text_regex.cpp
  text_chunk_node.cpp
  text_corpus_source_node.cpp
  text_embedding_node.cpp
  text_rerank_node.cpp
  text_rule_match_node.cpp
  text_template_node.cpp
  vector_top_k_node.cpp)
