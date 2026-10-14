// 供 tools/prepare_bpe_tokenizer.py verify 使用的驱动程序，不注册为 CTest
// 用例。
//
// 用法：bpe_tokenizer_driver <file.eftok>，从 stdin 逐行读取，每行输出一行：
//   E <UTF-8 文本的十六进制>  输出以空格分隔的 token ID（可为空行）
//   D <id> <id> ...           输出 Decode 结果的十六进制
// 失败输出 ERR。
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "engine/text/bpe_tokenizer.h"

namespace {

int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool FromHex(const std::string& hex, std::string* bytes) {
  if (hex.size() % 2 != 0) return false;
  bytes->clear();
  bytes->reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    const int high = HexDigit(hex[i]);
    const int low = HexDigit(hex[i + 1]);
    if (high < 0 || low < 0) return false;
    bytes->push_back(static_cast<char>(high * 16 + low));
  }
  return true;
}

std::string ToHex(const std::string& bytes) {
  static const char kDigits[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(bytes.size() * 2);
  for (unsigned char byte : bytes) {
    hex.push_back(kDigits[byte >> 4]);
    hex.push_back(kDigits[byte & 15]);
  }
  return hex;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: bpe_tokenizer_driver <file.eftok>\n";
    return 2;
  }
  std::string diagnostic;
  const auto tokenizer =
      llm_edgeflow::text::BpeTokenizer::Load(argv[1], &diagnostic);
  if (!tokenizer) {
    std::cerr << diagnostic << "\n";
    return 1;
  }
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) {
      std::cout << "ERR\n";
      continue;
    }
    const char kind = line[0];
    const std::string argument = line.size() > 2 ? line.substr(2) : "";
    if (kind == 'E') {
      std::string text;
      std::vector<int32_t> ids;
      if (!FromHex(argument, &text) ||
          tokenizer->Encode(text, &ids, &diagnostic) != 0) {
        std::cout << "ERR\n";
        continue;
      }
      for (size_t i = 0; i < ids.size(); ++i) {
        std::cout << (i ? " " : "") << ids[i];
      }
      std::cout << "\n";
    } else if (kind == 'D') {
      std::istringstream stream(argument);
      std::vector<int32_t> ids;
      for (int64_t id; stream >> id;) ids.push_back(static_cast<int32_t>(id));
      std::string text;
      if (tokenizer->Decode(ids, &text, &diagnostic) != 0) {
        std::cout << "ERR\n";
      } else {
        std::cout << ToHex(text) << "\n";
      }
    } else {
      std::cout << "ERR\n";
    }
  }
  return 0;
}
