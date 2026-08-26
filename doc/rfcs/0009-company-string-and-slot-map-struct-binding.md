# RFC 0009: 统一 CompanyString 封装与槽位 Mapname 结构体映射契约

- **RFC 编号**：0009-company-string-and-slot-map-struct-binding
- **创建日期**：2026-08-26
- **文档状态**：Completed
- **关联分支**：`feat/architecture-contract-consolidation`
- **目标版本**：v2.2.0
- **负责人 / 作者**：LLM-EdgeFlow Team

---

## 1. 背景与动机 (Motivation & Context)

- **当前痛点**：
  1. 现有 C ABI 头文件（`include/company_alg_interface.h`）中，输入输出字符串混用了裸指针 `const char*` 和固定长度字符数组（如 `char answer_text[1024]`、`char match_result_json[2048]` 等），缺乏显式的缓冲区容量元数据，存在越界写风险，且无法支持零拷贝视图。
  2. 公司外部调度框架（Host / Platform Scheduler）统一采用槽位命名格式 `"xxx.mapname"`（例如 `"client_channel.keyword_in"` 中的 `keyword_in`），要求在 C ABI 头文件接口处显式维护 `mapname` 到对应解析 C 结构体的元信息映射，以便平台调度层和算法框架能够据此精准解析输入输出。
- **业务需求**：
  1. 统一封装 C 风格字符串结构体 `CompanyString`（包含 `data`, `length`, `capacity`），所有输入输出结构体中涉及字符串的字段均采用 `CompanyString*`（或 `const CompanyString*`）引用。
  2. 在 `include/company_alg_interface.h` 维护输入/输出槽位的 `mapname` 到 C 解析结构体的元数据映射表，使算法框架内部（如 `PlatformIoRegistry`）能够自动化按 `mapname` 进行动态解析与分发。
- **预期收益**：
  1. 提升 C ABI 内存安全性，彻底消除定长数组溢出风险与无效截断。
  2. 实现与公司外部调度框架的高内聚绑定，统一槽位路由与结构体反序列化标准。

---

## 2. 设计范围与边界 (Scope & Non-Goals)

### 2.1 范围内 (In-Scope)
- [x] 在 `include/company_alg_interface.h` 中定义 `CompanyString`，重构全部 7 个业务的输入输出结构体。
- [x] 在 `include/company_alg_interface.h` 中定义槽位映射元数据结构体 `CompanySlotStructMapping` 与标准 `mapname` 常量。
- [x] 重构 Layer 1 适配层（`src/adapter/adapters/*` 与 `PlatformIoRegistry`），支持通过 `mapname` 自动映射与 `CompanyString` 安全解包/打包。
- [x] 重构全量 Demo（`demo/`）与全量测试套件（`tests/`），全面适配 `CompanyString`。
- [x] 维持 C11 兼容性、6 导出函数 `noexcept` 异常安全屏障及四层架构隔离。

### 2.2 非目标 (Out-of-Scope)
- 不修改底层推理引擎（Layer 4）的硬件算子实现。
- 不引入 C++ STL 类型至 C ABI 头文件。

---

## 3. 总体技术方案与架构设计 (Architecture & Technical Design)

### 3.1 架构分层映射 (4-Tier Mapping)
- **Layer 1 (C ABI & Platform Adapter)**:
  - `include/company_alg_interface.h`：定义 `CompanyString`、7 对业务 C 结构体及 `CompanySlotStructMapping` 映射表。
  - `src/adapter/adapters/`：更新各业务 Adapter 的 `Unpack` / `Pack` 逻辑。
  - `src/adapter/platform_operator_adapter.cpp`：通过 `mapname` 解析输入输出。
- **Layer 2 (Pipeline & Blackboard)**:
  - `AlgContext` 保持内部强类型 `std::string` / `vector` 存储，由 Adapter 完成零拷贝或有界拷贝转换。
- **Layer 3 (Nodes)**:
  - 节点计算逻辑保持不变，继续使用 typed `BlackboardKey<T>`。
- **Layer 4 (Engines)**:
  - 引擎推理与批调度逻辑保持不变。

### 3.2 核心数据结构定义

```c
typedef struct {
    char*   data;        /* 字符缓冲区首地址 (以 '\0' 结尾) */
    size_t  length;      /* 字符串有效长度 (字节数，不含 '\0') */
    size_t  capacity;    /* 缓冲区最大容量 (字节数，含 '\0') */
} CompanyString;

typedef struct {
    const char* slot_mapname;    /* 槽位 mapname (如 "keyword_in", "doc_in") */
    CompanyAlgBizType biz_type;  /* 对应业务枚举 */
    const char* struct_name;     /* 对应的 C 结构体名称 */
    size_t struct_size;          /* 结构体内存大小 (sizeof) */
    int is_input;                /* 1: 输入结构体, 0: 输出结构体 */
} CompanySlotStructMapping;
```

---

## 4. 验证与门禁计划

1. **C11 严格合规性**：`gcc -std=c11 -pedantic -Wall -Werror` 验证 `test_c11_abi_compliance.c`。
2. **CTest 单元与集成测试**：全量 CTest 套件 100% PASS。
3. **6 阶段全量回归**：`./scripts/run_all_tests.sh` 100% PASS。
4. **Sanitizer 内存检测**：`./scripts/run_sanitizers.sh --fast` 100% PASS。
