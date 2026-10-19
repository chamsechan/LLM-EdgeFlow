#pragma once

#include <memory>
#include <string>

#include "engine/backend_identity.h"
#include "engine/backend_interface.h"

namespace llm_edgeflow {

class InferletBackend final : public BackendIdentity<InferletBackend> {
 public:
  inline static constexpr char kBackendType[] = "inferlet";

  ~InferletBackend() override = default;
  std::shared_ptr<IBackendSession> Load(
      const BackendLoadSpec& spec,
      std::string* diagnostic = nullptr) noexcept override;
};

}  // namespace llm_edgeflow
