# RFC 0041: Control 开发路径的最小收敛

- **RFC 编号**：0041-control-authoring
- **创建日期**：2026-09-07
- **文档状态**：Completed
- **关联分支**：`feat/control-authoring`
- **目标版本**：v10.x
- **负责人 / 作者**：LLM-EdgeFlow maintainers / Codex

## 1. 背景与动机

业务作者已有 `ControlNode` 和 `NodeDefinition.control_commands` 扩展入口，但仍需复制
参数检查、维护失败诊断，并在 Operator 中为每种业务命令添加枚举与参数转换分支。
现有 Node 脚手架没有可执行的 Control 入门路径。目标是让具备普通 C++ 能力的作者能在
所属节点内新增 JSON 控制逻辑，通过原有 Demo 验证，日常无需修改 Pipeline 或 Operator。

## 2. 范围与边界

范围内：

- 复用现有有限 schema 校验，提供普通 C++ 解析函数；保留 Node 的业务语义校验。
- 补齐 Control 的失败诊断；节点声明、实现和基础校验使用同一份 schema。
- 新增固定结构的 Operator JSON 入口及 Demo `--control-cmd`，保留已有调用方式。
- 提供一个可编译的值类型配置更新模板、脚手架选项、教程与现有套件内的业务测试。
- 在现有 Catalog 内检查跨节点命令 ID 冲突，显式标记允许共享的同义命令。

非目标：完整 JSON Schema、字段反射 DSL、新全局注册系统、新节点继承体系、实例寻址、
命令名称协议迁移、跨节点事务、模型/资源重载及内网公司 SDK 接入。

## 3. 技术方案

### 3.1 职责映射

- **接入适配层 / Integration**：Operator 参数类型决定转换方式，拷贝借用 payload；
  C ABI 保留六个导出函数及异常屏障。
- **流程编排层 / Orchestration**：调用共用 schema 校验、已有整数命令路由、Catalog 冲突检查
  和失败诊断。普通命令仍向所有声明支持它的节点广播。
- **能力节点层 / Capability Nodes**：控制语义和配置更新。模板使用拥有数据的临时值
  与读写锁，不持有平台指针，不保存请求状态。
- **Tooling / Demo**：沿用现有脚手架、Catalog 和统一 Demo，不维护第二份命令清单。

### 3.2 接口与兼容

Operator 在枚举尾部添加 `ControlCommand::kJson = 4`，对应唯一参数类型：

```cpp
struct ControlJsonParam {
  int32_t cmd_id = 0;
  const char* json_param_str = nullptr;
};
```

外层枚举只选择参数结构；内部 `cmd_id` 仍是节点声明的正整数。输入字符串借用到同步调用
返回，Integration 采用已有的非空、长度小于 65536 字节和 JSON object 校验并拥有拷贝。
未知内部命令仍返回 unsupported，不会绕过 Catalog。已有枚举 1/2/3、函数表布局及 C ABI
布局保持不变。

Demo 添加 `--control-cmd <positive-int>`，与 `--control-file` 配合使用；Profile 可声明
同名 `control_cmd`，CLI 优先。省略时保留各 Demo 的默认命令；所有 payload 统一通过
`kJson` 传递，不能将任意命令强转成 `ControlUpdateRulesParam`。显式命令必须有显式文件，
不能隐式复用 Demo 的默认规则 payload。

`Pipeline::Control` 添加可选错误字符串输出；返回码保持已有兼容语义。诊断包含命令、
节点实例 ID/类型与失败原因。一个节点失败不回滚其他节点，多次 Control 也不是事务；
同句柄 C ABI / Operator 的 Process 与 Control 继续串行，内部直接调用需遵守同步契约。

schema 校验从 Pipeline 提取到 `include/contracts/control_payload.h` 的普通 inline 函数，
沿用现有中立契约工具的组织方式，由 Pipeline、节点和 Integration 复用，避免反向实现依赖。
保留当前关键字子集；新增业务约束写普通 C++。规则和模板采用同文件的 schema 定义，
补齐规则元素未知字段/缺失 pattern 检查和模板锁外读取问题，避免错误示例被继续复制。

### 3.3 命令与源码管理

标准命令 ID 继续保留；新 custom 命令建议从 1000 起按项目分配，固定在节点同文件的具名
常量中，发布后不复用 ID。Catalog 拒绝跨类型重复 ID；明确需要多个类型共享时，两端
`shared_id=true` 且 name/schema/hot-swap 声明一致才允许。重复节点实例不属于重复注册。
此审计复用 Catalog/NodeFactory 的 fail-closed 路径，不增加注册表。

脚手架通过显式 `--control-id` 生成一个 TextBatch → TextBatch 的前缀更新入门节点。
这是受限、可编译的教学模板；复杂 Node 可将其 Control 片段加入现有类，不强制改变
ModelBoundNode 等继承关系。模板及生成代码只进入测试注册；生产 Catalog 只在作者
实际生成、登记节点源码后增加操作。测试片段加入现有套件。

## 4. 权衡

保留整数路由、既有接口与多次 JSON 解析，避免为低频控制改造整个调用链。有限 schema
之外的业务校验明确留在节点。配置更新只保证模板中值类型配置的单节点失败保持原状，
不承诺通用深拷贝或外部副作用回滚。未来实例定位和成套切换分别按真实需求设计。

## 5. 验证计划

- [x] 现有 Definition/Node 套件覆盖命令冲突、共享声明及公共 payload 校验。
- [x] Rule/Template 套件覆盖非法更新保持旧配置和有效更新的业务输出。
- [x] Operator 套件通过通用入口调用测试自定义命令，验证参数边界、错误详情及不同节点的连续更新。
- [x] 脚手架生成产物编译进现有 runner，验证有效更新、无效更新、输入输出来源。
- [x] Demo 测试验证 `--control-cmd`/Profile 合并、失败行为及实际处理结果。
- [x] 教程中的生成、Catalog、Demo 命令完成可复现验证。
- [x] `./scripts/run_all_tests.sh` 完整默认门禁通过。

2026-09-07 本地验证：统一门禁 89/89 CTest 通过；教程生成的节点经实际 Catalog
校验和 Demo 执行，命中结果从 false 变为 true，非法前缀返回退出码 5。临时教学节点
已清理，生产 Catalog 不含教学或测试注册。分层门禁发现并纠正了共用校验的归属：
实现最终位于中立契约目录，未放宽依赖检查。

## 6. 实施里程碑

1. [x] 在独立分支编写 RFC 并按最小范围自评。
2. [x] 完成共用校验/诊断、Operator/Demo 通路与节点模板。
3. [x] 完成现有套件内的针对性测试和教程验证。
4. [x] 运行统一交付门禁并更新状态；远端交付按用户明确授权执行。

## 7. 变更记录

| 日期 | 版本 | 变更内容 | 作者 |
| :--- | :--- | :--- | :--- |
| 2026-09-07 | 1 | 按方案自审结果收缩开发体验改造范围 | Codex |
| 2026-09-07 | 2 | 完成实现、教程验证及 89 项统一门禁测试 | Codex |
