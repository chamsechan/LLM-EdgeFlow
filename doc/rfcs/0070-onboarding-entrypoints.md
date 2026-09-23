# RFC 0070：新人入口与接入名称信息收敛

- **RFC 编号**：0070-onboarding-entrypoints
- **创建日期**：2026-09-23
- **文档状态**：Completed
- **关联分支**：`refactor/onboarding-entrypoints`
- **目标版本**：投产前
- **负责人 / 作者**：LLM-EdgeFlow maintainers
- **关联决策**：延续 RFC-0059 的独立转换器与 Binding；对 RFC-0066 保持 Catalog 字段的范围作增量扩展。

## 1. 问题与范围

新人入口分散，已有 Recipe 未成为相应任务的默认入口。业务契约、Demo 入口和外部槽
名称承担不同职责，Catalog 尚未完整导出槽的类型与后缀；工具还维护了一份会漂移的
业务到 Binding 名称表。本次仅涉及接入适配层元数据、工具展示和任务文档。

## 2. 决策与权衡

- 新人导航收敛为已有能力编排、新增自定义 Node、新增外部业务契约三条可组合路径；
  复用现有 Recipe、脚手架和无模型关键词样例，其余参考保留在进阶入口。
- Catalog v4 的 `external_slots` 增加 `type_id`、`type_suffix`、`key_suffix`。
  `key_suffix` 导出 `ExternalSlotDefinition::KeySuffix()` 的有效值，其他两项直接来自
  Definition；不由业务名称或槽名猜测。原字段和值保持，版本仍为 4。
- Studio 在运行页的可展开契约详情中展示 Pipeline 业务契约、Demo 入口、Binding、
  Converter 和槽位名称。仅展示 Catalog 与当前配置；未指定绑定时列出候选，不代替
  原生解析选择或验证。旧工具未导出的字段显示未提供。
- 删除 Python 工具的固定业务名称表，沿用配置中的显式绑定及原生部署校验。
  需要从 Profile 补充部署时复用该 Profile 的已校验配置，不维护第二套绑定推断规则。
- 浏览器验收暴露既有模型改名遗漏 `deployment.model_paths` 引用的问题；在现有编辑函数
  同步更新该键，保留路径值与其他覆盖，继续通过原生 Validator 校验与现有撤销恢复。
- 不增加业务 DSL、注册表、聚合宏、通用业务生成器或新的运行命令；现有 Converter、
  ValueType 和 Demo 按需复用。本次不改变算法、Operator 载荷、内存所有权或 CLI 参数。

## 3. 兼容与迁移

Catalog 为增量字段扩展；Studio 兼容缺少新字段的 v4 工具。槽后缀的运行时匹配规则
不变，所有名称仍由原注册定义持有。显式配置继续优先；缺失或非法绑定由既有原生
部署校验报告。文档与工具同步调整，无新旧运行时并存。

## 4. 验证与完成条件

- 现有 Catalog 契约套件覆盖默认及非同名槽的类型后缀、有效外部 key 后缀。
- 现有工具测试证明不依赖固定业务名，显式绑定保持，缺失绑定不被静默猜测。
- Studio 浏览器测试覆盖实际契约展示、方案切换与窄屏布局；检查浏览器错误和截图。
- 无模型关键词配置通过原生校验、部署解析及 Demo，核对四条预期输出。
- 按 CONTRIBUTING 执行单一最终门禁。开发者首次成功时间的实际试用仍未覆盖，
  不以工程测试代替体验验收。

## 5. 实施与最终结果

已收敛三条新人路径，新增 Catalog 槽位信息与 Studio 接入详情，删除工具中的固定
Binding 映射；一并修复浏览器验收发现的模型改名遗留部署引用。

聚焦验证：四个相关构建目标成功；`CatalogContractSsotTest` 通过；运行方案与效果验证
套件 16 项通过（其中浏览器当次跳过），随后启用 Playwright 单独完成浏览器完整流程
与模型表单检查 2 项，无跳过；Recipe 套件 21 项通过。桌面与 390px 截图已检查。
关键词效果测试经过真实 Operator，四条输入及预期输出全部匹配。

最终交付以 `./scripts/run_all_tests.sh` 成功为准。开发者试用、真实模型效果与目标硬件
验收不在本次范围。现行入口见[任务导航](../README.md#按任务开始)与
[Studio 首次编排](../../tools/pipeline_studio/README.md#第一次编排)。
