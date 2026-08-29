# llama.cpp commit 70adb1b embeds a ggml CMake project that unconditionally
# discovers Git from its source directory. FetchContent extracts below the host
# repository, so Git walks upward and embeds the host commit in GGML_COMMIT.
# Replace only that known discovery block and fail closed if upstream changes.

if(NOT DEFINED LLM_EDGEFLOW_LLAMA_SOURCE_DIR OR
   NOT DEFINED LLM_EDGEFLOW_LLAMA_COMMIT)
  message(FATAL_ERROR "llama.cpp source directory and commit are required")
endif()

set(ggml_cmake
    "${LLM_EDGEFLOW_LLAMA_SOURCE_DIR}/ggml/CMakeLists.txt")
if(NOT EXISTS "${ggml_cmake}")
  message(FATAL_ERROR "Expected llama.cpp ggml CMakeLists.txt is missing")
endif()

file(READ "${ggml_cmake}" ggml_content)
set(patch_marker "# LLM-EdgeFlow: deterministic FetchContent build metadata")
if(ggml_content MATCHES "${patch_marker}")
  if(NOT ggml_content MATCHES
     "set\\(GGML_BUILD_COMMIT \"${LLM_EDGEFLOW_LLAMA_COMMIT}\"\\)")
    message(FATAL_ERROR "Existing ggml build-info patch has another commit")
  endif()
  return()
endif()

set(block_start "find_program(GIT_EXE NAMES git git.exe NO_CMAKE_FIND_ROOT_PATH)")
set(block_end "set(GGML_VERSION \"\${GGML_VERSION_BASE}\")")
string(FIND "${ggml_content}" "${block_start}" start_index)
string(FIND "${ggml_content}" "${block_end}" end_index)
if(start_index EQUAL -1 OR end_index EQUAL -1 OR
   end_index LESS_EQUAL start_index)
  message(FATAL_ERROR "Unsupported ggml build-info block; pinned patch not applied")
endif()

string(SUBSTRING "${ggml_content}" 0 ${start_index} content_before)
string(SUBSTRING "${ggml_content}" ${end_index} -1 content_after)
set(replacement
    "${patch_marker}\nset(GGML_BUILD_COMMIT \"${LLM_EDGEFLOW_LLAMA_COMMIT}\")\n\n")
file(WRITE "${ggml_cmake}" "${content_before}${replacement}${content_after}")
