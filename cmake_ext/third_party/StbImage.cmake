# 版本固定的图像解码器。不提交任何第三方源码。
include(cmake_ext/third_party/ThirdPartyCacheMetadata.cmake)
set(_stb_commit "013ac3beddff3dbffafd5177e7972067cd2b5083")
set(_stb_dir "${CMAKE_SOURCE_DIR}/3rdparty/stb_image/${_stb_commit}")
edgeflow_download_file("${_stb_dir}/stb_image.h"
  URL "https://raw.githubusercontent.com/nothings/stb/${_stb_commit}/stb_image.h")
add_library(edgeflow_stb_image INTERFACE)
target_include_directories(edgeflow_stb_image SYSTEM INTERFACE "${_stb_dir}")
