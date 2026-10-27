# nlohmann/json：复用已缓存的头文件，缓存不可用时下载固定版本。

set(JSON_3RDPARTY_DIR "${CMAKE_SOURCE_DIR}/3rdparty/nlohmann_json")
include(cmake_ext/third_party/ThirdPartyCacheMetadata.cmake)
edgeflow_prepare_third_party_cache(
  NAME nlohmann_json
  VERSION 3.11.3
  SOURCE_SHA256 0d8ef5af7f9794e3263480193c491549b2ba6cc74bb018906202ada498a79406
  CACHE_DIR "${JSON_3RDPARTY_DIR}"
  KIND HEADER_ONLY
  REQUIRED_FILES include/nlohmann/json.hpp
  OUT_VALID _JSON_CACHE_VALID
  OUT_MARKER _JSON_CACHE_MARKER)

if(_JSON_CACHE_VALID)
  message(STATUS "[3rdparty] Using cached nlohmann/json from ${JSON_3RDPARTY_DIR}")
  add_library(nlohmann_json INTERFACE)
  add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
  target_include_directories(nlohmann_json INTERFACE "${JSON_3RDPARTY_DIR}/include")
else()
  include(FetchContent)

  # 自动从 GitHub 下载 nlohmann/json 开源库 (v3.11.3 tarball)
  FetchContent_Declare(
      nlohmann_json
      URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz
      URL_HASH SHA256=0d8ef5af7f9794e3263480193c491549b2ba6cc74bb018906202ada498a79406
      ${EDGEFLOW_FETCHCONTENT_TIMESTAMP_ARGS}
  )

  # 禁用 nlohmann 自带测试以加快 CMake 配置与构建速度
  set(JSON_BuildTests OFF CACHE INTERNAL "")
  set(JSON_Install OFF CACHE INTERNAL "")

  FetchContent_MakeAvailable(nlohmann_json)

  # 自动同步头文件至 3rdparty/nlohmann_json
  file(MAKE_DIRECTORY "${JSON_3RDPARTY_DIR}/include")
  file(COPY "${nlohmann_json_SOURCE_DIR}/include/nlohmann" DESTINATION "${JSON_3RDPARTY_DIR}/include")
  configure_file("${_JSON_CACHE_MARKER}"
                 "${JSON_3RDPARTY_DIR}/.edgeflow-cache-fingerprint"
                 COPYONLY)
endif()
