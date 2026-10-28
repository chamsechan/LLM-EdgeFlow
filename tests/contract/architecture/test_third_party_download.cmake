cmake_minimum_required(VERSION 3.19)
if(NOT PROJECT_SOURCE_DIR OR NOT TEST_ROOT)
  message(FATAL_ERROR "Supply PROJECT_SOURCE_DIR and isolated TEST_ROOT")
endif()
file(REMOVE_RECURSE "${TEST_ROOT}")
include("${PROJECT_SOURCE_DIR}/cmake_ext/third_party/ThirdPartyDownload.cmake")

# 下载成功后才发布文件，已有缓存不需要访问来源。
file(WRITE "${TEST_ROOT}/source/payload.txt" "download payload\n")
set(url_download "${TEST_ROOT}/downloads/url/payload.txt")
edgeflow_download_file("${url_download}"
  URL "file://${TEST_ROOT}/source/payload.txt")
set(command_download "${TEST_ROOT}/downloads/command/payload.txt")
edgeflow_download_file("${command_download}"
  COMMAND "${CMAKE_COMMAND}" -E copy "${TEST_ROOT}/source/payload.txt"
          "${TEST_ROOT}/downloads/command/staged.txt"
  DOWNLOADED "${TEST_ROOT}/downloads/command/staged.txt")
foreach(download IN ITEMS "${url_download}" "${command_download}")
  if(NOT EXISTS "${download}")
    message(FATAL_ERROR "Successful download was not published: ${download}")
  endif()
  file(READ "${download}" content)
  if(NOT content STREQUAL "download payload\n")
    message(FATAL_ERROR "Published download does not match its source: ${download}")
  endif()
endforeach()
function(expect_download_failure name script diagnostic)
  file(WRITE "${TEST_ROOT}/${name}.cmake"
    "include(\"${PROJECT_SOURCE_DIR}/cmake_ext/third_party/ThirdPartyDownload.cmake\")\n${script}\n")
  execute_process(COMMAND "${CMAKE_COMMAND}" -P "${TEST_ROOT}/${name}.cmake"
                  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "${diagnostic}")
    message(FATAL_ERROR "${name} must fail with '${diagnostic}':\n${output}${error}")
  endif()
endfunction()
expect_download_failure(missing-url
  "edgeflow_download_file(\"${TEST_ROOT}/downloads/missing/payload.txt\" URL \"file://${TEST_ROOT}/source/missing.txt\")"
  "Cannot download")
set(retry_download "${TEST_ROOT}/downloads/retry/payload.txt")
file(WRITE "${retry_download}.download" "interrupted download\n")
expect_download_failure(failed-command
  "edgeflow_download_file(\"${retry_download}\" COMMAND \"${CMAKE_COMMAND}\" -E false FAILURE_MESSAGE \"Download command failed\")"
  "Download command failed")
if(EXISTS "${TEST_ROOT}/downloads/missing/payload.txt" OR EXISTS "${retry_download}")
  message(FATAL_ERROR "Failed download must not become a cached file")
endif()
edgeflow_download_file("${retry_download}" URL "file://${TEST_ROOT}/source/payload.txt")
file(READ "${retry_download}" content)
if(NOT content STREQUAL "download payload\n" OR EXISTS "${retry_download}.download")
  message(FATAL_ERROR "Retry must publish the complete download and consume the staging file")
endif()
file(REMOVE "${TEST_ROOT}/source/payload.txt")
file(WRITE "${url_download}" "local cached payload\n")
edgeflow_download_file("${url_download}" URL "file://${TEST_ROOT}/source/payload.txt")
edgeflow_download_file("${command_download}" COMMAND "${CMAKE_COMMAND}" -E false)
file(READ "${url_download}" content)
if(NOT content STREQUAL "local cached payload\n")
  message(FATAL_ERROR "Existing download must be reused without content verification")
endif()

message(STATUS "PASS staged third-party downloads and cached file reuse")
