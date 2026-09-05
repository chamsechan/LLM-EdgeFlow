# 3rdparty 依赖库目录规范

本项目采用统一的 `3rdparty/` 本地持久化归档体系，用于缓存第三方开源依赖（包括预编译静态库 `.a`、动态库 `.so` 及对应的头文件）。

用户指定的私有 GitHub kiteLLM 依赖使用 `kite_llm/<release>/<platform>/` 缓存已校验
的 Release 归档；启用方式、读取授权和平台限制见 [kiteLLM 接入说明](../doc/kitellm.md)。

> **注意**：除本 `README.md` 外，`3rdparty/` 下的所有自动生成与下载内容均已被 `.gitignore` 忽略，不会提交至 Git 仓库。

---

## 1. 目录结构规范

```text
3rdparty/
├── llama_cpp/                  # llama.cpp 编译产物 (静态库与头文件)
│   ├── include/                # llama.h, ggml.h, gguf.h 等
│   └── lib/                    # libllama.a, libggml.a, libggml-base.a, libggml-cpu.a
│
├── onnxruntime/                # ONNX Runtime 官方预编译发行包
│   ├── include/                # onnxruntime_c_api.h, onnxruntime_cxx_api.h 等
│   └── lib/                    # libonnxruntime.so (Linux) 或 libonnxruntime.dylib (macOS)
│
├── googletest/                 # Google Test 单元测试框架
│   ├── include/                # gtest/gtest.h 等
│   └── lib/                    # libgtest.a, libgtest_main.a
│
└── nlohmann_json/              # nlohmann/json 现代化 C++ JSON 库 (Header-only)
    └── include/                # nlohmann/json.hpp
```

每个依赖目录还包含自动生成的 `.edgeflow-cache-fingerprint`。该文件记录固定版本、
源码 SHA-256、缓存类型，以及二进制缓存所需的平台、工具链和 ABI 选项。

---

## 2. 自动化缓存与零重编机制

1. **自动生成与归档**：
   - 首次执行 CMake 构建（如 `cmake -B build`）且本地 `3rdparty/` 为空时，构建系统会自动拉取依赖并完成编译/规整，将产物保存到 `3rdparty/` 对应子目录中。
2. **零编译 / 零下载秒级复用**：
   - 当 `3rdparty/<lib>/` 存在有效库、头文件及匹配的缓存指纹时，后续清理或新建构建目录，CMake 均会直接以 `IMPORTED` 静态库/动态库秒级导入。
   - 指纹缺失或与当前固定源码、平台、编译器、ABI 选项不匹配时，CMake 会 fail-closed 地忽略旧缓存，并从已校验的固定来源重新生成和归档。
3. **强制重新编译 / 刷新**：
   - 如需更新或重新编译 llama.cpp，可在 CMake 中指定 `-DLLAMA_CPP_FORCE_REBUILD=ON`，或直接删除 `3rdparty/llama_cpp` 目录。

---

## 3. 离线与边缘硬件手动放置说明

在无法连接公网的离线服务器或嵌入式边缘设备（如 ARM / NPU 板卡）上，可复制由同一项目配置在兼容工具链中生成的完整 `3rdparty/<lib>/` 目录（包括缓存指纹）。只复制来源不明的库和头文件会被拒绝，避免静默加载 ABI 不兼容产物。
