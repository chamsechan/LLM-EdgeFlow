#pragma once

#include <cstdint>

namespace alg_demo {

// 可执行程序与工具投影共用的 Demo/Profile 默认值。
inline constexpr int kDemoBatchSize = 1;
inline constexpr int kDemoDeviceId = 0;
inline constexpr char kDemoChip[] = "cpu";
inline constexpr uint32_t kDemoDepth = 1;

}  // namespace alg_demo
