# Changelog

## Unreleased

Node 作者接口：Batch `Run`、`BuildPrompt`、`FormatAnswer` 签名不符时，编译期直接给出可接受的签名。
两个 LLM 钩子统一返回值规则：接受能隐式转换为 `std::string` 的类型、`std::string_view` 及其 `NodeResult`。
约束收紧：`FormatAnswer` 不再接受 `char`、`int` 等算术类型返回值（此前会被当作单个字符写入结果）。
约束放宽：`BuildPrompt` 可以返回 `std::string_view`；两个钩子的视图先复制再写入，保留来源与内容。

校验诊断：未注册的节点类型、模型类型和 Backend 给出原因与相近的已注册名称；
未注册模型不再连带报告引用错误，业务出口与 IO 边界的同一缺失键只报告一次。
未知节点显式映射的键没有任何已知生产者或 ingress 时，不再连带报告缺少生产者；
已知来源的类型不符、生产者不唯一、ingress 冲突及其他模型的能力不符仍照常报告。
未知配置字段的建议按相似度排序。生产版 `alg_pipeline_tool` 在 stderr 提示检查构建变体
或改用 `alg_pipeline_tool_test`，stdout JSON 保持原有结构。

业务源码自动收录：`src/custom_nodes/`、`src/common_nodes/`、`src/adapter/{input,output,biz}/`
与 `demo/biz/` 下的 `.cpp`（含子目录）在下次构建时自动编入，不再需要修改 CMake。
脚手架删除 `--add-to-cmake`，recipe 不再修改 CMakeLists。

LLM-EdgeFlow 尚未正式发布。当前产品版本标识为 **v11.0.0**，公共 **ABI major 为 9**；
它们描述当前构建与接口基线，不代表已经交付的正式 Release。

Node 的 `OptionalValue` 允许请求缺值，但拒绝已连接输入的运行时类型错误，失败时不调用业务
函数或发布输出。效果验收在独立 Pipeline 副本中继承输出池配置，避免把自身配置合成误报为
执行期间资产变化；验收记录保留实际执行配置，部署变更仍使旧证据失效。

当前基线采用四层架构，通过 C++ Operator SDK 接入宿主；Pipeline JSON 描述明确的数据连接、
模型配置与接入绑定，函数式 Node 使用统一 Spec 声明端口、参数、模型能力和 Control。
现行规则见[架构设计](architecture.md)、[开发者指南](developer_guide.md)和
[配置说明](../configs/README.md)，可用能力以目标构建的 Catalog 为准。

模型效果、目标设备、内部 SDK 接入和开发者体验的验证范围见
[模型、构建与效果验收](VERIFIABLE_SELECTION.md#验收范围与发布准备)。

研发过程与变更历史通过 Git 追溯。正式发布后，本文件按发布版本记录用户可感知的能力、
契约变化及必要的迁移说明。

当前修正包括：线程池部分创建失败时完整回收线程；接入 Binding 批次上限参与运行时
限额；Model 纯配置语义在 Backend 加载前校验；vendor 编译依赖隔离至 Backend。
LLM 节点复用生成参数声明和解析，保留原有字段及默认值。公共 Operator ABI 与
Pipeline 配置格式保持不变。

接入适配层去掉重复声明。删除 `BizExposureDefinition`、`REGISTER_BIZ_EXPOSURE` 及
`IoBindingRegistry` 的 Exposure 接口，注册审计不再检查"曝光业务必须有绑定"。批次上限在绑定上
声明一次：转换器的 `max_batch_size` 默认改为 0（不设限），有效上限取绑定与两个转换器中正值的
最小值，三者显式为 0 时注册审计和部署准备报错。Binding 现默认使用框架标准批次上限 64，
只有实测确需更小值时才覆盖 `max_batch_size`。
绑定的 `input_ports` / `output_ports` 可以省略同名映射，需要完整映射的代码改用 `EffectivePortMapping`。
`ValidateDecodeRequest`、`DecodeRequestRows` 删除批次上限参数，改读 Operator 填入的
`InputDecodeOptions::max_batch_size`。Catalog 中生产转换器的 `max_batch_size` 由 64 变为 0；各业务的
有效批次上限、Pipeline 配置格式与公共 Operator ABI 不变。

开发 Skills 按业务方案规划、Adapter、Map/LLM/Batch Node、Model 和 Backend 提供独立入口，
由业务需求生成组件增补与 DAG 建议，并复用现有作者 API、Catalog 和验证流程。

构建预设、验证脚本和 Kite CI 共用 `CMakePresets.json` 中的场景参数，脚本需要 CMake 3.19+。
默认门禁沿用 `build/`；新的 sanitizer 和真实模型构建分别使用 `build/sanitizers/` 与
`build/real-models/`，旧目录可按需重建或通过 sanitizer 目录变量复用。测试源码与 runner
归属集中在 `tests/RuntimeTests.cmake`，普通测试按目录自动收集 `test_*.cpp`，自定义 Node
测试无需额外维护源码清单；保留独立测试目标、CTest 分组及必需测试清单校验。

Studio 应用表单时，未修改的数值、布尔、枚举、数组和对象默认值继续保持未配置；已有显式值原样
保留，清空或选回“默认”可移除这些字段的覆盖。字符串清空仍表示显式空字符串。Backend 参数
收进“部署高级设置”，已有显式值时自动展开。

接入层输入长度上限集中在 `biz_input_constraints.h`，接受/拒绝边界、诊断和返回码不变；输出
转换器的容量字段由 ValueType 推导，显式列出时必须与 ValueType 一致。Catalog 中
`audio_result`、`audit_result`、`doc_answer` 输出转换器的容量字段按字典序报告。

`resolve-conf` 增加单次有效批次 `effective_process_batch_limit`、规范化池深
`effective_frame_depth` 和池深硬上限 `max_frame_depth_limit`。Operator 使用解析器给出的同一
批次值；Demo 超限后提示查询命令，原有错误和退出码不变。现有配置中的显式默认值保持原样。

请求编号回传移出业务端口：删除 `kRawRequestIds`，`DecodeRequestRows` / `EncodeResultRows`
去掉请求编号端口参数。`InputDecodeOptions` / `OutputEncodeOptions` 新增 `request_ids`，由
Operator 提供本次调用的编号表；自行组织转换的实现改用 `PublishRequestIds` / `RequestIds`，
直接调用转换器的代码需在选项中提供编号表。解码成功却少记或漏记编号时，在租用输出块前返回
`COMPANY_ALG_ERR_INVALID_INPUT`（`-3`）并报告转换器与数量。Catalog、Binding 端口映射和
Studio 的 `$ingress` 不再列出 `raw_request_ids`；Operator 输出的 `request_id`、公共 ABI
与配置格式不变。

`max_parallel_workers > 1` 时，Validator 将未声明并行安全的节点，以及会共享串行模型的
节点拆到单独的层顺序执行；原本因此在 Create 时返回 `-2` 的配置现在可以运行。
`NODE_NOT_PARALLEL_SAFE`、`SERIALIZED_MODEL_CONCURRENCY` 两个诊断码已删除，
`plan` 的 `layers` 与 `topological_order` 反映拆分后的执行顺序；原始层的写冲突仍会被拒绝。

开发工具目录调整：Node 脚手架与开发 Recipe 移至 `tools/scaffold_custom_node.py`、
`tools/dev_recipe.py`；根目录 `show` 软链接删除，Studio 改由 `./tools/pipeline_studio/server.py`
启动，参数不变。C++ 命令行工具源码由 `src/tools/` 改名为 `src/cli/`，可执行文件仍输出到
`build/`。架构图源文件改名为 `doc/architecture_classes.puml` 与 `doc/architecture_flow.puml`。

业务标识统一：每个业务的配置与 Demo 文件名、IoBinding ID（`<词根>.operator.v1`）和
`biz_name`（`<词根>`，不带版本号）使用同一词根，中文名统一使用 BizDefinition 的显示名。
`biz_name` 变更为 `keyword_match`、`entity_extract`、`translate`、`doc_qa`、`cross_rerank`、
`dialogue_audit`、`ocr_invoice_qa`、`audio_asr_intent`；IoBinding `compliance_audit.operator.v1`
改为 `dialogue_audit.operator.v1`，`ocr_doc_qa.operator.v1` 改为 `ocr_invoice_qa.operator.v1`；
OCR 与语音方案的配置、Demo、数据集和 Profile 改用 `ocr_invoice_qa`、`audio_asr_intent` 词根。
Demo 结果目录随 `biz_name` 变化。正式上线前不保留旧名别名。
