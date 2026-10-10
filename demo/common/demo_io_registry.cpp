#include "demo/common/demo_io_registry.h"

#include <algorithm>

namespace alg_demo {

DemoIoRegistry& DemoIoRegistry::Instance() {
  static DemoIoRegistry registry;
  return registry;
}

bool DemoIoRegistry::RegisterInput(std::string types, BuildRequestsFn build) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (types.empty() || !build ||
      !inputs_.emplace(std::move(types), build).second) {
    has_conflict_ = true;
    return false;
  }
  return true;
}

bool DemoIoRegistry::RegisterOutput(std::string type, ShowResultFn show) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (type.empty() || !show ||
      !outputs_.emplace(std::move(type), show).second) {
    has_conflict_ = true;
    return false;
  }
  return true;
}

BuildRequestsFn DemoIoRegistry::FindInput(const std::string& types) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = inputs_.find(types);
  return it == inputs_.end() ? nullptr : it->second;
}

ShowResultFn DemoIoRegistry::FindOutput(const std::string& type) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = outputs_.find(type);
  return it == outputs_.end() ? nullptr : it->second;
}

std::vector<std::string> DemoIoRegistry::ListInputs() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> types;
  for (const auto& item : inputs_) types.push_back(item.first);
  std::sort(types.begin(), types.end());
  return types;
}

std::vector<std::string> DemoIoRegistry::ListOutputs() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> types;
  for (const auto& item : outputs_) types.push_back(item.first);
  std::sort(types.begin(), types.end());
  return types;
}

bool DemoIoRegistry::HasConflict() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return has_conflict_;
}

}  // namespace alg_demo
