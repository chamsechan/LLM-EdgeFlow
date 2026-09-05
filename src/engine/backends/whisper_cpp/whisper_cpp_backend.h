#pragma once

#include <functional>
#include <memory>
#include <string>

#include "engine/backend_interface.h"

namespace llm_edgeflow {

class WhisperCppBackend final : public IInferenceBackend {
 public:
  inline static constexpr char kBackendType[] = "whisper_cpp";

  ~WhisperCppBackend() override = default;

  const std::string& BackendType() const noexcept override;

  std::shared_ptr<IBackendSession> Load(
      const BackendLoadSpec& spec,
      std::string* diagnostic = nullptr) noexcept override;

  void SetLoadHook(std::function<void()> hook) noexcept {
    test_load_hook_ = std::move(hook);
  }

 private:
  std::function<void()> test_load_hook_;
};

}  // namespace llm_edgeflow
