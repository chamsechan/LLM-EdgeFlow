# RFC 0072：业务身份与输出配置收敛

- **RFC 编号**：0072-business-configuration-convergence
- **创建日期**：2026-09-23
- **文档状态**：Completed
- **关联分支**：`refactor/business-config-convergence`
- **目标版本**：投产前
- **负责人 / 作者**：LLM-EdgeFlow maintainers
- **关联决策**：接续 RFC-0071；取代 RFC-0070 的独立 Demo 名及必须显式选择 binding 的范围；调整 RFC-0061 的部署必填项及 RFC-0059 的普通绑定选择方式。

## 1. 问题与范围

普通路径重复维护业务名、Demo 别名和 binding ID，输出配置还重复声明注册信息已确定的类型。
本次统一业务入口并删除重复配置，不增加业务别名、模型 preset、PortFlow preset 或状态标签。
涉及接入适配层、BizDefinition 元数据、Demo 和配置工具；算法与模型运行协议不变。

## 2. 决策与权衡

- `biz_name` 是 Pipeline、Demo `--biz`、Profile 的统一业务身份。删除 `BizDefinition.demo_biz`
  及 Demo 的 `expected_binding_id`。SDK 预检辅助函数改为 `ValidateOperatorConfigBiz`，验证
  解析后的业务身份，保留 `noexcept` 和双异常屏障。Registry 与部署预检共用同业务外部契约
  一致性校验：Converter 协议 ID/版本、载体类型、有效外部后缀、槽类型/方向/必需性及输出
  数量/容量策略必须一致。逻辑槽名和批次上限可不同；不同载体属于不同外部业务契约。
  这避免相同业务下切换 binding 后，Demo 用既有载体转换不兼容的 `void*`。
- 原生 `PrepareDeploymentDocument` 唯一负责选择 binding：显式 `io_binding` 优先；省略时按
  `biz_name` 查注册表，恰好一个候选才选择。没有候选或多个候选均报错，禁止靠名称拼写猜测。
  Converter、slot、ValueType 身份仍由既有注册关系确定；它们并非业务名的同义词。
- `deployment`、`io`、`io_binding`、`output_allocations` 均可省略。已注册的必需输出槽自动采用
  默认分配配置，可选输出槽仍由显式槽配置启用。未知槽、非法类型和未知字段继续拒绝。
- 删除持久配置 `output_allocations.<slot>.type`，运行时类型直接取 slot Definition。
  默认 allocator、默认容量和零 metadata 从既有注册/规范化流程获得；非默认配置继续使用
  原有 allocator、params、capacities、meta_num、metadata_type_id，保留全部预算和布局校验。
  缺少必需布局参数的自定义 allocator 仍报错，不凭业务名称推导布局。
- 普通逐条保序 Node 使用现有 `MakeMapSpec` / `MakeLlmTextSpec` 自动保留来源关系；非默认
  关系才声明 `PortFlow`。保留现有 `parallel_safe` 的保守默认，仅并行开发时说明其要求。
- 模型能力沿用 RFC-0071 从 Definition 推导；模型类型、backend 和实例引用不通过文件扩展名
  猜测，不增加运行时模型资产引用层。已有资产清单与当前模型字段保持一致。

## 3. 兼容与迁移

投产前直接迁移，不提供旧别名或双解析兼容层。SDK 辅助函数改名，ABI/SOVERSION 升为 8。Catalog 删除 `demo_biz`；更新 Demo 注册、
Profile、脚本、Studio、SDK 导出、测试和现行文档。旧输出 `type` 作为未知字段拒绝。
普通样例删除 binding ID、输出类型、默认 allocator、零 metadata；容量只有与注册默认相等
时才删除，确保原有效容量不变。内部输出规格仍保留完整类型和实际容量。

## 4. 验证与完成条件

1. 原生解析测试覆盖唯一/零/多个 binding、显式选择及业务不匹配、可省略部署结构与非法字段。
2. 输出测试覆盖默认必需槽、可选槽、类型字段拒绝、显式容量覆盖、特殊 allocator、预算与回滚。
3. Demo、Catalog、Schema 与工具测试证明业务身份一致，工具不复制 binding 推导规则。
   `validate/plan` 对不含 deployment 的文档保留中性 Pipeline 校验；`validate-io/resolve-conf`
   与 Operator 执行完整部署解析。中性校验成功不代表已完成绑定、输出分配或路径的部署验收。
4. 运行迁移后的无模型关键词和 Mock Demo，检查完整响应、状态与请求 ID。
5. 独立审查 SDK/接入边界，按 CONTRIBUTING 执行单一最终门禁。

## 5. 实施与最终结果

原生解析、SDK ABI 8、八个 Demo 入口、22 个 Profile、28 份 Pipeline 文档与现行作者指南已迁移。
标准输出删掉重复配置，唯一非默认的翻译输出容量 8191 保留；资产清单删除已失效的模型字段。
脚手架不再输出默认输入 PortFlow，普通来源维护沿用 Map/LLM 的既有实现。

聚焦验证：首次 12 个相关 CTest 套件全部通过；补齐同业务外部载体一致性检查后，11 个相关
套件再次全部通过，覆盖真实注册载体冲突、无需 Init 的公开预检及兼容备用 binding。
独立源码与测试审查未留下阻断问题。Studio 97 项（含真实浏览器，无跳过）、Recipe 22 项、
Scaffold 21 项、JSON Demo 6 项及三个 JS 模块通过。
最终门禁首次发现 CMake 精排 Demo 测试命令遗留别名，已迁移正常与缺模型两条命令；
两项定向测试通过，负向输出确认真正到达缺失模型路径检查。

26 份可枚举方案分别执行 validate、plan、validate-io、resolve-conf；19 份全部通过，解析后的
binding、输出槽、类型、容量及 metadata 与基线一致。其余七份仅因当前构建未启用 backend
失败（六份 Kite、一份 Whisper），不计为部署验证成功。关键词四条效果输入及三组 Demo
（关键词两条、普通自定义 Node 一条、复杂自定义 Node 两条）状态、请求编号和完整输出均符合预期。

最终交付由 `./scripts/run_all_tests.sh` 确认；现行用法见[配置指南](../../configs/README.md)、
[业务接入](../dev_guide/business_onboarding.md)和[Node 按需参考](../dev_guide/custom_node_concepts.md)。
真实模型效果、禁用 backend 和目标硬件不属于本次验收；未进行远程提交。

