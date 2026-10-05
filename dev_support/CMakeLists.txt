# mock Demo Profile 和测试使用的确定性运行时注册。
# OBJECT 库确保每个消费者都包含全部注册翻译单元。
add_library(edgeflow_test_backend_fixtures OBJECT
  inference/test_tensor_backend.cpp
  inference/test_causal_lm_backend.cpp)
target_link_libraries(edgeflow_test_backend_fixtures PRIVATE
  edgeflow_internal_headers llm_edgeflow::internal_runtime)

add_library(edgeflow_test_biz_model_fixtures OBJECT
  inference/test_biz_models.cpp)
target_link_libraries(edgeflow_test_biz_model_fixtures PRIVATE
  edgeflow_internal_headers llm_edgeflow::internal_runtime)
