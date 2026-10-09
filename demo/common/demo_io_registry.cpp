#include "demo/common/demo_io_registry.h"

#include <iostream>

namespace alg_demo {

template <typename Fn, typename Tag>
DemoCarrierRegistry<Fn, Tag>& DemoCarrierRegistry<Fn, Tag>::Instance() {
  static DemoCarrierRegistry instance;
  return instance;
}

template <typename Fn, typename Tag>
bool DemoCarrierRegistry<Fn, Tag>::Register(std::string carrier, Fn fn) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (carrier.empty()) {
    std::cerr << "[DemoIoRegistry ERROR] Empty carrier name provided"
              << std::endl;
    has_conflict_ = true;
    return false;
  }
  if (!fn) {
    std::cerr << "[DemoIoRegistry ERROR] Null callback for carrier: " << carrier
              << std::endl;
    has_conflict_ = true;
    return false;
  }
  if (entries_.find(carrier) != entries_.end()) {
    std::cerr << "[DemoIoRegistry ERROR] Duplicate registration for carrier: "
              << carrier << std::endl;
    has_conflict_ = true;
    return false;
  }
  entries_.emplace(std::move(carrier), fn);
  return true;
}

template <typename Fn, typename Tag>
Fn DemoCarrierRegistry<Fn, Tag>::Find(std::string_view carrier) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(carrier);
  return it == entries_.end() ? nullptr : it->second;
}

template <typename Fn, typename Tag>
std::vector<std::string> DemoCarrierRegistry<Fn, Tag>::List() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> names;
  names.reserve(entries_.size());
  for (const auto& entry : entries_) names.push_back(entry.first);
  return names;
}

template <typename Fn, typename Tag>
bool DemoCarrierRegistry<Fn, Tag>::HasConflict() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return has_conflict_;
}

template <typename Fn, typename Tag>
void DemoCarrierRegistry<Fn, Tag>::ResetForTesting() {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
  has_conflict_ = false;
}

template <typename Fn, typename Tag>
std::map<std::string, Fn> DemoCarrierRegistry<Fn, Tag>::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::map<std::string, Fn>(entries_.begin(), entries_.end());
}

template class DemoCarrierRegistry<BuildRequestsFn, DemoInputTag>;
template class DemoCarrierRegistry<ShowResultFn, DemoOutputTag>;

std::string DemoInputCarrierKey(
    const std::vector<llm_edgeflow::operator_api::OperatorIoEntry>& inputs) {
  std::string key;
  for (const auto& input : inputs) {
    if (!key.empty()) key += ",";
    key += input.type_name;
  }
  return key;
}

std::string DemoIoKey(
    const llm_edgeflow::operator_api::OperatorIoEntry& entry) {
  return entry.name + "." + entry.type;
}

}  // namespace alg_demo
