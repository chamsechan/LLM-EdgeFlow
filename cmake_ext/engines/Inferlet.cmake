set(LLM_EDGEFLOW_HAS_INFERLET OFF)
if(NOT ENABLE_INFERLET)
  return()
endif()
if(LLM_EDGEFLOW_TARGET_ARCH STREQUAL "x86_64")
  set(_inferlet_platform "x64")
  set(_inferlet_sha256 "3c5e09b43053cc0bddc9ff4c9353be59e810ecf0a41ccd0bb0fed1ddfa77d807")
else()
  set(_inferlet_platform "arm")
  set(_inferlet_sha256 "de7aa0078191e5c4450c057de89206dcb164f5d01892f3b13e6aac83633bf841")
endif()

include(cmake_ext/third_party/ThirdPartyCacheMetadata.cmake)
set(_inferlet_version "v0.1.1")
set(_inferlet_asset "inferlet-${_inferlet_platform}.tar.gz")
# 内网可预置同一归档以离线配置。
set(_inferlet_archive
    "${CMAKE_SOURCE_DIR}/3rdparty/inferlet/${_inferlet_version}/${_inferlet_platform}/${_inferlet_asset}")
edgeflow_download_verified_file("${_inferlet_archive}" "${_inferlet_sha256}"
  URL "https://github.com/chamsechan/Inferlet/releases/download/${_inferlet_version}/${_inferlet_asset}")
edgeflow_import_release_package(Inferlet 0.1.1 "${_inferlet_archive}"
  "${CMAKE_BINARY_DIR}/_deps/inferlet_release" "inferlet-${_inferlet_platform}" libinferlet.a)

include(CheckCXXSourceCompiles)
include(CMakePushCheckState)
cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_LIBRARIES Inferlet::Inferlet)
unset(INFERLET_API_COMPATIBLE CACHE)
check_cxx_source_compiles([[
  #include <inferlet.h>
  int main() {
    inferlet_init();
    void* parameters = inferlet_parameter_allocate();
    inferlet_parameter_set_load_from_file_sync(parameters, 1);
    void* model = inferlet_load_from_file("model.gguf", parameters);
    inferlet_unload(model);
    inferlet_parameter_deallocate(parameters);
    inferlet_deinit();
  }
]] INFERLET_API_COMPATIBLE)
cmake_pop_check_state()
if(NOT INFERLET_API_COMPATIBLE)
  message(FATAL_ERROR "Inferlet C API failed to compile/link; check toolchain and system runtime compatibility")
endif()
set(LLM_EDGEFLOW_HAS_INFERLET ON)
list(APPEND LLM_EDGEFLOW_BACKEND_COMPILE_DEFINITIONS HAVE_INFERLET=1)
list(APPEND LLM_EDGEFLOW_BACKEND_LIBRARIES Inferlet::Inferlet)
