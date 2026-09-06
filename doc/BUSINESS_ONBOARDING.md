# 新业务接入的最短路径

现有四层继续保留。先运行 `build/alg_pipeline_tool catalog`，判断是否已有可组合的操作与模型能力。

1. **同一外部契约的新方案**：直接在 Studio 新建/克隆 Pipeline，选择模型并连接端口。无需添加 Adapter、Operator、Model 或 Backend。
2. **新的外部输入/输出结构**：在 Layer 1 增加业务 key/契约和注册 Adapter。`Unpack` 负责外部结构转 Traceable Batch；`PackTyped<Output>` 只写一次业务结果校验和映射。参考 `src/adapter/adapters/doc_qa_adapter.cpp`。
3. **需要同时提供 C ABI 和 Operator**：Adapter 继承 `ResultPackingAdapter<Adapter, COutput, Result>`。COutput 使用公共固定数组契约，Result 使用 Layer 1 自有字符串。字段字符串通过 `CopyResultString` 写出：固定 C 数组不足返回错误，可变长结果完整保存。
4. **Operator 接入**：单输入/单输出业务用 `MakeSingleSlotBizBridge<Result>` 生成描述符、槽声明及结果分配，只实现外部输入转换和结果到输出池的复制。多输入业务参考 OCR bridge。现有已注册输入/输出类型可复用；全新宿主结构仍需注册 ValueType 的容量、初始化与释放契约，这部分是内存安全边界，不能省略。
5. **只有 Catalog 证明操作能力不足时才新增 Node**。当前业务规则/提示词/组合放在 Pipeline。新增模型语义或硬件 Backend 只针对确有协议能力缺口的情况。

六个单槽 bridge 已使用共享构造函数；七个内置业务均使用共同的双出口打包基类。无需修改中央业务 dispatch switch；已有业务的新方案通常只改 JSON。

## 输出容量

Operator 路径是 `Unpack → Pipeline → 可变长业务 Result → 已租用输出池`。Result 与请求 Context 均不跨 Process 保存。字符串上限由 `.conf` 的 `data.mem_que.capacities` 决定，不再受中间 `Company*OutputStruct` 的 512/1024/2048 字节数组限制。超过输出池容量时返回 `-4`，所有尚未发布的输出租约回滚。

公共 C ABI 继续使用其已发布的固定数组大小；需要大结果的接入应选择 Operator 或显式增加新的外部 ABI 版本，不改变旧结构布局。

## 最小验证

- Adapter：乱序/重复/缺失请求、失败结果、两种输出表示的一致性。
- Operator：超过旧 C 数组但在池容量内的完整输出；池容量不足时无部分发布且后续请求能复用租约。
- 配置：`alg_pipeline_tool validate`、选定业务的端到端样本。
- 提交前：`./scripts/run_all_tests.sh`。
