# Pipeline 配置改造第 1–6 步审计

> **状态**：审计完成；以下问题与未完成的验收项待整改、复查。
> **审查分支**：`refactor/pipeline-config`。
> **基线**：`main@cda1f5c`。
> **审查版本**：`13ea391fd57c55e1332862868da3be8c3b3a3c2e`。
> **性质**：在进行中的 refactor 的阶段审计与整改清单，不是现行契约或修复完成声明。
> **生命周期**：随本轮 refactor 收尾删除；形成的规则写入对应现行指南，审计历史保留在 Git / PR 中。

## 1. 结论与范围

第 1–6 步的主线方向符合设计，但尚不能全部按文档验收。确认了三处实现偏差、
一处基线遗留的第 6 步验收缺口，以及一项参数测试覆盖不足。
现有过渡门禁、黄金测试和 smoke 通过，不能替代下述边界场景的验证。

依据：

- [实施总说明](PIPELINE_REFACTOR_GUIDE.md)：第 1–6 步、代码范围和过渡期门禁。
- [I/O 设计](PIPELINE_IO_DESIGN.md)：阶段 0、1、2；以总说明规定的延期范围为准。
- [模型设计](PIPELINE_MODEL_DESIGN.md)：阶段 1，即统一参数机制。
- [节点设计](PIPELINE_NODE_DESIGN.md)：阶段 1，即参数机制与文本处理类节点。

本次审查包含生产代码、相关测试、配置迁移及现行文档；未实施修复。
第 7–9 步的模型格式、节点连线格式和工具改造不属于本次验收。
元素 `Prepare` / `Validate` 失败的字段级诊断路径已由总说明明确安排到第 8 步，未计为缺陷。

| 步骤 | 审查结论 |
| --- | --- |
| 1：参数声明下沉 | 迁移与改名符合范围；该步的节点生产代码只改类名 |
| 2：Demo 与 SDK I/O 预检 | 主体符合设计；原 smoke 输出一致，SDK、Demo 相关过渡测试通过 |
| 3：模型与 Backend 参数统一 | 四段写法、加载前解析、BGE 可选参数与模型形状解析已实现；逐实现测试需补值断言 |
| 4：节点参数机制 | 元素类型、Include、生成参数和字段 Control 主体符合设计；与第 6 步的参数读取组合存在 R2 |
| 5：文本处理类节点 | 声明迁移与旧 API 删除主体符合设计；存在 R4 的空正则行为变化 |
| 6：I/O 格式切换 | 格式、注册、参数和 Core 去业务化主体符合设计；存在 R1、R2、R3，Whisper 方案专项验证未完成 |

## 2. 问题与整改验收

### R1：可选输出槽丢失批次索引，结果可能错配请求

**优先级**：P1。**归属**：第 6 步验收缺口；该机制在基线已存在，非本轮新引入。

位置：[operator_adapter.cpp，第 423–424 行](../src/adapter/operator/operator_adapter.cpp#L423)。

I/O 设计 §5.4 规定，可选输出槽总是分配输出池，宿主每次调用可以省略对应 key。
当前构造 `ExternalOutputBatchView` 时对已租用块执行 `push_back`，没有使用
`AcquiredOutputBlock::frame_idx`。省略某行输出后，后续块在槽内的索引被前移，
而 converter 仍按原批次索引读取结果与请求 ID。

已复现：使用支持跳过空目标的测试 converter，声明 `slot.required = false`。
两行输入的请求 ID 分别为 42、43；第 0 行省略输出 key，第 1 行提供输出 key。
Create 和 Process 均返回 0，但第 1 行输出的 `request_id` 是 42，预期为 43。
此外，直接把现有逐行输出登记改为可选、整批省略目标时，Create 成功而 Process 返回 -4。
逐行 helper 本来只适用于必填槽，这一后者现象不能替代前述错配复现。

整改方向：输出视图保留完整批次索引，省略位置保持空目标；同时明确并实现可选槽的
编码和 `written_count` 规则，保证按原行关联结果。

- [ ] 覆盖全部输出省略、首行省略、中间行省略、不同输出项分别省略的场景。
- [ ] 核对每个实际输出的请求 ID、业务字段和所属行，保证无错配。
- [ ] 验证失败时所有输出租约归还、无残留发布。

### R2：结构体元素不能通过 ParameterSet 完整解析

**优先级**：P2。**归属**：第 4 步元素支持与第 6 步 Read / Effective 的组合。

位置：[parameters.h，第 535–536 行](../include/contracts/parameters.h#L535)，
调用方：[parameter_set.h，第 131 行](../include/contracts/parameter_set.h#L131)。

`ConcreteFieldBinding::Read()` 直接调用容器的 `FieldTypeTraits::ToJson()`，没有使用
`.Items(Parameters<E>)` 的元素声明。结构体元素进入 `ElementToJson` 后抛出
`Struct elements cannot be written as a default value`。

节点设计 §5.1 要求支持结构体元素；I/O 设计 §5.2 要求读取 Prepare 后的声明字段，
形成生效值。当前普通 Node 的 `Parameters::Parse` 能成功，而使用 `ParameterSet`
的 Model、Backend、converter 对合法的非空结构体数组或映射会解析失败。

最小复现：

```cpp
struct Element { std::string text; };
struct Params { std::vector<Element> rows; };

auto elements = Parameters<Element>({Field("text", &Element::text).Required()});
auto spec = Parameters<Params>({
    Field("rows", &Params::rows).Required().Items(elements)});
ParameterSet set(spec);

auto config = nlohmann::json::parse(R"({"rows":[{"text":"ok"}]})");
// spec.Parse(config, &error) 成功。
// set.Parse(config, &values, &error) 返回 false，报上述异常。
```

整改方向：Items 同时提供元素解析与读取能力，递归导出已声明字段；不把编译正则等
派生状态加入生效参数。

- [ ] 覆盖 `vector<E>`、`map<string, E>` 及可选容器的 `ParameterSet::Parse`。
- [ ] 检查 `Effective()` 包含元素默认值及 Prepare 后的字段值。
- [ ] 验证派生成员不参与配置序列化。

### R3：同侧 type 唯一限制超出了设计

**优先级**：P2。**归属**：第 6 步 I/O 契约。

位置：[deployment_preparation.cpp，第 89–93 行](../src/adapter/deployment_preparation.cpp#L89)。

I/O 设计 §4.2 只规定同侧 `(type, name)` 唯一，并明确单独的 type 或 name 都可能重复。
实现另外检查 `type` 唯一，因此同一结构体上的不同业务也无法在同侧组合。

已复现：选用两个已登记、业务名和 service_type 均不同的 `keyword_out` 输出项。
`PrepareDeploymentDocument` 返回 false，诊断为 `INVALID_COMBINATION`，路径
`/io/output/1`，消息为 `Host struct type 'keyword_out' is used by more than one io output item`。

当前槽视图与输出池也以 type 为键，因此不能只删除这一检查；否则可能产生槽或池覆盖。
整改需核对宿主 map 的寻址方式，实现按项区分，或明确修改设计契约后再验收。

- [ ] 明确同 type、不同 name 的输入与输出项如何寻址。
- [ ] 按最终约定验证不同业务、不同参数与不同布局的项互不覆盖。
- [ ] 保留 `(type, name)` 重复和输入端口重复生产者的拒绝测试。

### R4：空 regex 从不命中变为命中所有文本

**优先级**：P2。**归属**：第 5 步文本规则节点。

位置：[text_rule_match_node.cpp，第 60–65 行](../src/common_nodes/text_rule_match_node.cpp#L60)。

基线仅在 `strategy == "regex" && !pattern.empty()` 时编译正则；空 pattern 留下空的
compiled_regex，执行时判为不命中。新 Prepare 去掉了空 pattern 检查，PCRE2 会成功
编译空正则并匹配任意文本。

已通过 Operator 复现：配置 `{"strategy":"regex","pattern":"","category":"ALL"}`，
输入 `ordinary text`，Create 和 Process 均成功，结果为 `is_hit=1`、`intent="ALL"`。
`update_rules` 也会运行相同的 Prepare，因此同样受影响。

总说明要求保持行为；节点设计 §6.4 和阶段 1 验收没有授权这项变化，且要求
`update_rules` 的行为与改造前相同。

- [ ] 保留基线的空 regex 行为，或先明确修订设计中的行为约定。
- [ ] 初始化与 Control 更新均覆盖空 pattern、普通输入和空输入。

## 3. 参数测试覆盖不足

位置：[test_model_backend_parameters.cpp，第 192–207 行](../tests/unit/engine/test_model_backend_parameters.cpp#L192)。

逐实现测试只断言 `Parse` 成功或失败，没有检查返回的参数值。即使某个覆盖值被忽略，
或默认值赋错，测试仍可能通过，不能充分证明模型设计第 15 节的“每个实现默认值正确、覆盖生效”。

- [ ] 通过 `Effective()` 或实际行为断言每个实现的默认值、覆盖值及可选参数省略状态。
- [ ] 区分未编入构建的 Backend 与真正执行过的参数验收。

## 4. 已完成的验证

以下结果对应上述审查版本的实现；保存本文件不代表问题已经修复。

| 检查 | 结果 |
| --- | --- |
| `./scripts/format.sh --check` | 通过 |
| `git diff --check` | 通过 |
| dev-gate 配置与完整构建 | 通过，依赖复用本地缓存 |
| 过渡门禁 `ctest -LE tier4` | 80/80 通过，0 失败 |
| Demo smoke | 10 个 profile、16 条记录全部成功 |
| 与原 smoke 基线比较 | 原 9 个 profile、14 条记录的 status/output 逐条完全一致；新增 keyword_match_control 的 2 条记录 |
| 普通 `configs/` 方案 | 10 个使用生产工具 validate/plan 全部通过 |
| `demo/fixtures/mock/` 方案 | 9 个使用同一构建的测试工具 validate/plan 全部通过 |
| Kite 方案 | 6 个使用 kite-cpu 构建的生产工具 validate/plan 全部通过 |
| kite-cpu 完整构建 | 通过；本地发布归档的 SHA256 与 CMake pin 一致，API 编译/链接探针通过 |
| Kite 专项 `ctest -L kite` | 6/6 通过，0 失败 |
| 原 Operator 黄金测试期望值 | 未修改；对应过渡测试通过 |

过渡门禁命令：

```bash
./scripts/format.sh --check
git diff --check
./scripts/configure_build.sh "$PWD" build dev-gate
cmake --build build -j"$(nproc)"
(cd build && ctest -j"$(nproc)" --output-on-failure --no-tests=error -LE tier4)
```

按构建选择方案验证工具：

```bash
./build/alg_pipeline_tool validate <普通方案>
./build/alg_pipeline_tool plan <普通方案>
./build/alg_pipeline_tool_test validate <mock方案>
./build/alg_pipeline_tool_test plan <mock方案>
./build/variants/kite-cpu/alg_pipeline_tool validate <kite方案>
./build/variants/kite-cpu/alg_pipeline_tool plan <kite方案>
(cd build/variants/kite-cpu && ctest -j"$(nproc)" -L kite --output-on-failure --no-tests=error)
```

## 5. 未验证范围

共 26 个生产及 Demo mock 方案，其中 25 个完成 validate/plan。
剩余 `configs/pipeline_audio_asr_intent_cpu.json` 在 dev-gate 构建中因未启用
`whisper_cpp` 报 `UNKNOWN_BACKEND`。已有 Whisper 构建补建工具触发重编后已中断，
该方案的专项验证尚未完成，不能据此判断方案本身有缺陷。

补验命令：

```bash
cmake --build build/real-models/whisper -j"$(nproc)" --target alg_pipeline_tool
./build/real-models/whisper/alg_pipeline_tool validate configs/pipeline_audio_asr_intent_cpu.json
./build/real-models/whisper/alg_pipeline_tool plan configs/pipeline_audio_asr_intent_cpu.json
```

默认构建的 DemoRunnerTest / LlamaCppBackendTest 内部有 8 项 GoogleTest 跳过。
Kite 专项内部有 9 项跳过：3 项因该构建禁用 llama.cpp，6 项因真实模型资产或运行开关未提供。
CTest 通过不代表真实文本、视觉、token embedding、GGUF 或目标部署效果验收通过。

本次未下载权重、未访问公司内部 SDK。按实施总说明，未运行 tier4 和完整
`run_all_tests.sh`；工具及可视化的最终门禁安排在第 9 步。
