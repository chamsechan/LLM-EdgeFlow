# RFC 0033: 按 kiteLLM 原生接口传递设备选择

- **RFC 编号**：0033-kitellm-native-device-contract
- **创建日期**：2026-09-05
- **文档状态**：Completed
- **关联分支**：`fix/kite-native-device-contract`
- **目标版本**：v10.x
- **负责人 / 作者**：LLM-EdgeFlow Team

## 1. 背景与动机

RFC-0026 把设备选择归于 run-config，拒绝任何显式 ExecutionTarget；RFC-0032
沿用此约定，阻塞了要求显式平台和设备的 Operator/demo。这是本项目的接入政策，
并非 kiteLLM 原生接口限制。本 RFC 替代这两个历史 RFC 的设备独占约定。

核对对象为当前固定 v0.1.0 发布头及上游提交
`5e58820f39919fce2046e2fd703c62601a5df59b`：

- [原生头](https://github.com/chamsechan/kiteLLM/blob/5e58820f39919fce2046e2fd703c62601a5df59b/include/kiteLLM.h)
  同时声明 SetDeviceId、SetDeviceList、SetRunConfigFile。
- [参数实现](https://github.com/chamsechan/kiteLLM/blob/5e58820f39919fce2046e2fd703c62601a5df59b/src/core/model_config.cpp)
  中 SetDeviceId(-1) 为 Auto，非负值为 Single；运行 JSON 并不覆盖此设备字段。
- [加载实现](https://github.com/chamsechan/kiteLLM/blob/5e58820f39919fce2046e2fd703c62601a5df59b/src/engine/llama_engine.cpp)
  将 ID 解释为 ggml 后端设备枚举索引，检查范围并将选择交给模型加载。
  该 ID 不能直接假定为 CUDA ordinal。
- [发布构建](https://github.com/chamsechan/kiteLLM/blob/5e58820f39919fce2046e2fd703c62601a5df59b/CMakeLists.txt)
  将 ggml-cpu 等归档打包；当前 Linux 固定包按 CPU 接入，未提供框架平台选择 API。

## 2. 设计范围与边界

仅调整 Layer 4 Kite Backend 的参数映射、构建接口探测、分层门禁、测试和使用说明。
保留四层依赖、公共 C ABI、Operator 参数布局及校验、Core、Node、Model、Pipeline
Schema 和现有业务名。不给上层增加 Kite 分支，不新增通用设备策略或伪造平台 setter。
本次不增加多设备公开配置、GPU/NPU 支持或非文本生成协议。

## 3. 接口与数据流

Operator/CreateParam → RuntimeOptions → ExecutionTarget → KiteLlmBackend →
`kiteLLM_Parameter_SetDeviceId` → `kiteLLM_LoadFromFile`。

- 未指定设备保持原生默认；显式 -1 交给原生 Auto；>=0 原样传给原生 setter。
  小于 -1 拒绝；具体可用索引由原生加载校验。
- 平台支持未指定、UNKNOWN、CPU、CPU_GENERIC（大小写归一化）。显式 CPU 使用
  默认/自动设备或设备 0，其他 CPU ID 拒绝。CUDA、NPU 等平台明确拒绝，不静默回退。
- run_config_file 仍为可选的模型目录内相对路径，保留路径边界和原生格式解析。
  显式 CPU 与 JSON model.gpu_layers > 0 冲突时，Backend 在加载前拒绝；只检查
  这个与框架平台有关的约束，其余格式与字段仍交给原生解析。
- 缺 SDK 仍不注册；不改变固定 seed、stop words、互斥构建或会话生命周期策略。

## 4. 权衡与兼容性

保留当前 Operator 设备语义即可恢复 CPU Kite 路径，无需为了错误的独占假设改造上层。
既有自动设备调用保持兼容。CPU 接入范围是当前发布包的框架支持边界，不代表 Kite
全部硬件能力。升级发布包或增加其他平台时应重新核对设备编号和运行配置。
供应商头只在具体 Backend 内使用，RAII 与异常屏障保持原有实现。

### 后续能力扩展的分层约束

此次设备适配不扩展中性协议。后续增加文本处理业务时，先复用已有 Node 组合；增加模型
语义时在 Model 内实现；只有现有中性协议无法表达新的能力时才设计协议扩展。业务提示
模板和模型聊天格式不进入 Kite Backend，Kite setter 和运行配置解析不进入上层。
该边界通过 LayerGuard 补充直接 `kiteLLM.h` 的归属检查，并用 Model、Node、Core、
Adapter、Demo 和其他 Backend 的头文件注入验证拦截能力，避免只靠文档维持隔离。

## 5. 验证计划

- 扩展 LlamaCppBackendTest：平台、设备边界、CPU/run-config 冲突、原生显式设备加载
  与生成，以及未指定设备的回归。
- 扩展 DemoRunnerTest：真实 GGUF 经既有实体抽取 Demo → Operator 完成创建、推理、
  输出解析、释放；以非空结构化结果和 request_id 证明业务路径，移除测试配置中的
  JSON fallback，避免用兜底输出冒充真实推理。
- CMake 编译链接探测增加原生 SetDeviceId。
- LayerGuard/SelfTest 覆盖直接 Kite 头文件的边界及带相对目录的 include 写法。
- 真实用例沿用 LLM_EDGEFLOW_TEST_KITELLM_MODEL；缺 SDK/模型明确跳过，跳过不算验收。
- 运行默认 ./scripts/run_all_tests.sh，以及 Kite 非默认构建的真实模型专项测试；
  通过 alg_pipeline_tool 验证/规划测试部署并运行 alg_demo 可执行文件。

## 6. 实施与验收

- [x] 对照上游锁定实现完成设计自评。
- [x] Backend 映射、接口探测与回归测试。
- [x] 默认门禁及 Kite 真实模型、Operator/demo 验收。
- [x] 更新当前使用文档、Changelog 与 RFC 状态。

### 验收记录

- 本地 Linux aarch64 固定发布包构建成功，原生 SetDeviceId 编译链接探测通过。
- 默认 `LLM_EDGEFLOW_JOBS=8 ./scripts/run_all_tests.sh`：88/88 CTest 通过，包含
  默认后端构建、格式、架构分层、ABI/导出、原有 Operator 和 Demo 回归。
- 分层加固后再次通过默认 88/88 门禁；LayerGuardSelfTest 在 7 个错误归属位置
  分别注入两种 Kite include 写法，14 种违规均被目标规则拦截。
- Kite 构建设置真实 Qwen2.5 0.5B GGUF 后运行 LlamaCppBackendTest 与 DemoRunnerTest：
  两个套件通过，新增真实设备与 Demo 用例实际执行，未跳过。
- `alg_pipeline_tool validate` 和 `plan` 对既有实体抽取 DAG 的 Kite 配置均返回 ok=true。
- 独立 `alg_demo --biz entity_extract --chip cpu_generic --device-id 0`，使用临时
  部署配置、原生运行文件和真实模型，退出码 0。输入“张三在北京工作。”，输出
  `request_id=30001, status=0, entities=["张三","北京"]`；结构化解析使用 fail 策略。
- 证据仅覆盖本地 aarch64 CPU；未执行 x64/GPU/NPU 硬件验收，无远程上传或 CI 运行。

## 7. 变更记录

| 日期 | 版本 | 变更内容 | 作者 |
| :--- | :--- | :--- | :--- |
| 2026-09-05 | v1.0.0 | 以原生 setter 替代 run-config 独占设备的本地限制 | LLM-EdgeFlow Team |
