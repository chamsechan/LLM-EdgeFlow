set(LLM_EDGEFLOW_HAS_INFERLET OFF)
if(NOT ENABLE_INFERLET)
  return()
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  set(_inferlet_platform "x64")
  set(_inferlet_sha256 "7dd915e4289c608580cbda927a80834cb0fbae56f0e420c450d034660b125701")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
       CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  set(_inferlet_platform "arm")
  set(_inferlet_sha256 "e5c735c36629d425e89e7086c910fa1af75965b55f5fcfc88df6794202437247")
else()
  message(FATAL_ERROR "Inferlet integration supports Linux x86_64/aarch64 only")
endif()

include(cmake_ext/ThirdPartyCacheMetadata.cmake)
set(_inferlet_version "v0.1.0")
set(_inferlet_asset "inferlet-${_inferlet_platform}.tar.gz")
set(_inferlet_cache "${CMAKE_SOURCE_DIR}/3rdparty/inferlet/${_inferlet_version}/${_inferlet_platform}")
set(_inferlet_archive "${_inferlet_cache}/${_inferlet_asset}")
file(MAKE_DIRECTORY "${_inferlet_cache}")
file(LOCK "${_inferlet_cache}/.download.lock" GUARD PROCESS TIMEOUT 60)
if(NOT EXISTS "${_inferlet_archive}")
  file(DOWNLOAD
    "https://github.com/chamsechan/Inferlet/releases/download/${_inferlet_version}/${_inferlet_asset}"
    "${_inferlet_archive}.download"
    TLS_VERIFY ON STATUS _inferlet_download_status TIMEOUT 120)
  list(GET _inferlet_download_status 0 _inferlet_download_code)
  if(NOT _inferlet_download_code EQUAL 0)
    message(FATAL_ERROR "Cannot download Inferlet: ${_inferlet_download_status}")
  endif()
  file(SHA256 "${_inferlet_archive}.download" _inferlet_actual_sha256)
  if(NOT _inferlet_actual_sha256 STREQUAL _inferlet_sha256)
    message(FATAL_ERROR "Downloaded Inferlet SHA-256 mismatch")
  endif()
  file(RENAME "${_inferlet_archive}.download" "${_inferlet_archive}")
endif()
file(SHA256 "${_inferlet_archive}" _inferlet_actual_sha256)
if(NOT _inferlet_actual_sha256 STREQUAL _inferlet_sha256)
  message(FATAL_ERROR "Cached Inferlet SHA-256 mismatch: ${_inferlet_archive}")
endif()

# 从已校验归档重建当前构建的包；内网可预置同一归档以离线配置。
set(_inferlet_extract "${CMAKE_BINARY_DIR}/_deps/inferlet_release")
file(REMOVE_RECURSE "${_inferlet_extract}")
file(MAKE_DIRECTORY "${_inferlet_extract}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xzf "${_inferlet_archive}"
  WORKING_DIRECTORY "${_inferlet_extract}" RESULT_VARIABLE _inferlet_extract_result)
if(NOT _inferlet_extract_result EQUAL 0)
  message(FATAL_ERROR "Cannot extract verified Inferlet release")
endif()
edgeflow_prepare_third_party_cache(
  NAME inferlet VERSION "${_inferlet_version}" SOURCE_SHA256 "${_inferlet_sha256}"
  CACHE_DIR "${_inferlet_cache}" KIND PREBUILT
  OUT_VALID _inferlet_cache_valid OUT_MARKER _inferlet_marker)
configure_file("${_inferlet_marker}" "${_inferlet_cache}/.edgeflow-cache-fingerprint" COPYONLY)

set(_inferlet_root "${_inferlet_extract}/inferlet-${_inferlet_platform}")
unset(Inferlet_DIR CACHE)
set(Inferlet_DIR "${_inferlet_root}/lib/cmake/Inferlet")
find_package(Inferlet 0.1.0 EXACT CONFIG REQUIRED
  PATHS "${Inferlet_DIR}" NO_DEFAULT_PATH)
get_target_property(_inferlet_library Inferlet::Inferlet IMPORTED_LOCATION_RELEASE)
if(NOT _inferlet_library STREQUAL "${_inferlet_root}/lib/libinferlet.a")
  message(FATAL_ERROR "Inferlet must resolve to the verified release archive")
endif()

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
list(APPEND THIRD_PARTY_ENGINE_LIBS Inferlet::Inferlet)
message(STATUS "[Inferlet] Verified ${_inferlet_version}: ${_inferlet_library}")
