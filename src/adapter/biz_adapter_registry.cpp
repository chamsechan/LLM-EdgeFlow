#include "adapter/biz_adapter_registry.h"

#include "core/pipeline_catalog.h"

namespace llm_edgeflow {

bool BizAdapterRegistry::RegisterAdapter(std::shared_ptr<IBizAdapter> adapter) {
  if (!adapter) {
    std::lock_guard<std::mutex> lock(mutex_);
    has_conflict_ = true;
    registration_errors_.push_back("Null adapter pointer passed to registry");
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  CompanyAlgBizType biz_type = adapter->BizType();

  if (biz_type == ALG_BIZ_TYPE_UNKNOWN) {
    has_conflict_ = true;
    std::string err = "Cannot register adapter '" +
                      std::string(adapter->AdapterName()) +
                      "' with ALG_BIZ_TYPE_UNKNOWN";
    registration_errors_.push_back(err);
    ALG_LOG_ERROR("[BizAdapterRegistry] %s\n", err.c_str());
    return false;
  }

  // 冲突与契约检查 0: 必须满足框架当前可执行的 Descriptor 策略 (RECHECK-003,
  // ADP-008)
  const auto& desc = adapter->GetDescriptor();
  if (desc.ownership_policy != OwnershipPolicy::kCopyIn) {
    has_conflict_ = true;
    std::string err = "Unsupported OwnershipPolicy in adapter '" +
                      std::string(adapter->AdapterName()) +
                      "': only kCopyIn is currently supported";
    registration_errors_.push_back(err);
    ALG_LOG_ERROR("[BizAdapterRegistry] %s\n", err.c_str());
    return false;
  }
  if (desc.thread_model != ThreadModel::kStatelessThreadSafe) {
    has_conflict_ = true;
    std::string err = "Unsupported ThreadModel in adapter '" +
                      std::string(adapter->AdapterName()) +
                      "': only kStatelessThreadSafe is currently supported";
    registration_errors_.push_back(err);
    ALG_LOG_ERROR("[BizAdapterRegistry] %s\n", err.c_str());
    return false;
  }
  if (desc.cardinality != OutputCardinality::kOneToOne) {
    has_conflict_ = true;
    std::string err = "Unsupported OutputCardinality in adapter '" +
                      std::string(adapter->AdapterName()) +
                      "': only kOneToOne is currently supported";
    registration_errors_.push_back(err);
    ALG_LOG_ERROR("[BizAdapterRegistry] %s\n", err.c_str());
    return false;
  }
  if (desc.biz_type != biz_type ||
      desc.adapter_name != adapter->AdapterName()) {
    has_conflict_ = true;
    std::string err =
        "Descriptor inconsistency for adapter '" +
        std::string(adapter->AdapterName()) +
        "': BizType/AdapterName mismatch between methods and descriptor";
    registration_errors_.push_back(err);
    ALG_LOG_ERROR("[BizAdapterRegistry] %s\n", err.c_str());
    return false;
  }

  // 冲突检查 1: 业务 ID 重复冲突
  auto it = adapters_.find(biz_type);
  if (it != adapters_.end()) {
    has_conflict_ = true;
    std::string err = "Conflict: BizType [" + std::to_string(biz_type) +
                      "] already registered by '" + it->second->AdapterName() +
                      "'. Cannot register '" + adapter->AdapterName() + "'.";
    registration_errors_.push_back(err);
    ALG_LOG_ERROR("[BizAdapterRegistry] %s\n", err.c_str());
    return false;
  }

  // 冲突检查 2: 业务名称重复冲突
  for (const auto& kv : adapters_) {
    if (kv.second->AdapterName() == std::string(adapter->AdapterName())) {
      has_conflict_ = true;
      std::string err = "Conflict: AdapterName '" +
                        std::string(adapter->AdapterName()) +
                        "' already registered under BizType [" +
                        std::to_string(kv.first) + "].";
      registration_errors_.push_back(err);
      ALG_LOG_ERROR("[BizAdapterRegistry] %s\n", err.c_str());
      return false;
    }
  }

  // 冲突检查 3: 业务 Pipeline 契约必须声明且在 Catalog 中无冲突
  if (desc.biz_definitions.empty()) {
    has_conflict_ = true;
    std::string err =
        "Adapter '" + std::string(adapter->AdapterName()) +
        "' must declare at least one BizDefinition in biz_definitions";
    registration_errors_.push_back(err);
    ALG_LOG_ERROR("[BizAdapterRegistry] %s\n", err.c_str());
    return false;
  }

  for (const auto& biz_definition : desc.biz_definitions) {
    if (biz_definition.biz_name.empty()) {
      has_conflict_ = true;
      std::string err = "Adapter '" + std::string(adapter->AdapterName()) +
                        "' has a biz contract with empty biz_name";
      registration_errors_.push_back(err);
      return false;
    }
  }
  if (!PipelineCatalog::RegisterBizDefinitions(desc.biz_definitions)) {
    has_conflict_ = true;
    std::string err = "Conflict: one or more pipeline biz names in adapter '" +
                      std::string(adapter->AdapterName()) +
                      "' are invalid, duplicated, or already registered";
    registration_errors_.push_back(err);
    ALG_LOG_ERROR("[BizAdapterRegistry] %s\n", err.c_str());
    return false;
  }

  adapters_[biz_type] = adapter;
  ALG_LOG_VERBOSE(
      "[BizAdapterRegistry] Registered adapter for BizType [%d]: %s (SDK ABI: "
      "%s)\n",
      static_cast<int>(biz_type), adapter->AdapterName(),
      adapter->GetDescriptor().sdk_abi_version.c_str());
  return true;
}

}  // namespace llm_edgeflow
