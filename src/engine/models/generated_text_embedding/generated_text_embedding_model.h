#pragma once

#include "engine/backend_interface.h"
#include "engine/model_identity.h"
#include "engine/model_interface.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {

// 对生成 token 的隐藏状态做池化。其向量空间不同于编码器 embedding：
// 更换模型、prompt、生成上限或池化方式时，调用方须评估检索质量并重建索引。
class GeneratedTextEmbeddingModel final
    : public ModelIdentity<GeneratedTextEmbeddingModel, IEmbeddingModel> {
 public:
  inline static constexpr char kImplName[] = "generated_text_embedding";
  static constexpr InferenceConcurrency kConcurrency =
      InferenceConcurrency::kConcurrent;
  static std::shared_ptr<IModel> Create(const ModelCreateContext& context,
                                        std::string* diagnostic);
  /**
   * @brief 为每条文本添加配置词缀，生成 token 隐藏状态并按 mean 或 last 池化。
   * 校验 token 与向量的数量、维度及有限性后，按配置归一化。
   * 立即 EOS 视为无效输出；任一条目失败时清空整批结果，成功时保留来源编号。
   */
  int Embed(const TextBatch& inputs, EmbeddingBatch* outputs,
            std::string* diagnostic = nullptr) noexcept override;

 private:
  std::shared_ptr<IGeneratedTokenEmbeddingSession> session_;
  std::string prompt_prefix_;
  std::string prompt_suffix_;
  std::string pooling_;
  int embedding_dim_ = 0;
  int max_tokens_ = 1;
  bool add_bos_ = false;
  bool normalize_ = true;
};

}  // namespace llm_edgeflow
