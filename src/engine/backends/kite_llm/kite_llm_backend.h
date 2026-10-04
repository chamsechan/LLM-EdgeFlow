#pragma once

#include <memory>
#include <string>

#include "engine/backend_identity.h"
#include "engine/backend_interface.h"

namespace llm_edgeflow {

class KiteLlmBackend final : public BackendIdentity<KiteLlmBackend> {
 public:
  inline static constexpr char kBackendType[] = "kite_llm";

  ~KiteLlmBackend() override = default;
  std::shared_ptr<IBackendSession> Load(
      const BackendLoadSpec& spec,
      std::string* diagnostic = nullptr) noexcept override;
};

}  // namespace llm_edgeflow
