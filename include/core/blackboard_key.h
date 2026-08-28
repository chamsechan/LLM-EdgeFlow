#pragma once

#include <string>

namespace alg_framework {

/**
 * @brief 类型化 Blackboard Key。名称与 C++ 类型共同构成节点端口契约。
 */
template <typename T>
struct BlackboardKey {
  const char* name;
  const char* type_id;
};

/**
 * @brief 编译期 Blackboard 类型标识萃取器 (SSOT Type Traits)
 */
template <typename T>
struct BlackboardTypeTraits {
  static constexpr const char* TypeName() { return "Unknown"; }
};

}  // namespace alg_framework
