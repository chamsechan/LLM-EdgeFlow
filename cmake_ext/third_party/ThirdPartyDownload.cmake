include_guard(GLOBAL)

include(CMakeParseArguments)

# 较旧的 CMake 本就保留归档时间戳；显式开关在 3.24 才引入。
# 在所有支持的版本上保持相同的解压行为。
set(EDGEFLOW_FETCHCONTENT_TIMESTAMP_ARGS)
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.24")
  set(EDGEFLOW_FETCHCONTENT_TIMESTAMP_ARGS DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
endif()

# 先下载到暂存文件，下载成功后再改名，避免共享缓存使用中断的下载。
# 已有文件直接复用；URL 直接下载，COMMAND 用于需要认证的下载工具。
function(edgeflow_download_file path)
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
    file(RENAME "${_downloaded}" "${path}")
  endif()
endfunction()

# 每次配置把下载的归档重新解压到当前构建，避免残留文件和旧环境的绝对路径；
# 只接受解压目录内与固定版本完全一致的 CMake 包及其静态库。
function(edgeflow_import_release_package package version archive extract_dir package_root library)
  file(REMOVE_RECURSE "${extract_dir}")
  file(MAKE_DIRECTORY "${extract_dir}")
  execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xzf "${archive}"
                  WORKING_DIRECTORY "${extract_dir}" RESULT_VARIABLE _result)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "Cannot extract ${package} release: ${archive}")
  endif()
  set(_root "${extract_dir}/${package_root}")
  unset(${package}_DIR CACHE)
  find_package(${package} ${version} EXACT CONFIG REQUIRED
               PATHS "${_root}/lib/cmake/${package}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  get_target_property(_library ${package}::${package} IMPORTED_LOCATION_RELEASE)
  if(NOT _library STREQUAL "${_root}/lib/${library}")
    message(FATAL_ERROR "${package} must resolve to the selected release archive")
  endif()
  message(STATUS "[3rdparty] Imported ${package} ${version}: ${_library}")
endfunction()
