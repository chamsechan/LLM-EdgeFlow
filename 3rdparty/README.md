# 3rdparty 依赖下载目录

`3rdparty/` 只保存需要跨构建目录复用的下载文件：固定版本的 Inferlet、kiteLLM 发布归档和
stb_image 头文件。CMake 不校验这些文件的内容 hash。用户指定的私有 GitHub kiteLLM 依赖的启用方式、
读取授权和平台限制见 [kiteLLM 接入说明](../doc/kitellm.md)。

> **注意**：除本 `README.md` 外，`3rdparty/` 下的下载内容均已被 `.gitignore` 忽略，不会提交至 Git 仓库。

```text
3rdparty/
├── inferlet/<release>/<platform>/   # 发布归档，每次配置解压到 build/_deps
├── kite_llm/<release>/<platform>/   # 发布归档，每次配置解压到 build/_deps
└── stb_image/<commit>/              # 单头文件
```

文件不存在时，配置阶段先下载到暂存文件，成功后再改名；已有文件直接复用。
需要刷新时删除对应文件再重新配置。版本号和 URL 不变时，上游替换文件不会使已有文件自动失效。

nlohmann/json、GoogleTest、PCRE2、ONNX Runtime、llama.cpp 和 whisper.cpp 由 FetchContent
按固定版本下载到各构建目录的 `_deps/`，并在该构建目录内源码构建或解压，不写入 `3rdparty/`。

## 离线配置

- Inferlet、kiteLLM、stb_image：把同一文件预置到上述路径。
- FetchContent 依赖：预先解压固定版本源码或发布包，配置时用
  `-DFETCHCONTENT_SOURCE_DIR_<NAME>=<目录>` 指定，`<NAME>` 为声明名的大写形式，例如
  `LLAMA_CPP_SOURCE`、`WHISPER_CPP_SOURCE`、`ONNXRUNTIME_PREBUILT`、`GOOGLETEST`、`PCRE2`、
  `NLOHMANN_JSON`。
