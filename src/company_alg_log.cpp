#include "company_alg_log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace {

std::atomic<int>& GlobalLogLevel() {
  static std::atomic<int> level{ALG_BASE_LOG_DEFAULT_LEVEL};
  return level;
}

std::mutex& GlobalLogMutex() {
  static std::mutex mutex;
  return mutex;
}

void PrintRecord(const char* level_label, const char* name, const char* fmt,
                 va_list args) {
  const char* safe_level = level_label ? level_label : "Unknown";
  const char* safe_name = (name && name[0] != '\0') ? name : "LLM_EDGEFLOW";
  std::fprintf(stderr, "[%s][%s] ", safe_level, safe_name);
  std::vfprintf(stderr, fmt, args);
  if (std::strcmp(safe_level, "Fatal") == 0) {
    std::fflush(stderr);
  }
}

}  // namespace

extern "C" int AlgBase_setLogLevelByName(const char* name, int level) noexcept {
  (void)name;
  if (level < E_ALG_BASE_LOG_LEVEL_FATAL ||
      level > E_ALG_BASE_LOG_LEVEL_VERBOSE) {
    return -1;
  }
  GlobalLogLevel().store(level, std::memory_order_release);
  return 0;
}

extern "C" int AlgBase_getLogLevelByName(const char* name) noexcept {
  (void)name;
  return GlobalLogLevel().load(std::memory_order_acquire);
}

extern "C" void AlgBase_logPrint(const char* level_label, const char* name,
                                 const char* fmt, ...) noexcept {
  if (!fmt) return;

  va_list args;
  va_start(args, fmt);
  try {
    const std::lock_guard<std::mutex> lock(GlobalLogMutex());
    PrintRecord(level_label, name, fmt, args);
  } catch (...) {
    PrintRecord(level_label, name, fmt, args);
  }
  va_end(args);
}
