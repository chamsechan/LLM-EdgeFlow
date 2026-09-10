# Pinned, verified image decoder. No third-party source is committed.
set(_stb_commit "013ac3beddff3dbffafd5177e7972067cd2b5083")
set(_stb_sha "594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3")
set(_stb_dir "${CMAKE_SOURCE_DIR}/3rdparty/stb_image/${_stb_commit}")
file(MAKE_DIRECTORY "${_stb_dir}")
file(LOCK "${_stb_dir}/.download.lock" GUARD FILE TIMEOUT 60)
if(NOT EXISTS "${_stb_dir}/stb_image.h")
  file(DOWNLOAD
    "https://raw.githubusercontent.com/nothings/stb/${_stb_commit}/stb_image.h"
    "${_stb_dir}/stb_image.h.download"
    EXPECTED_HASH "SHA256=${_stb_sha}" TLS_VERIFY ON)
  file(RENAME "${_stb_dir}/stb_image.h.download" "${_stb_dir}/stb_image.h")
endif()
file(SHA256 "${_stb_dir}/stb_image.h" _stb_actual)
if(NOT _stb_actual STREQUAL _stb_sha)
  message(FATAL_ERROR "Cached stb_image SHA-256 mismatch: ${_stb_dir}/stb_image.h")
endif()
add_library(edgeflow_stb_image INTERFACE)
target_include_directories(edgeflow_stb_image SYSTEM INTERFACE "${_stb_dir}")
