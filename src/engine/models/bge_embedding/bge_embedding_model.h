#pragma once

#include <memory>
#include <string>
#include <vector>

#include "engine/backend_interface.h"
#include "engine/fixed_batch_executor.h"
#include "engine/model_identity.h"
#include "engine/model_interface.h"
#include "engine/model_registry.h"
#include "engine/models/bge_common/bert_wordpiece_tokenizer.h"

namespace llm_edgeflow {

/**
 * @brief BGE 文本向量模型（ITensorGraphSession + WordPiece 分词器）
 *
 * 架构隔离：
 * - 纯 Model 层实现，继承 IEmbeddingModel；
 * - 只依赖中性张量图协议 ITensorGraphSession，不引用第三方头文件；
 * - 加载并验证声明的 WordPiece 词表，执行真实 WordPiece 分词与 padding；
 * - 负责池化 (Pooling: CLS / Mean) 与 L2 归一化；
 * - 使用 FixedBatchExecutor 驱动批次并保持 (req_id, sub_id) 溯源。
 */
class BgeEmbeddingModel final
    : public ModelIdentity<BgeEmbeddingModel, IEmbeddingModel> {
 public:
  inline static constexpr char kImplName[] = "bge_embedding";
  static constexpr InferenceConcurrency kConcurrency =
      InferenceConcurrency::kConcurrent;

  /**
   * @brief 从 TensorGraph 的固定形状和已解析参数确定编码长度与向量维度，
   * 校验 BERT 输入和指定输出的元数据后加载词表并创建模型。
   * 资源形状、配置或词表不匹配时返回空指针。
   */
  static std::shared_ptr<IModel> Create(const ModelCreateContext& ctx,
                                        std::string* diagnostic);

  BgeEmbeddingModel(std::shared_ptr<ITensorGraphSession> session,
                    BertWordPieceTokenizer tokenizer, size_t max_tokens,
                    std::string pooling, std::string output_name,
                    size_t embedding_dim, bool normalize);

  ~BgeEmbeddingModel() override = default;

  int Embed(const TextBatch& inputs, EmbeddingBatch* outputs,
            std::string* diagnostic = nullptr) noexcept override;

 private:
  /**
   * @brief 为一个执行切片编码文本并补齐空文本，运行 TensorGraph
   * 后提取指定输出。三维输出按 attention mask 做 mean 池化或取
   * CLS，二维输出直接作为向量。返回包含填充项的 execution_count
   * 个向量；去填充和来源恢复由调用方负责。
   */
  int RawEmbedSlice(const TextBatch& all_inputs, const BatchSlice& slice,
                    std::vector<std::vector<float>>* batch_embeddings,
                    bool normalize_flag, std::string* diagnostic) noexcept;

  std::shared_ptr<ITensorGraphSession> session_;
  BertWordPieceTokenizer tokenizer_;
  size_t max_tokens_ = 512;
  std::string pooling_ = "cls";
  std::string output_name_ = "last_hidden_state";
  size_t embedding_dim_ = 384;
  bool normalize_ = true;
};

}  // namespace llm_edgeflow
