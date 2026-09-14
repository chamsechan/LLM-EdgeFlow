# RFC-0055 修复与验证记录（2026-09-14）

本记录区分工程验证、Mock 方案执行与真实开发者试用。测量环境为 Ubuntu 24.04.4 LTS、
Linux 6.17.0-1020-oracle aarch64、GCC 13.3.0、CMake 3.28.3；不含公司内网 SDK 或目标硬件。

## 修复与覆盖

- Join/Group 的工厂和直接视图构造器，以及 Selection 工厂，拒绝 const/non-const 临时批次。
  `CompileTimeRejectionOfRvalues` 覆盖左右输入的组合；原 const 值返回工厂复现现在编译失败。
- SelectBatch 的载荷谓词与 TraceableItem 谓词分支都只接受 bool/NodeResult<bool>。
  原返回 int 的谓词复现现在由 static_assert 拒绝，不能静默跳过回调。
- AuthorNode 统一格式化 BatchFailureDetail，保留回调数值码和原消息。缺少 sub_id、数字前缀、
  `xreq_id` 等非完整 token 不会阻止补充真正的父 key。聚焦测试包括 Select、Split、Map 的实际
  AuthorNode 诊断，以及没有输出发布的保序失败路径。
- `detail::CheckedSplitCount` 被生产 SplitPayloads 复用。测试直接传入 0、INT32_MAX-1、
  INT32_MAX、INT32_MAX+1 和 SIZE_MAX，验证受检转换及失败详情，不分配巨量子批次。
  子编号测试覆盖 UINT32_MAX 处一个子项成功、随后零子项成功、额外子项失败。
- TextChunk 的 children 仍为 `1:N/generate_sub_id`，counts 为 `1:1/preserve`；历史错误码、
  UTF-8、空字符串、overlap 及交错父项来源由现有 TextChunk 套件验证。
- 独立 Reviewer 只读复核上述修复和新增测试，未发现剩余确定缺陷。

测试来源：[批次工具](../../../tests/unit/nodes/test_traceable_batch_operations.cpp)、
[TextChunk](../../../tests/unit/nodes/test_text_chunk_node.cpp)。

## ASan/UBSan 与门禁

复用隔离的 sanitizer 构建目录，只构建节点 runner；关闭 vendor backend，覆盖本次公共工具、
函数式 Node 与 TextChunk。完整默认后端配置由最后一次 canonical gate 单独验证。

```bash
cmake -S . -B build-sanitizers-address-undefined-fast \
  -DBUILD_TESTING=ON -DENABLE_SANITIZERS=ON \
  -DLLM_EDGEFLOW_SANITIZERS=address,undefined -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_LLAMACPP=OFF -DENABLE_ONNXRUNTIME=OFF \
  -DENABLE_KITELLM=OFF -DENABLE_WHISPERCPP=OFF \
  -DLLM_EDGEFLOW_SHARDED_TEST_RUNNERS=ON -DLLM_EDGEFLOW_TEST_PCH=OFF
cmake --build build-sanitizers-address-undefined-fast \
  --target edgeflow_test_nodes_runner -j2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ./build-sanitizers-address-undefined-fast/edgeflow_test_nodes_runner \
  --gtest_filter='TraceableBatchOperationsTest.*:TextChunkNodeTest.*:FunctionNodeTest.*'
./scripts/run_all_tests.sh
```

执行结果：Debug ASan/UBSan/LSan 聚焦检查 **95/95** 通过（45 项批次工具、11 项 TextChunk、
39 项函数式 Node），无 sanitizer 报告；Release canonical gate **97/97** CTest 项通过。
canonical gate 的 sanitizer 关闭，不将其描述为同时完成 Debug/Release 或 sanitizer 验证。

## Pipeline 与 Demo

```bash
./build/alg_pipeline_tool describe-node TextChunkNode
./build/alg_pipeline_tool validate configs/pipeline_doc_qa_default.json
./build/alg_pipeline_tool plan configs/pipeline_doc_qa_default.json
./build/alg_pipeline_tool_test validate demo/fixtures/mock/pipeline_doc_qa.json
./build/alg_pipeline_tool_test plan demo/fixtures/mock/pipeline_doc_qa.json
./build/alg_demo --profile doc_qa_mock --output-dir /tmp/rfc0055-demo
```

静态 validate/plan 成功。Mock Profile 使用自己的注册模型和配置，通过 Demo 的 Operator/C ABI
路径执行；不是实际模型效果验收。两条输出均 status=0，request_id 分别为 10001、10002，
chunk_count 均为 2。答案逐项检查为：

1. `【LLM总结】文档核心为现代软件工程化设计，包含松耦合、状态隔离与跨平台编译。`
2. `【LLM意图分析】检测到售后退款诉求。建议操作：7天无理由退货审核流程。`

counts 在 Node 内保存父来源计数，经 DocQA Adapter 组装为外部 `chunk_count`，两项均与已知
语料的拆分期望一致。默认真实模型 Pipeline 仅完成静态验证，没有声明真实推理效果。

## 批次耗时、内存与索引

使用[独立测量程序](../../../dev_support/benchmarks/traceable_batch_operations.cpp)，不增加测试
runner 或生产 Catalog 注册。构造 1,000/10,000/100,000 项批次，每项 128 字节、每请求四项；
右侧用种子 55 打乱，选择一半项，Split 每父项生成两段 64 字节文本。

```bash
c++ -std=c++17 -O3 -DNDEBUG -Iinclude -I3rdparty/nlohmann_json/include \
  dev_support/benchmarks/traceable_batch_operations.cpp -o /tmp/rfc0055-benchmark
for count in 1000 10000 100000; do
  for operation in join group select scatter split; do
    /tmp/rfc0055-benchmark "$operation" "$count"
  done
done
```

每个操作独立进程，一轮预热、七轮测量取中位数。输入构造不计时，Scatter 的 Selection 与
Materialize 不计时；输出构造及复制计时，析构在计时外（Split 的 counts 析构包含在包装中）。
RSS 为 Linux 进程峰值，包含输入、预备数据及分配器保留内存，不等于 helper 净分配量。
这是一台外部开发机的代表性成本记录，不是跨版本加速比或生产性能承诺。

原始测量见 [JSON 记录](0055-traceable-batch-benchmark.json)。耗时单位 ms，RSS 单位 KiB：

| 操作 | 1,000 项 | 10,000 项 | 100,000 项 | 100,000 项进程峰值 RSS |
| --- | ---: | ---: | ---: | ---: |
| join | 0.193 | 2.251 | 42.177 | 48236 |
| group | 0.277 | 3.319 | 100.476 | 53912 |
| select | 0.082 | 1.084 | 22.905 | 44212 |
| scatter | 0.145 | 2.120 | 39.727 | 70344 |
| split | 0.240 | 2.885 | 45.236 | 76704 |

测量期间无并发项目构建。小规模 RSS 含进程启动基线；更大批次的缓存/分配器成本也体现在
耗时中，不用三个测量点声称严格线性性能或绝对内存开销。

| 操作 | 每次调用的哈希索引构建次数（源码检查） | 用途 |
| --- | --- | --- |
| Join | 2 | 左 key 位置、右 key 引用 |
| Group | 3 | anchor 去重、member 去重、req 到组位置；返回视图持有后者 |
| Select | 1 | anchor 去重 |
| Scatter | 2 | replacement key 索引、selected key 集合 |
| Split | 2 | 输入去重、每请求下一个 sub_id |

这些索引均在单次调用内建立，循环内只增量查询/插入，不为每个请求重新扫描或建立全批次
索引。预期时间、额外空间均随项数线性增长；哈希容器不承诺最坏情况常数查询。

## 作者体验与剩余边界

工程检查使用三个编译 starter：Join、参考 Group、Select/Scatter。分组示例无需作者手写
key 索引；条件生成示例正常路径调用 generator 一次、polisher 一次，第二轮输入恰为选中项；
全不选时 polisher 零次，失败直接传播。测试同时断言 prompt、来源和最终文本。

真实开发者将参考分组和条件二次推理组合为一个 Node 的试用尚未进行，求助次数、编辑位置、
耗时等真实体验数据保留待办。上述 Agent 工程检查不能代替该记录，不将其写成“真实试用通过”。
