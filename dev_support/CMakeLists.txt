# Only mock Demo and test tools link these kFixture registrations.
# A single OBJECT target inserts each registration exactly once per consumer.
file(GLOB_RECURSE EDGEFLOW_DEMO_FIXTURE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/inference/*.cpp")
add_library(edgeflow_demo_fixture_models OBJECT ${EDGEFLOW_DEMO_FIXTURE_SOURCES})
target_link_libraries(edgeflow_demo_fixture_models PRIVATE
  edgeflow_internal_headers llm_edgeflow::internal_runtime)
