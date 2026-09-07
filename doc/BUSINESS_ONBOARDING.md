# 新业务接入的最短路径

现有四层继续保留。先运行 `build/alg_pipeline_tool catalog`，判断是否已有可组合的操作与模型能力。

1. **同一外部契约的新方案**：直接在 Studio 新建/克隆 Pipeline，选择模型并连接端口。无需添加 Adapter、Operator、Model 或 Backend。
2. **新的外部输入/输出结构**：在 Layer 1 增加业务 key/契约和注册 Adapter。`Unpack` 负责外部结构转 Traceable Batch；`PackTyped<Output>` 只写一次业务结果校验和映射。参考 `src/adapter/adapters/doc_qa_adapter.cpp`。
3. **需要同时提供 C ABI 和 Operator**：Adapter 继承 `ResultPackingAdapter<Adapter, COutput, Result>`。COutput 使用公共固定数组契约，Result 使用 Layer 1 自有字符串。字段字符串通过 `CopyResultString` 写出：固定 C 数组不足返回错误，可变长结果完整保存。
4. **Operator 接入**：单输入/单输出业务用 `MakeSingleSlotBizBridge<Result>` 生成描述符、槽声明及结果分配，只实现外部输入转换和结果到输出池的复制。多输入业务参考 OCR bridge。现有已注册输入/输出类型可复用；全新宿主结构仍需注册 ValueType 的容量、初始化与释放契约，这部分是内存安全边界，不能省略。
5. **只有 Catalog 证明操作能力不足时才新增 Node**。可配置的规则/提示词/组合放在 Pipeline；缺失的领域算法放在统一的 `src/custom_nodes/`，可在一个 Node 中完成前处理、调用已绑定模型和后处理，也可跨方案复用。框架通用操作继续放在 `src/common_nodes/`。详见[自定义 Node 接入指南](../src/custom_nodes/README.md)。新增模型语义或硬件 Backend 只针对确有协议能力缺口的情况。

六个单槽 bridge 已使用共享构造函数；七个内置业务均使用共同的双出口打包基类。无需修改中央业务 dispatch switch；已有业务的新方案通常只改 JSON。

按实际接入范围复用代码：修改已有 C ABI 路径，不必另建 Operator 或 Demo。但向当前
共享 SDK 注册**全新生产 Adapter** 时，要补齐匹配的 bridge；
[`OperatorBizBridgeRegistry::GlobalInit`](../src/adapter/operator/operator_biz_bridge_registry.cpp)
会审计所有 Adapter，缺失 bridge 会使 Operator 初始化失败。当前没有仅注册 C ABI
业务的豁免模式；若需这样的交付方式，应另行设计，不能通过省略注册绕过检查。

## 统一 Demo 接入

已有外部结构的新方案沿用对应数据集格式与 Demo 函数。准备 Pipeline 和指向它的
`.conf`，即可通过显式 `--biz`、`--config`、`--dataset` 运行；也可复用兼容 Profile
并用 `--config` 覆盖原配置。只有需要保存可重复调用的预设或加入套件时，才在
`demo/profiles.json` 中新增 Profile。命令、工具选择和结果检查见
[运行当前方案](../tools/pipeline_studio/README.md#运行当前方案)，可运行的双方案示例见
[自定义 Node 指南](../src/custom_nodes/README.md)。

需要让统一 Demo 支持新外部结构时，实现数据集到输入结构的转换，以及输出结构到结果字段的转换；继续复用
`RunOperatorWithExtractor` 和 `ResultWriter`。通过
`REGISTER_DEMO_BIZ(name, title, run_function, biz_type)` 注册，业务类型必须显式给出且非
UNKNOWN。运行器只读取注册描述符，不维护中央业务名分支；这不能替代 Adapter 或
Operator bridge 的协议注册与内存契约。

## 输出容量

Operator 路径是 `Unpack → Pipeline → 可变长业务 Result → 已租用输出池`。Result 与请求 Context 均不跨 Process 保存。字符串上限由 `.conf` 的 `data.mem_que.capacities` 决定，不再受中间 `Company*OutputStruct` 的 512/1024/2048 字节数组限制。超过输出池容量时返回 `-4`，所有尚未发布的输出租约回滚。

公共 C ABI 继续使用其已发布的固定数组大小；需要大结果的接入应选择 Operator 或显式增加新的外部 ABI 版本，不改变旧结构布局。

## 最小验证

- Adapter：乱序/重复/缺失请求、失败结果、两种输出表示的一致性。
- Operator：超过旧 C 数组但在池容量内的完整输出；池容量不足时无部分发布且后续请求能复用租约。
- 配置与运行：按[工具选择](../tools/pipeline_studio/README.md#校验工具选择)执行 validate / plan，
  再运行本次修改的方案并核对结果。仅使用 C ABI 的路径用对应端到端测试验证；统一 Demo
  使用 Operator。真实模型效果和目标平台验收与 Smoke 结果分别记录。
- 提交前：`./scripts/run_all_tests.sh`。
