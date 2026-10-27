#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace llm_edgeflow {

/**
 * @brief 遵循 BERT / BGE 规范的 WordPiece 分词器
 *
 * - 从 vocab.txt 加载词表，校验必需的特殊 token：[PAD]、[UNK]、[CLS]、[SEP]。
 * - 严格校验 UTF-8，处理 CJK 字符、标点和空白切分及大小写（do_lower_case）。
 * - 非法 UTF-8 序列返回 false（fail-closed），不静默跳过。
 * - 按贪心最长匹配做 WordPiece 切分（## 前缀）。
 * - 产出 input_ids 与 attention_mask，并截断或填充至 max_tokens。
 */
class BertWordPieceTokenizer {
 public:
  BertWordPieceTokenizer() = default;
  ~BertWordPieceTokenizer() = default;

  /**
   * @brief 从指定词表文件加载
   * @param vocab_file_path 词表绝对路径或安全解析后的路径
   * @param do_lower_case 是否转换为小写
   * @param diagnostic 错误诊断信息
   * @return true 成功加载并验证特殊 token
   */
  bool Load(const std::string& vocab_file_path, bool do_lower_case,
            std::string* diagnostic = nullptr);

  /**
   * @brief 对输入文本进行分词、截断并填充至 max_tokens
   * @param text 输入文本
   * @param max_tokens 目标序列长度
   * @param input_ids 产出的 token ID 数组（大小为 max_tokens）
   * @param attention_mask 产出的 attention mask 数组（大小为 max_tokens）
   * @param diagnostic 错误诊断信息（如非法 UTF-8）
   * @return true 分词成功，false 编码异常（如非法 UTF-8）
   */
  bool Encode(const std::string& text, size_t max_tokens,
              std::vector<int64_t>* input_ids,
              std::vector<int64_t>* attention_mask,
              std::string* diagnostic = nullptr) const;

  /**
   * @brief 对 (Query, Candidate) 样本对进行分词、截断并填充至 max_tokens
   * 遵循 BERT Cross-Encoder 规范：[CLS] query [SEP] candidate [SEP] [PAD]...
   * token_type_ids: query 侧为 0，candidate 与尾部 [SEP] 侧为 1，padding 为 0
   * 截断策略：longest-first 优先截断较长一侧，长度相同时优先截断 candidate
   */
  bool EncodePair(const std::string& query, const std::string& candidate,
                  size_t max_tokens, std::vector<int64_t>* input_ids,
                  std::vector<int64_t>* attention_mask,
                  std::vector<int64_t>* token_type_ids,
                  std::string* diagnostic = nullptr) const;

 private:
  bool InitializeVocab(const std::vector<std::string>& lines,
                       std::string* diagnostic);

  bool BasicTokenize(const std::string& text, std::vector<std::string>* words,
                     std::string* diagnostic) const;
  std::vector<int64_t> WordPieceTokenize(
      const std::vector<std::string>& words) const;

  std::unordered_map<std::string, int64_t> vocab_;
  bool do_lower_case_ = true;
  bool is_loaded_ = false;

  int64_t pad_token_id_ = 0;
  int64_t unk_token_id_ = 100;
  int64_t cls_token_id_ = 101;
  int64_t sep_token_id_ = 102;
};

}  // namespace llm_edgeflow
