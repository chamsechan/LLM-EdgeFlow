include(FetchContent)

# 自动从 GitHub 下载 nlohmann/json 开源库 (v3.11.3 tarball)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# 禁用 nlohmann 自带测试以加快 CMake 配置与构建速度
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")

FetchContent_MakeAvailable(nlohmann_json)
