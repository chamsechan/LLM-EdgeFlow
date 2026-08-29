#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace alg_framework {

/**
 * @brief 遵循 BERT / BGE 规范的 WordPiece 分词器
 *
 * 特性：
 * - 从 vocab.txt 文件加载词表并校验必填特殊 token ([PAD], [UNK], [CLS],
 * [SEP])；
 * - 支持 UTF-8 严格校验与 CJK 字符分词、标点符号切分、空格切分与大小写处理
 * (do_lower_case)；
 * - 非法 UTF-8 序列严格 fail-closed 返回 false，禁止静默跳过；
 * - 支持贪心最长匹配 WordPiece (## 前缀切分)；
 * - 产出 input_ids 与 attention_mask，执行截断与填充至 max_length。
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
   * @brief 从内存中的 token 列表加载 (供测试使用)
   */
  bool LoadFromTokens(const std::vector<std::string>& tokens,
                      bool do_lower_case, std::string* diagnostic = nullptr);

  /**
   * @brief 对输入文本进行分词、截断并填充至 max_length
   * @param text 输入文本
   * @param max_length 目标序列长度
   * @param input_ids 产出的 token ID 数组 (大小为 max_length)
   * @param attention_mask 产出的 attention mask 数组 (大小为 max_length)
   * @param diagnostic 错误诊断信息 (如非法 UTF-8)
   * @return true 分词成功，false 编码异常 (如非法 UTF-8)
   */
  bool Encode(const std::string& text, size_t max_length,
              std::vector<int64_t>* input_ids,
              std::vector<int64_t>* attention_mask,
              std::string* diagnostic = nullptr) const;

  int64_t PadTokenId() const noexcept { return pad_token_id_; }
  int64_t UnkTokenId() const noexcept { return unk_token_id_; }
  int64_t ClsTokenId() const noexcept { return cls_token_id_; }
  int64_t SepTokenId() const noexcept { return sep_token_id_; }
  size_t VocabSize() const noexcept { return vocab_.size(); }
  bool IsLoaded() const noexcept { return is_loaded_; }
  bool DoLowerCase() const noexcept { return do_lower_case_; }

 private:
  bool InitializeVocab(const std::vector<std::string>& lines,
                       std::string* diagnostic);

  bool BasicTokenize(const std::string& text, std::vector<std::string>* words,
                     std::string* diagnostic) const;
  std::vector<int64_t> WordPieceTokenize(
      const std::vector<std::string>& words) const;

  std::unordered_map<std::string, int64_t> vocab_;
  std::vector<std::string> inv_vocab_;
  bool do_lower_case_ = true;
  bool is_loaded_ = false;

  int64_t pad_token_id_ = 0;
  int64_t unk_token_id_ = 100;
  int64_t cls_token_id_ = 101;
  int64_t sep_token_id_ = 102;
};

}  // namespace alg_framework
