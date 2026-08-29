#include "engine/models/bge_embedding/bert_wordpiece_tokenizer.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace alg_framework {

namespace {

bool IsWhitespace(uint32_t cp) noexcept {
  if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') {
    return true;
  }
  if (cp == 0x00A0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) ||
      cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F ||
      cp == 0x3000) {
    return true;
  }
  return false;
}

bool IsControl(uint32_t cp) noexcept {
  if (cp == '\t' || cp == '\n' || cp == '\r') {
    return false;
  }
  if ((cp <= 0x001F) || (cp >= 0x007F && cp <= 0x009F)) {
    return true;
  }
  return false;
}

bool IsPunctuation(uint32_t cp) noexcept {
  if ((cp >= 33 && cp <= 47) || (cp >= 58 && cp <= 64) ||
      (cp >= 91 && cp <= 96) || (cp >= 123 && cp <= 126)) {
    return true;
  }
  if ((cp >= 0x2000 && cp <= 0x206F) || (cp >= 0x3000 && cp <= 0x303F) ||
      (cp >= 0xFF00 && cp <= 0xFF0F) || (cp >= 0xFF1A && cp <= 0xFF20) ||
      (cp >= 0xFF3B && cp <= 0xFF40) || (cp >= 0xFF5B && cp <= 0xFF65)) {
    return true;
  }
  return false;
}

bool IsCJK(uint32_t cp) noexcept {
  return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
         (cp >= 0x20000 && cp <= 0x2A6DF) || (cp >= 0x2A700 && cp <= 0x2B73F) ||
         (cp >= 0x2B740 && cp <= 0x2B81F) || (cp >= 0x2B820 && cp <= 0x2CEAF) ||
         (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x2F800 && cp <= 0x2FA1F);
}

// 解码 UTF-8 单个 code point，返回消耗字节数 (0 表示非法)
size_t DecodeUtf8CodePoint(const char* s, size_t len, uint32_t* cp) noexcept {
  if (len == 0 || s == nullptr || cp == nullptr) return 0;
  unsigned char c0 = static_cast<unsigned char>(s[0]);
  if (c0 < 0x80) {
    *cp = c0;
    return 1;
  } else if ((c0 & 0xE0) == 0xC0) {
    if (len < 2) return 0;
    unsigned char c1 = static_cast<unsigned char>(s[1]);
    if ((c1 & 0xC0) != 0x80) return 0;
    *cp = ((c0 & 0x1F) << 6) | (c1 & 0x3F);
    if (*cp < 0x80) return 0;  // 过长编码
    return 2;
  } else if ((c0 & 0xF0) == 0xE0) {
    if (len < 3) return 0;
    unsigned char c1 = static_cast<unsigned char>(s[1]);
    unsigned char c2 = static_cast<unsigned char>(s[2]);
    if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) return 0;
    *cp = ((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
    if (*cp < 0x800 || (*cp >= 0xD800 && *cp <= 0xDFFF)) return 0;
    return 3;
  } else if ((c0 & 0xF8) == 0xF0) {
    if (len < 4) return 0;
    unsigned char c1 = static_cast<unsigned char>(s[1]);
    unsigned char c2 = static_cast<unsigned char>(s[2]);
    unsigned char c3 = static_cast<unsigned char>(s[3]);
    if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) {
      return 0;
    }
    *cp = ((c0 & 0x07) << 18) | ((c1 & 0x3F) << 12) | ((c2 & 0x3F) << 6) |
          (c3 & 0x3F);
    if (*cp < 0x10000 || *cp > 0x10FFFF) return 0;
    return 4;
  }
  return 0;
}

// 编码 Unicode code point 到 UTF-8
void EncodeUtf8CodePoint(uint32_t cp, std::string* out) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0x10FFFF) {
    out->push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

}  // namespace

bool BertWordPieceTokenizer::Load(const std::string& vocab_file_path,
                                  bool do_lower_case, std::string* diagnostic) {
  if (vocab_file_path.empty()) {
    if (diagnostic) *diagnostic = "Vocab file path is empty";
    return false;
  }

  std::error_code ec;
  if (!std::filesystem::is_regular_file(vocab_file_path, ec) || ec) {
    if (diagnostic) {
      *diagnostic = "Vocab file does not exist or is not a regular file: " +
                    vocab_file_path;
    }
    return false;
  }

  std::ifstream infile(vocab_file_path);
  if (!infile.is_open()) {
    if (diagnostic) {
      *diagnostic = "Cannot open vocab file: " + vocab_file_path;
    }
    return false;
  }

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(infile, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(line);
  }

  do_lower_case_ = do_lower_case;
  return InitializeVocab(lines, diagnostic);
}

bool BertWordPieceTokenizer::LoadFromTokens(
    const std::vector<std::string>& tokens, bool do_lower_case,
    std::string* diagnostic) {
  do_lower_case_ = do_lower_case;
  return InitializeVocab(tokens, diagnostic);
}

bool BertWordPieceTokenizer::InitializeVocab(
    const std::vector<std::string>& lines, std::string* diagnostic) {
  vocab_.clear();
  inv_vocab_.clear();
  is_loaded_ = false;

  if (lines.empty()) {
    if (diagnostic) *diagnostic = "Vocab list is empty";
    return false;
  }

  for (size_t i = 0; i < lines.size(); ++i) {
    const auto& token = lines[i];
    auto it = vocab_.find(token);
    if (it != vocab_.end()) {
      if (diagnostic) {
        *diagnostic = "Duplicate token in vocab at index " + std::to_string(i) +
                      ": " + token;
      }
      vocab_.clear();
      inv_vocab_.clear();
      return false;
    }
    vocab_[token] = static_cast<int64_t>(i);
    inv_vocab_.push_back(token);
  }

  auto it_pad = vocab_.find("[PAD]");
  auto it_unk = vocab_.find("[UNK]");
  auto it_cls = vocab_.find("[CLS]");
  auto it_sep = vocab_.find("[SEP]");

  if (it_pad == vocab_.end() || it_unk == vocab_.end() ||
      it_cls == vocab_.end() || it_sep == vocab_.end()) {
    if (diagnostic) {
      *diagnostic =
          "Vocab is missing required BERT special tokens ([PAD], [UNK], "
          "[CLS], [SEP])";
    }
    vocab_.clear();
    inv_vocab_.clear();
    return false;
  }

  pad_token_id_ = it_pad->second;
  unk_token_id_ = it_unk->second;
  cls_token_id_ = it_cls->second;
  sep_token_id_ = it_sep->second;

  is_loaded_ = true;
  return true;
}

std::vector<std::string> BertWordPieceTokenizer::BasicTokenize(
    const std::string& text) const {
  std::string clean_text;
  clean_text.reserve(text.size() * 2);

  const char* ptr = text.data();
  size_t len = text.size();
  size_t offset = 0;

  while (offset < len) {
    uint32_t cp = 0;
    size_t consumed = DecodeUtf8CodePoint(ptr + offset, len - offset, &cp);
    if (consumed == 0) {
      // 非法 UTF-8 字节，跳过该字节并插入空格
      clean_text.push_back(' ');
      offset += 1;
      continue;
    }

    offset += consumed;

    if (cp == 0 || cp == 0xFFFD || IsControl(cp)) {
      continue;
    }

    if (IsWhitespace(cp)) {
      clean_text.push_back(' ');
      continue;
    }

    if (do_lower_case_) {
      if (cp >= 'A' && cp <= 'Z') {
        cp = cp + ('a' - 'A');
      } else if (cp >= 0xFF21 && cp <= 0xFF3A) {  // 全角英文字母
        cp = (cp - 0xFF21) + 'a';
      } else if (cp >= 0xFF41 && cp <= 0xFF5A) {
        cp = (cp - 0xFF41) + 'a';
      }
    }

    if (IsCJK(cp) || IsPunctuation(cp)) {
      clean_text.push_back(' ');
      EncodeUtf8CodePoint(cp, &clean_text);
      clean_text.push_back(' ');
    } else {
      EncodeUtf8CodePoint(cp, &clean_text);
    }
  }

  std::vector<std::string> words;
  std::stringstream ss(clean_text);
  std::string word;
  while (ss >> word) {
    words.push_back(std::move(word));
  }
  return words;
}

std::vector<int64_t> BertWordPieceTokenizer::WordPieceTokenize(
    const std::vector<std::string>& words) const {
  std::vector<int64_t> token_ids;
  static constexpr size_t kMaxCharsPerWord = 200;

  for (const auto& word : words) {
    if (word.size() > kMaxCharsPerWord) {
      token_ids.push_back(unk_token_id_);
      continue;
    }

    auto it = vocab_.find(word);
    if (it != vocab_.end()) {
      token_ids.push_back(it->second);
      continue;
    }

    bool is_bad = false;
    size_t start = 0;
    size_t len = word.size();
    std::vector<int64_t> sub_ids;

    while (start < len) {
      size_t end = len;
      int64_t cur_id = -1;
      size_t cur_len = 0;

      while (start < end) {
        std::string substr = word.substr(start, end - start);
        if (start > 0) {
          substr = "##" + substr;
        }
        auto sub_it = vocab_.find(substr);
        if (sub_it != vocab_.end()) {
          cur_id = sub_it->second;
          cur_len = end - start;
          break;
        }
        end--;
      }

      if (cur_id == -1) {
        is_bad = true;
        break;
      }

      sub_ids.push_back(cur_id);
      start += cur_len;
    }

    if (is_bad) {
      token_ids.push_back(unk_token_id_);
    } else {
      token_ids.insert(token_ids.end(), sub_ids.begin(), sub_ids.end());
    }
  }

  return token_ids;
}

void BertWordPieceTokenizer::Encode(
    const std::string& text, size_t max_length, std::vector<int64_t>* input_ids,
    std::vector<int64_t>* attention_mask) const {
  if (!input_ids || !attention_mask) return;
  input_ids->assign(max_length, pad_token_id_);
  attention_mask->assign(max_length, 0);

  if (max_length < 2) {
    if (max_length == 1) {
      (*input_ids)[0] = cls_token_id_;
      (*attention_mask)[0] = 1;
    }
    return;
  }

  std::vector<std::string> words = BasicTokenize(text);
  std::vector<int64_t> body_ids = WordPieceTokenize(words);

  size_t max_body_len = max_length - 2;
  if (body_ids.size() > max_body_len) {
    body_ids.resize(max_body_len);
  }

  size_t total_len = body_ids.size() + 2;
  (*input_ids)[0] = cls_token_id_;
  (*attention_mask)[0] = 1;

  for (size_t i = 0; i < body_ids.size(); ++i) {
    (*input_ids)[1 + i] = body_ids[i];
    (*attention_mask)[1 + i] = 1;
  }

  (*input_ids)[1 + body_ids.size()] = sep_token_id_;
  (*attention_mask)[1 + body_ids.size()] = 1;

  (void)total_len;
}

}  // namespace alg_framework
