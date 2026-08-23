#pragma once

#include <string>

namespace alg_framework {

/**
 * @brief 类型化 Blackboard Key。名称与 C++ 类型共同构成节点端口契约。
 */
template <typename T>
struct BlackboardKey {
  const char* name;
};

}  // namespace alg_framework
