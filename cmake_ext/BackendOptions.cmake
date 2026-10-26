# 在下载或加载厂商包前统一确定 Backend 选择；模块只负责选中依赖的接入。
# 现有 Inferlet/Kite 包只覆盖 Linux，macOS 默认使用 llama.cpp CPU。
if(LLM_EDGEFLOW_TARGET_OS STREQUAL "macos")
  set(_edgeflow_default_inferlet OFF)
  set(_edgeflow_default_llamacpp ON)
else()
  set(_edgeflow_default_inferlet ON)
  set(_edgeflow_default_llamacpp OFF)
endif()
option(ENABLE_INFERLET "Enable the pinned Inferlet CPU release (Linux x86_64/aarch64)" ${_edgeflow_default_inferlet})
option(ENABLE_ONNXRUNTIME "Enable ONNX Runtime" OFF)
option(ENABLE_LLAMACPP "Enable llama.cpp" ${_edgeflow_default_llamacpp})
option(ENABLE_WHISPERCPP "Enable whisper.cpp (requires llama.cpp)" OFF)
option(ENABLE_KITELLM "Enable the pinned Kite CPU release" OFF)

if((ENABLE_INFERLET AND (ENABLE_KITELLM OR ENABLE_LLAMACPP OR ENABLE_WHISPERCPP))
   OR (ENABLE_KITELLM AND (ENABLE_LLAMACPP OR ENABLE_WHISPERCPP)))
  message(FATAL_ERROR
      "Inferlet, Kite and standalone llama.cpp/Whisper bundle conflicting runtimes. "
      "To select another runtime, explicitly set -DENABLE_INFERLET=OFF; "
      "enable only one of Inferlet, Kite or llama.cpp/Whisper.")
endif()
if(ENABLE_WHISPERCPP AND NOT ENABLE_LLAMACPP)
  message(FATAL_ERROR "ENABLE_WHISPERCPP=ON requires ENABLE_LLAMACPP=ON for shared GGML")
endif()

if((ENABLE_INFERLET OR ENABLE_KITELLM) AND NOT LLM_EDGEFLOW_TARGET_OS STREQUAL "linux")
  message(FATAL_ERROR
    "Inferlet and Kite packages support Linux x86_64/arm64 only; use macos-arm64-cpu on Apple Silicon")
endif()
