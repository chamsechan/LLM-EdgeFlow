# RFC 0073：以 I/O 契约作为配置的唯一入口

- **RFC 编号**：0073-io-contract-entry
- **创建日期**：2026-09-23
- **文档状态**：Completed
- **关联分支**：`refactor/io-contract-entry`
- **目标版本**：投产前
- **负责人 / 作者**：LLM-EdgeFlow maintainers
- **关联决策**：接续 RFC-0072；取代其中由 JSON biz_name 反查唯一 binding、可省略整个 deployment 及 Demo/Profile 重复填写业务名的规则。

## 1. 问题与范围

配置作者需要显式选择外部 I/O 契约并覆盖输出内存参数。现有 binding 已唯一关联内部业务边界，
因此持久文档同时维护业务身份和 I/O 选择没有必要。本次保留 `io_binding`，将
`output_allocations` 改名为 `out_mem`；不采用 `in_mem`，因为 binding 同时选择输入与输出转换，
不表示输入内存。上一轮已验证改动完整保留，按本 RFC 调整入口契约。

## 2. 决策与权衡

- 外部 Pipeline JSON 必须包含 `deployment.io.io_binding`；删除根级 `biz_name`，出现即拒绝。
  不再按业务名或唯一候选数推导 binding。`out_mem` 仍可省略，语义与上一轮输出分配覆盖一致。
- 接入适配层先解析 binding，再把其注册的 `biz_name` 注入内部中性 Pipeline 文档。Core
  继续验证内部业务端口、Node 业务限制及数据闭环，不依赖 Adapter 或平台 C 结构体。
- CLI validate/plan/edit 与 Operator 共用原生准备入口；取消 CLI 对缺 deployment 文档的中性
  校验回退。内部 Core 测试和 NodeHarness 仍可直接使用中性文档，不能通过它接受旧外部格式。
- Studio 和 authoring 通过明确的 binding ID 查询 Catalog/注册关系，展示并使用内部业务边界；
  保存和编辑结果不写回推导出的 `biz_name`。CLI init/catalog 使用 `--io-binding` 选择契约。
- Demo 删除 `--biz` 和 Profile 的 `biz` 字段，从配置解析出的业务身份选择已注册 runner。
  用 `ResolveOperatorConfigBiz` 替换原预检辅助函数，返回已解析业务名，仍只有一个预检/查询
  入口，保留 noexcept 与双异常屏障。SDK ABI/SOVERSION 升为 9，标准 C++ string 仅承载结果。
- 保留同业务跨 binding 的外部载体/协议一致性检查。输出布局、默认容量、覆盖、预算、租约及
  失败回滚规则不变；`out_mem.params` 仍是分配器布局参数，不冒充 Converter 参数通道。
  本次不增加未明确需要的字段映射 DSL、通用转换参数或 preset。

## 3. 兼容与迁移

投产前直接迁移：外部 JSON 删除 biz_name、显式写入已有注册 binding；输出配置改为 out_mem。
旧 output_allocations、旧 Demo --biz、旧 Profile biz 拒绝，不保留同义字段。
同步生产与 Mock 配置、SDK 导出、Demo、Studio、Recipe、Schema、效果验证与测试。
诊断和 Catalog 可展示推导出的业务名；它不是第二个需要作者填写的配置选择。

## 4. 验证与完成条件

1. 外部入口覆盖缺 binding、未知 binding、旧字段拒绝；Core 中性结构及单次 Validator 计划保持。
2. 同业务多个合法 binding 可显式选择；内部端口闭环、外部载体一致性、输出默认/覆盖/多槽及预算保持。
3. SDK 查询测试覆盖无 Init、空指针、错误清理与安全屏障；Demo 单独 --config 和 Profile 均自动选对 runner。
4. CLI/Studio 编辑保存不出现 biz_name 或旧内存字段；效果工具与 Recipe 不维护绑定推导表。
5. 迁移样例实际运行并核对 ID、状态及完整输出；独立审查后执行 CONTRIBUTING 的统一最终门禁。

## 5. 实施与最终结果

- 原生解析、SDK 查询、Demo/Profile、CLI/Schema、Studio/Recipe/效果工具及当前指南完成迁移。
  删除不再需要的 deployment/io 可省略状态字段；旧名称没有兼容别名。
- `resolve-conf` 使用同一次解析得到的外部文档快照，保留 deployment 与解析后的模型路径，
  不再二次读取磁盘；派生业务只出现在查询元数据。Core 业务诊断映射到外部 binding 路径，
  写回内部业务名的修复被剔除，其他有效修复保留。
- 独立审查完成，发现的快照一致性与旧 Profile 字段入口差异均已修复并复审。
- 原生聚焦覆盖 16 组 CTest；Studio 100 项（含真实 Chromium 浏览器）、Recipe 23 项、
  Scaffold 21 项、JSON Demo 6 项和 3 个 JS 测试脚本通过。
- 当前构建的 19 份生产/Mock 配置通过原生验证；6 份 Kite 与 1 份 Whisper 配置因对应后端
  未启用而不进入运行验收。关键词配置单独运行 4 条、实体抽取 Mock Profile 1 条全部成功。
  翻译 Mock 的 `out_mem.entity_out.capacities.entities_json=8191` 覆盖经预检和实际请求确认，
  请求 ID、状态与完整 JSON 响应一致。
- 统一交付门禁为 `LLM_EDGEFLOW_JOBS=6 ./scripts/run_all_tests.sh`；Completed 状态纳入同一次
  最终门禁，失败时恢复实施状态并修复。无远程交付；真实模型效果及目标硬件未验收。
