#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "core/node_base.h"

namespace alg_framework {

class NodeFactory {
 public:
  using CreatorFunc = std::function<std::unique_ptr<INode>()>;

  static NodeFactory& Instance() {
    static NodeFactory instance;
    return instance;
  }

  void Register(const std::string& node_type, CreatorFunc creator) {
    creators_[node_type] = creator;
  }

  std::unique_ptr<INode> Create(const std::string& node_type) {
    auto it = creators_.find(node_type);
    if (it == creators_.end()) return nullptr;
    return it->second();
  }

 private:
  std::unordered_map<std::string, CreatorFunc> creators_;
};

template <typename T>
class NodeRegisterHelper {
 public:
  NodeRegisterHelper(const std::string& node_type) {
    NodeFactory::Instance().Register(node_type,
                                     []() { return std::make_unique<T>(); });
  }
};

#define REGISTER_NODE(NodeType)                                             \
  static alg_framework::NodeRegisterHelper<NodeType> g_reg_node_##NodeType( \
      #NodeType)

}  // namespace alg_framework
