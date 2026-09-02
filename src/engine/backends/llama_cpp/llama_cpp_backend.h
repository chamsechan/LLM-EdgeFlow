#pragma once

#include <memory>
#include <string>

#include "engine/backend_interface.h"

namespace llm_edgeflow {

/**
 * @brief llama.cpp GGUF provider for the neutral text-generation protocol.
 *
 * Vendor declarations are intentionally hidden in the implementation file.
 * This class owns no chat template, sampling, stop-word, or generation-loop
 * semantics.
 */
class LlamaCppBackend final : public IInferenceBackend {
 public:
  inline static constexpr char kBackendType[] = "llama_cpp";

  ~LlamaCppBackend() override = default;

  const std::string& BackendType() const noexcept override;

  std::shared_ptr<IBackendSession> Load(
      const BackendLoadSpec& spec,
      std::string* diagnostic = nullptr) noexcept override;
};

}  // namespace llm_edgeflow
