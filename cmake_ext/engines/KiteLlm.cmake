# 经用户授权的私有 GitHub 依赖。认证信息保留在 gh 凭据存储或 GH_TOKEN 中；
# 绝不能把 token 写入 CMake 文件或缓存。
set(LLM_EDGEFLOW_HAS_KITELLM OFF)
if(NOT ENABLE_KITELLM)
  return()
endif()

include(cmake_ext/third_party/ThirdPartyDownload.cmake)

set(_kite_release "v0.1.0")
if(LLM_EDGEFLOW_TARGET_ARCH STREQUAL "x86_64")
  set(_kite_platform "x64")
else()
  set(_kite_platform "arm")
endif()
set(_kite_asset "kiteLLM-${_kite_platform}.tar.gz")
set(_kite_cache "${CMAKE_SOURCE_DIR}/3rdparty/kite_llm/${_kite_release}/${_kite_platform}")
set(_kite_archive "${_kite_cache}/${_kite_asset}")
# 不同构建目录共享此归档缓存；只有缺少归档时才调用 gh。
find_program(KITELLM_GH_EXECUTABLE NAMES gh)
file(MAKE_DIRECTORY "${_kite_cache}/download")
edgeflow_download_file("${_kite_archive}"
  COMMAND "${KITELLM_GH_EXECUTABLE}" release download "${_kite_release}"
          --repo chamsechan/kiteLLM --pattern "${_kite_asset}"
          --dir "${_kite_cache}/download" --clobber
  DOWNLOADED "${_kite_cache}/download/${_kite_asset}"
  FAILURE_MESSAGE
    "Cannot download private kiteLLM release. Install gh and run gh auth login; "
    "the account (or CI GH_TOKEN) must have read access to chamsechan/kiteLLM.")
edgeflow_import_release_package(kiteLLM 0.1.0 "${_kite_archive}"
  "${CMAKE_BINARY_DIR}/_deps/kite_llm_release" "kiteLLM-${_kite_platform}" libkiteLLM.a)

set(LLM_EDGEFLOW_HAS_KITELLM ON)
list(APPEND LLM_EDGEFLOW_BACKEND_COMPILE_DEFINITIONS HAVE_KITELLM=1)
list(APPEND LLM_EDGEFLOW_BACKEND_LIBRARIES kiteLLM::kiteLLM)
