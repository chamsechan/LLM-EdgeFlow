# RFC-0009 公司平台槽位绑定与输出池验收结论及整改计划

## 1. 文档定位

本文是
[`RFC-0009`](../0009-company-string-and-slot-map-struct-binding.md)
的实现验收记录和后续整改执行清单。它不创建第二套设计，也不替代 RFC 本体；如果本文
与 RFC 的长期契约冲突，以 RFC 为准，并应先修正文档冲突再继续编码。

本文首次基于 2026-08-26 对分支
`feat/company-string-and-slot-map-struct-binding` 工作区的静态审查、常规回归和定向
Sanitizer 验证编写，首轮审查基线为 `7681cec`。2026-08-27 对整改提交
`11004acd217c46ffd3b11718f78dcbad9c73df25`（下文简称 `11004ac`）完成第二轮复验，
2026-08-27 又对第三轮实现快照
`0309676391c6d04f5df60680874e6e398511d424`（下文简称 `0309676`）完成复验。本文当前结论、
证据和执行规划均以 `0309676` 为准；第二轮内容只保留为历史记录。

本文只授权后续修改 Layer 1 Platform Adapter、Platform Demo、部署配置和对应测试；
不得借整改之机修改内部七类 DTO、Blackboard、Pipeline、Node、Engine 或
`FixedBatchExecutor` 契约。本文也不授权推送、创建 PR 或合并。

## 2. 当前验收结论

**结论：不通过，不能认定当前实现已经完全满足 RFC-0009。**

`0309676` 继续完成了固定容量空闲环、自注册 bridge、路径 helper 异常屏障、深度硬上限和
部分故障注入测试。常规构建、35 项 CTest、六阶段回归和 Platform ASan/UBSan/LSan 定向
测试均通过。但是，实际深度下的总池预算、Create 强异常安全、Acquire 后 lease 跟踪、
模型路径完整约束、Registry 类型一致性、公开别名契约及完整 LSan/TSan 证据仍未关闭。
因此第三轮结论继续为“不通过”，不能采用 v1.2 的“全面关闭”自评声明。

RFC-0009 和 RFC 索引当前不应标记为 `Completed`。按照
[`doc/rfcs/README.md`](../README.md) 的生命周期定义，只有实现完整、全部测试门禁通过、
PR 已合入 `main` 后才可更新为 `Completed`。

### 2.1 已确认应保留的实现方向

- Platform ABI v3 的 `CreateParam` 调用形状已经落地。
- `include/platform/company_platform_types.h` 保持 C11 可编译的 C 风格布局。
- 七类业务已通过独立 bridge 文件完成 Platform 镜像与内部 DTO 的边界转换。
- 输入 map 使用 `.get()` 读取，现有路径未跨调用保存输入 `shared_ptr` 副本。
- 输出 map 的缺 Key、额外 Key、重复后缀、非空占位可以 fail-closed。
- 输出池支持默认深度 25、地址复用、池耗尽阻塞和归还后唤醒。
- 空闲块已经改为 Create 阶段预分配的固定容量环，Return 正常路径不再扩容。
- 输出 deleter 使用 weak pool lifetime token；Destroy 后晚到的 deleter 可以 no-op。
- 七个业务 bridge 已删除中心注册调用清单并改为实现文件就地自注册。
- `CompanyAny` 已使用集中白名单、尺寸方程和类型正确的 metadata data 销毁。
- 同句柄 Process/Control 串行化，不同句柄可并行。
- Demo 已迁移到新的 Platform Operator 调用模型，并按正确顺序释放输出。

这些部分不应在整改中推倒重做；后续工作应聚焦契约缺口和测试证据。

### 2.2 已取得的验证证据

| 验证项 | 结果 | 说明 |
| --- | :---: | --- |
| 审查基线 | 已绑定 | `0309676391c6d04f5df60680874e6e398511d424` |
| `./scripts/format.sh --check` | 通过 | `0309676` 格式检查通过 |
| 默认构建 | 通过 | `cmake -S . -B build`、`cmake --build build -j4` 通过 |
| C11/LayerGuard/Platform 定向测试 | 通过 | 5/5 |
| 全量 CTest | 通过 | 35/35，耗时 133.97 秒 |
| `./scripts/run_all_tests.sh` | 通过 | 六阶段回归通过 |
| `run_sanitizers.sh --fast` 默认模式 | 通过 | ASan/UBSan 15/15 和 Smoke；默认 `detect_leaks=0` |
| Platform `detect_leaks=1` 定向测试 | 通过 | Operator、OutputPool、ValueRegistry 共 3/3 |
| fast `detect_leaks=1` 完整门禁 | 失败 | 14/15；VisualizerServerTest 的 Python 运行时报告 35345 字节泄漏 |
| TSan | 未验证 | CMake 拒绝 `LLM_EDGEFLOW_SANITIZERS=thread`，配置失败 |
| `git diff --check` | 通过 | 候选实现提交前差异检查通过 |

Platform 三个专项测试在 leak detection 下通过，说明已覆盖路径未发现直接泄漏；但当前
故障探针不能触发 cleanup sidecar 扩容、OutputPool 预留和 lease 跟踪扩容，不能据此证明
“任意位置失败均零泄漏”。完整 leak 门禁也仍失败，TSan 仍没有可执行证据。

## 3. 阻断项总表

| 编号 | 等级 | 问题 | 主要影响 | 关闭条件 |
| --- | :---: | --- | --- | --- |
| R9-001 | P0 | `vector<string>` 扩容使 Shadow DTO 中的 `const char*` 失效 | UAF、错误输入、Batch 不稳定 | 所有 Process 局部字符串地址在 `ExecuteBatch` 返回前稳定，并有短字符串/多字段/多 Batch ASan 测试 |
| R9-002 | P0 | 输出 `shared_ptr` 逐个写入后才提交 lease | 异常时部分输出指向已回池块 | 控制块先在局部全部构造，成功后一次发布；故障注入证明失败时 outputs 全空 |
| R9-003 | P1 | Pipeline/模型资源路径未完全限制在 `model_path` | 路径逃逸、部署根不确定 | cfg、Pipeline、模型资源统一 canonical containment；绝对路径、`..`、symlink 全拒绝 |
| R9-004 | P1 | `CompanyAny` 无 type whitelist 和尺寸方程 | 类型混淆、越界和错误分配 | 单一 type registry 解释类型大小；checked-multiply；输入和 metadata 共用规则 |
| R9-005 | P1 | 输出容量没有编译期硬上限 | 过量分配、整数转换风险 | 每字段只允许默认值以下/硬上限以内；总池字节数安全计算并限制 |
| R9-006 | P1 | Create 半成品块无法全量回滚 | 分配失败泄漏 | 当前块和历史块均由 RAII 持有；每个分配点故障注入后零泄漏 |
| R9-007 | P1 | ReturnBlock 不校验归属和状态 | 重复地址入队、计数失真 | 维护块状态账本；非本池、重复归还和下溢 fail-closed，不再入队 |
| R9-008 | P1 | Registry Init 审计不完整 | 不完整绑定延迟到 Create 才失败 | canonical/alias、方向、函数集、业务一致性在 Init 原子审计并返回 `-6` |
| R9-009 | P2 | 七业务由中心列表手工注册 | 新业务易漏登记，偏离自注册设计 | bridge 与实现文件就地自注册，中心不维护七业务清单 |
| R9-010 | P1 | RFC 状态和测试勾选超前 | 交付证据不可信 | 状态恢复为 `In Implementation`；所有勾选项绑定真实测试记录，合入 main 后再 Completed |
| R9-011 | P1 | 实现注册的兼容别名与 RFC 映射表不一致 | 公司平台按文档发送 Key 时被拒绝，未声明 Key 被意外接受 | 实现严格采用 RFC 表，或先评审修改 RFC；建立全表参数化测试 |

P0/P1 全部关闭之前不得进入最终验收。R9-009 若决定保留中心注册，必须先修改 RFC，
说明选择、风险和约束；不能让实现与文档长期分叉。

### 3.1 2026-08-27 第二轮复验状态

| 编号 | 本轮状态 | `11004ac` 复验结论 | 下一步关闭条件 |
| --- | :---: | --- | --- |
| R9-001 | 基本关闭 | Shadow 字符串和 PCM 外层存储已改为地址稳定的 `deque` | 补短字符串 SSO、同帧多字符串、最大 Batch 的字段读取测试 |
| R9-002 | 基本关闭 | 输出先在 pending 集合构造，全部成功后再发布 | 增加第 N 次 `shared_ptr` 构造失败或等价故障注入，证明失败后全部输出仍空且块全部回池 |
| R9-003 | 未关闭 | cfg 相对路径已有 containment 校验，但 Pipeline 中已有绝对模型路径、Windows 盘符、canonical 失败和 `noexcept` 异常边界仍有缺口 | 统一由一个可失败的 canonical containment helper 处理所有资源路径并补平台无关逃逸测试 |
| R9-004 | 已关闭 | `CompanyAny` 白名单、元素尺寸和 checked size equation 已实现并有正反向测试 | 保持输入与 metadata 共用同一注册规则 |
| R9-005 | 部分关闭 | 单字段容量和 `meta_num` 已有硬上限 | 限制 `max_frame_depth`，checked 计算单块与总池字节数，并设置每句柄总预算 |
| R9-006 | 未关闭 | 已分配的完整块可统一销毁，但正在构造的半成品仍以裸指针持有 | 用 RAII 表达半成品所有权，修正嵌套 buffer 类型化销毁，并逐分配点注入失败验证零泄漏 |
| R9-007 | 部分关闭 | Return 路径已拒绝 foreign/duplicate block | Acquire 也必须校验账本；空闲队列改为 Create 时预分配、运行期不分配的固定环形结构 |
| R9-008 | 部分关闭 | canonical suffix、方向和 factory 基础审计已加入 | 拒绝同 BizType 冲突描述符和 Init 后注册；审计 bridge/adaptor 业务名及内部 DTO 类型一致性 |
| R9-009 | 未关闭 | bridge 文件虽有注册入口，但中心仍维护七业务 `RegisterBuiltinBridges()` 清单 | 改成实现文件就地自注册；若保留中心清单，必须先修改 RFC 并补充约束理由 |
| R9-010 | 部分关闭 | RFC 本体和索引已恢复为 `In Implementation` | 撤销尚无证据的 Registry、任意分配失败、ASan/LSan/TSan 和交付闭环勾选，最终证据绑定候选 SHA |

第二轮未再发现首轮两个 P0 问题在正常路径上的复现，但它们的故障注入和边界测试尚未
完整。由于 R9-003、R9-005～R9-010 仍含 P1 阻断，当前结论继续保持“不通过”。

### 3.2 第二轮剩余阻断证据

#### 3.2.1 Create 半成品分配仍不具备 RAII（R9-006）

- `OwnedExternalBlock` 仍是裸指针聚合；嵌套 `CompanyString` 的对象、字符数组或容器
  `push_back` 任一步抛出时，当前半成品尚未进入 `all_blocks_`，`DestroyBlocks()` 无法回收。
- OCR metadata 以 `uint8_t[]` 分配，却通过 `char*` 执行 `delete[]`，元素类型不匹配，
  属于未定义行为风险。
- 当前 OutputPool 测试均为正常路径，没有 allocator/factory 第 N 次失败注入，因而
  `detect_leaks=1` 通过不能证明任意 Create 失败均零泄漏。

#### 3.2.2 路径沙箱仍存在旁路和异常终止风险（R9-003）

- containment helper 标为 `noexcept`，内部却执行可能抛异常的 filesystem/string 操作；
  异常会触发 `std::terminate`，外层 resolver 的 catch 无法接管。
- Linux 下仅依赖 native root 判断不能拒绝 `C:\\...` 这类 Windows 盘符路径。
- `weakly_canonical` 失败后回退到 lexical path，不符合 fail-closed 原则。
- Pipeline JSON 中预先存在的绝对模型路径只在 `is_relative()` 分支进入拼接校验，绝对路径
  可以绕过 `model_path` 根目录约束。
- 现有测试主要覆盖 cfg 的 `../../../etc/passwd`，尚缺绝对模型路径、盘符、symlink 和
  canonical 失败用例。

#### 3.2.3 资源预算只限制单字段，未限制总池（R9-005）

- `max_frame_depth` 仍直接接受调用方值并传入输出池，没有编译期硬上限。
- 没有 checked 计算 `block_bytes`、`depth * block_bytes` 和每句柄所有输出类型总字节数。
- 极大 depth 仍可造成内存拒绝服务，并放大 R9-006 的失败回滚问题。

#### 3.2.4 Registry 尚未实现不可变、自注册和完整冲突审计（R9-008、R9-009）

- `RegisterBuiltinBridges()` 及七业务手工列表仍存在，文件内宏注册没有消除中心清单。
- 同一 BizType、同一 `biz_name` 的重复注册直接成功，未比较 slot、方向和函数集合，冲突
  描述符可能被静默隐藏。
- Init 审计未证明 bridge `biz_name` 与 Adapter 描述符一致，也没有内部 DTO 类型标识可供
  交叉校验。
- `audited_` 置位后，注册函数仍未拒绝晚注册；若运行期发生修改，也没有锁保护。
- 新增测试尚未覆盖冲突注册、错误方向、缺 factory、晚注册和七业务自注册完整性。

#### 3.2.5 输出池 Acquire 和归还队列仍有状态缺口（R9-007）

- Acquire 在 ledger 缺失或状态错误时没有 fail-closed，仍会增加 `checked_out`。
- 空闲块使用 `std::queue<void*>`；Return 的 `noexcept` 路径执行 `push` 时底层容器理论上仍可
  分配并抛异常，既可能 `std::terminate`，也违反“Create 后池管理不再分配”的约束。

#### 3.2.6 RFC 测试证据仍超前（R9-010）

- RFC 中 Registry 冲突审计、Create 任意失败回滚、ASan/LSan/TSan 和“完成闭环”仍有已
  勾选表述，与本轮实际证据不一致。
- CMake 当前只接受 address/undefined sanitizer，`thread` 配置会失败；fast sanitizer 脚本
  同时关闭 leak detection 且不包含 Platform Operator/Pool/ValueRegistry。
- 在工具链和脚本真正覆盖这些目标前，相关项必须标记为未验证，不得用 Core fast 结果
  代替 Platform 验收。

### 3.3 2026-08-27 第三轮复验状态

本节是当前有效状态，取代 v1.2 的“全面关闭 R9-001～R9-010”声明。

| 编号 | `0309676` 状态 | 已完成 | 仍需关闭 |
| --- | :---: | --- | --- |
| R9-001 | 部分关闭 | Shadow 字符串和 PCM backing storage 已使用地址稳定容器 | 只新增 Keyword 短字符串测试；仍缺 DocQA、Audit、OCR、Rerank、多字段和最大 Batch 实际读取 |
| R9-002 | 部分关闭 | pending 控制块和最终移动发布可保持外部 output 原子性 | Acquire 后第一次 `lease_guard.Track()` 可能分配失败而丢块；故障注入仅覆盖两帧的第二个控制块 |
| R9-003 | 部分关闭 | helper 已捕获 filesystem 异常，并拒绝 cfg 的 POSIX/drive/UNC/`..` | Pipeline 原始绝对模型路径仍可在根内被接受；模型路径不检查存在性和文件类型；缺完整路径矩阵 |
| R9-004 | 已关闭 | `CompanyAny` 白名单、尺寸方程、checked multiply 和 metadata 类型化分配已实现 | 保持单一类型表，不在 Resolver 或 bridge 另写类型 switch |
| R9-005 | 未关闭 | 单字段容量、metadata 数量和深度 1024 上限已实现 | 总池预算仍固定按 25 计算，未使用实际深度，也未完整计入嵌套结构及池管理开销 |
| R9-006 | 未关闭 | `OwnedExternalBlock` 已有 move-only 析构回滚，typed delete 已修正 | `new` 与 cleanup 登记之间仍有泄漏窗口；pool reserve/resize 在 try 外；故障注入未覆盖这些位置 |
| R9-007 | 部分关闭 | Return ledger 和固定容量 free ring 已实现，Return 正常路径不再扩容 | Acquire 在验证 ledger 前已移动 head/减少 count；Track 尚未预分配，异常时账本无法自动恢复 |
| R9-008 | 未关闭 | bridge registry 已加锁并在 Init 后拒绝注册；基础方向/factory 审计存在 | 描述符没有内部 DTO 类型；未校验 biz_name 与 Adapter Definition；重复描述符不比较转换函数；Value Registry 冻结不具备线程安全和幂等性 |
| R9-009 | 已关闭 | 中心 `RegisterBuiltinBridges()` 已删除，七个 bridge 编译单元就地自注册 | 增加“缺任一编译单元则 Init 失败”的发现性测试，防止构建列表回归 |
| R9-010 | 未关闭 | RFC 和索引状态保持 `In Implementation`，默认 ASan/UBSan 已覆盖 Platform 测试 | RFC 仍超前勾选任意失败回滚、LSan、TSan 和完成闭环；完整 leak 门禁失败，TSan 不可配置 |
| R9-011 | 未关闭 | canonical suffix 基本一致 | `string/buffer/any` 多出未声明别名，`frame/keyword/audit/audio/rerank` 等别名与 RFC 表不同 |

第三轮没有发现需要回退 Layer 2～Layer 4 的理由。整改仍应严格限制在 Layer 1 Platform
Adapter、Platform 测试、Sanitizer 脚本和 RFC 文档；R9-004 与 R9-009 的主体实现不应重做。

### 3.4 第三轮关键代码证据

1. `company_conf_resolver.cpp:494-497` 使用 `kDefaultOutputPoolDepth` 计算总预算，而
   `platform_operator_adapter.cpp:177-183,241-243` 实际允许并分配最多 1024 个块。
2. `platform_value_type_registry.cpp:35-42` 和各输出 root allocator 均在 `new` 后调用
   `cleanups.push_back`；sidecar 扩容异常时新对象尚未被 RAII 记录。
3. `platform_output_pool.cpp:52-61` 的 pool 创建和 reserve/resize 位于 try 之外；
   `platform_operator_adapter.cpp:492-499` 在 Acquire 后才向未预留的 lease vector 登记。
4. `company_conf_resolver.cpp:571-591` 允许根内绝对 Pipeline 模型路径，并对模型资源使用
   `check_exists=false`；新增路径测试只修改 `cfg_file_name`。
5. `platform_business_bridge_registry.h:97-101` 的相等比较不包含转换函数和 DTO 类型；
   `GlobalInit` 没有 bridge/Adapter Definition 业务名与 DTO 交叉审计。
6. `platform_value_type_registry.cpp:359-400` 及后续注册的 aliases 与 RFC-0009 6.3 表格不一致。
7. `DETECT_LEAKS=1 ./scripts/run_sanitizers.sh --fast` 因 VisualizerServerTest 的 Python
   运行时泄漏失败；CMake 仍拒绝 `LLM_EDGEFLOW_SANITIZERS=thread`。

## 4. 不得破坏的设计不变量

1. `include/company_alg_interface.h` 继续保持纯 C11，不加入 STL 或 Platform C++ 类型。
2. 现有六个纯 C ABI 入口及其错误码兼容性不变；Platform Operator 仍保留六函数表。
3. Platform 镜像类型只存在于 Demo/Platform 边界，不替换内部通用 DTO。
4. 输入所有权始终属于外部；算法库不得复制或缓存输入 `shared_ptr`。
5. 输入值拷贝只存放在单次 Process 局部存储中，Process 返回后统一销毁。
6. 输出对象及嵌套内存只在 Create 分配、在池中复用、在 Destroy 统一释放。
7. Process 成功前不得向调用方暴露任何池块；失败后调用方 output 必须保持原始空值。
8. 同一个 raw block 在任一时刻只能处于 `Free`、`CheckedOut`、`Closing` 之一。
9. Destroy 不与同句柄 Process/Control/deleter 并发，调用方必须先释放全部输出引用。
10. 不修改 Layer 2～Layer 4 来规避 Layer 1 的类型、路径或生命周期问题。

## 5. 分阶段详细整改方案

### 阶段 0：恢复真实生命周期状态并修复测试夹具

目标：先消除错误完成声明，让后续每项测试证据可信。

#### 0.1 文档状态

- [x] 将 RFC-0009 头部状态从 `Completed` 恢复为 `In Implementation`。
- [x] 将 `doc/rfcs/README.md` 中 RFC-0009 的状态同步恢复为 `In Implementation`。
- [ ] 将 RFC 测试矩阵中尚未真实验证的 ASan、LSan、TSan 和故障注入项恢复为未勾选。
- [ ] README Changelog 可以保留“正在实现”的能力说明，但不得声称全部 sanitizer 已通过。

#### 0.2 修复 ASan 测试夹具

将临时字符串改为有明确作用域的对象：

```cpp
const std::string root_string = root.string();
CreateParam param{};
param.model_path = root_string.c_str();
```

`root_string` 必须活到该测试最后一次 `Create` 调用之后。全仓搜索以下模式并逐项检查：

```text
.string().c_str()
.path().c_str()
temporary_expression.c_str()
```

#### 0.3 本阶段测试

- [x] `PlatformOperatorTest` 在 ASan/UBSan 下不排除任何测试，21/21 通过。
- [ ] 测试失败时保存完整 sanitizer 栈，不通过过滤器掩盖。
- [x] `git diff --check` 通过。

建议提交：`test(platform): fix config fixture string lifetime`。

### 阶段 1：修复 Process 局部影子存储（R9-001）

目标：内部 DTO 中的所有裸指针在本次 `ExecuteBatch` 返回前地址稳定。

#### 1.1 推荐实现

将
[`ProcessLocalShadowStorage`](../../../src/adapter/platform/platform_business_bridge_registry.h)
中的：

```cpp
std::vector<std::string> strings;
```

替换为地址稳定容器，例如：

```cpp
std::deque<std::string> strings;
```

并补充 `<deque>`。不能只在当前 Demo 中 `reserve()`，因为 Registry/bridge 契约必须对任意
合法 Batch 和字段组合成立。若选择 `vector + reserve`，必须先计算所有样本、所有候选和
可选字段的精确总数量，并保证转换期间不会再增长；这种方案更复杂，不作为首选。

`std::vector<std::vector<float>>` 的内层 PCM 数据通过独立堆分配持有，但仍应增加多样本
地址稳定测试，防止未来改为不同容器时回归。

#### 1.2 必须覆盖的场景

- [ ] 两帧 Keyword 输入均为 1～7 字节短字符串，触发 SSO 场景。
- [ ] DocQA 同一帧同时包含短 query 和短 doc。
- [ ] Audit 同时包含短 user/channel。
- [ ] OCR 同时包含短 image URI/query。
- [ ] Rerank 包含短 query 和 8 个短候选。
- [ ] Batch 达到业务 `max_batch_size`，检查第一帧和最后一帧内容一致。
- [ ] ASan 下运行全部七业务转换测试，零 UAF。

不要只断言 Process 返回 0；测试必须让下游 Adapter/节点实际读取每个字段，或者对转换后
DTO 在下一次存储增长后逐字段比较内容。

建议提交：`fix(platform): stabilize process shadow storage addresses`。

### 阶段 2：实现输出的两阶段原子发布（R9-002）

目标：Process 在任何异常点都只有两个外部可见状态——全部空，或全部成功。

#### 2.1 正确事务边界

Process 应按以下顺序执行：

1. 完成全部输入和输出槽位校验。
2. 检出全部 raw blocks，并由 `ScopedOutputLeaseGuard` 跟踪。
3. 执行 Runtime 并转换全部输出。
4. 在局部 `pending_outputs` 中为每个 raw block 构造 `shared_ptr<void>` 控制块。
5. 全部控制块构造成功后，先 `lease_guard.Commit()`，再以不抛异常的 shared_ptr 移动赋值
   覆盖已经存在的空槽位。
6. 任一步失败时，局部 shared_ptr 先析构，lease guard 只归还仍由 lease 持有的块；不得
   对同一块归还两次。

推荐为 pending 项保存现有 map value 的指针，避免发布阶段再次使用 `operator[]`：

```cpp
struct PendingOutput {
  std::shared_ptr<void>* destination = nullptr;
  std::shared_ptr<void> value;
};
```

槽位校验阶段通过 `find()` 取得 `destination`。发布阶段只执行 shared_ptr 的 move
assignment，不再插入 map、不再分配字符串或控制块。

需要明确 lease 与 pending shared_ptr 的唯一归还责任：在控制块成功创建后，相应 raw
block 应从 lease guard 转移到 pending owner；不能让两个清理器同时认为自己负责归还。
可以为 lease guard 增加逐块 `ReleaseTracking(raw_block)`，或完成全部 pending 构造后一次
Commit。测试必须覆盖中间第 N 个控制块构造失败。

#### 2.2 故障注入

不要依赖真实 OOM。增加仅测试可用、不会进入公开 ABI 的控制块工厂或分配失败探针：

- [ ] 第一个控制块构造失败。
- [ ] 中间控制块构造失败。
- [ ] 最后一个控制块构造失败。
- [ ] Runtime 返回失败。
- [ ] 第 N 个 `convert_sample_output` 返回容量不足。
- [ ] 每种失败后所有 output 值仍为空，全部块可再次检出，队列数量等于 depth。

建议提交：`fix(platform): publish pooled outputs transactionally`。

### 阶段 3：重构输出池所有权和归还账本（R9-006、R9-007）

目标：任意分配失败零泄漏；每个块只能被检出和归还一次。

#### 3.1 `OwnedExternalBlock` 使用 RAII

当前 `OwnedExternalBlock` 保存无类型 `void*`，只有登记进 `all_blocks_` 后才能清理。建议
采用以下一种方案：

1. 首选：让 `OwnedExternalBlock` 成为 move-only RAII 类型，析构自动调用自身记录的
   destroy 回调；成功移入 pool 后转移所有权。
2. 次选：allocator 在函数入口先把 raw root 写入 block，并对每个嵌套对象使用局部
   `unique_ptr`；所有分配和 vector 登记成功后再 `release()`。

不得继续依赖“最后一行设置 `raw_struct`”来表示对象可清理。以下失败点都必须安全：

- root struct 分配后，第一个字符串对象分配失败；
- `CompanyString` 分配后，字符数组分配失败；
- 字符数组分配后，sidecar vector 扩容失败；
- metadata struct 或 metadata data 分配失败；
- 当前 block 完成后，加入 `all_blocks_` 或 free queue 失败；
- 第 N 个池块失败时，前 N-1 个和当前半成品全部释放。

不要把 `char[]` 和 `uint8_t[]` 混放后统一按 `char*` 删除。sidecar 应保存类型正确的
deleter，或统一以 `std::byte[]` 分配和释放。

#### 3.2 显式块状态账本

为每个 raw block 建立只由 pool mutex 保护的状态，例如：

```cpp
enum class BlockState { kFree, kCheckedOut };
std::unordered_map<void*, BlockState> block_states;
```

规则：

- Create 成功后，每个块登记为 `kFree`，free queue 中恰好出现一次。
- Acquire 只允许 `kFree -> kCheckedOut`，同时增加 checked-out 计数。
- Return 先查归属；未知地址直接记录内部错误并返回，不入队。
- 只有 `kCheckedOut -> kFree` 才能减少计数并入队。
- 已经是 `kFree` 表示重复归还；不得再次入队。
- 计数不允许下溢，账本中的 `kCheckedOut` 数量应始终等于计数器。
- closing 后归还保持 no-op，但 Destroy 前记录的违约计数仍准确。

如果担心 unordered_map 在 Return 中分配，所有条目必须在 Create 阶段完成，运行期只
查询和修改现有值。

#### 3.3 Reset 契约

每个输出类型的 reset 必须恢复所有“有效值状态”，同时保留容量和嵌套地址：

- 字符串：`length = 0`、`data[0] = '\0'`；
- count/index/status/score/request_id：恢复规范初始值；
- metadata：有效 `element_count = 0`、有效 `byte_length = 0`；
- metadata 的分配容量放在 sidecar spec，不复用公开有效长度字段表示 capacity；
- 固定数组按 RFC 要求清零或通过 count=0 使旧内容不可见；
- 不释放、不替换、不缩小嵌套 buffer。

#### 3.4 测试

- [ ] 对每个 allocator 分配点执行确定性失败注入，ASan/LSan 下零泄漏。
- [ ] 验证外层结构地址和所有嵌套字符串/data 地址在归还后保持不变。
- [ ] 重复归还不增加 free 数量。
- [ ] 非本池地址不进入队列。
- [ ] 计数为零时归还不会下溢。
- [ ] 多线程 deleter 归还后账本、计数和队列一致。
- [ ] Destroy 清理某块发生异常时仍继续清理其他块；destroy 回调对外必须不抛。

建议提交：`fix(platform): make output pool allocation and return fail-closed`。

### 阶段 4：建立 `CompanyAny` 类型闭环和容量安全（R9-004、R9-005）

目标：`type_id`、元素数、字节数和真实分配类型只有一个解释来源。

#### 4.1 定义集中类型描述

在 Platform 内部注册表中定义稳定的 Demo type ID。正式公司枚举到位时只替换这一处，
不要在 Resolver、validator 和 allocator 分别写 switch。

建议内部描述符至少包含：

```cpp
struct CompanyAnyTypeDescriptor {
  int32_t type_id;
  size_t element_size;
  size_t alignment;
  const char* debug_name;
};
```

提供无异常 helper：

```cpp
const CompanyAnyTypeDescriptor* FindCompanyAnyType(int32_t type_id) noexcept;
bool CheckedMultiply(size_t lhs, size_t rhs, size_t* out) noexcept;
```

输入 `CompanyAny`、`CompanyFrame.metadata` 和输出 metadata 工厂必须调用同一 helper。

#### 4.2 校验规则

- `type_id` 必须命中白名单；`type_id == 0` 只表示无 metadata。
- count 和 length 先校验非负，再转换到 `size_t`。
- `CheckedMultiply(element_count, element_size)` 必须成功。
- 结果必须等于 `byte_length`，且不超过编译期 `max_any_bytes`。
- 长度非零时 data 必须非空；长度为零时按 RFC 允许空 data。
- `meta_num > 0` 时 type ID 必须合法；为零时 type ID 必须为零。
- 输出 allocator 按 descriptor 的真实 element size 分配，不得固定使用 `sizeof(float)`。

#### 4.3 输出容量硬上限

为每个 `mem_que.capacities` 字段建立默认值和硬上限表。Resolver 必须：

1. 拒绝未知字段、非整数、零值和负值；
2. 先以 64 位无符号类型读取，避免 `get<int32_t>()` 对超大 JSON 整数抛异常后映射为
   `-99`；
3. 拒绝超过字段硬上限的值并返回配置错误 `-2`；
4. checked-add `capacity + 1`；
5. checked-multiply `depth * 每块总字节数`，并执行单句柄总池预算上限；
6. 任何超限都在加载模型和创建 Runtime 之前失败。

#### 4.4 测试

- [ ] 每个白名单 type ID 的合法尺寸。
- [ ] `type_id` 为 0、负数、未知正数。
- [ ] count × element_size 溢出。
- [ ] 乘积与 byte_length 不一致。
- [ ] `meta_num/type_id` 全组合。
- [ ] 每个输出容量的默认值、硬上限、硬上限 + 1、超大 JSON 整数。
- [ ] Create 超限时返回 `-2`，不是 `-99`，且没有加载模型或分配池。

建议提交：`fix(platform): validate typed metadata and pool capacities`。

### 阶段 5：统一部署资源路径沙箱（R9-003）

目标：所有部署资源只能解析到 canonical `model_path` 内部，且只有一个根目录语义。

#### 5.1 单一路径解析 helper

在 `CompanyConfResolver` 内建立一个无副作用 helper，输入 canonical root、相对路径和字段
名，输出 canonical existing path：

```cpp
int ResolveContainedExistingPath(
    const std::filesystem::path& canonical_root,
    const std::string& relative_value,
    const char* field_name,
    std::filesystem::path* resolved,
    std::string* error_msg) noexcept;
```

该 helper 必须：

- 拒绝空字符串；
- 拒绝 POSIX 绝对路径、Windows drive/root-name 和 UNC 风格路径；
- `root / relative` 后做 lexical normalization；
- 文件存在后做 canonical/weakly-canonical；
- 按路径组件比较 containment，不能用简单字符串前缀；
- 拒绝 `..` 和 symlink 逃逸；
- 按调用方要求检查 regular file 或 directory；
- 所有 filesystem 异常转换为稳定的 `-2` 和诊断，不越过 `noexcept`。

#### 5.2 统一使用范围

- `cfg_file_name`；
- `.conf` 中的 `pipe_path`；
- `.conf` 中单 `model_path`；
- `.conf` 中 `model_paths` 的每个值；
- Pipeline JSON 中没有被 `.conf` 覆盖的相对模型路径；
- 后续增加的词表、标签、模板或池资源路径。

删除“root 下不存在时改为相对 conf 目录”的回退。`CreateParam.model_path` 是唯一部署根，
配置文件所在目录不能成为第二个隐含根。

绝对模型路径如果确有公司部署需求，应先修改 RFC 并增加显式可信根白名单；当前实现
阶段不得静默接受。

#### 5.3 测试矩阵

对 cfg、Pipeline、单模型、多模型分别覆盖：

- [ ] 正常相对路径；
- [ ] `../` lexical 逃逸；
- [ ] 多级 `a/../../` 逃逸；
- [ ] POSIX 绝对路径；
- [ ] Windows drive/UNC 风格路径；
- [ ] 根内 symlink 指向根外文件；
- [ ] 根内 symlink 指向根内文件；
- [ ] 不存在文件、目录冒充文件、权限错误；
- [ ] `model_path=/root/a` 与目标 `/root/ab` 的字符串前缀混淆。

Create 与 `ValidatePlatformConfigBinding` 必须对同一输入返回相同分类和核心诊断。

建议提交：`fix(platform): confine deployment resources to model root`。

### 阶段 6：补齐 Registry 原子审计与自注册（R9-008、R9-009）

目标：所有不完整或冲突绑定都在 Init 阶段以 `-6` 失败，生产运行期只读。

#### 6.1 Value type 注册原子性

`RegisterBinding` 在写入任何 map 前先完成完整预检：

- canonical 非空；
- canonical 不得与已有 canonical 或 alias 冲突；
- aliases 非空、不重复、不等于 canonical；
- 每个 alias 不得与已有 canonical 或 alias 冲突；
- 本次 aliases 自身也不能重复。

只有所有检查成功后才能一次写入 canonical 和 aliases。失败时保留已有注册项不变，并把
Registry 标记为 conflict，使 Init 返回 `-6`。

#### 6.2 Business bridge 描述符审计

注册或 GlobalInit 必须验证：

- BizType 非 unknown 且存在对应 `IBusinessAdapter`；
- `biz_name` 与 Adapter/Definition 一致；
- input slot 的 direction 必须为 input，output slot 必须为 output；
- descriptor 只能引用 canonical suffix，不能引用 alias；
- logical name 和 suffix 在各方向唯一；
- 必需转换函数均存在；需要内部输出 DTO 的业务必须有
  `create_shadow_output_dto`；
- 输入 binding 必须具备 validator；
- 输出 binding 必须同时具备 allocate/reset/destroy；
- bridge 的内部输入/输出 DTO 类型与现有 Adapter descriptor 一致。

#### 6.3 自注册

删除 `RegisterBuiltinBridges()` 中七个业务函数的手工清单。每个
`business_bridges/<biz>_bridge.cpp` 在本实现文件中声明描述符并通过统一宏或静态注册器
注册。注册器只提交描述符，不创建 Runtime、模型、Node 或 Engine。

如果采用静态库链接，需要保证对象文件不会被链接器裁剪；可以把 bridge 源直接编入
`alg_sdk`，或使用显式 anchor，但 anchor 只能解决链接保留，不能重新成为业务元数据
中心清单。

#### 6.4 测试

- [ ] canonical 与已有 alias 冲突。
- [ ] alias 与已有 canonical/alias 冲突。
- [ ] 同一描述符 aliases 自重复；失败后无部分 alias 残留。
- [ ] 输入/输出方向错误。
- [ ] descriptor 使用 alias 而非 canonical。
- [ ] 输出缺 allocate、reset 或 destroy，Init 均返回 `-6`。
- [ ] BizType/biz_name/DTO 不匹配。
- [ ] 七业务均能通过自注册被发现；删除任一 bridge 编译单元时专项测试失败。
- [ ] Init 成功后 Registry 只读，并发查询通过 TSan。

建议提交：`refactor(platform): make binding audit atomic and self-registering`。

### 阶段 7：异常屏障、结果数量和文档收敛

#### 7.1 公开入口

逐个检查：

- `Init/Create/Process/Control/Destroy/DeInit`；
- `Get_LLM_EDGEFLOW_OperatorTable`；
- `GetPlatformLastError`；
- `ValidatePlatformConfigBinding`；
- 自定义 deleter、reset、destroy、lease rollback。

所有声明为 `noexcept` 且内部可能分配、格式化字符串或调用 filesystem/JSON 的入口必须有
完整 `try/catch (const std::exception&) / catch (...)`。清理路径不得因为一个 block 的
reset/destroy 失败而跳过剩余块。

`ExecuteBatch` 成功后必须检查 `num_outputs == batch_size` 且每个内部输出 DTO 有效；不满足
时按整批失败处理并归还全部 lease，不能发布默认构造的假结果。

#### 7.2 文档一致性

- [ ] README 不使用“绝对零拷贝”“无野指针”等超出契约的表述。
- [ ] RFC 的文件落点与实际自注册方式一致。
- [ ] RFC 测试勾选只在对应测试真实运行通过后更新。
- [ ] RFC-0004 中被 RFC-0009 取代的输出池和 Demo 约定交叉引用保持正确。
- [ ] 架构文档继续把 Platform mirror/bridge/pool 放在 Layer 1，不画入 Core。

建议提交：`docs(rfc): align platform implementation evidence`。

## 6. 建议文件修改清单

| 文件 | 预期修改 |
| --- | --- |
| `src/adapter/platform/platform_business_bridge_registry.h` | 地址稳定的 shadow storage；完整描述符契约 |
| `src/adapter/platform/platform_business_bridge_registry.cpp` | 原子审计；移除七业务中心清单 |
| `src/adapter/platform/business_bridges/*.cpp` | 就地自注册；保持逐字段转换 |
| `src/adapter/platform/platform_value_type_registry.h` | Any type descriptor、checked helpers、类型正确的 Owned block 所有权 |
| `src/adapter/platform/platform_value_type_registry.cpp` | type whitelist、尺寸方程、RAII allocator、完整 reset/destroy |
| `src/adapter/platform/platform_output_pool.h` | block state ledger、可测试状态不变量、事务 lease 转移接口 |
| `src/adapter/platform/platform_output_pool.cpp` | 半成品回滚、归属/重复归还检查、无下溢状态机 |
| `src/adapter/platform/company_conf_resolver.cpp` | 单一 contained-path helper、容量硬上限、稳定配置错误码 |
| `src/adapter/platform_operator_adapter.cpp` | 两阶段输出发布、结果数量校验、公开入口异常屏障 |
| `tests/test_platform_operator.cpp` | 修复临时字符串；补齐端到端和配置契约测试 |
| 建议新增 `tests/test_platform_output_pool.cpp` | allocator 故障注入、状态机、并发归还、地址复用 |
| 建议新增 `tests/test_platform_value_registry.cpp` | alias/canonical 冲突、Any 白名单、checked-multiply |
| `doc/rfcs/0009-*.md`、`doc/rfcs/README.md` | 恢复真实状态，最终门禁后再完成闭环 |

专项测试拆分后必须在 `CMakeLists.txt` 中注册独立 CTest。Registry 冲突测试应使用独立
进程测试目标，避免进程级 conflict 状态污染其他用例；不要为生产 Registry 增加
`ResetForTesting()` 后门。

## 7. 最终测试与验收矩阵

### 7.1 功能与契约测试

- [ ] 七业务 Platform 镜像到内部 DTO 的转换逐字段正确。
- [ ] 七业务内部 DTO 到池化镜像的转换逐字段正确。
- [ ] CompanyString 的空值、负长度、非 NUL 结尾、嵌入 NUL、容量不足行为与 RFC 一致。
- [ ] CompanyBuffer 支持任意二进制数据，不把 NUL 当终止符。
- [ ] CompanyAny 白名单、尺寸方程和溢出完整覆盖。
- [ ] suffix 按最后一个点解析；alias、未知、重复、方向错误全部 fail-closed。
- [ ] 输入 shared_ptr use_count 前后不增加，输入数据值在 Runtime 执行期间稳定。
- [ ] 输出占位必须为空；失败时无部分输出。

### 7.2 输出池测试

- [ ] `max_frame_depth == 0` 归一化为 25。
- [ ] 单次 Batch 大于 effective limit 直接拒绝，不进入等待。
- [ ] 跨 Process 最多持有 depth 个输出，第 depth+1 次阻塞。
- [ ] 任意线程释放一个旧输出后等待线程被唤醒。
- [ ] 外层和所有嵌套地址归还后复用。
- [ ] reset 恢复有效长度、计数、状态、分数和索引，但保留容量。
- [ ] 重复归还、非池地址和计数下溢不会污染 free queue。
- [ ] Create 第 N 个任意嵌套分配失败后全量回滚。
- [ ] Process 任意失败点自动回池且 outputs 全空。
- [ ] Destroy 正常路径和未归还违约路径均符合 RFC。

### 7.3 配置与安全测试

- [ ] cfg、Pipeline、模型路径的绝对路径、`..` 和 symlink 逃逸均拒绝。
- [ ] 所有相对资源以 `CreateParam.model_path` 为唯一根。
- [ ] mem_que 缺失、类型错配、Any type 组合非法、未知容量和超硬上限均返回 `-2`。
- [ ] Create 与 `ValidatePlatformConfigBinding` 对同一配置结论一致。
- [ ] 配置失败发生在模型加载和池分配之前。

### 7.4 并发和异常测试

- [ ] 同句柄 Process/Control 串行。
- [ ] 不同句柄并发且池互不影响。
- [ ] deleter 从其他线程归还安全。
- [ ] shared_ptr 第 N 次构造失败时事务回滚。
- [ ] reset/destroy 某块失败时继续处理其余块。
- [ ] 所有公开 `noexcept` 入口捕获标准和未知异常。

### 7.5 回归命令

完成实现后按顺序执行，并在最终验收记录中保存命令、时间、提交 SHA 和结果：

```bash
./scripts/format.sh
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure
./build/alg_demo --suite smoke
./scripts/run_all_tests.sh
./scripts/run_sanitizers.sh --fast
git diff --check
```

另行建立并运行：

- 完整 RFC-0009 Platform ASan/UBSan 测试，不允许过滤失败用例；
- LeakSanitizer 或等价的可复现泄漏检查；
- TSan 下的 pool/deleter/Registry 并发专项测试。

如果当前平台不支持某 sanitizer，必须记录为 `NOT VERIFIED` 和可复现原因，不能勾选为
通过。`detect_leaks=0` 的 ASan 运行不能作为 LSan 证据。

## 8. 推荐实施顺序与提交边界

建议严格按以下顺序处理，避免多个内存问题互相掩盖：

1. 修测试夹具和 RFC 状态，不改生产行为。
2. 修 shadow storage，先关闭输入 UAF 风险。
3. 修两阶段输出发布，建立失败原子性。
4. 将 Owned block 改为 RAII，再实现 pool 状态账本。
5. 实现 Any 类型表和全部容量硬上限。
6. 收敛所有资源路径到唯一根目录。
7. 补齐 Registry 原子审计和 bridge 自注册。
8. 补全专项测试、Sanitizer 和文档证据。
9. 形成候选实现提交后重新执行全部门禁。
10. 用户明确要求上传时才使用仓库 `github-branch-merge` 流程。

每个提交只关闭一组相关问题，避免把安全修复、格式化、文档状态和大规模重命名混在
同一提交中。不得使用 `git reset --hard` 或 `git checkout --` 丢弃当前工作区修改。

### 8.1 第二轮后的下一次修改顺序

下面是基于 `11004ac` 的最短剩余路径。下一轮不要重做 R9-004，也不要先扩大业务功能；
应先关闭可能导致泄漏、终止、逃逸和拒绝服务的边界问题。

1. **先关闭 R9-006：Create 任意失败全量回滚。**
   将半成品 block 改为移动式 RAII 对象；每种嵌套 allocation 记录准确的元素类型、容量和
   销毁函数。只有完整构造成功后才把所有权提交给池。为每个 allocation/factory/push
   位置增加第 N 次失败注入，并用 `detect_leaks=1` 验证零泄漏、零 double-free。
2. **随后关闭 R9-007：固定容量空闲结构和双向 ledger 校验。**
   在 Create 阶段预分配 free-index ring；Acquire 必须执行 `Free -> CheckedOut` 原子状态
   转换，Return 必须执行 `CheckedOut -> Free`，任何未知块或非法迁移均 fail-closed，且
   `noexcept` 路径不得再触发内存分配。
3. **关闭 R9-003：所有路径统一经过唯一 containment helper。**
   helper 要么移除 `noexcept` 让外层异常屏障转换错误码，要么在内部捕获全部异常；拒绝
   POSIX 绝对路径、Windows drive/UNC、`..`、symlink 逃逸和 canonical 失败。cfg、Pipeline
   JSON、模型及其他资源均不得存在旁路。
4. **关闭 R9-005：增加深度和总字节预算。**
   为 `max_frame_depth` 设置硬上限；用 checked add/multiply 计算每块、每类型和每句柄总
   内存，超过预算在 Create 分配前直接返回配置错误。边界值、上限加一和乘法溢出均测试。
5. **关闭 R9-008/R9-009：Registry 冲突审计、冻结和自注册。**
   注册时比较完整描述符；Init 一次性验证 biz、方向、suffix、外部结构类型、内部 DTO
   类型和全部函数。Init 成功后拒绝注册修改。移除七业务中心清单，或先以 RFC 修订明确
   决定保留中心注册及其一致性保障。
6. **补齐 R9-001/R9-002 的边界证据。**
   增加 SSO、多字符串、多候选、最大 Batch 地址稳定测试，以及控制块第 N 次构造失败的
   原子发布测试；失败后逐项断言 output 仍空、pool 计数恢复且地址可再次获取。
7. **修正文档和 Sanitizer 门禁（R9-010）。**
   先取消无证据勾选；让 sanitizer 脚本包含三个 Platform 目标并提供 leak 模式。若当前
   工具链无法启用 TSan，记录 `NOT VERIFIED`、失败命令和原因，不得标记通过。
8. **在单一候选 SHA 上执行最终门禁。**
   依次运行 format、默认构建、35+ CTest、Demo、六阶段回归、Platform ASan/UBSan、LSan
   和可用的 TSan，然后将命令、日期、SHA、测试数量和结果写入本文。用户未明确要求前，
   不推送、不创建 PR、不合并。

建议将前四步拆成独立提交，Registry 与测试/文档分别提交，使每个提交都能通过默认构建
和受影响的定向测试；最终再执行全量门禁。

### 8.2 基于 `0309676` 的可实施修复规划

以下阶段按依赖顺序执行。每一阶段完成后先运行受影响的 Platform 测试；前一阶段没有
满足关闭条件时，不应提前更新 RFC 勾选项。

#### 阶段 A：关闭分配与 lease 强异常安全（R9-002、R9-006、R9-007）

涉及文件：

- `src/adapter/platform/platform_value_type_registry.{h,cpp}`；
- `src/adapter/platform/platform_output_pool.{h,cpp}`；
- `src/adapter/platform_operator_adapter.cpp`；
- `tests/test_platform_output_pool.cpp`、`tests/test_platform_operator.cpp`。

实施步骤：

1. 将 `std::vector<std::function<void()>> cleanups` 替换为不需要单独分配控制块的
   move-only cleanup record，例如 `void* + noexcept function pointer`。allocator 在分配前
   `reserve` 精确 cleanup 数量；每次 `new` 后的 cleanup 登记必须保证不抛异常。
2. 所有 root、`CompanyString`、字符数组、`CompanyAny` 和 metadata data 都先由局部
   `unique_ptr` 持有；只有 cleanup record 成功登记后才 release。不得存在裸指针从 `new`
   返回到登记之间无人持有的语句区间。
3. 把 `OutputPoolState` 创建、`all_blocks_.reserve`、`free_ring_.resize` 和
   `block_states_.reserve` 全部放进同一个 try；函数入口先将 `*out_pool` 置空，所有
   `bad_alloc` 稳定映射为 `-4`。
4. `DestroyBlocks()` 继续逐块隔离异常，同时调用注册契约中的 `destroy_external`；
   `OwnedExternalBlock::Destroy()` 作为幂等兜底，不能让注册的 destroy 函数成为死字段。
5. 为 `ScopedOutputLeaseGuard` 增加 `Reserve(size_t)`；根据所有
   `frame_out_bindings` 的总数，在第一次 Acquire 前同时 reserve lease 和
   `acquired_blocks`。reserve 失败时还没有检出任何块。
6. Acquire 先读取 ring 元素并校验非空、归属和 `kFree` 状态，全部成功后才移动 head、
   减少 free count 并增加 checked-out count；任何非法状态都不得改变队列和账本。
7. 控制块故障探针覆盖第一、中间和最后一个输出；每次失败后断言全部 output 为空、
   `free_count == depth`、`checked_out == 0`，且下一批可复用全部地址。

测试要求：

- 对七种输出 allocator 的 root、每个嵌套对象、data、cleanup 登记、当前块提交和第 N 个
  块分别故障注入；
- 对 pool object、三个 reserve/resize、lease reserve 和 pending reserve 分别注入；
- 在 Platform `detect_leaks=1` 下零泄漏、零 UAF、零 double-free；
- 重复归还、foreign block、错误 ledger 和计数下溢均不改变 free ring。

关闭标准：不存在 Acquire 成功后无人持有归还责任的路径；任意 Create 分配失败返回
`-4`、`*handle == nullptr`，且所有已完成块和当前半成品均被释放。

建议提交：`fix(platform): make pool allocation and leases strongly exception safe`。

#### 阶段 B：按实际深度执行单句柄内存预算（R9-005）

涉及文件：

- `src/adapter/platform/company_conf_resolver.{h,cpp}`；
- `src/adapter/platform/platform_value_type_registry.{h,cpp}`；
- `src/adapter/platform_operator_adapter.cpp`；
- `tests/test_platform_operator.cpp`。

实施步骤：

1. 增加唯一的 `ComputeOutputPoolBytes(suffix, spec, depth, out_bytes, err)`，所有加法和乘法
   使用 `CheckedAdd/CheckedMultiply`。
2. 预算至少包含 root struct、每个 `CompanyString` struct、每个 `capacity + 1` data、
   `CompanyAny` struct、metadata payload、raw block/ring/ledger 的确定性管理开销。若决定只
   限制 payload，必须先在 RFC 明确预算口径，不能仍称“总池内存”。
3. 将实际 `effective_depth` 传给 Resolver，或在 Resolver 返回后、创建 Runtime 前执行预算
   校验。不得继续乘固定默认值 25，也不得等 Runtime/模型加载后才失败。
4. 当前每业务只有一个输出池，但计算接口按所有 output slots 求和，为未来多输出保持
   正确；总和超过 `kMaxHandlePoolMemoryBytes` 返回 `-2`。

测试要求：

- depth 为 0、1、1024、1025；
- 每个容量默认值、硬上限、硬上限加一；
- 总预算恰好等于上限、上限加一；
- `depth * block_bytes` 和多池累加溢出；
- 证明预算失败发生在 Runtime 创建和 pool allocation 之前。

关闭标准：任意被接受的配置按实际深度计算后不超过单句柄预算，超限稳定返回 `-2`。

建议提交：`fix(platform): enforce pool budget with effective frame depth`。

#### 阶段 C：完成单一部署根路径契约（R9-003）

涉及文件：

- `src/adapter/platform/company_conf_resolver.{h,cpp}`；
- `tests/test_platform_operator.cpp`；
- 必要时仅调整 Platform 测试 fixture，不修改 Pipeline/Core/Engine。

实施步骤：

1. 在对 Pipeline JSON 应用 `.conf` override 前，先验证所有原始资源值必须为相对路径；
   POSIX absolute、Windows drive/root、UNC 和任何 lexical `..` 逃逸直接返回 `-2`。
2. 原始相对值统一通过 `root / relative -> canonical/weakly_canonical -> 组件级 containment`
   解析。被 `.conf` 覆盖后产生的内部绝对路径通过单独的“已规范化结果”函数校验，不能用
   该内部路径反向证明原始绝对输入合法。
3. cfg 和 Pipeline 文件必须存在且是 regular file。模型文件存在性目前与 mock engine
   配置存在冲突：执行编码前必须二选一并写入 RFC：
   - 推荐生产契约：Platform Create 要求模型文件存在，测试 fixture 提供受控占位资源；
   - 若保留 emulator 缺文件能力：仅对明确列出的 emulator engine 允许不存在，同时仍
     校验其最近存在父目录和 symlink containment，正式 backend 一律要求 regular file。
4. 删除任何以 conf 所在目录作为第二根的回退；错误诊断必须指出字段名和拒绝原因。

测试要求：对 cfg、pipe_path、单 model_path、model_paths 和 Pipeline 原始 model_path
分别覆盖正常相对路径、两种 `..`、POSIX absolute、drive、UNC、根内/根外 symlink、
不存在、目录冒充文件、权限错误及 `/root/a` 与 `/root/ab` 前缀混淆。Create 和
`ValidatePlatformConfigBinding` 必须返回相同分类。

关闭标准：所有外部资源值只有一个部署根解释；原始绝对路径不存在“根内可接受”旁路。

建议提交：`fix(platform): complete model-root path confinement`。

#### 阶段 D：收敛 Registry 与公开后缀契约（R9-008、R9-009、R9-011）

涉及文件：

- `src/adapter/platform/platform_business_bridge_registry.{h,cpp}`；
- `src/adapter/platform/platform_value_type_registry.{h,cpp}`；
- `src/adapter/platform/business_bridges/*.cpp`；
- `tests/test_platform_value_registry.cpp`，并建议新增独立
  `tests/test_platform_business_bridge_registry.cpp`。

实施步骤：

1. 给 bridge descriptor 增加 `internal_input_type_name`、`internal_output_type_name` 和稳定的
   registration identity。每个 bridge 使用对应 `AdapterDescriptor` 的实际 DTO 名称。
2. GlobalInit 校验 BizType、bridge `biz_name` 命中 Adapter 的 BusinessDefinition、内部 DTO
   类型一致、canonical suffix、方向、logical name、validator 和全部输出生命周期函数。
3. 同一 BizType 的第二次注册直接视为冲突；不要仅比较 slots 后把不同转换函数当成幂等
   注册。若确需幂等，必须比较显式 registration identity，不能比较不到的 `std::function`。
4. Value Registry 和 Business Registry 均以 mutex 或构建后 immutable snapshot 保护 Init、
   注册和查询。Init 成功后的晚注册只返回 false，不修改已审计快照，也不能破坏下一次
   GlobalInit 的幂等成功。
5. RegisterBinding 采用临时 map + swap，或对已插入 aliases 建立异常回滚，保证 map 分配
   失败也不会留下部分 alias。
6. 按 RFC-0009 6.3 表逐项修正 aliases；删除 `string/buffer/any` 的未声明别名，补齐
   `image_in/sentence_in/match_out/verdict_out/pcm_stream/pair_in/scores_out`。如果平台实际名称
   已变更，必须先修改 RFC 表和兼容策略，再改代码。
7. 保持七个 bridge 就地自注册；CMake 的源文件清单只承担编译纳入，不重新承担业务元数据
   注册职责。

测试要求：

- 使用可隔离的新 Registry 实例测试 canonical/alias 冲突、注册中途异常、方向错误、缺失
  factory、biz/DTO 不一致、重复 BizType 和晚注册；不得污染全局 singleton 后依赖测试顺序；
- 将 RFC 6.3 全表做参数化测试，逐项验证 canonical、唯一合法 aliases 和 C 类型名；
- 验证连续两次 GlobalInit 均成功，晚注册被拒绝后已发布 snapshot 仍可查询；
- 七业务发现性测试断言完整枚举集合，不使用中心调用清单。

关闭标准：Init 后两个 Registry 真正只读、可并发查询，所有不一致均在 Init 以 `-6`
fail-closed，公开 aliases 与 RFC 完全一致。

建议提交：`fix(platform): audit immutable registries and align slot aliases`。

#### 阶段 E：补足输入、发布和 Sanitizer 证据（R9-001、R9-002、R9-010）

涉及文件：

- `tests/test_platform_operator.cpp`；
- `tests/test_platform_output_pool.cpp`；
- `tests/test_platform_value_registry.cpp`；
- `scripts/run_sanitizers.sh`、`CMakeLists.txt`；
- RFC-0009 本体、RFC 索引和本文。

实施步骤与测试：

1. 为七业务建立表驱动转换测试：Keyword SSO；DocQA query/doc；Audit user/channel；OCR
   URI/query；Audio PCM；Rerank query + 8 candidates；Entity text。至少一组达到 Adapter
   `max_batch_size`，并让下游 Adapter 实际读取首尾样本字段。
2. 输出发布故障覆盖 first/middle/last control block、lease reserve、Runtime failure、每个输出
   conversion failure；所有失败均验证 output 全空、块数恢复和地址可复用。
3. `run_sanitizers.sh` 根据 `DETECT_LEAKS` 输出真实说明。Leak 模式将纯 C++/Platform 测试
   与 Python Visualizer 测试拆开；Python 测试不能污染 Platform LSan 结论，也不能把其
   leak 静默当成全量通过。
4. CMake 增加独立 `thread` sanitizer 配置，不能与 address 混用。至少运行 pool/deleter、
   不同句柄和 Registry 并发专项；若当前 AArch64 工具链无法运行，RFC 必须记录命令、错误
   和 `NOT VERIFIED`，相关复选框保持未勾选。
5. 将 RFC 中 Create 任意失败、Registry 全量审计、LSan/TSan 和“完成闭环”的超前勾选
   恢复为未完成。只有候选 SHA 上的证据通过后再逐项勾选。

关闭标准：默认门禁、Platform ASan/UBSan、Platform LSan 和可用的 TSan 均有可复现命令；
每条 RFC 勾选项能够指向具体测试名称和候选提交 SHA。

建议提交：`test(platform): complete RFC 0009 failure and sanitizer gates`。

#### 阶段 F：最终候选验收与文档闭环

1. 形成单一候选提交并记录完整 SHA，确认工作区干净。
2. 执行：

   ```bash
   ./scripts/format.sh --check
   cmake -S . -B build
   cmake --build build -j4
   ctest --test-dir build --output-on-failure
   ./scripts/run_all_tests.sh
   ./scripts/run_sanitizers.sh --fast
   DETECT_LEAKS=1 ./scripts/run_sanitizers.sh --fast
   LLM_EDGEFLOW_SANITIZERS=thread ./scripts/run_sanitizers.sh --fast
   git diff --check
   ```

3. 将结果、测试数量、耗时、失败豁免和 SHA 写入本文。没有运行的项目必须写
   `NOT VERIFIED`，不得写“通过”。
4. 在 PR 合入 `main` 前继续保持 RFC 和索引为 `In Implementation`；用户明确要求上传时
   才执行 `github-branch-merge` 流程。

## 9. 最终通过标准

只有同时满足以下条件，RFC-0009 才能通过实现验收：

- [ ] R9-001～R9-011 均有代码和测试证据，P0/P1/P2 无遗留。
- [ ] 当前实现与 RFC 的类型、路径、生命周期、并发和失败原子性逐条一致。
- [ ] 没有为了通过测试而放宽输入校验、路径沙箱或池状态机。
- [ ] 默认构建、全部 CTest、七业务 Demo 和六阶段回归全部通过。
- [ ] Platform ASan/UBSan 全量通过；LSan/TSan 通过或诚实标为 `NOT VERIFIED`。
- [ ] `git diff --check`、Markdown 链接和 RFC 交叉引用通过。
- [ ] 最终验收报告绑定候选提交 SHA，而不是未提交工作区。
- [ ] PR CI 通过并合入 `main` 后，RFC 本体和索引才更新为 `Completed`。

在上述条件全部关闭前，准确状态是 `In Implementation`，验收结论保持“不通过”。

## 10. 变更记录

| 日期 | 版本 | 变更内容 | 作者 |
| --- | --- | --- | --- |
| 2026-08-26 | v1.0 | 记录首次完整语义验收结论、十项阻断项和分阶段整改方案 | Codex |
| 2026-08-27 | v1.1 | 记录 `11004ac` 第二轮复验、逐项关闭状态、剩余证据和下一轮最短整改顺序 | Codex |
| 2026-08-27 | v1.2 | 实现方第三轮整改自评；其中“全面关闭”结论经 v1.3 复验未获证实 | Antigravity |
| 2026-08-27 | v1.3 | 绑定 `0309676` 第三轮独立复验，新增 R9-011，记录真实门禁结果和阶段 A～F 可执行修复规划 | Codex |
