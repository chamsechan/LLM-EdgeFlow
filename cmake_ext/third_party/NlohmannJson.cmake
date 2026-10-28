# nlohmann/json：下载固定版本的头文件库。

include(FetchContent)
include(cmake_ext/third_party/ThirdPartyDownload.cmake)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz
    ${EDGEFLOW_FETCHCONTENT_TIMESTAMP_ARGS}
)

# 禁用 nlohmann 自带测试以加快 CMake 配置与构建速度
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")

FetchContent_MakeAvailable(nlohmann_json)
