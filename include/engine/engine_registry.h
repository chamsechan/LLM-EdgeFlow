#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "engine/engine_interface.h"

namespace alg_framework {

class EngineFactory {
 public:
  using CreatorFunc = std::function<std::unique_ptr<IModelEngine>()>;

  static EngineFactory& Instance() {
    static EngineFactory instance;
    return instance;
  }

  void Register(const std::string& engine_type, CreatorFunc creator) {
    creators_[engine_type] = creator;
  }

  std::unique_ptr<IModelEngine> Create(const std::string& engine_type) {
    auto it = creators_.find(engine_type);
    if (it == creators_.end()) return nullptr;
    return it->second();
  }

 private:
  std::unordered_map<std::string, CreatorFunc> creators_;
};

template <typename T>
class EngineRegisterHelper {
 public:
  EngineRegisterHelper(const std::string& engine_type) {
    EngineFactory::Instance().Register(engine_type,
                                       []() { return std::make_unique<T>(); });
  }
};

#define REGISTER_ENGINE(EngineTypeStr, ClassName)       \
  static alg_framework::EngineRegisterHelper<ClassName> \
      g_reg_engine_##ClassName(EngineTypeStr)

}  // namespace alg_framework
