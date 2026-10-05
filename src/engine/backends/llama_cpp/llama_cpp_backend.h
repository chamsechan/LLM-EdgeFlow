#pragma once

#include <memory>
#include <string>

#include "engine/backend_identity.h"
#include "engine/backend_interface.h"

namespace llm_edgeflow {

/**
 * @brief 面向中性文本生成协议的 llama.cpp GGUF 提供者。
 *
 * 厂商声明有意隐藏在实现文件中。本类不承担 chat 模板、采样、停止词或
 * 生成循环语义。
 */
class LlamaCppBackend final : public BackendIdentity<LlamaCppBackend> {
 public:
  inline static constexpr char kBackendType[] = "llama_cpp";

  ~LlamaCppBackend() override = default;

  std::shared_ptr<IBackendSession> Load(
      const BackendLoadSpec& spec,
      std::string* diagnostic = nullptr) noexcept override;
};

}  // namespace llm_edgeflow
