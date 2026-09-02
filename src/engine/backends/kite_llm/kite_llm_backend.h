#pragma once

#include <memory>
#include <string>

#include "engine/backend_interface.h"

namespace alg_framework {

class KiteLlmBackend final : public IInferenceBackend {
 public:
  inline static constexpr char kBackendType[] = "kite_llm";

  ~KiteLlmBackend() override = default;
  const std::string& BackendType() const noexcept override;
  std::shared_ptr<IBackendSession> Load(
      const BackendLoadSpec& spec,
      std::string* diagnostic = nullptr) noexcept override;
};

}  // namespace alg_framework
