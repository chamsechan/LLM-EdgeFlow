# 只有 mock Demo 和测试工具链接这些 kFixture 注册。
# 单个 OBJECT 目标保证每个使用方恰好插入一次各项注册。
file(GLOB_RECURSE EDGEFLOW_DEMO_FIXTURE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/inference/*.cpp")
add_library(edgeflow_demo_fixture_models OBJECT ${EDGEFLOW_DEMO_FIXTURE_SOURCES})
target_link_libraries(edgeflow_demo_fixture_models PRIVATE
  edgeflow_internal_headers llm_edgeflow::internal_runtime)
