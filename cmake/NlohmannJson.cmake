# cmake/NlohmannJson.cmake
# nlohmann/json 现代化 C++ JSON 库智能缓存配置

set(JSON_3RDPARTY_DIR "${CMAKE_SOURCE_DIR}/3rdparty/nlohmann_json")

if(EXISTS "${JSON_3RDPARTY_DIR}/include/nlohmann/json.hpp")
  message(STATUS "[3rdparty] Using cached nlohmann/json from ${JSON_3RDPARTY_DIR}")
  if(NOT TARGET nlohmann_json::nlohmann_json)
    add_library(nlohmann_json INTERFACE)
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
    target_include_directories(nlohmann_json INTERFACE "${JSON_3RDPARTY_DIR}/include")
  endif()
else()
  include(FetchContent)

  # 自动从 GitHub 下载 nlohmann/json 开源库 (v3.11.3 tarball)
  FetchContent_Declare(
      nlohmann_json
      URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz
      URL_HASH SHA256=0d8ef5af7f9794e3263480193c491549b2ba6cc74bb018906202ada498a79406
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )

  # 禁用 nlohmann 自带测试以加快 CMake 配置与构建速度
  set(JSON_BuildTests OFF CACHE INTERNAL "")
  set(JSON_Install OFF CACHE INTERNAL "")

  FetchContent_MakeAvailable(nlohmann_json)

  # 自动同步头文件至 3rdparty/nlohmann_json
  file(MAKE_DIRECTORY "${JSON_3RDPARTY_DIR}/include")
  file(COPY "${nlohmann_json_SOURCE_DIR}/include/nlohmann" DESTINATION "${JSON_3RDPARTY_DIR}/include")
endif()
