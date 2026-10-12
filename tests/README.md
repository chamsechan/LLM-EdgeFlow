# 测试职责与维护

测试从当前生产契约设计。公共规则由一处负责，组件保留独有语义；调用层验证接线、
错误转发和完整外部响应。预期使用独立字面量，不调用被测代码计算自己的答案。

| 责任 | 归属 |
| --- | --- |
| 参数类型、范围、嵌套、绑定与 Control 载体 | `unit/contracts/` |
| AuthorNode 输入、输出、来源、生命周期、快照与模型调用 | `unit/nodes/test_function_node.cpp` |
| Join、Group、Select、Scatter 与固定批执行 | `unit/nodes/test_traceable_batch_operations.cpp`、`unit/engine/test_batch_executor.cpp` |
| Blackboard、注册、模型所有权与 Session 单次并发创建 | `unit/core/` 对应机制文件 |
| 解析、Validator 计划、Pipeline 调度与失败收敛 | `unit/core/test_pipeline_config.cpp`、`test_validated_pipeline_plan.cpp`、`test_dag_pipeline.cpp` |
| 十二种真实 Node 的算法与业务参数 | `unit/nodes/test_*_node.cpp` |
| 六种真实 Model 的输入构造、数值与输出解释 | `unit/engine/test_*_model.cpp` |
| Backend 协议、加载、真实小图执行与可选权重 | `unit/engine/test_*_backend.cpp` |
| 部署路径、Converter、行关联与槽位 | `unit/adapter/`、`integration/operator/test_io_parameters.cpp` |
| 输出池、租约、发布、异常屏障与句柄 | `unit/operator/`、`contract/abi/test_operator_safety.cpp`、`integration/operator/test_operator_api.cpp` |
| 平台 mock 载体的实际读取与边界检查 | `contract/abi/test_adapter_contract_security.cpp` |
| 八种业务的完整请求及响应 | `integration/operator/test_keyword_response.cpp`、`test_business_responses.cpp` |
| SDK 消费者、CLI、Studio、Demo、脚手架与工程契约 | `contract/`、`tooling/`、`integration/demo/`、`integration/pipeline/` |
| 真实权重的物理执行 | `e2e/real_models/`，显式开启 |

同一普通载体上的不同业务分别验证字段含义。完整业务从 Operator 到最终响应，实际执行
Converter、Validator、Pipeline 和 Node；仅替换模型边界。Demo/Python 保留完整请求和 SDK
输出，不代替 Adapter 选择字段或组装业务响应。

`support/` 只保存多个真实使用方需要的薄 helper：Node 初始化、Model 参数解析、Converter
调用、注册表状态恢复、线程局部分配故障。它们不计算业务预期。`dev_support/inference/`
提供 Demo 的确定性 mock 能力，用 `kFixture` 和显式 Backend 白名单隔离；它们不进入 SDK
或生产 CLI，也不作为实际 Model/Backend 推理证据。

## 开发与收集

按最小责任运行实际测试，例如：

```bash
cmake --build build --target edgeflow_test_nodes_runner -j4
./build/edgeflow_test_nodes_runner --gtest_filter='TextChunkNodeTest.*'
(cd build && ctest -R '^OperatorOutputPoolTest\.' --output-on-failure --no-tests=error)
```

四个 runner 的源码目录在 `RuntimeTests.cmake` 中定义。GoogleTest 从编译出的用例生成 CTest
入口，包含脚手架的原样生成测试；新 suite 不需要加入历史名称清单。`TestLabelsContractTest`
把实际用例与 CTest 过滤器对照，拒绝空匹配、漏执行、重复执行及不明确的 tier，
同时拒绝禁用的 CTest 条目和仅列举 GoogleTest 用例的命令。
独立 registry/SDK 目标保持进程和公开依赖隔离。

测试 main 拒绝零执行和必需测试跳过，重复执行中任何一轮的必需跳过都会失败。可选资产
用例在跳过前记录非空 `optional_runtime_asset`。单个全可选跳过返回 77，CTest 仅以此状态
记录跳过；其默认输出匹配跳过规则被移除，以免覆盖实际失败。

修改共享规则时更新其唯一矩阵，调用层只补独特接入风险；修改算法时补正常结果及特有
边界。并发测试使用同步点观察真实等待和锁，故障只注入承担分配或提交责任的操作。
不要为每个业务复制通用空指针、类型和异常矩阵。脚手架生成一条最小业务契约测试，
修改算法后填写独立预期。

## 验证边界

唯一完整门禁为 `LLM_EDGEFLOW_JOBS=4 ./scripts/run_all_tests.sh`。所有权和并发改动使用
现有 `scripts/run_sanitizers.sh` 入口。分配故障只在测试可执行文件替换 `new/delete`，不跟踪
其他线程或 `malloc`；资源归还由实际池账本和对象生命周期断言证明，sanitizer 补充检查。
ELF 的 `posix_memalign` 故障包装仅在 Tensor 测试链接时启用。

默认构建执行 ONNX Runtime 小图和 llama.cpp 的实际加载失败路径。没有 GGUF 时，物理生成
用例明确跳过；`EDGEFLOW_TEST_LLAMA_GGUF` 和 `EDGEFLOW_TEST_WHISPER_MODEL` 可提供相应
Backend 资产。可选 E2E 用 `ENABLE_REAL_MODEL_TESTS=ON` 开启，并使用仓库固定权重和 PCM。
Studio 浏览器用 `STUDIO_PLAYWRIGHT_MODULE` 指定 Playwright；可另设 `STUDIO_CHROMIUM_PATH`。
缺少该依赖时只跳过物理浏览器，其必需 HTTP/JS/文件事务仍执行。

这些证据不代表真实权重效果、速度、内部 SDK 或目标硬件验收；范围见
[验证与选择指南](../doc/VERIFIABLE_SELECTION.md)。

## 维护成本

当前人工维护范围为 **22,465 行**，包含注释和空行，同一文件只计一次。
第三方、构建产物、生成输出和二进制资产不计；生成器及手写模板计入。
这仍比设计预算上沿 20,200 行多 **2,265 行**，规模验收尚未达标。

| 范围 | 行数 |
| --- | ---: |
| 单元测试与工程、SDK 契约 | 14,426 |
| 集成与工具入口测试 | 3,490 |
| 可选真实权重 E2E | 170 |
| helper、文本夹具、测试构建/发现配置与源码观测接口 | 1,415 |
| Demo mock 场景、dev_support 替身与作者模板 | 1,757 |
| ONNX 生成器与脚手架测试模板 | 306 |
| 门禁、专项执行、资产获取脚本、sanitizer 配置与 CI | 901 |
| **合计** | **22,465** |

`tests/`、`dev_support/` 和 `demo/fixtures/mock/` 的人工代码与文本配置全部计入；
脚本计入 `generate_test_onnx_model.py`、`run_all_tests.sh`、`run_sanitizers.sh`、
`run_real_model_e2e.sh`、`fetch_real_test_models.sh` 以及完整 `ci.yml`。
另计 `ubsan_suppressions.txt` 4 行、`GoogleTest.cmake` 75 行、
`LayerHeaderViews.cmake` 的测试编译清单生成函数及其说明 31 行、根 CMake 的测试接线
6 行，以及生产源码中仅服务测试的注册表清理/访问和等待观测接口 34 行。
通用生产工具只计其测试专用部分：`scaffold_custom_node.py` 中
`render_standalone_test` 的完整函数，共 165 行。Markdown 使用指南属于文档。
后续调整仍使用这一口径，不将 mock 场景、共享 helper 或模板移出统计范围。
