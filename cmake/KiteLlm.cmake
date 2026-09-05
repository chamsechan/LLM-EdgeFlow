# User-authorized private GitHub dependency. Authentication remains in gh's
# credential store or GH_TOKEN; never materialize tokens in CMake/cache files.
option(ENABLE_KITELLM "Download and link the pinned kiteLLM static release" OFF)
set(LLM_EDGEFLOW_HAS_KITELLM OFF)
if(NOT ENABLE_KITELLM)
  return()
endif()
if(ENABLE_LLAMACPP)
  message(FATAL_ERROR
    "kiteLLM embeds a different llama.cpp/ggml version. Use "
    "-DENABLE_KITELLM=ON -DENABLE_LLAMACPP=OFF to avoid symbol collisions.")
endif()
if(KITELLM_ROOT)
  message(FATAL_ERROR
    "KITELLM_ROOT is obsolete. Remove it with -UKITELLM_ROOT; "
    "kiteLLM is now obtained from the pinned GitHub release.")
endif()

set(_kite_release "v0.1.0")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  set(_kite_platform "x64")
  set(_kite_sha256 "bddaa55abe37cfc87f481139c41aadf79ad579b7cc20fe2b12c01978a2d3b1a0")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
       CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  set(_kite_platform "arm")
  set(_kite_sha256 "1388da99c8fd4a45d0f85a489d29b2a12189e4955302ed75fd8bbddd1ebfc70d")
else()
  message(FATAL_ERROR
    "Pinned kiteLLM integration supports Linux x86_64/aarch64 only; got "
    "${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}")
endif()
set(_kite_asset "kiteLLM-${_kite_platform}.tar.gz")
set(_kite_cache "${LLM_EDGEFLOW_3RDPARTY_DIR}/kite_llm/${_kite_release}/${_kite_platform}")
set(_kite_archive "${_kite_cache}/${_kite_asset}")
file(MAKE_DIRECTORY "${_kite_cache}")
# Separate build directories share this archive cache.
file(LOCK "${_kite_cache}/.download.lock" GUARD PROCESS TIMEOUT 60)
if(NOT EXISTS "${_kite_archive}")
  find_program(KITELLM_GH_EXECUTABLE NAMES gh)
  if(NOT KITELLM_GH_EXECUTABLE)
    message(FATAL_ERROR
      "kiteLLM is a private GitHub dependency. Install gh and run gh auth login "
      "with repository read access (CI: set GH_TOKEN).")
  endif()
  message(STATUS "[kiteLLM] Downloading chamsechan/kiteLLM ${_kite_release}/${_kite_asset}")
  # Download into a staging directory so interrupted downloads never become a
  # reusable archive. The checksum below is authoritative even if a tag moves.
  set(_kite_download "${_kite_cache}/download")
  file(MAKE_DIRECTORY "${_kite_download}")
  execute_process(
    COMMAND "${KITELLM_GH_EXECUTABLE}" release download "${_kite_release}"
            --repo chamsechan/kiteLLM --pattern "${_kite_asset}"
            --dir "${_kite_download}" --clobber
    RESULT_VARIABLE _kite_download_result
    OUTPUT_QUIET ERROR_QUIET)
  if(NOT _kite_download_result EQUAL 0)
    message(FATAL_ERROR
      "Cannot download private kiteLLM release. Check network and gh auth status; "
      "the account (or CI GH_TOKEN) must have read access to chamsechan/kiteLLM.")
  endif()
  file(SHA256 "${_kite_download}/${_kite_asset}" _kite_actual_sha256)
  if(NOT _kite_actual_sha256 STREQUAL _kite_sha256)
    message(FATAL_ERROR "Downloaded kiteLLM release SHA-256 mismatch")
  endif()
  file(RENAME "${_kite_download}/${_kite_asset}" "${_kite_archive}")
endif()
file(SHA256 "${_kite_archive}" _kite_actual_sha256)
if(NOT _kite_actual_sha256 STREQUAL _kite_sha256)
  message(FATAL_ERROR
    "Cached kiteLLM SHA-256 mismatch. Remove ${_kite_archive} and configure again.")
endif()

# Re-extract only verified bytes into this build, avoiding stale/tampered headers
# or libraries and machine-specific absolute paths from previous environments.
set(_kite_extract "${CMAKE_BINARY_DIR}/_deps/kite_llm_release")
file(MAKE_DIRECTORY "${_kite_extract}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xzf "${_kite_archive}"
  WORKING_DIRECTORY "${_kite_extract}" RESULT_VARIABLE _kite_extract_result)
if(NOT _kite_extract_result EQUAL 0)
  message(FATAL_ERROR "Cannot extract verified kiteLLM release")
endif()
set(_kite_root "${_kite_extract}/kiteLLM-${_kite_platform}")
edgeflow_prepare_third_party_cache(
  NAME kite_llm VERSION "${_kite_release}" SOURCE_SHA256 "${_kite_sha256}"
  CACHE_DIR "${_kite_cache}" KIND PREBUILT
  OUT_VALID _kite_cache_valid OUT_MARKER _kite_marker)
configure_file("${_kite_marker}" "${_kite_cache}/.edgeflow-cache-fingerprint" COPYONLY)

unset(kiteLLM_DIR CACHE)
set(kiteLLM_DIR "${_kite_root}/lib/cmake/kiteLLM")
find_package(kiteLLM 0.1.0 EXACT CONFIG REQUIRED
  PATHS "${_kite_root}/lib/cmake/kiteLLM" NO_DEFAULT_PATH)
get_target_property(_kite_imported_library kiteLLM::kiteLLM IMPORTED_LOCATION_RELEASE)
if(NOT _kite_imported_library STREQUAL "${_kite_root}/lib/libkiteLLM.a")
  message(FATAL_ERROR "kiteLLM package must resolve to the verified release archive")
endif()

include(CheckCXXSourceCompiles)
include(CMakePushCheckState)
cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_LIBRARIES kiteLLM::kiteLLM)
unset(KITELLM_DIRECT_API_COMPATIBLE CACHE)
check_cxx_source_compiles([[
  #include <kiteLLM.h>
  int main() {
    kiteLLM_Init();
    void* param = kiteLLM_Parameter_Allocate();
    kiteLLM_Parameter_SetLoadFromFileSync(param, 1);
    kiteLLM_Parameter_SetRunConfigFile(param, "run.json");
    void* model = kiteLLM_LoadFromFile("model.gguf", param);
    void* input = kiteLLM_TaskInput_Allocate();
    int count = 0, token = 0;
    int status = kiteLLM_Tokenizer_Encode(model, "x", 1, &token, 1, &count, 0, 1);
    status += kiteLLM_TaskInput_SetPromptTokens(input, &token, 1);
    status += kiteLLM_TaskInput_SetMaxOutputTokens(input, 1);
    status += kiteLLM_TaskInput_SetTemperature(input, 0.0f);
    status += kiteLLM_TaskInput_SetTopK(input, 0);
    status += kiteLLM_TaskInput_SetTopP(input, 1.0f);
    status += kiteLLM_TaskInput_SetRepetitionPenalty(input, 1.0f);
    void* output = nullptr;
    status += kiteLLM_Run(model, input, &output);
    const int* tokens = kiteLLM_TaskOutput_GetResultTokens(output, &count);
    int length = 0;
    status += kiteLLM_Tokenizer_Decode(model, tokens, count, nullptr, 0, &length, 0);
    kiteLLM_TaskOutput_Deallocate(output);
    kiteLLM_TaskInput_Deallocate(input);
    kiteLLM_Unload(model);
    kiteLLM_Parameter_Deallocate(param);
    kiteLLM_DeInit();
    return status;
  }
]] KITELLM_DIRECT_API_COMPATIBLE)
cmake_pop_check_state()
if(NOT KITELLM_DIRECT_API_COMPATIBLE)
  message(FATAL_ERROR
    "kiteLLM direct C API failed to compile/link. Check the release platform, "
    "toolchain and OpenMP/system runtime compatibility in CMake's configure log.")
endif()
set(LLM_EDGEFLOW_HAS_KITELLM ON)
message(STATUS "[kiteLLM] Verified ${_kite_release}: ${_kite_imported_library}")
