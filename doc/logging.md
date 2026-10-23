# 公共日志 API

`include/edgeflow/log.h` 是统一日志包含入口，当前转发到
[`platform_mock/log.h`](../include/platform_mock/log.h)，供外网开发、Demo 和测试使用。
本地替身兼容 C11，C 和 C++ 均可调用。`ALG_LOG_*` 的正式实现由平台提供；
进入授权内网后，核对真实日志头和接口，再替换该入口的转发及本地等级函数实现。
本页的进程级阈值和输出行为描述当前替身，不代表平台日志的实现承诺。

## 基本使用

```c
#include "edgeflow/log.h"

AlgBase_setLogLevelByName("LLM_EDGEFLOW",
                          E_ALG_BASE_LOG_LEVEL_DEBUG);
ALG_LOG_DEBUG("hello[%d], reasoning[%d]\n", session_id, reasoning);
```

`ALG_LOG_*` 默认使用 `LLM_EDGEFLOW` 作为日志名称。如需为接入方设置不同名称，必须在包含头文件前定义：

```c
#define EDGEFLOW_LOG_NAME "MY_ALGORITHM"
#include "edgeflow/log.h"
```

## 日志等级

| 数值 | 等级 | 含义 |
| :---: | :--- | :--- |
| 0 | `SILENT` | 静默模式，关闭全部等级日志 |
| 1 | `FATAL` | 致命错误；只记录，不终止进程 |
| 2 | `ERROR` | 当前操作失败 |
| 3 | `WARNING` | 默认阈值；异常但可继续 |
| 4 | `INFO` | 关键生命周期信息 |
| 5 | `DEBUG` | 调试细节 |
| 6 | `VERBOSE` | 高频诊断信息 |

`enum AlgBaseLogLevel`（也可用 typedef 名 `AlgBaseLogLevel`）中的
`E_ALG_BASE_LOG_LEVEL_*` 映射到同名 `ALG_BASE_LOG_LEVEL_*` 数值宏。
日志阈值对整个进程生效，支持线程安全访问。`AlgBase_setLogLevelByName` 接受 `0..6`。
设置成功时返回 `0`。值非法时返回 `-1`，并保留原阈值。
六个等级宏仅在日志等级不大于阈值时输出，被过滤日志的格式化参数不求值。
`SILENT` 是阈值，没有对应的打印宏；设置为 `0` 时连 `FATAL` 也不输出。
本地替身用 `fprintf` / `vfprintf` 写入 `stderr`，不自动添加换行。
替身不提供完整记录的并发原子性保证；输出格式、刷新策略及线程同步由正式平台实现负责。

## Demo 环境变量

Demo 启动时读取 `LLMEDGEFLOW_LEVEL`：

```bash
LLMEDGEFLOW_LEVEL=5 ./build/alg_demo --suite smoke
```

环境变量使用单个数字 `0..6`；`0` 关闭等级日志，`6` 启用全部等级。
环境变量缺失或无效时，Demo 保留当前阈值（启动默认值为 `WARNING`）。

## 接口约束

- SDK 仅导出 `AlgBase_setLogLevelByName`、`AlgBase_getLogLevelByName` 两个日志函数，
  遵循 C ABI，C++ 声明带有 `noexcept`。打印由头文件内实现支持，不提供导出的打印函数。
- 使用 `ALG_LOG_*` 或带名称的 `ALG_BASE_LOG_*` 等级宏记录日志。
  底层 `ALG_BASE_LOG_PRINT` 宏直接输出，不做等级过滤，通常应使用等级宏。
- 本地打印替身直接使用 stdio，保留 printf 格式检查；空格式指针不输出。
- `FATAL` 仅表达严重程度，不调用 `abort` 或 `exit`。
- `name` 参数不用于隔离日志阈值。所有名称共用进程级阈值。
- 头文件会拒绝与已有 `ALG_LOG_*` 宏发生静默冲突。

## 测试边界

- 本地等级函数保留最小的设置、读取和 `0/-1` 返回值检查。
- Demo 验证环境变量解析；Operator 验证框架选择的等级、消息内容、记录次数及诊断保留，
  不匹配平台日志前缀或换行格式。
- 保留日志入口的 C11 编译、头文件可见性和 SDK 导出符号检查。
- 平台日志的格式化、名称覆盖、长日志、并发输出及参数求值不在本仓库重复测试。

包含入口见 [`edgeflow/log.h`](../include/edgeflow/log.h)。
