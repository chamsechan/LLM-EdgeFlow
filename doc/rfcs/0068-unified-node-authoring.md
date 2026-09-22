# RFC 0068：统一全部生产 Node 的作者契约

- **RFC 编号**：0068-unified-node-authoring
- **创建日期**：2026-09-22
- **文档状态**：Completed
- **关联分支**：`refactor/authoring-friction`
- **设计基线**：`87a28b7d1d2dde135b08d1a655b15a002b0c272c`，叠加工作区 RFC-0067
- **关联决策**：取代 RFC-0052、0054 及 0067 中长期保留 basic/advanced 两种生产 Node 作者路径的决定；保留其运行时、参数、快照和模型依赖契约。

## 1. 问题与范围

12 个生产 Node 均仍手写生命周期。已有 BatchSpec 限定单输出、1:1、固定 anchor，
无法表达 OCR 双输出、切块、语料源、检索、动态模板及会话缓存。只迁移简单节点会让业务
作者继续学习两套标准。本次以全部既有节点为范围，补齐已证明需要的表达能力，并同步迁移
脚手架、示例和现行指南。保留 common/custom 目录归属，不新增无消费者的能力或运行时语言。

## 2. 决策与不变量

- 统一通过 `REGISTER_FUNCTION_NODE` 注册。BatchSpec 接收普通输入视图、参数、模型及
  返回结果的算法函数；Map/LLM 是便利组合。NodeBase 仅保留框架异常屏障和内部测试用途。
- 输入声明支持已有 PortContract 的完整流属性。可选连接与允许请求缺值分别明确声明；
  后者用于模板缺值策略、条件读取等业务语义，不将所有 optional 的缺值静默吞掉。
- 输出可以是一个 batch 或普通多输出结构。`OutputsOf` 用类型化成员声明生成 Definition、
  绑定和发布，避免在构造器、Init、Process、Definition 重复端口事实。保序输出显式声明
  anchor 并复用统一校验；产生新编号、聚合和源节点使用真实流契约及其算法校验，不伪造
  1:1 anchor。多输出先构造全部结果、检查保序关系，再发布；不新增多键事务，异常仍使
  Pipeline 失败，不承诺分配失败时已经发布的键自动回滚。
- 模型依赖继续由统一 Plan 解析；补齐 ASR/OCR/Rerank 调用门面，复用空批次、错误原因和
  数量/来源检查。字段默认模型 ID 可显式声明。删除为节点旧错误码而反向映射通用失败的代码；
  模型原始错误码和具体业务失败原因继续传播。
- 会话缓存通过窄 `SessionResources` 门面显式作为算法参数注入，只开放现有缓存和版本查询，
  不提供任意模型获取或请求黑板访问。模型声明仍是依赖事实源；不在参数预检中执行资源 I/O。
- 复杂 Control 与普通字段 Control 接入同一个 Spec；复杂命令声明 schema 和构建下一状态的
  普通函数，框架执行解析、writer 串行事务与发布。Process 只持有一次不可变快照，更新失败
  保留旧值。复用现有模板、规则算法和 schema，不引入另一个 Control 协议。
- 保留节点名称、端口、配置默认值/范围、模型依赖、Control wire schema、算法及缓存身份。
  源码作者接口投产前集中迁移，不保留 advanced/unary 生成模式和旧便利基类兼容层。

## 3. 实施与回退

1. 扩展并测试统一声明/执行契约，保持现有函数式消费者可编译。
2. 迁移全部 11 个 common Node 和 1 个 custom Node；保留普通算法辅助函数，删除手写绑定、
   生命周期和快照路由。由既有逐节点测试检查行为及失败边界。
3. 脚手架、recipe、编译示例、测试及当前指南收敛为同一接口，清理无消费者的旧基类。
4. 比对迁移前后生产 Catalog，运行集中测试、独立 review 和最终门禁。

回退以本变更整体恢复统一接口、节点与作者工具，不并存同名注册或长期旧路径。
工作区无关 Schema、Adapter、测试夹具等 RFC-0067 改动保留。

## 4. 验证与完成条件

- 12/12 生产节点使用统一注册；源码与脚手架没有生产作者的 advanced 回退。
- 多输出、非 1:1、无必需输入、ImageRefBatch、多输入可选值、模型默认值、session 缓存、
  两种复杂 Control 都有真实生产消费者及既有或新增行为测试。
- 保序校验失败和算法失败不发布输出；多个输出的发布前校验全部通过才开始写入。
- 模型错误原因、空输入跳过、Control 并发快照/失败回滚、缓存同一并发创建失败共享及重试
  均通过测试；Catalog 比较不隐藏字段、默认值、端口或 Control schema 的变化。
- 更新的生成源码实际编译并执行；相关 ownership 路径运行独立 sanitizer 检查。
- 最终执行 `LLM_EDGEFLOW_JOBS=4 ./scripts/run_all_tests.sh`；不将代码行数或 Agent 测试
  等同于真实业务开发者体验验收，也不声称验证真实模型效果或目标硬件。

## 5. 最终结果

全部 12 个生产 Node 已使用同一 Spec 注册；common/custom 的文件归属保持不变。
OCR 从 119 行降为 49 行，Embedding 从 235 行降为 117 行；全部生产节点由 3336 行降为
2386 行。该计数只说明删除了作者重复代码，不代替真实开发者体验或性能测量。

删除 `ModelBoundNode`、`TraceableUnaryInferenceNode`、旧 Definition helper 和 advanced starter；
脚手架/recipe 删除 `--authoring` 与 `unary_inference` 分流，五种模型能力使用同一执行契约。
清理旧节点通用错误码及仅锁定这些码的测试断言，保留真实业务错误。

生产 Catalog 整体比较一致，仅 LlmGenerate、PromptGuided、TextEmbedding、TextRerank 的
配置字段排列由“模型字段优先”变为“参数字段优先”；字段内容、默认值、范围、描述、端口、
Control schema 和模型依赖均无变化。比较基线保存于 `/tmp/node-convergence-baseline/catalog.json`，
比较结果为 `/tmp/node-convergence-catalog-comparison.json`。

已完成的聚焦验证：

- `edgeflow_test_nodes_runner --gtest_brief=1`：286/286，包括全部生产节点、生成代码和新增
  多输出、ImageRefBatch、无输入源、复杂 Control、浮点参数和声明错误测试。
- `edgeflow_test_core_runner --gtest_filter='NodeBaseContractsTest.*'`：13/13。
- `python3 tests/tooling/test_scaffold_custom_node.py`：20/20；
  `python3 tests/tooling/test_dev_recipe.py`：21/21。
- 独立只读检查覆盖模型错误、配置、缓存及快照生命周期；修正了复杂/普通 Control 混合声明
  的重复 ID 校验顺序差异，并补双向拒绝测试。测试计划改用真实归一化流程，未保留不完整
  `normalized_config` 的兼容处理。
- 全量门禁暴露既有并发压力测试的启动竞态：writer 可先结束，reader 零次执行。测试改为
  共享起跑门和每个 reader 固定 30 次执行，保留批次快照、Control 结果与最终状态断言；
  `--gtest_repeat=100 --gtest_break_on_failure` 连续 100 次通过。

- 独立 Debug 构建启用 ASan/UBSan（关闭 ONNX、llama.cpp、Whisper、Kite Backend）：
  286 项中 285 通过、1 项因 ONNX 未启用而显式跳过，无 sanitizer 报告。跳过的
  `TextEmbeddingNodeTest.StrictPlanKeepsDistinctCorpusCacheIdentities` 已在默认构建通过；
  并发缓存失败共享、重试和键碰撞检查在 sanitizer 构建通过。
- 历史作者 benchmark 使用固定提交的旧 helper 作为临时私有编译输入，不恢复生产兼容头；
  Python 语法与历史 starter 编译检查通过，未运行完整性能测量。

最终交付门禁为 `LLM_EDGEFLOW_JOBS=4 ./scripts/run_all_tests.sh`；按 CONTRIBUTING，
本完成状态随最终改动进入门禁，只有门禁成功才确认完成。现行入口见
[作者指南](../dev_guide/custom_node_concepts.md)及[生产示例](../../src/custom_nodes/README.md)。
