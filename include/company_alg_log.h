#ifndef COMPANY_ALG_LOG_H_
#define COMPANY_ALG_LOG_H_

#include "company_alg_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  E_ALG_BASE_LOG_LEVEL_FATAL = 0,
  E_ALG_BASE_LOG_LEVEL_ERROR = 1,
  E_ALG_BASE_LOG_LEVEL_WARNING = 2,
  E_ALG_BASE_LOG_LEVEL_INFO = 3,
  E_ALG_BASE_LOG_LEVEL_DEBUG = 4,
  E_ALG_BASE_LOG_LEVEL_VERBOSE = 5
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
 * @brief Set the process-wide log level.
 * @param name Compatibility name for the AlgBase logging contract.
 * @param level Integer log level in the inclusive range [0, 5].
 * @return 0 on success, -1 when level is outside [0, 5].
 */
COMPANY_ALG_API int AlgBase_setLogLevelByName(const char* name,
                                              int level) ALG_LOG_NOEXCEPT;

/**
 * @brief Return the current process-wide log level.
 * @param name Compatibility name for the AlgBase logging contract.
 */
COMPANY_ALG_API int AlgBase_getLogLevelByName(const char* name)
    ALG_LOG_NOEXCEPT;

/**
 * @brief Write one formatted log record to stderr.
 *
 * The function preserves fmt exactly and does not append a newline.
 */
ALG_LOG_PRINTF_ATTRIBUTE(3, 4)
COMPANY_ALG_API void AlgBase_logPrint(const char* level_label, const char* name,
                                      const char* fmt, ...) ALG_LOG_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#ifndef COMPANY_ALG_LOG_NAME
#define COMPANY_ALG_LOG_NAME "LLM_EDGEFLOW"
#endif

#if defined(ALG_BASE_LOG_PRINT) || defined(ALG_BASE_LOG_FATAL) ||   \
    defined(ALG_BASE_LOG_ERROR) || defined(ALG_BASE_LOG_WARNING) || \
    defined(ALG_BASE_LOG_INFO) || defined(ALG_BASE_LOG_DEBUG) ||    \
    defined(ALG_BASE_LOG_VERBOSE) || defined(ALG_LOG_FATAL) ||      \
    defined(ALG_LOG_ERROR) || defined(ALG_LOG_WARNING) ||           \
    defined(ALG_LOG_INFO) || defined(ALG_LOG_DEBUG) ||              \
    defined(ALG_LOG_VERBOSE)
#error "company_alg_log.h conflicts with an existing ALG_LOG_* definition"
#endif

#define ALG_BASE_LOG_PRINT(level_label, name, ...) \
  AlgBase_logPrint(level_label, name, __VA_ARGS__)

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

#define ALG_LOG_FATAL(...) ALG_BASE_LOG_FATAL(COMPANY_ALG_LOG_NAME, __VA_ARGS__)
#define ALG_LOG_ERROR(...) ALG_BASE_LOG_ERROR(COMPANY_ALG_LOG_NAME, __VA_ARGS__)
#define ALG_LOG_WARNING(...) \
  ALG_BASE_LOG_WARNING(COMPANY_ALG_LOG_NAME, __VA_ARGS__)
#define ALG_LOG_INFO(...) ALG_BASE_LOG_INFO(COMPANY_ALG_LOG_NAME, __VA_ARGS__)
#define ALG_LOG_DEBUG(...) ALG_BASE_LOG_DEBUG(COMPANY_ALG_LOG_NAME, __VA_ARGS__)
#define ALG_LOG_VERBOSE(...) \
  ALG_BASE_LOG_VERBOSE(COMPANY_ALG_LOG_NAME, __VA_ARGS__)

#endif  // COMPANY_ALG_LOG_H_
