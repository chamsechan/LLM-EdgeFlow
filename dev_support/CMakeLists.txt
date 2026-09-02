# Deterministic runtime registrations used by mock Demo profiles and tests.
# OBJECT libraries keep every registration translation unit in each consumer.
add_library(edgeflow_test_backend_fixtures OBJECT
  inference/test_tensor_backend.cpp
  inference/test_causal_lm_backend.cpp)
target_link_libraries(edgeflow_test_backend_fixtures PRIVATE
  edgeflow_internal_headers llm_edgeflow::sdk)

add_library(edgeflow_test_business_model_fixtures OBJECT
  inference/test_business_models.cpp)
target_link_libraries(edgeflow_test_business_model_fixtures PRIVATE
  edgeflow_internal_headers llm_edgeflow::sdk)
