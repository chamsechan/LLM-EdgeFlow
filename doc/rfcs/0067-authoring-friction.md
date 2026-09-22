# RFC 0067：作者布线、配置补全与端到端测试的定向简化

- **RFC 编号**：0067-authoring-friction
- **创建日期**：2026-09-22
- **文档状态**：Completed
- **关联分支**：`refactor/authoring-friction`
- **目标版本**：投产前
- **负责人 / 作者**：LLM-EdgeFlow maintainers
- **关联决策**：承接 RFC-0066 延期的 Schema/测试便利性；保持 RFC-0052 的 basic/advanced 作者路径。

## 1. 问题与范围

高级 Node 仍重复声明端口；手写 Pipeline 缺少编辑器补全；Operator 业务测试重复创建和释放。
用户授权在降低开发心智且不改变外部契约的前提下实施，要求避免过度实现。
本次增加离线 Schema 导出、测试专用生命周期辅助，以及复用 typed key 的机械布线简化；
同时收敛 Schema 固定结构和 Node 底层绑定的重复实现。

## 2. 决策与权衡

- 能力节点层：新增变参 BindPorts，按参数顺序调用现有 BindPort；typed key 同时用于成员就地初始化和 Definition。
  先迁移代表性多输入、多输出及 session 节点和高级 starter。保留现有基础 Spec、BindPort 与全部生命周期入口。
  不新增反射、自注册、空端口默认状态、端口 DSL 或另一套运行时绑定；不以行数目标改变读取时机和错误行为。
- Tooling：新增 export-schema，stdout 输出当前构建 Catalog 对应的 JSON Schema，编辑器通过外置关联使用。
  Node/Model/Backend 字段来自 Definition，固定文档结构由 Core / Integration 声明并同时供解析器使用，
  覆盖现有完整 Pipeline（包括 deployment）。
  Schema 是编辑提示，不替代唯一 Validator；复杂语义、动态引用、DAG 和并行约束仍走 validate/plan。
  默认值仅提示；不改变 required/null 或运行时归一化。复杂 object/array 不猜测嵌套语义。
  不增加运行时 Schema 验证依赖、在线服务、生成源码或语言服务器。
- Tests：tests/support 的 fixture 管全局 Init/DeInit；局部不可复制句柄辅助仅管 Create/Destroy。
  Close 返回 SDK 原始状态及诊断，析构兜底失败须可见；输出和进行中调用必须先结束，Destroy 错误后不重试。
  首批迁移 Golden 和 DocQA 正向测试；DTO、命名槽、真实 converter 和响应断言保持显式。
  生命周期/并发/非法参数契约测试继续直接控制 API；不建立通用 PipelineTestRig 或全局隔离机制。

## 3. 兼容与迁移

后续审查确认 Schema 固定结构双写和 Node 底层绑定重复，用户已授权修复。
固定结构以 Core / Integration 各自持有的 JSON 结构声明为唯一来源；原解析器逐项调用浅层检查，
保留检查次序、原错误文本和语义逻辑。工具只复制结构并填充 Catalog，不再另列固定字段/范围。
浅层谓词不承担递归校验、规范化或 DAG/业务判断，不增加依赖。
统一的是现有结构约束来源；新增字段仍需在原解析器中显式消费并提供诊断。
Node 六处查计划、检查类型、Resolve 统一为 detail helper；缺绑定策略、Unbind 与诊断继续归调用入口。

Operator 函数表、DTO、完整请求响应、错误码/诊断、池深度与租约、Pipeline/conf、Catalog 字段和值不变。
不向 Pipeline 添加 $schema；现有严格未知字段拒绝保持。旧 CLI 行为保持，新增命令独立定义。
Node 仅改布线，不改 Definition 的名称、顺序、来源、cardinality、lifetime 或并行声明。
三个工作包可独立回退；已有扩展入口保留，不引入双份业务实现。

## 4. 验证与完成条件

1. 基线与变更后生产/测试 Catalog、既有命令结果一致；代表性 Node 原行为测试与绑定重映射/可选输入/错误顺序通过。
2. Schema 按当前注册生成，重复导出确定；节点/模型/后端分支、字段提示及完整 deployment 覆盖。
   检查合法配置无结构误报和可表达负例；语义检查能力边界有说明。编辑关联和刷新入口可直接使用。
3. 测试句柄创建失败不销毁，提前退出会清理，Close 错误不重试，多句柄互不提前销毁；迁移用例完整响应保持。
4. 记录机械改动前后样板及公共支持成本，更新指南；不以 Agent 自测替代真实新手试用。
5. 独立评审绑定/生命周期与 Schema 兼容性，最终执行 CONTRIBUTING 的统一门禁。

## 5. 实施与最终结果

Node 采用低成本机械复用：更复杂静态成员映射尚无额外收益证据，未纳入本轮。
高级作者仍显式维护成员、绑定列表与 Definition；本次不宣称已实现自动布线。

- 三个生产节点的13个端口名称和类型各有一个 typed 来源；连同 advanced starter，15次逐项绑定变为4次批量绑定。
  算法、绑定时机、端口元数据和错误优先级保持；六处底层绑定共用一个查找、类型检查与 Resolve 实现。
- 新测试辅助81行；八个 Golden 与 DocQA 的每例创建样板由约10–12行降为4–5行。
  初次抽取有公共支持成本，新增测试也会增加总行数，不宣称整个测试目录净缩小。
- Schema 导出器只叠加 Catalog 的动态选择，不再维护固定结构的字段、必填项或范围副本。
  Core / Integration 共用少量浅层结构谓词，保留原解析流程；不增加递归校验器或项目依赖。
  [编辑工作区](../../edgeflow.code-workspace)提供刷新任务和配置文件关联，不追踪个人 .vscode 配置。
- 现行使用说明：[手写配置](../../configs/README.md#手写-json-的补全)、[高级端口](../dev_guide/custom_node_concepts.md)、
  [端到端测试](../../tests/README.md)。

初轮聚焦验证结果（后续去重修复另行验收）：

- 8/8 CTest（NodeBase、Embedding、TextTemplate、OCR、CommonNodes、FunctionNode、Golden、DocQA）通过；
  新增2项绑定用例、4项句柄辅助用例和双句柄独立关闭用例均进入现有测试入口。
- 7/7 Schema CLI/产物测试通过；生产及测试 Catalog、既有 keyword validate/plan 共四项基线输出逐字节一致。
- 使用环境已有 Python jsonschema 独立检查 Draft 7：10份生产与11份 Mock/测试配置无结构误报，
  12个结构负例同时被 Schema 和原生工具拒绝；7种 allocator JSON 形态不被通用 Schema 限制；
  循环依赖仍由原生 Validator 拒绝，Schema 不冒充语义验证器。
- 在 /tmp 独立安装 vscode-json-languageservice 5.6.3（不加入仓库依赖），实测7组补全：Node 类型、
  两类 Node 参数、OCR 输入/输出端口、Model/Backend 参数；合法完整文档无编辑器诊断。
- 独立评审发现并修正 allocator params 的错误 object 限制及非 Node 注册冲突未阻止导出的问题。
  新导出路径拒绝冲突，既有 catalog 行为不变。

基线和原始验证产物位于 /tmp/authoring-friction-baseline、/tmp/authoring-friction-*；编辑器探针位于
/tmp/edgeflow-schema-editor-check。当前构建未启用 kite_llm/whisper_cpp，另外7份生产配置原生验证返回
UNKNOWN_BACKEND，未计作通过；没有执行真实模型效果或目标硬件验收。
以上证明工程去重和补全能力，真实新手的完成时间与求助次数尚未测量。

## 6. 重复机制收敛验收

- Core / Integration 固定结构分别供原解析器与 Schema 导出器使用，导出器不再另列固定字段和范围；
  NodeBase / FunctionNode 六条绑定路径复用一个底层实现，各入口保留原有缺失策略和诊断。
- 生产和测试 CLI 各执行671组旧/新 validate 差分，返回码、stdout、stderr 全部一致。
  覆盖必填、未知字段、类型/空值、数值及数组边界、条件约束和首错顺序；测试构建的模型基准验证成功。
  原始二进制/Schema 位于 /tmp/edgeflow-single-source-baseline，差分结果位于 /tmp/edgeflow-compat-prod 和
  /tmp/edgeflow-compat-test；脚本为 /tmp/edgeflow_structure_compat.py。
- 两个构建的 Schema 与 Catalog 逐字节不变；重新执行7项 Schema 测试、21份可用配置/12个负例的
  Draft 7 检查和7组真实编辑器补全探针均通过。
- 7/7聚焦 CTest通过；新增绑定回归覆盖17个 FunctionNode 变体及3个 NodeBase 输出异常变体。
  独立只读评审未发现阻塞项，确认层依赖、空 key 优先和必填失败不 Unbind 的行为。
- 最终统一门禁使用 `LLM_EDGEFLOW_JOBS=4 ./scripts/run_all_tests.sh`；临时差分工具和第三方编辑器探针不进入项目依赖。

