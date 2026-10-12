# Only mock Demo and test tools link these kFixture registrations.
# A single OBJECT target inserts each registration exactly once per consumer.
add_library(edgeflow_demo_fixture_models OBJECT inference/test_biz_models.cpp)
target_link_libraries(edgeflow_demo_fixture_models PRIVATE
  edgeflow_internal_headers llm_edgeflow::internal_runtime)
