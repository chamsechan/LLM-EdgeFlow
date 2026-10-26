include_guard(GLOBAL)

include(CMakeParseArguments)

# 较旧的 CMake 本就保留归档时间戳；显式开关在 3.24 才引入。
# 在所有支持的版本上保持相同的解压行为。
set(EDGEFLOW_FETCHCONTENT_TIMESTAMP_ARGS)
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.24")
  set(EDGEFLOW_FETCHCONTENT_TIMESTAMP_ARGS DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
endif()

# 准备可检查的缓存标记，并报告现有缓存是否由相同的固定源码
# 和 ABI 相关构建环境生成；REQUIRED_FILES 缺失时同样不可复用。
function(edgeflow_prepare_third_party_cache)
  set(_one_value_args
      NAME VERSION SOURCE_SHA256 CACHE_DIR KIND MARKER_ROOT OUT_VALID OUT_MARKER OUT_CACHE_DIR)
  set(_multi_value_args ABI_OPTIONS REQUIRED_FILES)
  cmake_parse_arguments(ARG "" "${_one_value_args}" "${_multi_value_args}"
                        ${ARGN})

  foreach(_required NAME VERSION SOURCE_SHA256 CACHE_DIR KIND OUT_VALID OUT_MARKER)
    if(NOT ARG_${_required})
      message(FATAL_ERROR
        "edgeflow_prepare_third_party_cache requires ${_required}")
    endif()
  endforeach()
  if(NOT ARG_KIND MATCHES "^(HEADER_ONLY|PREBUILT|STATIC)$")
    message(FATAL_ERROR
      "Unsupported third-party cache kind '${ARG_KIND}'")
  endif()

  set(_basis
      "schema=1\nname=${ARG_NAME}\nversion=${ARG_VERSION}\nsource_sha256=${ARG_SOURCE_SHA256}\nkind=${ARG_KIND}\n")
  if(ARG_KIND STREQUAL "PREBUILT" OR ARG_KIND STREQUAL "STATIC")
    set(_target_arch "${CMAKE_SYSTEM_PROCESSOR}")
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND CMAKE_OSX_ARCHITECTURES)
      set(_target_arch "${CMAKE_OSX_ARCHITECTURES}")
    endif()
    string(APPEND _basis
      "system_name=${CMAKE_SYSTEM_NAME}\nsystem_processor=${_target_arch}\npointer_size=${CMAKE_SIZEOF_VOID_P}\n")
  endif()
  if(ARG_KIND STREQUAL "STATIC")
    string(APPEND _basis
      "osx_architectures=${CMAKE_OSX_ARCHITECTURES}\nosx_sysroot=${CMAKE_OSX_SYSROOT}\n"
      "osx_deployment_target=${CMAKE_OSX_DEPLOYMENT_TARGET}\nsysroot=${CMAKE_SYSROOT}\n"
      "apple_sdk_path=${LLM_EDGEFLOW_APPLE_SDK_PATH}\napple_sdk_version=${LLM_EDGEFLOW_APPLE_SDK_VERSION}\n")
    string(APPEND _basis
      "c_compiler=${CMAKE_C_COMPILER_ID}-${CMAKE_C_COMPILER_VERSION}\n"
      "cxx_compiler=${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}\n"
      "cxx_standard=${CMAKE_CXX_STANDARD}\n"
      "c_compiler_path=${CMAKE_C_COMPILER}\ncxx_compiler_path=${CMAKE_CXX_COMPILER}\n"
      "c_target=${CMAKE_C_COMPILER_TARGET}\ncxx_target=${CMAKE_CXX_COMPILER_TARGET}\n"
      "build_type=${CMAKE_BUILD_TYPE}\npic=${CMAKE_POSITION_INDEPENDENT_CODE}\n"
      "sanitizers_enabled=${ENABLE_SANITIZERS}\nsanitizers=${LLM_EDGEFLOW_SANITIZERS}\n")
    if(CMAKE_TOOLCHAIN_FILE AND EXISTS "${CMAKE_TOOLCHAIN_FILE}")
      file(SHA256 "${CMAKE_TOOLCHAIN_FILE}" _toolchain_hash)
      string(APPEND _basis "toolchain_sha256=${_toolchain_hash}\n")
    endif()
    foreach(variable CMAKE_C_FLAGS CMAKE_CXX_FLAGS CMAKE_EXE_LINKER_FLAGS
        CMAKE_SHARED_LINKER_FLAGS CMAKE_STATIC_LINKER_FLAGS
        CMAKE_SYSROOT_COMPILE CMAKE_SYSROOT_LINK CMAKE_C_COMPILER_EXTERNAL_TOOLCHAIN
        CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN)
      string(APPEND _basis "${variable}=${${variable}}\n")
    endforeach()
    if(CMAKE_BUILD_TYPE)
      string(TOUPPER "${CMAKE_BUILD_TYPE}" config)
      foreach(prefix CMAKE_C_FLAGS CMAKE_CXX_FLAGS CMAKE_EXE_LINKER_FLAGS CMAKE_SHARED_LINKER_FLAGS CMAKE_STATIC_LINKER_FLAGS)
        string(APPEND _basis "${prefix}_${config}=${${prefix}_${config}}\n")
      endforeach()
    endif()
  endif()
  if(ARG_ABI_OPTIONS)
    list(SORT ARG_ABI_OPTIONS)
    string(JOIN "," _abi_options ${ARG_ABI_OPTIONS})
  else()
    set(_abi_options "none")
  endif()
  string(APPEND _basis "abi_options=${_abi_options}\n")
  string(SHA256 _fingerprint "${_basis}")
  set(_metadata "${_basis}fingerprint=${_fingerprint}\n")

  # 原始下载包可以共享；构建出的库按完整身份分目录，平台/配置切换不会覆盖。
  set(_cache_dir "${ARG_CACHE_DIR}")
  if(ARG_OUT_CACHE_DIR AND NOT ARG_KIND STREQUAL "HEADER_ONLY")
    set(_cache_platform "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
    if(LLM_EDGEFLOW_TARGET_PLATFORM)
      set(_cache_platform "${LLM_EDGEFLOW_TARGET_PLATFORM}")
    endif()
    set(_cache_dir "${ARG_CACHE_DIR}/${_cache_platform}/${_fingerprint}")
  endif()

  if(ARG_MARKER_ROOT)
    set(_marker_dir "${ARG_MARKER_ROOT}")
  else()
    set(_marker_dir "${CMAKE_BINARY_DIR}/third_party_cache_markers")
  endif()
  file(MAKE_DIRECTORY "${_marker_dir}")
  set(_expected_marker "${_marker_dir}/${ARG_NAME}.txt")
  file(WRITE "${_expected_marker}" "${_metadata}")

  set(_cache_marker "${_cache_dir}/.edgeflow-cache-fingerprint")
  set(_valid OFF)
  if(EXISTS "${_cache_marker}")
    file(READ "${_cache_marker}" _cached_metadata)
    if(_cached_metadata STREQUAL _metadata)
      set(_valid ON)
    else()
      message(STATUS
        "[3rdparty] Ignoring incompatible ${ARG_NAME} cache: ${_cache_dir}")
    endif()
  elseif(EXISTS "${_cache_dir}")
    message(STATUS
      "[3rdparty] Ignoring unverified ${ARG_NAME} cache: ${_cache_dir}")
  endif()
  # 指纹只证明身份，归档仍须完整才能导入。
  foreach(_required IN LISTS ARG_REQUIRED_FILES)
    if(_valid AND NOT EXISTS "${_cache_dir}/${_required}")
      message(STATUS
        "[3rdparty] Ignoring incomplete ${ARG_NAME} cache: missing ${_required}")
      set(_valid OFF)
    endif()
  endforeach()

  set(${ARG_OUT_VALID} "${_valid}" PARENT_SCOPE)
  set(${ARG_OUT_MARKER} "${_expected_marker}" PARENT_SCOPE)
  if(ARG_OUT_CACHE_DIR)
    set(${ARG_OUT_CACHE_DIR} "${_cache_dir}" PARENT_SCOPE)
  endif()
endfunction()

# 源码构建完成后把库和头文件发布到缓存身份目录；指纹最后写入，
# 中断的发布不会成为可导入缓存。
function(edgeflow_publish_third_party_cache name)
  cmake_parse_arguments(ARG "" "CACHE_DIR;MARKER" "DEPENDS;LIBRARIES;HEADERS;HEADER_DIRS"
                        ${ARGN})
  set(_commands
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${ARG_CACHE_DIR}/lib" "${ARG_CACHE_DIR}/include")
  foreach(_library IN LISTS ARG_LIBRARIES)
    list(APPEND _commands
         COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_library}" "${ARG_CACHE_DIR}/lib/")
  endforeach()
  foreach(_header IN LISTS ARG_HEADERS)
    list(APPEND _commands
         COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_header}" "${ARG_CACHE_DIR}/include/")
  endforeach()
  foreach(_directory IN LISTS ARG_HEADER_DIRS)
    list(APPEND _commands
         COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_directory}" "${ARG_CACHE_DIR}/include")
  endforeach()
  list(APPEND _commands
       COMMAND "${CMAKE_COMMAND}" -E copy_if_different
               "${ARG_MARKER}" "${ARG_CACHE_DIR}/.edgeflow-cache-fingerprint")
  add_custom_target(archive_${name}_to_3rdparty ALL ${_commands}
      DEPENDS ${ARG_DEPENDS}
      COMMENT "[3rdparty] Archiving ${name} to ${ARG_CACHE_DIR}"
      VERBATIM)
endfunction()

# 先下载到暂存文件并校验 SHA-256 再改名，共享缓存只出现完整文件；
# 已有文件每次配置都重新校验。URL 直接下载，COMMAND 用于需要认证的下载工具。
function(edgeflow_download_verified_file path sha256)
  cmake_parse_arguments(ARG "" "URL;DOWNLOADED;FAILURE_MESSAGE" "COMMAND" ${ARGN})
  get_filename_component(_directory "${path}" DIRECTORY)
  file(MAKE_DIRECTORY "${_directory}")
  file(LOCK "${_directory}/.download.lock" GUARD FUNCTION TIMEOUT 60)
  if(NOT EXISTS "${path}")
    message(STATUS "[3rdparty] Downloading ${path}")
    set(_downloaded "${path}.download")
    if(ARG_URL)
      file(DOWNLOAD "${ARG_URL}" "${_downloaded}" TLS_VERIFY ON STATUS _status TIMEOUT 120)
      list(GET _status 0 _result)
    else()
      if(ARG_DOWNLOADED)
        set(_downloaded "${ARG_DOWNLOADED}")
      endif()
      execute_process(COMMAND ${ARG_COMMAND} RESULT_VARIABLE _result OUTPUT_QUIET ERROR_QUIET)
      set(_status "${_result}")
    endif()
    if(NOT _result EQUAL 0)
      if(ARG_FAILURE_MESSAGE)
        message(FATAL_ERROR "${ARG_FAILURE_MESSAGE}")
      endif()
      message(FATAL_ERROR "Cannot download ${path}: ${_status}")
    endif()
    file(SHA256 "${_downloaded}" _actual)
    if(NOT _actual STREQUAL sha256)
      message(FATAL_ERROR "Downloaded SHA-256 mismatch: ${path}")
    endif()
    file(RENAME "${_downloaded}" "${path}")
  endif()
  file(SHA256 "${path}" _actual)
  if(NOT _actual STREQUAL sha256)
    message(FATAL_ERROR "Cached SHA-256 mismatch. Remove ${path} and configure again.")
  endif()
endfunction()

# 每次配置把已校验归档重新解压到当前构建，避免残留文件和旧环境的绝对路径；
# 只接受解压目录内与固定版本完全一致的 CMake 包及其静态库。
function(edgeflow_import_release_package package version archive extract_dir package_root library)
  file(REMOVE_RECURSE "${extract_dir}")
  file(MAKE_DIRECTORY "${extract_dir}")
  execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xzf "${archive}"
                  WORKING_DIRECTORY "${extract_dir}" RESULT_VARIABLE _result)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "Cannot extract verified ${package} release: ${archive}")
  endif()
  set(_root "${extract_dir}/${package_root}")
  unset(${package}_DIR CACHE)
  find_package(${package} ${version} EXACT CONFIG REQUIRED
               PATHS "${_root}/lib/cmake/${package}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  get_target_property(_library ${package}::${package} IMPORTED_LOCATION_RELEASE)
  if(NOT _library STREQUAL "${_root}/lib/${library}")
    message(FATAL_ERROR "${package} must resolve to the verified release archive")
  endif()
  message(STATUS "[3rdparty] Verified ${package} ${version}: ${_library}")
endfunction()
