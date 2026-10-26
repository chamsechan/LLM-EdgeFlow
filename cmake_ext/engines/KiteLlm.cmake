# 经用户授权的私有 GitHub 依赖。认证信息保留在 gh 凭据存储或 GH_TOKEN 中；
# 绝不能把 token 写入 CMake 文件或缓存。
set(LLM_EDGEFLOW_HAS_KITELLM OFF)
if(NOT ENABLE_KITELLM)
  return()
endif()

include(cmake_ext/third_party/ThirdPartyCacheMetadata.cmake)

set(_kite_release "v0.1.0")
if(LLM_EDGEFLOW_TARGET_ARCH STREQUAL "x86_64")
  set(_kite_platform "x64")
  set(_kite_sha256 "bddaa55abe37cfc87f481139c41aadf79ad579b7cc20fe2b12c01978a2d3b1a0")
else()
  set(_kite_platform "arm")
  set(_kite_sha256 "1388da99c8fd4a45d0f85a489d29b2a12189e4955302ed75fd8bbddd1ebfc70d")
endif()
set(_kite_asset "kiteLLM-${_kite_platform}.tar.gz")
set(_kite_cache "${CMAKE_SOURCE_DIR}/3rdparty/kite_llm/${_kite_release}/${_kite_platform}")
set(_kite_archive "${_kite_cache}/${_kite_asset}")
# 不同构建目录共享此归档缓存；只有缺少归档时才调用 gh。
# 即使 tag 被移动，也以固定校验和为准。
find_program(KITELLM_GH_EXECUTABLE NAMES gh)
file(MAKE_DIRECTORY "${_kite_cache}/download")
edgeflow_download_verified_file("${_kite_archive}" "${_kite_sha256}"
  COMMAND "${KITELLM_GH_EXECUTABLE}" release download "${_kite_release}"
          --repo chamsechan/kiteLLM --pattern "${_kite_asset}"
          --dir "${_kite_cache}/download" --clobber
  DOWNLOADED "${_kite_cache}/download/${_kite_asset}"
  FAILURE_MESSAGE
    "Cannot download private kiteLLM release. Install gh and run gh auth login; "
    "the account (or CI GH_TOKEN) must have read access to chamsechan/kiteLLM.")
edgeflow_import_release_package(kiteLLM 0.1.0 "${_kite_archive}"
  "${CMAKE_BINARY_DIR}/_deps/kite_llm_release" "kiteLLM-${_kite_platform}" libkiteLLM.a)

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
    kiteLLM_Parameter_SetDeviceId(param, 0);
    kiteLLM_Parameter_SetRunConfigFile(param, "run.json");
    void* model = kiteLLM_LoadFromFile("model.gguf", param);
    void* input = kiteLLM_TaskInput_Allocate();
    kiteLLM_MultiModal_ChatHistory chat{};
    kiteLLM_TaskInput_SetMultiModal_ChatHistory(input, &chat, 1);
    int count = 0, token = 0;
    int status = kiteLLM_Tokenizer_Encode(model, "x", 1, &token, 1, &count, 0, 1);
    status += kiteLLM_TaskInput_SetPromptTokens(input, &token, 1);
    status += kiteLLM_TaskInput_SetMaxOutputTokens(input, 1);
    status += kiteLLM_TaskInput_SetTemperature(input, 0.0f);
    status += kiteLLM_TaskInput_SetTopK(input, 0);
    status += kiteLLM_TaskInput_SetTopP(input, 1.0f);
    status += kiteLLM_TaskInput_SetRepetitionPenalty(input, 1.0f);
    status += kiteLLM_TaskInput_SetBeamSize(input, 1);
    status += kiteLLM_TaskInput_SetVerboseDataFlag(input, KLLM_VERBOSE_F_OUTPUT_EMBEDDING);
    void* output = nullptr;
    status += kiteLLM_Run(model, input, &output);
    const int* tokens = kiteLLM_TaskOutput_GetResultTokens(output, &count);
    int rows = 0, dim = 0;
    kiteLLM_TaskOutput_GetOutputEmbedding(output, &rows, &dim);
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
list(APPEND LLM_EDGEFLOW_BACKEND_COMPILE_DEFINITIONS HAVE_KITELLM=1)
list(APPEND LLM_EDGEFLOW_BACKEND_LIBRARIES kiteLLM::kiteLLM)
