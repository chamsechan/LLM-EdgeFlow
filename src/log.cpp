#include "edgeflow/log.h"

#include <atomic>

namespace {

std::atomic<int>& GlobalLogLevel() {
  static std::atomic<int> level{ALG_BASE_LOG_DEFAULT_LEVEL};
  return level;
}

}  // namespace

extern "C" int AlgBase_setLogLevelByName(const char* name, int level) noexcept {
  (void)name;
  if (level < E_ALG_BASE_LOG_LEVEL_SILENT ||
      level > E_ALG_BASE_LOG_LEVEL_VERBOSE) {
    return -1;  // 公开 C 接口沿用 doc/logging.md 约定，不返回框架码
  }
  GlobalLogLevel().store(level, std::memory_order_release);
  return 0;
}

extern "C" int AlgBase_getLogLevelByName(const char* name) noexcept {
  (void)name;
  return GlobalLogLevel().load(std::memory_order_acquire);
}
