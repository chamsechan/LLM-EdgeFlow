# 平台与调度参数归位：实施与验收

> 本文件是阶段 5 的工作计划，不是现行规则。全部步骤验收后，把 2.3 的判定原则写入
> [`CONTRIBUTING.md`](../CONTRIBUTING.md#3-design-and-current-contracts) §3，各步骤的现行用法写入对应指南，
> 然后删除本文件（`CONTRIBUTING.md` §3、§5 不保留提案和实施报告）。
>
> 核对基线：`refactor/adapter-binding-dedup@c894657`（阶段 3）。文中行号以该提交为准，实施时以符号名为准。

## 1. 目标与步骤

业务开发者（编写 Node 函数、转换器 Decode/Encode、编排 Pipeline JSON 的人）不了解平台资源和框架调度，也能接入并运行。
平台限制和调度参数由框架给默认值或自动推导；只有负责性能验收或部署的人，在有依据时才显式覆盖。

| 步骤 | 内容 | 规模 | 设计审查 | 前置条件 | 建议分支 | 状态 |
| --- | --- | --- | --- | --- | --- | --- |
| 5.1 | Studio 保留"未配置"状态；Backend 字段收进"部署高级设置" | 小 | 不需要 | 无 | `fix/studio-unset-defaults` | 已完成（PR #150） |
| 5.2 | 接入层平台数值归位：Binding 默认批次上限、输入长度上限单一来源、输出容量字段从 ValueType 推导 | 小 | 不需要 | 阶段 3 已合入 `main` | `refactor/adapter-platform-limits` | 已完成（PR #150） |
| 5.3 | 单次有效批次可查询；删除无读取方的字段；存量默认值清单 | 小 | 不需要 | 阶段 3 已合入 `main` | `chore/effective-limits-tooling` | 已完成（PR #150） |
| 5.4 | 请求编号回传移出业务契约 | 中 | 需要 | 5.2 已合入（改动同一批转换器文件） | `refactor/adapter-request-ids` | 已完成（PR #151） |
| 5.5 | 并行层按约束自动串行 | 中 | 需要 | D1 已确认 | `feat/auto-serialize-parallel-layers` | 未开始，待 D1 确认 |

建议顺序：5.1 → 5.2 → 5.3 → 5.4；5.5 与其他步骤没有代码依赖，D1 确认后即可进行。

## 2. 约束

### 2.1 不变的外部契约与检查方法

| 契约 | 检查方法 |
| --- | --- |
| 公共 Operator 接口（`include/edgeflow/`）和返回码 | `git diff --stat main...HEAD -- include/edgeflow include/platform_mock` 无输出；`CppOperatorSdkTest`、`OperatorSafetyTest` 通过 |
| 宿主结构体、槽位名、`request_id` 字段、`COMPANY_OPERATOR_MAX_RERANK_CANDIDATES` | 同上 |
| `.conf` 格式、Pipeline JSON 格式和现有字段语义 | 基线对比中 `pipeline-schema.json` 一致；`git diff --stat main...HEAD -- configs demo/fixtures demo/profiles.json` 无输出 |
| 输出池语义和容量 | 基线对比中 `resolve-conf` 的 `output_pools` 一致 |
| Demo 与 SDK 的交互 | Demo 只调用公共 Operator 接口；smoke 结果与基线一致 |
| 已发布的 Control 命令 ID | 基线对比中 Catalog 的 `nodes` 一致 |
| 输入的接受/拒绝边界、检查顺序和返回码 | 5.2 新增的边界测试 |
| 各业务的有效批次上限（64） | 基线对比中 `validate-io` 的 `effective_max_batch_size` 一致 |

唯一有意改变的外部行为是 5.5：原本 Create 返回 `-2` 的并行配置可以运行（见 D1）。

下列内部输出允许变化，对比时按第 3 节的参数排除：

| 步骤 | 允许的变化 |
| --- | --- |
| 5.2 | Catalog 中 `audio_result`、`audit_result`、`doc_answer` 三个输出转换器的 `capacity_fields` 改为字典序 |
| 5.3 | `resolve-conf` 新增 `effective_frame_depth`、`effective_process_batch_limit`、`max_frame_depth_limit` |
| 5.4 | Catalog 与 `validate-io` 中不再出现 `raw_request_ids` |
| 5.5 | `max_parallel_workers > 1` 的配置，`plan` 中的 `layers` 按约束拆分（仓库内现有配置都是 1，不受影响） |

### 2.2 决策点

| 编号 | 问题 | 默认做法 | 确认人 |
| --- | --- | --- | --- |
| D1 | 5.5 的兼容放宽：原本因 `NODE_NOT_PARALLEL_SAFE`、`SERIALIZED_MODEL_CONCURRENCY` 被拒绝、Create 返回 `-2` 的配置变为可运行 | 未确认前不实施 5.5 | 项目负责人 |
| D2 | Binding 默认批次上限放在哪个分支 | 随 5.2。若希望随阶段 3 一起合入，把 5.2.1 单独提交到 `refactor/adapter-binding-dedup` | 项目负责人 |
| D3 | rerank 候选段落的上限：Operator 层 10 MiB，转换器 64 KiB | 5.2 保持现有行为，只改为具名常量 | 方案负责人 |
| D4 | 配置中显式写出、且等于默认值的字段是否删除 | 5.3 只出清单，不改配置 | 方案负责人 |
| D5 | 5.4 中转换器记录的请求编号数量不对、或辅助函数拿不到请求编号表时的返回码。两者都是接入代码缺陷而非宿主输入错误，但平台错误码没有"内部错误"，且不能新增；`-4` 是 `COMPANY_ALG_ERR_BUFFER_TOO_SMALL`，现有"编码写出数量不符"沿用它，语义同样不贴切 | 用 `-3`（`COMPANY_ALG_ERR_INVALID_INPUT`），诊断文本写明是转换器问题 | 设计审查：**已定为 `-3`**，已随 5.4 实现（理由见 PR #151） |

### 2.3 判定原则（阶段完成后写入 CONTRIBUTING §3）

- 存在对所有场景都正确的保守值（最多慢一些），就由框架给默认值。
- 运行时另有硬上限兜底时，也可以给默认值。
- 语义契约（端口、类型、数量关系、执行协议、外部协议 ID）不给默认值，靠清晰的报错引导。
- 默认值写在 Definition 或注册表上，Catalog 和工具可见；工具不得把未修改的默认值写成显式配置。
- 覆盖途径：组件能力上限写在代码里；按部署调整写在 Pipeline JSON 的现有字段里；`.conf` 由平台定义，不加字段。

## 3. 每个步骤的通用流程

1. 从最新 `main` 建分支。需要设计审查的步骤，先在 PR 描述中写清问题、方案、受影响的契约、取舍、验收标准和回退方式。
2. 构建并采集基线（脚本见附录 A，保存到已被 Git 忽略的 `build/param-baseline/`）：

   ```bash
   cmake --build build -j 4
   build/param-baseline/capture_baseline.sh before
   ```

3. 先写本步骤"测试"中锁定现有行为的用例，确认它们在未改动的代码上通过；标明"改动前失败"的用例除外。
4. 实施改动，运行本步骤的聚焦测试。
5. 更新本步骤列出的文档；用户可感知的变化写进 `doc/CHANGELOG.md` 的 Unreleased。
6. 运行门禁 `./scripts/run_all_tests.sh`。
7. 采集改动后的结果并对比：

   ```bash
   build/param-baseline/capture_baseline.sh after-5.x
   python3 build/param-baseline/compare_baseline.py \
     build/param-baseline/before build/param-baseline/after-5.x <本步骤的对比参数>
   ```

8. 逐项勾选验收清单，把命令和结果记录在 PR 中。

## 4. 步骤 5.1：Studio 保留"未配置"状态（已完成）

**状态**：已随 PR #150 合入 `main`（`b24be92`），验收通过；下文保留实施时的步骤与验收清单，仅供参考。

**问题**：`appendConfigField`（`tools/pipeline_studio/web/editor.js:92`）对未配置的字段直接把默认值填进控件（`:112`），
`readConfigFields`（`:142`）应用时只对字符串字段判断"是否修改"，数值、布尔、枚举、数组和对象字段都会被写成显式值。
Node、Model、Backend 三处表单共用这两个函数。用附录 B 的测试检查，仓库内 26 份配置中有 129 个表单在不修改直接应用后发生变化。

### 改动

1. `editor.js` 的 `appendConfigField`：
   - 计算 `hasDefault = field.default !== undefined && field.default !== null`。
   - 非必填、且不是模型引用（`modelChoices === null`）的下拉框（枚举、布尔），在最前面插入一个值为 `""` 的选项：
     有默认值时文字为 `默认（<默认值>）`，没有时为 `未设置`；并设置 `input.dataset.unsetOption = "true"`。
   - 字段未出现在配置中时，按下表设置控件；字段已出现时行为不变。

     | 字段 | 控件初值 | 提示 |
     | --- | --- | --- |
     | 非必填枚举、布尔 | `""`（即上面的选项） | 选项文字 |
     | 数值 | `""` | `placeholder` 为 `默认 <值>`，无默认值时为空 |
     | 数组、对象 | `""` | `placeholder` 为默认值的 JSON |
     | 字符串 | 不变：显示默认文本，未修改时省略，清空表示显式空字符串 | — |
     | 必填枚举、必填布尔、模型引用 | 不变 | — |

2. `editor.js` 的 `readConfigFields`：跳过 `dataset.unsetOption === "true"` 且值为 `""` 的下拉框。
   数值、数组、对象为空且非必填时，现有条件已经会跳过，不用改。
3. `editor.js` 的 `parseField`：数值类型遇到空字符串时抛出 `请输入数值`。现在 `Number("")` 得到 `0`，必填项只靠浏览器表单校验拦截。
4. `tools/pipeline_studio/web/index.html`：把

   ```html
   <p class="field-heading">Backend 参数</p><div id="backendConfigFields"></div>
   ```

   换成

   ```html
   <details id="backendAdvanced"><summary>部署高级设置（Backend 参数）</summary><div id="backendConfigFields"></div></details>
   ```

   `bufferKey` 仍按 `#backendConfigFields` 识别作用域，不受影响。
5. `tools/pipeline_studio/web/app.js` 的 `renderBackendFields`（`:933`）：渲染后执行
   `$("#backendAdvanced").open = Object.keys(values).length > 0;`，已有显式 Backend 字段时自动展开。
6. `readFormBuffer` / `restoreFormBuffer` 不用改：未配置的值是空字符串，草稿恢复后仍是未配置。

### 测试

1. `tests/tooling/studio_editor_test.mjs`，在现有字符串用例之后追加（复用文件中的 `renderFields`、`field`）：

   ```js
   const tuningFields = [
     { name: "top_p", type: "number", default: 0.9 },
     { name: "ratio", type: "number" },
     { name: "normalize", type: "boolean", default: true },
     { name: "policy", type: "string", enum: ["fail", "truncate"], default: "fail" },
     { name: "stop_words", type: "array", default: [] },
   ];
   for (const formId of ["configFields", "backendConfigFields"]) {
     const unset = renderFields({}, tuningFields, formId);
     assert.deepEqual(readConfigFields(unset), {}, "untouched defaults must stay unset");
     const repainted = renderFields({}, tuningFields, formId);
     restoreFormBuffer(repainted, readFormBuffer(unset));
     assert.deepEqual(readConfigFields(repainted), {}, "draft repaint must keep fields unset");
     const explicit = { top_p: 0.9, normalize: true, policy: "fail", stop_words: [] };
     const pinned = renderFields(explicit, tuningFields, formId);
     assert.deepEqual(readConfigFields(pinned), explicit, "explicit values equal to defaults stay explicit");
     for (const name of Object.keys(explicit)) field(pinned, name).value = "";
     assert.deepEqual(readConfigFields(pinned), {}, "clearing an override restores the default");
     field(unset, "top_p").value = "0.5";
     field(unset, "normalize").value = "false";
     field(unset, "policy").value = "truncate";
     assert.deepEqual(readConfigFields(unset), { top_p: 0.5, normalize: false, policy: "truncate" });
   }
   assert.throws(() => readConfigFields(renderFields({}, [{ name: "dim", type: "integer", required: true }])), /dim/);
   ```

2. 新增 `tests/tooling/studio_config_roundtrip_test.mjs`（内容见附录 B）：逐个渲染配置中每个 Node、Model、Backend 表单，
   不修改直接读回，断言与原值完全一致。
3. `tests/tooling/test_pipeline_studio.py`：在 `test_graph_navigation_routes_and_editor_history` 所在的类中新增：

   ```python
   @unittest.skipUnless(shutil.which("node"), "Node.js is required for Web module tests")
   def test_untouched_apply_preserves_repository_configs(self):
       code, catalog = PipelineCliTest.command(self, "catalog")
       self.assertEqual(code, 0, catalog)
       pipelines = [str(path) for pattern in ("configs/pipeline_*.json", "demo/fixtures/mock/pipeline_*.json")
                    for path in sorted(ROOT.glob(pattern))]
       with tempfile.TemporaryDirectory(prefix="studio-roundtrip-", dir=ROOT / "build") as directory:
           catalog_path = Path(directory) / "catalog.json"
           catalog_path.write_text(json.dumps(catalog), encoding="utf-8")
           process = subprocess.run(
               [shutil.which("node"), str(Path(__file__).with_name("studio_config_roundtrip_test.mjs")),
                str(catalog_path), *pipelines],
               text=True, capture_output=True, cwd=ROOT, check=False)
       self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
   ```

   这个用例在改动前应失败（报告 129 个表单变化），改动后通过。`PipelineCliTest.command` 使用
   `build/alg_pipeline_tool_test`，它的 Catalog 同时包含生产类型和测试类型，能覆盖两组配置。

### 文档与 CHANGELOG

- `tools/pipeline_studio/README.md`：编辑器只保存修改过的字段；未配置的字段只显示默认值提示；Backend 参数位于默认折叠的"部署高级设置"。
- CHANGELOG：Studio 应用表单时不再把未修改的默认值写入配置；已有显式值原样保留，清空或选回"默认"即恢复为未配置；Backend 参数收进"部署高级设置"。

### 验收

- [ ] `test_untouched_apply_preserves_repository_configs` 改动前失败、改动后通过。
- [ ] `node tests/tooling/studio_editor_test.mjs` 通过；`ctest --test-dir build -R PipelineStudioServerTest --output-on-failure` 通过。
- [ ] 手工检查：`./show --web` 打开 `configs/pipeline_doc_qa_cpu.json`，选中 `LlmGenerateNode` 不修改直接应用，再选中一个模型不修改应用，JSON 视图中的配置不变；该模型有显式 Backend 字段，"部署高级设置"为展开状态。
- [ ] 基线对比（无额外参数）0 个差异。
- [ ] 2.1 的外部契约检查通过；门禁通过。

**回退**：还原本步骤的提交。

## 5. 步骤 5.2：接入层平台数值归位（已完成）

**状态**：已随 PR #150 合入 `main`（`b24be92`），验收通过；下文保留实施时的步骤与验收清单，仅供参考。

三项改动分别提交，可以单独回退。

### 5.2.1 Binding 默认批次上限

**改动**

1. `include/adapter/io_binding.h`：

   ```cpp
   // Standard Operator batch bound applied when a binding does not override it.
   inline constexpr size_t kDefaultIoBindingMaxBatchSize = 64;

   struct IoBindingDefinition {
     // ...
     // Defaults to kDefaultIoBindingMaxBatchSize. Zero adds no bound; the
     // effective limit is the smallest positive value among the binding and its
     // converters, and at least one must be positive.
     size_t max_batch_size = kDefaultIoBindingMaxBatchSize;
   };
   ```

2. `src/adapter/biz/` 下 8 个文件删除 `def.max_batch_size = 64;`。
3. `EffectiveMaxBatchSize`、注册审计和部署准备都不改。"declares no batch limit"只在 Binding 显式写 0、且两个转换器也为 0 时出现。
4. 可选清理：以下测试中显式写的 `max_batch_size = 64` 可以删除：`tests/unit/adapter/test_text_converters.cpp:201`、
   `tests/integration/operator/test_operator_api.cpp:2389`、`tests/unit/adapter/test_adapter_purity.cpp:1073`、
   `tests/integration/pipeline/test_pipeline_catalog_validator.cpp:350`。

**测试**：`IoBindingRegistryTest` 新增 `BindingWithoutExplicitLimitUsesStandardDefault`：两个转换器的上限为 0、Binding 不赋值，
审计通过，`PrepareDeploymentDocument` 得到的 `effective_max_batch_size` 为 64。
现有的 `AuditAndPreparationRejectBindingWithoutBatchLimit`（显式写 0）和 `ComplexConvertersTest.AllEightBusinessesRegistered` 保持不变并通过。

**文档**：把"在 Binding 上声明批次上限"改为"批次上限默认为框架标准值 64，只有实测确需更小值时才在 Binding 上覆盖"：

- `doc/dev_guide/business_onboarding.md:88`、`:106-107`、`:125-133`
- `doc/developer_guide.md:84`
- `.agents/skills/edgeflow-adapter-developer/SKILL.md:25`
- `.agents/skills/llm-edgeflow-developer-guide/references/integration.md:31`
- `src/adapter/biz/README.md:7`

### 5.2.2 输入长度上限单一来源

**改动**

1. `include/adapter/biz_input_constraints.h` 新增：

   ```cpp
   inline constexpr size_t kMaxTextBytes = 64 * 1024;
   inline constexpr size_t kMaxDocTextBytes = 10 * 1024 * 1024;
   inline constexpr size_t kMaxImageUriBytes = 4096;
   ```

2. `include/adapter/operator_value_type.h` 的 `ResolvedInputLimits`：`max_text_bytes`、`max_doc_text_bytes`、`max_image_uri_bytes`
   的默认值改为引用上述常量；删除没有读取方的 `max_rerank_candidates`。
3. 输入转换器改为引用共享常量（需要时补 `#include "adapter/biz_input_constraints.h"`）。检查顺序、诊断文本和返回码都不变。

   | 文件 | 现有常量 | 改为 |
   | --- | --- | --- |
   | `src/adapter/input/text_input.cpp` | `kMaxSentenceLen` | `biz_input::kMaxTextBytes` |
   | `src/adapter/input/translate_json_input.cpp` | `kMaxSentenceLen` | `biz_input::kMaxTextBytes` |
   | `src/adapter/input/audit_input.cpp` | `kMaxTextLen` | `biz_input::kMaxTextBytes` |
   | `src/adapter/input/doc_query_input.cpp` | `kMaxQueryLen`、`kMaxDocLen` | `biz_input::kMaxTextBytes`、`biz_input::kMaxDocTextBytes` |
   | `src/adapter/input/image_query_input.cpp` | `kMaxPathLen`、`kMaxQueryLen` | `biz_input::kMaxImageUriBytes`、`biz_input::kMaxTextBytes` |
   | `src/adapter/input/rerank_input.cpp` | 查询的 `kMaxTextLen` | `biz_input::kMaxTextBytes` |
   | `src/adapter/input/rerank_input.cpp` | 候选段落的 `kMaxTextLen` | 具名业务限制，见下 |
   | `src/adapter/input/rerank_input.cpp` | 候选数量字面量 `8`（`:53`） | `COMPANY_OPERATOR_MAX_RERANK_CANDIDATES` |

   rerank 候选段落保持现有行为（D3）：

   ```cpp
   // Converter-level business limit. The Operator layer admits passages up to
   // max_doc_text_bytes; this stricter bound is the effective limit today and
   // its value awaits the solution owner's confirmation.
   constexpr size_t kMaxCandidatePassageBytes = biz_input::kMaxTextBytes;
   ```

   候选数量的诊断用常量拼出，文本仍为 `candidate_count out of valid range [1, 8]`。

**测试**（先写，在改动前通过）：在 `AdapterContractSecurityTest`（`tests/contract/abi/test_adapter_contract_security.cpp`）中新增：

- `InputLengthLimitsStayUnchanged`：仿照同文件的 `TranslationCrossSampleCarrierVsBizErrorPriority` 直接调用转换器的 `decode_fn`。
  对下表每一项，长度等于上限时解码成功；超出 1 时返回 `COMPANY_ALG_ERR_INVALID_INPUT`，`AdapterStatus::ToString()` 与基线一致
  （在未改动的代码上运行一次，把实际文本写进断言）。

  | 转换器 | 字段 | 上限 |
  | --- | --- | --- |
  | `keyword.plain.operator.v1`、`text.plain.operator.v1` | `sentence_text` | 64 KiB |
  | `translate.json.operator.v1` | `sentence_text`（完整 JSON 字符串） | 64 KiB |
  | `audit.plain.operator.v1` | `user_text` | 64 KiB |
  | `doc_query` 输入转换器 | `query_text`、`doc_text` | 64 KiB、10 MiB |
  | `image_query.plain.operator.v1` | `frame.image_uri`、`query` | 4096 字节、64 KiB |
  | `rerank.plain.operator.v1` | `query_text`、候选段落、`candidate_count` | 64 KiB、64 KiB、8 |

- `OperatorInputLimitsStayUnchanged`：经 Operator `Process` 检查 3 条路径，返回码和 `GetOperatorLastError()` 都与基线一致：
  keyword 文本 64 KiB / 64 KiB+1；rerank 候选段落 64 KiB+1（Operator 层放行、转换器拒绝）和 10 MiB+1（Operator 层拒绝）；
  图片路径 4096 / 4097 字节。

**文档**：`src/adapter/input/README.md` 说明长度上限引用 `adapter/biz_input_constraints.h`，只有业务确需更严格的限制时，才在转换器中定义具名常量。

### 5.2.3 输出容量字段从 ValueType 推导

**改动**

1. `include/adapter/io_binding_registry.h`（与 `EffectivePortMapping` 并列）声明，实现放在 `src/adapter/io_binding_registry.cpp`：

   ```cpp
   // Output slots that list no capacity_fields inherit the string capacity
   // fields of their registered ValueType, in lexicographic order. Input slots
   // and unknown ValueTypes return the declared list unchanged.
   std::vector<std::string> EffectiveCapacityFields(const ExternalSlotDefinition& slot);
   ```

   通过 `OperatorValueTypeRegistry::Instance().GetOutputBinding(slot.type_suffix, "")` 读取
   `output_layout.string_capacity_fields`。内置 ValueType 在注册表构造时登记，Catalog 和审计时总是可用。
2. 改用 `EffectiveCapacityFields` 的位置：`src/adapter/io_catalog.cpp` 的 `SlotJson`，`src/adapter/io_binding_registry.cpp` 的 `SameExternalSlots`，
   以及测试 `tests/integration/pipeline/test_pipeline_catalog_validator.cpp:271`。
3. 注册审计（`IoBindingRegistry::Audit` 第 4 部分的输出槽循环，`:371` 附近）：输出槽显式列出了 `capacity_fields`，
   但集合与 ValueType 不一致时报错：`Binding '<id>' output slot '<slot>' capacity_fields do not match ValueType '<suffix>'`。
4. `include/adapter/converter_authoring.h` 的 `ExternalOutputSlot<T>` 保留第二个参数，注释说明"省略时由 ValueType 推导，显式列出时必须与 ValueType 一致"。
5. 删除以下 7 个输出转换器中显式列出的容量字段：`audio_result_output.cpp`、`audit_result_output.cpp`、`doc_answer_output.cpp`、
   `invoice_result_output.cpp`、`keyword_result_output.cpp`、`structured_document_output.cpp`、`translation_json_output.cpp`
   （均在 `src/adapter/output/`；`rerank_result_output.cpp` 本来就没有列）。基线时这 8 个转换器的列表与 ValueType 逐一相同。

**测试**：`IoBindingRegistryTest` 新增 `CapacityFieldsDeriveFromValueType`（keyword 输出转换器不列字段时，Catalog 中仍是 `["match_result_json"]`），
以及 `AuditRejectsCapacityFieldsMismatch`（显式列出 ValueType 没有的字段，审计报错）。`CatalogContractSsotTest` 现有断言不变并通过。

**文档**：`doc/dev_guide/business_onboarding.md:151` 中 `ExternalOutputSlot<T>(slot, capacity_fields)` 改为 `ExternalOutputSlot<T>(slot)`，
说明容量字段由已注册的 ValueType 决定；`doc/dev_guide/source_layout.md:67` 同步检查。

### 5.2 的 CHANGELOG

Binding 默认使用框架标准批次上限 64，业务只在有实测依据时覆盖；输入长度上限集中在 `biz_input_constraints.h`，接受/拒绝边界不变；
输出转换器的容量字段由 ValueType 推导，显式列出时必须与 ValueType 一致。Catalog 中三个输出转换器的 `capacity_fields` 改为字典序。

### 5.2 验收

- [ ] `compare_baseline.py ... --sort-list capacity_fields` 0 个差异；不加该参数时，差异只在 `catalog.json`、`catalog-test.json` 中上述三个转换器的 `capacity_fields` 顺序。
- [ ] 以下检查都没有输出：

  ```bash
  grep -rn "max_batch_size = 64" src/adapter/biz
  grep -rnE "64 \* 1024|10 \* 1024 \* 1024|4096|> 8\)" src/adapter/input
  grep -rnE "ExternalOutputSlot<[^>]+>\([^)]*\{" src/adapter/output
  ```

- [ ] `InputLengthLimitsStayUnchanged`、`OperatorInputLimitsStayUnchanged` 在改动前后都通过。
- [ ] 聚焦测试通过：

  ```bash
  ctest --test-dir build --output-on-failure -R \
    "IoBindingRegistryTest|IoConverterTest|TextConvertersTest|ComplexConvertersTest|AdapterContractSecurityTest|OperatorSafetyTest|OperatorApiTest|AdapterPurityTest|CatalogContractSsotTest|PipelineStudioTest"
  ```

- [ ] 2.1 的外部契约检查通过；门禁通过。

**回退**：三项改动分别还原对应提交。

## 6. 步骤 5.3：单次有效批次可查询与清理（已完成）

**状态**：已随 PR #150 合入 `main`（`b24be92`），验收通过；下文保留实施时的步骤与验收清单，仅供参考。

**问题**：单次有效批次是 min(池深, Binding 有效上限)。`validate-io` 只给出 Binding 上限，`resolve-conf` 只给出池规格，
没有一处给出合成值；池深规范化在 Create（`operator_adapter.cpp:170-177`，字面量 25）和 `OperatorConfigResolver::Resolve`
（`operator_config_resolver.cpp:276-283`）各写一遍；`RuntimeOptions::platform_max_batch` 只赋值、没有读取方。

### 改动

1. `src/adapter/operator/operator_config_resolver.h` 的 `ResolvedOperatorConfig` 新增：

   ```cpp
   uint32_t effective_frame_depth = 0;          // normalized output pool depth
   uint32_t effective_process_batch_limit = 0;  // min(pool depth, binding limit)
   ```

2. `Resolve` 在接入计划解析成功后（`:331` 之后）填写这两个值：池深沿用已规范化的 `effective_depth`，
   批次取 `min(effective_depth, io_plan->effective_max_batch_size)`。
3. `src/adapter/operator/operator_adapter.cpp` 的 Create：
   - 池深预检保留原有顺序、诊断和返回码，字面量 `25` 改为 `kDefaultOutputPoolDepth`；
   - `effective_batch_limit` 直接取 `resolved_conf.effective_process_batch_limit`，删除本地的 `std::min` 计算；
   - 删除 `runtime_options.platform_max_batch = ...`。
4. `include/core/session_context.h`：删除 `RuntimeOptions::platform_max_batch`。
5. `src/tools/alg_pipeline_tool.cpp` 的 `ResolveConf`：`configuration` 新增 `effective_frame_depth`、`effective_process_batch_limit`，
   以及 `max_frame_depth_limit`（`kMaxOutputPoolDepth`）。只增字段。
6. `demo/common/operator_runner.h`：Demo 仍只通过 SDK 运行，Profile 的取值检查不变。
   - Process 失败且诊断包含 `exceeds effective batch limit` 时，在原报错后增加一行：
     `[OperatorRunner HINT] 单次批次超过有效上限；用 alg_pipeline_tool resolve-conf <conf> --root <root> --depth <depth> 查看 effective_process_batch_limit`。
   - Create 失败且诊断包含 `max_frame_depth` 时，提示同一命令和 `max_frame_depth_limit`。
7. 运行附录 A.3 的脚本，把输出交方案负责人（D4）；本步骤不修改任何配置。基线时的结果是：`configs/` 下 161 个字段等于注册默认值
   （另有 9 个字段属于当前构建未编入的类型，无法核对），`demo/fixtures/mock/` 下 29 个。

### 测试

- `tests/tooling/test_pipeline_studio.py` 的 `test_resolve_conf_exposes_model_sources_defaults_and_native_pool_errors`：
  补充断言 `--depth 1` 时 `effective_process_batch_limit` 为 1；另取 `--depth 0`、`100`、`1025` 三次，分别得到 25、64 和原有的超限错误。
- `OperatorApiTest` 新增 `ProcessUsesResolvedEffectiveBatchLimit`：`max_frame_depth = 100` 创建 keyword 句柄，64 条输入成功，
  65 条返回 `-3`，诊断为 `Input batch size 65 exceeds effective batch limit 64`。

### 文档与 CHANGELOG

- `doc/dev_guide/business_onboarding.md` 第 6 节：说明可用 `alg_pipeline_tool resolve-conf <conf> --root <root> --depth <depth>` 查看 `effective_process_batch_limit`。
- `tools/pipeline_studio/README.md` 中描述 `resolve-conf` 输出的段落同步补充。
- CHANGELOG：`resolve-conf` 输出单次有效批次、规范化池深和池深硬上限。

### 验收

- [ ] `compare_baseline.py ... --ignore-key effective_frame_depth --ignore-key effective_process_batch_limit --ignore-key max_frame_depth_limit` 0 个差异。
- [ ] 对 `configs/pipeline_keyword_match_rules.conf`，`--depth` 取 0、1、25、64、100 时，`effective_frame_depth` 依次为 25、1、25、64、100，
  `effective_process_batch_limit` 依次为 25、1、25、64、64；取 1025 时的输出与基线一致。
- [ ] `grep -rn platform_max_batch src include tests demo` 无输出。
- [ ] Demo 提示：生成 70 行数据集和一个 `batch_size`、`depth` 都为 70 的临时 Profile（`schema_version` 为 2）。运行后退出码仍为 5，
  报错仍是 `Input batch size 70 exceeds effective batch limit 64`，其后出现 HINT 行：

  ```bash
  for i in $(seq 70); do echo "初始化系统"; done > build/param-baseline/kw70.txt
  cat > build/param-baseline/profiles_kw70.json <<'EOF'
  {"schema_version": 2, "profiles": {"kw_batch70": {
    "config": "configs/pipeline_keyword_match_rules.conf",
    "dataset": "build/param-baseline/kw70.txt",
    "batch_size": 70, "depth": 70, "chip": "cpu", "device_id": 0}}}
  EOF
  ./build/alg_demo --profiles-file build/param-baseline/profiles_kw70.json \
    --profile kw_batch70 --output-dir build/param-baseline/kw70-out
  ```

- [ ] 附录 A.3 的清单已交方案负责人。
- [ ] 2.1 的外部契约检查通过；门禁通过。

**回退**：还原本步骤的提交。

## 7. 步骤 5.4：请求编号回传移出业务契约（已完成）

**状态**：已随 PR #151 合入 `main`（`a7d2c29`），验收通过。设计审查（问题、方案、受影响的契约、取舍、风险、回退）、
D5 的理由和验证记录见 PR #151 的描述；本节只记录实现后的现行行为，不再是实施指引。

**现行行为**

- `Process` 持有本次调用的请求编号表，通过 `InputDecodeOptions::request_ids` / `OutputEncodeOptions::request_ids` 交给转换器。
  请求编号不再是业务端口：Catalog 的 `bizs[].ingress`、转换器 `logical_ports`、Binding 端口映射、`validate-io` 和 Studio 的 `$ingress`
  中都没有 `raw_request_ids`。
- 单槽转换器使用 `DecodeRequestRows` / `EncodeResultRows`，由框架记录和恢复编号；自行组织多槽解码或多路结果的转换器，
  在全部行校验通过后调用 `PublishRequestIds`，编码时用 `RequestIds` 读取。
- 解码成功但记录的编号数与输入数不一致时，`Process` 在租用输出块之前返回 `-3`（D5）。宿主重复的 `request_id` 仍被接受。
- `CatalogContractSsotTest.CatalogHasNoRequestIdPort` 防止请求编号重新进入业务契约。

现行用法见 `doc/dev_guide/business_onboarding.md`，迁移说明见 `doc/CHANGELOG.md`。

## 8. 步骤 5.5：并行层按约束自动串行

**前提**：D1 已确认。

**问题**：`max_parallel_workers > 1` 时，Validator 拒绝含非 `parallel_safe` 节点的并行层，也拒绝共享串行模型的并行层
（`src/core/pipeline_validator.cpp:1490-1525`）。运行时对串行模型没有加锁，这个拒绝是唯一保护。
执行器对单节点层已在主线程顺序执行（`src/core/pipeline.cpp:419-432`），因此只改 Validator 就能做到"按约束安全执行"。

### 改动

1. `src/core/pipeline_validator.cpp` 中 `if (parsed.max_parallel_workers > 1)` 的代码块：
   - `kParallelWriteConflict` 检查保持不变，仍按原始层进行。
   - 不再产生 `kNodeNotParallelSafe` 和 `kSerializedModelConcurrency`，改为拆分层。新增匿名命名空间函数，按下面的规则处理每个多节点层：

     ```text
     parallel = []，serial = []，used = {}            # used：已进入并行组的串行模型
     按原顺序遍历层内节点 id：
       节点不是 parallel_safe                 → 加入 serial
       models = 该节点绑定的串行模型集合       # 与原检查相同：model_bindings 中
                                               # concurrency 为 kSerialized 的 model_id，去重
       models 与 used 有交集                 → 加入 serial
       否则                                   → 加入 parallel，used ∪= models
     返回：parallel（非空时）作为一层，随后 serial 中每个节点各自一层
     ```

   - 循环结束后，用拆分结果替换 `report.topological_layers`；`topological_order` 不变。单节点层和 `max_parallel_workers == 1` 的计划都不变。
2. `include/core/diagnostic_code.h`：保留两个诊断码及其名称，注释说明 Validator 不再产生它们。
3. 执行器不改：`Pipeline` 仍只消费 `ValidatedPipelinePlan`，拆出的单节点层在主线程执行。

### 测试

- `tests/unit/core/test_validated_pipeline_plan.cpp` 中断言这两个诊断码的 3 个用例（按 `kSerializedModelConcurrency`、`kNodeNotParallelSafe` 搜索，
  约在 `:480-515`、`:745-760`、`:990-1003`）：改为断言 `report.ok` 为真，并断言拆分后的层。
  例如两个节点共享同一个串行模型时，得到 `[["node_a"], ["node_b"]]`；不安全的节点单独成层。
- `tests/fixtures/pipelines/validation/invalid_pipeline_cases.json`：删除 `serialized_model_concurrency` 用例，
  改写为 `PipelineValidatorTest` 中的正向用例，断言层为 `[["pre"], ["left"], ["right"], ["post"]]`，且 Operator Create 成功。
- `DagPipelineTest` 新增：`max_parallel_workers = 4`，同一层有一个非 `parallel_safe` 节点和一个安全节点，`Execute` 成功，输出与 `max_parallel_workers = 1` 一致。

### 文档与 CHANGELOG

- `doc/dev_guide/custom_node_concepts.md:188-190`、`src/custom_nodes/README.md:37`、`.agents/skills/edgeflow-node-batch-developer/SKILL.md:59`、
  `.agents/skills/llm-edgeflow-developer-guide/references/capability-nodes.md:49`、`doc/architecture.md:184`、
  `.agents/skills/edgeflow-solution-planner/SKILL.md:61`：改为"不声明并发也能安全运行；框架把不安全的节点和共享串行模型的节点放到单独的层顺序执行；声明 `.ParallelSafe(true)` 后才可能并行"。
- CHANGELOG：`max_parallel_workers > 1` 时，Validator 不再拒绝非 `parallel_safe` 节点或共享串行模型的并行层，改为把它们拆到单独的层顺序执行；原本 Create 返回 `-2` 的这类配置现在可以运行。

### 验收

- [ ] 基线对比（无额外参数）0 个差异：仓库内配置都是 `max_parallel_workers = 1`，`plan` 输出不变。
- [ ] 聚焦测试通过：`ctest --test-dir build --output-on-failure -R "ValidatedPipelinePlanTest|DagPipelineTest|PipelineStudioTest|PipelineStudioServerTest"`。
- [ ] 2.1 的外部契约检查通过（只有 D1 确认的这一项行为变化）；门禁通过。

**回退**：还原本步骤的提交。

## 9. 阶段完成

- [x] 5.1–5.4 全部验收（PR #150、#151）。
- [ ] D1 确认后 5.5 已验收；未确认时在 PR 中注明未实施。
- [ ] 2.3 的判定原则写入 `CONTRIBUTING.md` §3。
- [ ] `FRAMEWORK_SIMPLIFICATION_PLAN.md` 标记阶段 5 完成。
- [ ] 删除本文件，并清理指向本文件的引用。

## 附录 A：基线与报告脚本

把以下三个文件保存到 `build/param-baseline/`（`build/` 已被 Git 忽略）并加上执行权限，从仓库根目录运行。
三个脚本都已在基线上运行过：连续采集两次，对比结果为 0 个差异。

### A.1 `capture_baseline.sh`

采集 Catalog、Pipeline Schema、26 份配置的 `validate-io`、`resolve-conf`（7 种池深）和 `plan`，以及 smoke 结果。
`configs/` 用 `alg_pipeline_tool`，`demo/fixtures/mock/` 用 `alg_pipeline_tool_test`。
kite、whisper 配置在默认构建中会失败，对比时只要求失败结果一致。

```bash
#!/usr/bin/env bash
# 用法：capture_baseline.sh <标签>      例如 before、after-5.2
# 从仓库根目录运行；结果写入 ${BASELINE_ROOT:-build/param-baseline}/<标签>/
set -uo pipefail
label="${1:?需要标签，例如 before}"
out="${BASELINE_ROOT:-build/param-baseline}/${label}"
rm -rf "${out}" && mkdir -p "${out}/validate-io" "${out}/resolve-conf" "${out}/plan"
tool=./build/alg_pipeline_tool
test_tool=./build/alg_pipeline_tool_test
"${tool}" catalog > "${out}/catalog.json" 2>&1
"${test_tool}" catalog > "${out}/catalog-test.json" 2>&1
"${tool}" export-schema > "${out}/pipeline-schema.json" 2>&1
capture_conf() {  # <工具> <conf> <输出名>
  local runner="$1" conf="$2" name="$3" depth
  "${runner}" validate-io "${conf}" --model-root . > "${out}/validate-io/${name}.json" 2>&1
  for depth in 0 1 25 64 100 1024 1025; do
    "${runner}" resolve-conf "${conf}" --root . --depth "${depth}" \
      > "${out}/resolve-conf/${name}.depth${depth}.json" 2>&1
  done
}
for conf in configs/*.conf; do capture_conf "${tool}" "${conf}" "$(basename "${conf}" .conf)"; done
for pipe in configs/*.json; do "${tool}" plan "${pipe}" > "${out}/plan/$(basename "${pipe}" .json).json" 2>&1; done
for pipe in demo/fixtures/mock/*.json; do "${test_tool}" plan "${pipe}" > "${out}/plan/mock_$(basename "${pipe}" .json).json" 2>&1; done
for conf in demo/fixtures/mock/*.conf; do capture_conf "${test_tool}" "${conf}" "mock_$(basename "${conf}" .conf)"; done
./build/alg_demo --suite smoke --output-dir "${out}/smoke" > "${out}/smoke.log" 2>&1
echo "smoke exit=$?" >> "${out}/smoke.log"
echo "baseline written to ${out}"
```

### A.2 `compare_baseline.py`

逐文件比较两次采集结果。默认忽略 smoke 的耗时字段，日志只比较 smoke 退出码。
`--ignore-key` 删除指定字段，`--drop-port` 删除指定端口条目和同名映射，`--sort-list` 把指定列表按集合比较。

```python
#!/usr/bin/env python3
"""比较两次 capture_baseline.sh 的结果。

用法：python3 compare_baseline.py <基线目录> <改动后目录>
          [--ignore-key KEY ...] [--drop-port NAME ...] [--sort-list KEY ...]
默认忽略 smoke 结果中的耗时字段；日志文件只比较末尾的 smoke 退出码。
"""
import argparse
import json
import pathlib
import sys

LATENCY_KEYS = {"latency_ms", "avg_latency_ms", "total_latency_ms"}


def normalize(value, ignore, drop_ports, sort_lists):
    if isinstance(value, dict):
        result = {}
        for key, item in value.items():
            if key in ignore or key in drop_ports:
                continue
            item = normalize(item, ignore, drop_ports, sort_lists)
            if key in sort_lists and isinstance(item, list):
                item = sorted(item, key=lambda x: json.dumps(x, sort_keys=True))
            result[key] = item
        return result
    if isinstance(value, list):
        return [normalize(item, ignore, drop_ports, sort_lists) for item in value
                if not (isinstance(item, dict) and item.get("key") in drop_ports)
                and not (isinstance(item, str) and item in drop_ports)]
    return value


def load(path, options):
    text = path.read_text(encoding="utf-8", errors="replace")
    if path.suffix == ".log":
        return [line for line in text.splitlines() if line.startswith("smoke exit=")]
    try:
        if path.suffix == ".jsonl":
            data = [json.loads(line) for line in text.splitlines() if line.strip()]
        else:
            data = json.loads(text)
    except json.JSONDecodeError:
        return text
    return normalize(data, LATENCY_KEYS | set(options.ignore_key),
                     set(options.drop_port), set(options.sort_list))


def first_difference(a, b, path=""):
    if type(a) is not type(b):
        return path or "/"
    if isinstance(a, dict):
        for key in sorted(set(a) | set(b)):
            if key not in a or key not in b:
                return f"{path}/{key}"
            found = first_difference(a[key], b[key], f"{path}/{key}")
            if found:
                return found
        return None
    if isinstance(a, list):
        if len(a) != len(b):
            return f"{path} (length {len(a)} != {len(b)})"
        for index, (x, y) in enumerate(zip(a, b)):
            found = first_difference(x, y, f"{path}/{index}")
            if found:
                return found
        return None
    return None if a == b else (path or "/")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("before")
    parser.add_argument("after")
    parser.add_argument("--ignore-key", action="append", default=[])
    parser.add_argument("--drop-port", action="append", default=[])
    parser.add_argument("--sort-list", action="append", default=[])
    options = parser.parse_args()
    before, after = pathlib.Path(options.before), pathlib.Path(options.after)
    files = sorted({p.relative_to(before) for p in before.rglob("*") if p.is_file()} |
                   {p.relative_to(after) for p in after.rglob("*") if p.is_file()})
    failures = 0
    for relative in files:
        a, b = before / relative, after / relative
        if not a.exists() or not b.exists():
            print(f"MISSING {relative} ({'before' if not a.exists() else 'after'})")
            failures += 1
            continue
        left, right = load(a, options), load(b, options)
        if left != right:
            print(f"DIFF {relative}: {first_difference(left, right)}")
            failures += 1
    print(f"{len(files)} files compared, {failures} difference(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
```

### A.3 `report_explicit_defaults.py`

只读，列出配置中"显式写出、且等于注册默认值"的字段，输出 Markdown 表格交方案负责人（5.3、D4）：

```bash
./build/alg_pipeline_tool_test catalog > build/param-baseline/catalog-test.json
python3 build/param-baseline/report_explicit_defaults.py build/param-baseline/catalog-test.json \
  configs/pipeline_*.json demo/fixtures/mock/pipeline_*.json > build/param-baseline/explicit-defaults.md
```

```python
#!/usr/bin/env python3
"""列出 Pipeline 配置中"显式写出且等于注册默认值"的字段，供方案负责人逐项确认。

用法：python3 report_explicit_defaults.py <catalog.json> <pipeline.json>...
只读，不修改任何配置；当前构建未编入的 Node/Model/Backend 标记为"无法核对"。
"""
import json
import sys


def defaults(catalog, kind, key):
    items = catalog.get(kind, [])
    items = items if isinstance(items, list) else list(items.values())
    return {item[key]: {field["name"]: field.get("default")
                        for field in item.get("config_fields", [])
                        if field.get("default") is not None}
            for item in items}


def main():
    catalog = json.load(open(sys.argv[1], encoding="utf-8"))
    nodes = defaults(catalog, "nodes", "node_type")
    models = defaults(catalog, "models", "model_type")
    backends = defaults(catalog, "backends", "backend_type")
    print("| 配置 | 位置 | 字段 | 值 |")
    print("| --- | --- | --- | --- |")
    total = unknown = 0
    for path in sys.argv[2:]:
        doc = json.load(open(path, encoding="utf-8"))
        sections = [(f"pipeline[{n['id']}].config", nodes.get(n["node_type"]), n.get("config", {}))
                    for n in doc.get("pipeline", [])]
        for m in doc.get("models", []):
            sections.append((f"models[{m['model_id']}].model_config", models.get(m["model_type"]), m.get("model_config", {})))
            sections.append((f"models[{m['model_id']}].backend_config", backends.get(m["backend"]), m.get("backend_config", {})))
        for where, known, values in sections:
            if known is None:
                unknown += len(values)
                continue
            for field, value in values.items():
                if field in known and known[field] == value:
                    total += 1
                    print(f"| `{path}` | `{where}` | `{field}` | `{json.dumps(value, ensure_ascii=False)}` |")
    print(f"\n共 {total} 个字段等于注册默认值；{unknown} 个字段属于当前构建未编入的类型，无法核对。")


if __name__ == "__main__":
    main()
```

## 附录 B：`tests/tooling/studio_config_roundtrip_test.mjs`

5.1 新增的回归测试，随代码提交。表单替身与 `studio_editor_test.mjs` 相同；`add(child, index)` 支持 5.1 在下拉框最前面插入"默认/未设置"选项。
在基线代码上运行，报告 129 个表单变化。

```js
// 用法：node studio_config_roundtrip_test.mjs <catalog.json> <pipeline.json>...
// 逐个渲染 Node、Model、Backend 表单，不修改直接读回，断言与配置中的原值完全一致。
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const source = readFileSync(new URL("../../tools/pipeline_studio/web/editor.js", import.meta.url), "utf8");
const { appendConfigField, readConfigFields } = await import(`data:text/javascript;base64,${Buffer.from(source).toString("base64")}`);

class FormElement {
  constructor(tag) {
    this.tagName = tag.toUpperCase(); this.type = "text"; this.dataset = {};
    this.children = []; this.rawValue = ""; this.listeners = {};
  }
  addEventListener(name, callback) { this.listeners[name] = callback; }
  set value(value) {
    value = String(value);
    this.rawValue = this.tagName === "INPUT" && this.type === "text" ? value.replace(/[\r\n]/g, "")
      : this.tagName === "TEXTAREA" ? value.replace(/\r\n?/g, "\n") : value;
  }
  get value() { return this.rawValue; }
  append(...children) { for (const child of children) { child.parent = this; this.children.push(child); } }
  add(child, index) {
    if (index === undefined) this.append(child);
    else { child.parent = this; this.children.splice(index, 0, child); }
  }
  closest(selector) { return this.id === selector.slice(1) ? this : this.parent?.closest(selector); }
  querySelectorAll(selector) {
    return this.children.flatMap(child => [
      ...(selector === "[data-field]" ? child.dataset.field
        : ["INPUT", "SELECT", "TEXTAREA"].includes(child.tagName)) ? [child] : [],
      ...child.querySelectorAll(selector),
    ]);
  }
}
globalThis.document = { createElement: tag => new FormElement(tag) };
globalThis.Option = class extends FormElement {
  constructor(text, value) { super("option"); this.textContent = text; this.value = value; }
};

const [catalogPath, ...pipelinePaths] = process.argv.slice(2);
const catalog = JSON.parse(readFileSync(catalogPath, "utf8"));
const list = value => Array.isArray(value) ? value : Object.values(value ?? {});
const find = (kind, key, name) => list(catalog[kind]).find(item => item[key] === name);
const roundTrip = (formId, fields, values, choicesFor = () => null) => {
  const form = new FormElement("form"); form.id = formId;
  for (const field of fields) appendConfigField(form, field, values, choicesFor(field));
  return readConfigFields(form);
};

let failures = 0;
for (const path of pipelinePaths) {
  const pipeline = JSON.parse(readFileSync(path, "utf8"));
  const modelIds = (pipeline.models ?? []).map(model => model.model_id);
  const check = (label, actual, expected) => {
    try { assert.deepStrictEqual(actual, expected); } catch {
      failures += 1;
      console.log(`FAIL ${path} ${label}\n  expected ${JSON.stringify(expected)}\n  actual   ${JSON.stringify(actual)}`);
    }
  };
  for (const node of pipeline.pipeline ?? []) {
    const definition = find("nodes", "node_type", node.node_type);
    if (!definition) continue;
    const dependencies = definition.model_dependencies ?? [];
    const choicesFor = field =>
      dependencies.some(dep => dep.config_field === field.name) || field.semantic === "model_ref" ? modelIds : null;
    check(`node ${node.id}`, roundTrip("configFields", definition.config_fields ?? [], node.config ?? {}, choicesFor), node.config ?? {});
  }
  for (const model of pipeline.models ?? []) {
    const modelDefinition = find("models", "model_type", model.model_type);
    const backendDefinition = find("backends", "backend_type", model.backend);
    if (modelDefinition) {
      check(`model ${model.model_id}.model_config`,
            roundTrip("modelConfigFields", modelDefinition.config_fields ?? [], model.model_config ?? {}),
            model.model_config ?? {});
    }
    if (backendDefinition) {
      check(`model ${model.model_id}.backend_config`,
            roundTrip("backendConfigFields", backendDefinition.config_fields ?? [], model.backend_config ?? {}),
            model.backend_config ?? {});
    }
  }
}
console.log(failures ? `${failures} form(s) changed on untouched apply` : "All forms round-trip unchanged");
process.exit(failures ? 1 : 0);
```

## 附录 C：现状证据（核对于 `c894657`）

| 发现 | 证据 | 步骤 |
| --- | --- | --- |
| Studio 应用表单时把未修改的数值、布尔、枚举、数组、对象字段写成显式值；26 份配置中 129 个表单在不修改应用后变化，例如 `LlmGenerateNode` 被写入 `top_k`、`top_p`、`repetition_penalty`、`stop_words` | `tools/pipeline_studio/web/editor.js:112`、`:142`；附录 B 的测试 | 5.1 |
| 去掉三份 CPU 配置中等于默认值的字段（分别 16、25、9 个）后，`resolve-conf` 的 `effective_pipeline` 逐字节一致，说明这些显式值不改变有效配置；`plan` 只输出拓扑，不能用来证明配置等价 | 附录 A 的工具 | 5.1、5.3 |
| 生产 Binding 必须声明批次上限，8 个业务都写 64；默认池深 25 时它不生效。64 首次出现在 `43c777f` / `0a27334`（2026-08-19），提交说明没有给出取值依据 | `src/adapter/io_binding_registry.cpp:345`；`src/adapter/operator/operator_adapter.cpp:170-194` | 5.2 |
| 输入长度上限维护两处：`ResolvedInputLimits` 的默认值和 6 个输入转换器文件中的同值常量；`ResolvedInputLimits` 只按默认值构造，没有配置入口 | `include/adapter/operator_value_type.h:23-35`；`src/adapter/operator/operator_config_resolver.cpp:344` | 5.2 |
| rerank 候选段落：Operator 层按 10 MiB 检查，转换器按 64 KiB 拒绝；`max_rerank_candidates` 没有读取方，转换器写死 8 | `src/adapter/operator/operator_builtin_value_types.cpp:218-236`；`src/adapter/input/rerank_input.cpp:53`、`:72` | 5.2 |
| 输出转换器的 `capacity_fields` 与 ValueType 的字符串容量字段重复（8 个逐一相同），没有交叉校验 | `src/adapter/output/keyword_result_output.cpp:45-46`；`src/adapter/operator/operator_builtin_value_types.cpp:73-77`；`src/adapter/io_binding_registry.cpp:13-31` | 5.2 |
| 没有一处给出单次有效批次；池深规范化写了两遍；`platform_max_batch` 只赋值不读取；Profile 接受 `batch_size`、`depth` 到 100000，SDK 池深上限 1024 | `operator_adapter.cpp:170-200`；`operator_config_resolver.cpp:276-283`；`include/core/session_context.h:45`；`demo/common/demo_options.cpp:293`、`:324` | 5.3 |
| `kRawRequestIds` 在业务契约、转换器端口和辅助函数参数中共 42 行，没有读取方 | `include/adapter/converter_authoring.h:232-301` | 5.4 |
| 并行时 Validator 拒绝不安全节点和共享串行模型；运行时对串行模型不加锁；执行器已在主线程顺序执行单节点层 | `src/core/pipeline_validator.cpp:1490-1525`；`src/core/pipeline.cpp:419-432` | 5.5 |

## 附录 D：不在本阶段做的事项

| 事项 | 结论 | 理由或触发条件 |
| --- | --- | --- |
| 部署配置与业务编排分离（按硬件拆出 `models`、`backend_config`） | 不做 | 改变 Pipeline JSON 格式 |
| 合并 `model_config.max_batch_size` 与 `backend_config.max_batch_size` | 不做 | 现有配置写了这两个字段，未知字段会被拒绝 |
| 新增 SDK 接口查询有效批次 | 不做 | 改变公共 Operator 接口；改由 `resolve-conf` 查询 |
| Demo 读取内部有效限制并自动分批 | 不做 | Demo 应和真实宿主一样只通过 SDK 运行 |
| 批量删除显式等于默认值的字段；`validate` 对此报警告 | 不做 | 显式值可能是有意固定；交方案负责人（D4） |
| Control 命令 ID 辅助分配 | 再新增 custom Control 时 | 脚手架建议未占用 ID 并检查冲突，ID 仍写在源码里长期固定；工具看不到其他分支和已退役 ID，注册时的冲突检查仍是最后防线 |
| 会话缓存 key 由框架组合 | 第二个 Node 需要会话缓存时 | 共享执行、失败共享和重试已由 `GetOrCreateResult` 负责，只需补 key 组合辅助；`lifetime` 同时决定端口兼容，语义不变 |
| Model 批处理要求改为声明 | 新增下一个 Model 时 | `ModelDefinition` 声明批处理方式，工厂统一校验；两个 `max_batch_size` 字段都保留 |
