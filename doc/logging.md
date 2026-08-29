# 公共日志 API

LLM-EdgeFlow 在 `include/company_alg_log.h` 中提供纯 C11 兼容的公共日志接口。它不暴露 STL 或第三方日志类型，可以由 C 或 C++ 调用。

## 基本使用

```c
#include "company_alg_log.h"

AlgBase_setLogLevelByName("LLM_EDGEFLOW",
                          E_ALG_BASE_LOG_LEVEL_DEBUG);
ALG_LOG_DEBUG("hello[%d], reasoning[%d]\n", session_id, reasoning);
```

`ALG_LOG_*` 默认使用 `LLM_EDGEFLOW` 作为日志名称。如需为接入方设置不同名称，必须在包含头文件前定义：

```c
#define COMPANY_ALG_LOG_NAME "MY_ALGORITHM"
#include "company_alg_log.h"
```

## 日志等级

| 数值 | 等级 | 含义 |
| :---: | :--- | :--- |
| 0 | `FATAL` | 致命错误；只记录，不终止进程 |
| 1 | `ERROR` | 当前操作失败 |
| 2 | `WARNING` | 默认阈值；异常但可继续 |
| 3 | `INFO` | 关键生命周期信息 |
| 4 | `DEBUG` | 调试细节 |
| 5 | `VERBOSE` | 高频诊断信息 |

日志阈值是线程安全的进程级状态。`AlgBase_setLogLevelByName` 接受 `0..5`，成功返回 `0`；非法值返回 `-1` 并保留原阈值。日志写入 `stderr`，接口不会自动追加换行。

## Demo 环境变量

Demo 启动时读取 `LLMEDGEFLOW_LEVEL`：

```bash
LLMEDGEFLOW_LEVEL=4 ./build/alg_demo --suite smoke
```

环境变量缺失或不是 `0..5` 的整数时，Demo 保留默认的 `WARNING` 阈值。

## 接口约束

- 公共日志函数遵循 C ABI；C++ 声明带有 `noexcept`。
- `FATAL` 仅表达严重程度，不调用 `abort` 或 `exit`。
- `name` 参数为兼容性命名字段，日志阈值仍是进程级而非按名称隔离。
- 头文件会拒绝与已有 `ALG_LOG_*` 宏发生静默冲突。

接口定义以 [`company_alg_log.h`](../include/company_alg_log.h) 为准，设计依据参见 [RFC-0014](rfcs/0014-public-log-api.md)。
