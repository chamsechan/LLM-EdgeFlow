# PCRE2：从固定版本源码构建。

include(FetchContent)
include(cmake_ext/third_party/ThirdPartyDownload.cmake)

# PCRE2 是 alg_sdk 的实现细节，静态链接时不得从公开共享库导出其 C 符号。
add_library(edgeflow_pcre2 INTERFACE)
target_compile_definitions(edgeflow_pcre2 INTERFACE PCRE2_STATIC)
if(LLM_EDGEFLOW_TARGET_OS STREQUAL "macos")
  # 按名称隐藏整个静态归档的符号。
  target_link_libraries(edgeflow_pcre2 INTERFACE "-Wl,-hidden-lpcre2-8")
else()
  target_link_options(edgeflow_pcre2 INTERFACE "LINKER:--exclude-libs,libpcre2-8.a")
endif()

# TextRuleMatchNode 使用 PCRE2 实现 Unicode 感知的后行断言和命名捕获。
# 该依赖须保持私有、源码构建、版本固定。
set(PCRE2_BUILD_PCRE2_8 ON CACHE BOOL "Build PCRE2 8-bit library" FORCE)
set(PCRE2_BUILD_PCRE2_16 OFF CACHE BOOL "Disable PCRE2 16-bit library" FORCE)
set(PCRE2_BUILD_PCRE2_32 OFF CACHE BOOL "Disable PCRE2 32-bit library" FORCE)
set(PCRE2_BUILD_PCRE2GREP OFF CACHE BOOL "Disable pcre2grep" FORCE)
set(PCRE2_BUILD_TESTS OFF CACHE BOOL "Disable PCRE2 upstream tests" FORCE)
set(PCRE2_SHOW_REPORT OFF CACHE BOOL "Disable PCRE2 configuration report" FORCE)
set(PCRE2_STATIC_PIC ON CACHE BOOL "Build position-independent PCRE2" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "Build static third-party libraries" FORCE)

FetchContent_Declare(
    pcre2
    URL https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.47/pcre2-10.47.tar.gz
    ${EDGEFLOW_FETCHCONTENT_TIMESTAMP_ARGS}
)
FetchContent_MakeAvailable(pcre2)
target_compile_options(pcre2-8-static PRIVATE -fvisibility=hidden)
add_dependencies(edgeflow_pcre2 pcre2-8-static)

target_include_directories(edgeflow_pcre2 SYSTEM INTERFACE "${pcre2_BINARY_DIR}/interface")
if(LLM_EDGEFLOW_TARGET_OS STREQUAL "macos")
  target_link_directories(edgeflow_pcre2 INTERFACE "$<TARGET_FILE_DIR:pcre2-8-static>")
else()
  target_link_libraries(edgeflow_pcre2 INTERFACE pcre2-8)
endif()
