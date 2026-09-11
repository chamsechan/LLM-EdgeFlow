#pragma once

#include <string>
#include <string_view>

namespace llm_edgeflow {

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

/**
 * @brief 从 BlackboardTypeTraits<T> 构造类型化 Blackboard
 * Key，未知类型编译期失败
 */
template <typename T>
constexpr BlackboardKey<T> MakeBlackboardKey(const char* name) {
  static_assert(
      std::string_view(BlackboardTypeTraits<T>::TypeName()) != "Unknown",
      "MakeBlackboardKey requires a registered BlackboardTypeTraits "
      "specialization");
  return BlackboardKey<T>{name, BlackboardTypeTraits<T>::TypeName()};
}

}  // namespace llm_edgeflow
