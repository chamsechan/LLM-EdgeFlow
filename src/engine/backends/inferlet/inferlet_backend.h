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
  /**
   * @brief 核对执行协议、设备和资源文件，同步加载模型并创建对应协议的 Session。
   * 视觉协议还要求有效的 projector，按 run config 所在目录解析其引用。
   * Session 接管运行时与模型句柄；加载失败返回空指针并释放已创建资源。
   */
  std::shared_ptr<IBackendSession> Load(
      const BackendLoadSpec& spec,
      std::string* diagnostic = nullptr) noexcept override;
};

}  // namespace llm_edgeflow
