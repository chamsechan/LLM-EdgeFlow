#include "demo/common/demo_registry.h"

#include <iostream>

namespace alg_demo {

DemoRegistry& DemoRegistry::Instance() {
  static DemoRegistry instance;
  return instance;
}

bool DemoRegistry::Register(DemoDescriptor descriptor) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (descriptor.biz_name.empty()) {
    std::cerr << "[DemoRegistry ERROR] Empty biz_name provided" << std::endl;
    has_conflict_ = true;
    return false;
  }
  if (!descriptor.run) {
    std::cerr << "[DemoRegistry ERROR] Null run function for biz: "
              << descriptor.biz_name << std::endl;
    has_conflict_ = true;
    return false;
  }

  if (descriptors_.find(descriptor.biz_name) != descriptors_.end()) {
    std::cerr << "[DemoRegistry ERROR] Duplicate registration for biz: "
              << descriptor.biz_name << std::endl;
    has_conflict_ = true;
    return false;
  }

  descriptors_[descriptor.biz_name] = std::move(descriptor);
  return true;
}

const DemoDescriptor* DemoRegistry::Find(std::string_view biz_name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = descriptors_.find(std::string(biz_name));
  if (it != descriptors_.end()) {
    return &it->second;
  }
  return nullptr;
}

std::vector<DemoDescriptor> DemoRegistry::ListDescriptors() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<DemoDescriptor> list;
  list.reserve(descriptors_.size());
  for (const auto& [name, desc] : descriptors_) {
    list.push_back(desc);
  }
  return list;
}

std::vector<std::string> DemoRegistry::ListBizNames() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> names;
  names.reserve(descriptors_.size());
  for (const auto& [name, desc] : descriptors_) {
    names.push_back(name);
  }
  return names;
}

bool DemoRegistry::HasConflict() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return has_conflict_;
}

void DemoRegistry::ResetForTesting() {
  std::lock_guard<std::mutex> lock(mutex_);
  descriptors_.clear();
  has_conflict_ = false;
}

}  // namespace alg_demo
