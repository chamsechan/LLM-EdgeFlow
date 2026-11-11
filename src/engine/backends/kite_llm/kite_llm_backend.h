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
  /**
   * @brief 核对执行协议、设备与 run config，加载模型并选择对应的中性 Session。
   * 视觉 projector 相对 run config 目录解析，并在加载前检查文件有效性。
   * Session 接管运行时与模型句柄，失败时返回空指针并提供诊断。
   */
  std::shared_ptr<IBackendSession> Load(
      const BackendLoadSpec& spec,
      std::string* diagnostic = nullptr) noexcept override;
};

}  // namespace llm_edgeflow
