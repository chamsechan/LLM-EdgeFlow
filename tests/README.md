# 测试职责与维护

根据当前接口和行为编写测试。公共规则集中测试，组件测试覆盖自身算法。
调用层测试检查连接、错误转发和完整响应。使用独立预期值，不用被测代码计算预期值。

| 责任 | 归属 |
| --- | --- |
| 参数类型、范围、嵌套、绑定与 Control 载体 | `unit/contracts/` |
| AuthorNode 输入、输出、来源、生命周期、快照与模型调用 | `unit/nodes/test_function_node.cpp` |
| Join、Group、Select、Scatter 与固定批执行 | `unit/nodes/test_traceable_batch_operations.cpp`、`unit/engine/test_batch_executor.cpp` |
| Blackboard、注册、模型所有权与 Session 单次并发创建 | `unit/core/` 对应机制文件 |
| 解析、Validator 计划、Pipeline 调度与失败处理 | `unit/core/test_pipeline_config.cpp`、`test_validated_pipeline_plan.cpp`、`test_dag_pipeline.cpp` |
| 真实 Node 的算法与业务参数 | `unit/nodes/test_*_node.cpp` |
| 真实 Model 的输入构造、数值与输出解释 | `unit/engine/test_*_model.cpp` |
| Backend 协议、加载、真实小图执行与可选权重 | `unit/engine/test_*_backend.cpp` |
| 部署路径、Converter、行关联与槽位 | `unit/adapter/`、`integration/operator/test_io_parameters.cpp` |
| Operator 失败阶段与类别判定 | `unit/operator/test_operator_failure_category.cpp` |
| 输出池与租约 | `unit/operator/` |
| 六个公共入口与 `ResolveOperatorConfigIo` 的壳无关行为、错误码镜像清单与违约自测 | `contract/operator_shell/`；驱动与清单在 `contract/operator_shell/<shell>/` |
| 所选壳的载体读取、路由、发布、原生 binding、边界日志格式与诊断文字 | `unit/operator/operator_shell/<shell>/` |
| 各业务的完整请求及响应 | `integration/operator/operator_shell/<shell>/` |
| Demo 载体构造与展示 | `integration/demo/operator_shell/<shell>/` |
| SDK 消费者、CLI、Studio、Demo、脚手架与工程契约 | `contract/`、`tooling/`、`integration/demo/`、`integration/pipeline/` |
| 真实权重的物理执行 | `e2e/real_models/`，显式开启 |

同一普通载体上的不同业务分别验证字段含义。完整业务从 Operator 到最终响应，实际执行
Converter、Validator、Pipeline 和 Node。仅替换模型边界。Demo/Python 保留完整请求和 SDK
输出，不代替 Adapter 选择字段或组装业务响应。

`support/` 只保存多个真实使用方需要的薄 helper：Node 初始化、Model 参数解析、Converter
调用、注册表状态恢复、线程局部分配故障。它们不计算业务预期。`dev_support/inference/`
提供 Demo 的确定性 mock 能力，用 `kFixture` 和显式 Backend 白名单隔离。它们不进入 SDK
或生产 CLI，也不作为实际 Model/Backend 推理证据。

## 开发与收集

按最小责任运行实际测试，例如：

```bash
cmake --build build --target edgeflow_test_nodes_runner -j4
./build/edgeflow_test_nodes_runner --gtest_filter='TextChunkNodeTest.*'
(cd build && ctest -R '^OperatorOutputPoolTest\.' --output-on-failure --no-tests=error)
```

四个 runner 的源码目录在 `RuntimeTests.cmake` 中定义。GoogleTest 从编译出的用例生成 CTest
入口，包括脚手架生成的测试。新测试套件无需手动注册名称。`contract/architecture/test_test_labels_contract.py`
把实际用例与 CTest 过滤器对照，拒绝空匹配、漏执行、重复执行及不明确的 tier，
同时拒绝禁用的 CTest 条目和仅列举 GoogleTest 用例的命令。
常规 `test_*.cpp` 和 `dev_support/` 所属目录中的 `.cpp` 递归收集，增删后在下次构建自动更新。
独立 registry/SDK 目标保持进程和公开依赖隔离；生成源码和可选 E2E 仍显式声明。

`contract/`、`tooling/` 下的 `test_*.py`、`test_*.sh`、`test_*.cmake` 自动注册，
CTest 名称为相对 `tests/` 的路径。只有独立测试入口使用 `test_*` 前缀。
特殊参数在 `ScriptTests.cmake` 中配置 `EDGEFLOW_TEST_ARGS`，环境和标签通过 `set_tests_properties` 配置。
Studio 自动发现 `studio_*_test.mjs`；浏览器测试仍按 Playwright 环境变量启用。

runner 只收集 `EDGEFLOW_OPERATOR_SHELL` 选中壳的 `operator_shell/<shell>/` 目录，规则见
[Operator 壳选择](../doc/dev_guide/source_layout.md#operator-壳选择)。一致性场景通过驱动调用
公共入口，不直接使用平台结构或数字错误码；场景检查返回 `::testing::AssertionResult`，
正式测试断言成功，违约自测包装函数表后断言失败。

测试 main 拒绝零执行和必需测试跳过，重复执行中任何一轮的必需跳过都会失败。可选资产
用例在跳过前记录非空 `optional_runtime_asset`。单个全可选跳过返回 77，CTest 仅以此状态
记录跳过。测试输出中的文本不会覆盖进程失败状态。

修改共享规则时，更新负责该规则的测试。修改算法时，补充正常结果和边界用例。
调用层只增加特有的接入测试。
并发测试用屏障、调用发起信号和完成状态检查阻塞及解除后的结果。
只向负责分配或提交的操作注入故障。
不要为每个业务复制通用空指针、类型和异常矩阵。脚手架生成一条最小业务契约测试，
修改算法后填写独立预期。

## 验证边界

唯一完整门禁为 `LLM_EDGEFLOW_JOBS=4 ./scripts/run_all_tests.sh`。所有权和并发改动使用
现有 `scripts/run_sanitizers.sh` 入口。分配故障只在测试可执行文件替换 `new/delete`，不跟踪
其他线程或 `malloc`。资源归还由实际池账本和对象生命周期断言证明，sanitizer 补充检查。
ELF 的 `posix_memalign` 故障包装仅在 Tensor 测试链接时启用。
注册表清理与状态恢复实现只在 `tests/support/`。三个注册表保留状态隔离所需的友元。
输出池、会话资源和运行时串行化测试只调用正常接口。生产代码不含等待计数、锁观测
友元或加载测试钩子，测试也不拦截标准库等待函数。会话工厂在测试屏障处暂停，检查
并发调用的工厂次数、共享对象及失败后重试。运行时屏障只暂停第一个 Node/Control，
避免第二个操作被测试屏障挡住而掩盖串行化失效。
调用发起信号和超时断言表明观察期间操作未完成，不证明线程已到达某个内部等待位置。
这些断言验证观察窗口内的外部行为，不依赖内部 waiter 数。

默认开发门禁使用 `dev-gate`（Inferlet）。ONNX Runtime、llama.cpp 和 Whisper 由显式
`full-cpu` 构建覆盖；构建选择见[构建变体](../doc/VERIFIABLE_SELECTION.md#构建变体)。

macOS Apple Silicon 使用 `LLM_EDGEFLOW_BUILD_PRESET=macos-arm64-cpu ./scripts/run_all_tests.sh`。
同一门禁覆盖 Mach-O 导出清单。平台选择和缓存身份的契约测试
可以在宿主用模拟 CMake 目标事实执行，但不代表 macOS 编译成功；实际编译和运行由对应平台 CI 验证。
交叉编译只产生目标产物，目标上的原生测试独立执行。
没有所需权重时，可选物理生成用例明确跳过。`EDGEFLOW_TEST_LLAMA_GGUF` 和 `EDGEFLOW_TEST_WHISPER_MODEL` 可提供相应
Backend 资产。`EDGEFLOW_TEST_BPE_TOKENIZER_DIR` 指向含 `.eftok` 文件的目录（见[配置说明](../configs/README.md#分词器资产onnx-llm)）时，
BPE 分词器测试额外对照 Hugging Face 的样例 ID。可选 E2E 用 `ENABLE_REAL_MODEL_TESTS=ON` 开启，并使用仓库固定权重和 PCM。
Studio 浏览器用 `STUDIO_PLAYWRIGHT_MODULE` 指定 Playwright。可另设 `STUDIO_CHROMIUM_PATH`。
缺少该依赖时只跳过物理浏览器，其必需 HTTP/JS/文件事务仍执行。

这些证据不代表真实权重效果、速度、内部 SDK 或目标硬件验收。范围见
[验证与选择指南](../doc/VERIFIABLE_SELECTION.md)。

## 维护成本

统计人工维护的测试源码、helper、假模型、文本场景、测试脚本、生成器、手写模板与构建配置，
包括 `tests/` 之外的测试支持。计入注释和空行，同一代码只计一次；第三方、构建产物、生成输出
和二进制资产不计入。通用生产工具只计测试专用部分。资产大小另列，不能通过搬目录、压缩
预期数据或压行降低统计值。

统计结果附当前提交和范围，放入该次变更说明；日常指南不维护固定行数和历史测试成绩。
