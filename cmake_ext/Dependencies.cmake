# 项目依赖按模块聚合；Whisper 复用 llama.cpp 的 GGML 目标。
include(cmake_ext/third_party/NlohmannJson.cmake)
include(cmake_ext/third_party/Pcre2.cmake)
include(cmake_ext/third_party/StbImage.cmake)
include(cmake_ext/engines/OnnxRuntime.cmake)
include(cmake_ext/engines/LlamaCpp.cmake)
include(cmake_ext/engines/WhisperCpp.cmake)
include(cmake_ext/engines/Inferlet.cmake)
include(cmake_ext/engines/KiteLlm.cmake)
