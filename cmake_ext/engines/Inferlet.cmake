set(LLM_EDGEFLOW_HAS_INFERLET OFF)
if(NOT ENABLE_INFERLET)
  return()
endif()
if(LLM_EDGEFLOW_TARGET_ARCH STREQUAL "x86_64")
  set(_inferlet_platform "x64")
else()
  set(_inferlet_platform "arm")
endif()

include(cmake_ext/third_party/ThirdPartyDownload.cmake)
set(_inferlet_version "v0.1.1")
set(_inferlet_asset "inferlet-${_inferlet_platform}.tar.gz")
# 内网可预置同一归档以离线配置。
set(_inferlet_archive
    "${CMAKE_SOURCE_DIR}/3rdparty/inferlet/${_inferlet_version}/${_inferlet_platform}/${_inferlet_asset}")
edgeflow_download_file("${_inferlet_archive}"
  URL "https://github.com/chamsechan/Inferlet/releases/download/${_inferlet_version}/${_inferlet_asset}")
edgeflow_import_release_package(Inferlet 0.1.1 "${_inferlet_archive}"
  "${CMAKE_BINARY_DIR}/_deps/inferlet_release" "inferlet-${_inferlet_platform}" libinferlet.a)

set(LLM_EDGEFLOW_HAS_INFERLET ON)
list(APPEND LLM_EDGEFLOW_BACKEND_COMPILE_DEFINITIONS HAVE_INFERLET=1)
list(APPEND LLM_EDGEFLOW_BACKEND_LIBRARIES Inferlet::Inferlet)
