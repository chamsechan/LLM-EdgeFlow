# 版本固定且经校验的图像解码器。不提交任何第三方源码。
include(cmake_ext/third_party/ThirdPartyCacheMetadata.cmake)
set(_stb_commit "013ac3beddff3dbffafd5177e7972067cd2b5083")
set(_stb_dir "${CMAKE_SOURCE_DIR}/3rdparty/stb_image/${_stb_commit}")
edgeflow_download_verified_file("${_stb_dir}/stb_image.h"
  "594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3"
  URL "https://raw.githubusercontent.com/nothings/stb/${_stb_commit}/stb_image.h")
add_library(edgeflow_stb_image INTERFACE)
target_include_directories(edgeflow_stb_image SYSTEM INTERFACE "${_stb_dir}")
