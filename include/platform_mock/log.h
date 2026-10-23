#ifndef EDGEFLOW_PLATFORM_MOCK_LOG_H_
#define EDGEFLOW_PLATFORM_MOCK_LOG_H_

#include <stdarg.h>
#include <stdio.h>

#include "edgeflow/export.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ALG_BASE_LOG_LEVEL_SILENT 0
#define ALG_BASE_LOG_LEVEL_FATAL 1
#define ALG_BASE_LOG_LEVEL_ERROR 2
#define ALG_BASE_LOG_LEVEL_WARNING 3
#define ALG_BASE_LOG_LEVEL_INFO 4
#define ALG_BASE_LOG_LEVEL_DEBUG 5
#define ALG_BASE_LOG_LEVEL_VERBOSE 6

typedef enum AlgBaseLogLevel {
  E_ALG_BASE_LOG_LEVEL_SILENT = ALG_BASE_LOG_LEVEL_SILENT,    ///< 静默模式
  E_ALG_BASE_LOG_LEVEL_FATAL = ALG_BASE_LOG_LEVEL_FATAL,      ///< 致命错误
  E_ALG_BASE_LOG_LEVEL_ERROR = ALG_BASE_LOG_LEVEL_ERROR,      ///< 错误消息
  E_ALG_BASE_LOG_LEVEL_WARNING = ALG_BASE_LOG_LEVEL_WARNING,  ///< 警告消息
  E_ALG_BASE_LOG_LEVEL_INFO = ALG_BASE_LOG_LEVEL_INFO,        ///< 关键信息
  E_ALG_BASE_LOG_LEVEL_DEBUG = ALG_BASE_LOG_LEVEL_DEBUG,      ///< 调试信息
  E_ALG_BASE_LOG_LEVEL_VERBOSE = ALG_BASE_LOG_LEVEL_VERBOSE   ///< 详细信息
} AlgBaseLogLevel;

#define ALG_BASE_LOG_DEFAULT_LEVEL E_ALG_BASE_LOG_LEVEL_WARNING

#ifdef __cplusplus
#define ALG_LOG_NOEXCEPT noexcept
#else
#define ALG_LOG_NOEXCEPT
#endif

#if defined(__GNUC__) || defined(__clang__)
#define ALG_LOG_PRINTF_ATTRIBUTE(format_index, first_argument) \
  __attribute__((format(printf, format_index, first_argument)))
#else
#define ALG_LOG_PRINTF_ATTRIBUTE(format_index, first_argument)
#endif

/**
 * @brief 设置进程级日志级别。
 * @param name 为兼容 AlgBase 日志契约保留的名称。
 * @param level 整数日志级别，闭区间 [0, 6]；0 关闭全部等级日志。
 * @return 成功返回 0；level 超出 [0, 6] 时返回 -1 并保留原阈值。
 */
EDGEFLOW_API int AlgBase_setLogLevelByName(const char* name,
                                           int level) ALG_LOG_NOEXCEPT;

/**
 * @brief 返回当前进程级日志级别。
 * @param name 为兼容 AlgBase 日志契约保留的名称。
 */
EDGEFLOW_API int AlgBase_getLogLevelByName(const char* name) ALG_LOG_NOEXCEPT;

#ifdef __cplusplus
}
#endif

// 外网开发用打印替身。正式平台接入时由平台日志实现替换。
// 只提供基本 stderr 输出；不承诺记录原子性或平台的输出格式。
ALG_LOG_PRINTF_ATTRIBUTE(3, 4)
static inline void MockLogWriteRecord(const char* level_label, const char* name,
                                      const char* fmt, ...) ALG_LOG_NOEXCEPT {
  if (!fmt) return;
  fprintf(stderr, "[%s][%s] ", level_label ? level_label : "Unknown",
          (name && name[0]) ? name : "LLM_EDGEFLOW");
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
}

#ifndef EDGEFLOW_LOG_NAME
#define EDGEFLOW_LOG_NAME "LLM_EDGEFLOW"
#endif

#if defined(ALG_BASE_LOG_PRINT) || defined(ALG_BASE_LOG_FATAL) ||   \
    defined(ALG_BASE_LOG_ERROR) || defined(ALG_BASE_LOG_WARNING) || \
    defined(ALG_BASE_LOG_INFO) || defined(ALG_BASE_LOG_DEBUG) ||    \
    defined(ALG_BASE_LOG_VERBOSE) || defined(ALG_LOG_FATAL) ||      \
    defined(ALG_LOG_ERROR) || defined(ALG_LOG_WARNING) ||           \
    defined(ALG_LOG_INFO) || defined(ALG_LOG_DEBUG) ||              \
    defined(ALG_LOG_VERBOSE)
#error "edgeflow/log.h conflicts with an existing ALG_LOG_* definition"
#endif

#define ALG_BASE_LOG_PRINT(level_label, name, ...) \
  MockLogWriteRecord(level_label, name, __VA_ARGS__)

#define ALG_BASE_LOG_IMPL(level, level_label, name, ...)  \
  do {                                                    \
    if ((level) <= AlgBase_getLogLevelByName(name)) {     \
      ALG_BASE_LOG_PRINT(level_label, name, __VA_ARGS__); \
    }                                                     \
  } while (0)

#define ALG_BASE_LOG_FATAL(name, ...) \
  ALG_BASE_LOG_IMPL(E_ALG_BASE_LOG_LEVEL_FATAL, "Fatal", name, __VA_ARGS__)
#define ALG_BASE_LOG_ERROR(name, ...) \
  ALG_BASE_LOG_IMPL(E_ALG_BASE_LOG_LEVEL_ERROR, "Error", name, __VA_ARGS__)
#define ALG_BASE_LOG_WARNING(name, ...) \
  ALG_BASE_LOG_IMPL(E_ALG_BASE_LOG_LEVEL_WARNING, "Warning", name, __VA_ARGS__)
#define ALG_BASE_LOG_INFO(name, ...) \
  ALG_BASE_LOG_IMPL(E_ALG_BASE_LOG_LEVEL_INFO, "Info", name, __VA_ARGS__)
#define ALG_BASE_LOG_DEBUG(name, ...) \
  ALG_BASE_LOG_IMPL(E_ALG_BASE_LOG_LEVEL_DEBUG, "Debug", name, __VA_ARGS__)
#define ALG_BASE_LOG_VERBOSE(name, ...) \
  ALG_BASE_LOG_IMPL(E_ALG_BASE_LOG_LEVEL_VERBOSE, "Verbose", name, __VA_ARGS__)

#define ALG_LOG_FATAL(...) ALG_BASE_LOG_FATAL(EDGEFLOW_LOG_NAME, __VA_ARGS__)
#define ALG_LOG_ERROR(...) ALG_BASE_LOG_ERROR(EDGEFLOW_LOG_NAME, __VA_ARGS__)
#define ALG_LOG_WARNING(...) \
  ALG_BASE_LOG_WARNING(EDGEFLOW_LOG_NAME, __VA_ARGS__)
#define ALG_LOG_INFO(...) ALG_BASE_LOG_INFO(EDGEFLOW_LOG_NAME, __VA_ARGS__)
#define ALG_LOG_DEBUG(...) ALG_BASE_LOG_DEBUG(EDGEFLOW_LOG_NAME, __VA_ARGS__)
#define ALG_LOG_VERBOSE(...) \
  ALG_BASE_LOG_VERBOSE(EDGEFLOW_LOG_NAME, __VA_ARGS__)

#endif  // EDGEFLOW_PLATFORM_MOCK_LOG_H_
