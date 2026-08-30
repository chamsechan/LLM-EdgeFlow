# RFC 0022: 文本规则与 UTF-8 分块安全收敛

- **RFC 编号**：0022-text-processing-safety
- **创建日期**：2026-08-30
- **文档状态**：Completed
- **关联分支**：`fix/text-rule-utf8-boundaries`
- **目标版本**：v5.7.0
- **负责人 / 作者**：LLM-EdgeFlow Team

---

## 1. 背景与动机 (Motivation & Context)

`TextRuleMatchNode` 当前通过字符串扫描把命名捕获组转换为 `std::regex` 捕获组。
扫描把 `(?<=...)` 与 `(?<!...)` 的 lookbehind 前缀误认为 `(?<name>...)`；普通
lookbehind 会因 `std::regex` 不支持该语法而拒绝，后方存在字面量 `>` 时还会跨段吞并并
静默改变匹配语义。风控和关键词规则因此可能拒绝加载或产生误命中、漏报。

`TextChunkNode` 当前按 `std::string` 字节下标切片，可能从 UTF-8 多字节字符中间截断。
下游 BERT tokenizer 对非法 UTF-8 严格 fail-closed，正常中文文档可能因此随机失败。

本 RFC 将正则和 UTF-8 边界处理封装在框架内部，使业务与算子开发者继续只声明规则及
分块参数，不接触第三方正则句柄、字符解码细节或同步机制。

## 2. 设计范围与边界 (Scope & Non-Goals)

### 2.1 范围内 (In-Scope)

- [x] `TextRuleMatchNode` 使用固定且校验来源的 PCRE2 8-bit 实现 regex 策略。
- [x] 支持正/负 lookbehind、`(?<name>...)` 与 `(?P<name>...)` 命名捕获。
- [x] 编译错误和运行时 UTF-8/正则错误 fail-closed，不发布部分输出。
- [x] `TextChunkNode` 按 Unicode code point 计算 `chunk_size` 与 `overlap`。
- [x] 非法 UTF-8 输入 fail-closed，并保留 ASCII 输入和 provenance 行为。
- [x] 扩展既有 Node 与 tokenizer 测试，覆盖中文、emoji、重叠和 lookbehind 回归。

### 2.2 非目标 (Non-Goals / Out-of-Scope)

- 不修改 C ABI、Operator、Blackboard 类型、Node 端口或持久 Pipeline Schema。
- 不把分块单位扩大为 grapheme cluster、词元、句子或模型 token。
- 不向 Pipeline 作者暴露 PCRE2 编译/匹配选项或原生句柄。
- 不重构其他 Node 的并发和热更新机制。

## 3. 总体技术方案与架构设计 (Architecture & Technical Design)

### 3.1 架构分层映射 (4-Tier Mapping)

- **Layer 1**：无修改。
- **Layer 2**：无修改。
- **Layer 3**：两个通用文本 Node 使用内部正则与 UTF-8 支持代码，端口保持不变。
- **Layer 4**：BERT tokenizer 复用中性的 UTF-8 解码函数，保持模型 fail-closed 语义。

依赖方向保持 Layer 1 → Layer 2 → Layer 3 → Layer 4；Layer 3 不依赖业务 Adapter，
Layer 4 不引用 Node 或 Pipeline。

### 3.2 正则编译与匹配

`CompiledTextRegex` 是 Layer 3 私有、move-only 的 RAII 值对象：编译阶段拥有 PCRE2
code，运行阶段为每次请求创建独立 match data。Node 只调用 `Compile` 与 `Search`，
命名捕获编号由 PCRE2 pattern metadata 取得，不再扫描或改写规则字符串。

编译启用 `PCRE2_UTF | PCRE2_UCP`。非法表达式在 Init/Control 阶段拒绝；非法 UTF-8
或运行时资源错误在 Process 阶段返回稳定错误且不写 `matches`。

PCRE2 10.47 由 CMake FetchContent 下载官方 release 源码，只构建 8-bit 静态库，关闭
工具和上游测试，并使用 SHA-256 校验，不提交第三方源码或二进制。

### 3.3 UTF-8 分块

中性 UTF-8 支持函数先严格解码输入并生成 code point 边界偏移。`TextChunkNode` 以边界
索引实现窗口和 overlap，再按对应字节区间构造字符串，因此输出不会包含半个 code
point。空字符串仍产生一个空 chunk；非法输入返回专用错误码且不发布任何输出。

## 4. 关键设计考量与权衡 (Design Trade-offs & Invariants)

1. **成熟正则语义优于自制解析器**：lookbehind 与命名组交给 PCRE2 统一解析，避免继续
   扩展脆弱的字符串重写器。
2. **底层资源封装**：PCRE2 句柄只存在于私有 RAII 类；每次匹配独立分配 match data，
   编译结果可并发只读，`parallel_safe` 声明保持真实。
3. **明确 Unicode 单位**：`chunk_size` 与 `overlap` 从隐含字节单位收敛为 code point；
   ASCII 行为不变，多字节文本获得稳定、可解释的窗口大小。
4. **严格失败**：非法 regex 和 UTF-8 均不降级、不跳字节、不发布部分结果。
5. **兼容边界**：Node 类型、字段名、默认值、端口和输出 provenance 不变；多字节文本的
   chunk 数量变化属于本次修复的有意行为。

## 5. 测试与质量验收计划 (Testing & Verification Plan)

- [x] `TextRuleMatchNodeTest` 覆盖正/负 lookbehind、命名捕获、后续字面量 `>` 和错误规则。
- [x] `TextChunkNodeTest` 覆盖中英文、4-byte emoji、overlap、非法 UTF-8 和 provenance。
- [x] `OnnxAndEmbeddingModelTest` 覆盖分块边界产生的残缺/孤立续字节仍被拒绝。
- [x] 验证受影响 DocQA Pipeline 的 Catalog、Validator 与 plan。
- [x] 运行 keyword-match Demo 验证 lookbehind 规则端到端输出。
- [x] 交付脚本在提交前运行一次 `./scripts/run_all_tests.sh`。

## 6. 实施路线与里程碑 (Implementation Milestones)

1. [x] **阶段一：建立并索引 RFC，确定依赖和兼容边界**。
2. [x] **阶段二：实现正则 RAII 支持、UTF-8 公共解码与 Node 修复**。
3. [x] **阶段三：补齐聚焦回归并验证受影响 Pipeline/Demo**。
4. [x] **阶段四：由交付脚本在提交前运行统一完整质量门禁**。
5. [x] **阶段五：按用户授权创建 PR 并验证 CI**。

## 7. 变更记录 (Changelog)

| 日期 | 版本 | 变更内容 | 作者 |
| :--- | :--- | :--- | :--- |
| 2026-08-30 | v1.0.0 | 建立文本规则与 UTF-8 分块安全方案 | LLM-EdgeFlow Team |
| 2026-08-30 | v1.1.0 | 完成实现、聚焦验证并收口交付状态 | LLM-EdgeFlow Team |
