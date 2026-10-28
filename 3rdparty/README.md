# 3rdparty 依赖库目录规范

本项目使用 `3rdparty/` 持久缓存固定版本的依赖，CMake 不校验下载文件的内容 hash。原始发布归档可以共享，
本地编译产物按目标平台和构建身份分目录，避免 Linux/macOS、CPU 架构及 Debug/Release 相互覆盖。

用户指定的私有 GitHub kiteLLM 依赖使用 `kite_llm/<release>/<platform>/` 缓存
的 Release 归档；启用方式、读取授权和平台限制见 [kiteLLM 接入说明](../doc/kitellm.md)。

> **注意**：除本 `README.md` 外，`3rdparty/` 下的所有自动生成与下载内容均已被 `.gitignore` 忽略，不会提交至 Git 仓库。

---

## 1. 目录结构规范

```text
3rdparty/
├── llama_cpp/<target>/<fingerprint>/       # 静态库与头文件
├── whisper_cpp/<target>/<fingerprint>/     # 身份还包含对应 GGML provider
├── pcre2/<target>/<fingerprint>/
├── googletest/<target>/<fingerprint>/
├── onnxruntime/<target>/<fingerprint>/     # 固定平台发行包
├── inferlet/<release>/<platform>/          # 下载归档，在各 build/_deps 解压
├── kite_llm/<release>/<platform>/
├── nlohmann_json/include/                  # 与平台无关的头文件
└── stb_image/                             # 与平台无关的头文件
```

库缓存目录包含 `.edgeflow-cache-fingerprint`，记录版本和目标 OS/架构。
源码编译的静态库另记录编译器、工具链、SDK/sysroot、当前构建类型的编译参数和 sanitizer 设置；
官方预编译包不因本机 SDK 更新而改变身份。`target` 例如 `linux-arm64`、`macos-arm64`。
项目使用 Ninja 等单配置生成器；`cmake_ext/third_party/ThirdPartyCacheMetadata.cmake`
拥有身份计算规则，各依赖模块消费其返回的路径。

---

## 2. 自动化缓存与零重编机制

1. **自动生成与归档**：
   - 首次执行 CMake 构建（如 `cmake -B build`）且本地 `3rdparty/` 为空时，构建系统会自动拉取依赖并完成编译/规整，将产物保存到 `3rdparty/` 对应子目录中。
2. **零编译 / 零下载秒级复用**：
   - 单配置构建在所选身份目录存在完整库、头文件和匹配指纹时，使用 `IMPORTED` 目标复用。
   - 指纹缺失或与当前源码版本、平台、编译器、ABI 选项不匹配时，CMake 会忽略旧缓存，并从固定版本来源重新生成和归档。
   - llama.cpp 只缓存项目提前确定的 GGML 选项组合。出现未纳入身份的 GGML/LLAMA 构建变量时，llama/Whisper 均使用源码增量构建且不读写共享归档；同一源码构建目录中上游新增的默认选项也按此处理，身份目录保持稳定。配置日志会列出禁用共享归档的变量名称，或需要保留源码目标依赖的 Metal/BLAS 组合。
3. **强制重新编译 / 刷新**：
   - 如需重新编译 llama.cpp，可指定 `-DLLAMA_CPP_FORCE_REBUILD=ON`。不同身份目录可以并存，无需删除其他平台的缓存。
   - 下载归档与单文件缓存存在时直接复用，不比较内容 hash；需要刷新时删除对应下载文件再重新配置。版本号和 URL 不变时，上游替换文件不会使已有缓存自动失效。

---

## 3. 离线与边缘硬件手动放置说明

离线构建可复制由同一项目配置和工具链生成的完整缓存目录，包括指纹及目录层级。
指纹或编译器/sysroot 路径不同会重新选择源码构建；此时还需预置固定版本源码或相同发布归档。
设备上实际使用哪些库，以当前配置输出及目标产物为准。
