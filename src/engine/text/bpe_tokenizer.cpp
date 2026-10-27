#include "engine/text/bpe_tokenizer.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>

#include "contracts/diagnostic.h"
#include "contracts/error_codes.h"
#include "engine/text/byte_level_pretokenizer.h"
#include "engine/text/unicode_tables.h"
#include "engine/text/utf8.h"

namespace llm_edgeflow {
namespace text {
namespace {

// ---- 文件布局常量（见 doc/dev_guide/bpe_tokenizer_format.md） ----

constexpr char kMagic[8] = {'E', 'F', 'B', 'P', 'E', '\r', '\n', '\0'};
constexpr size_t kMaxFileBytes = size_t{1} << 30;

// 头部是 40 个 u32：magic（2 个）、文件长度、CRC、五个参数、十个计数、
// 十六个区段偏移。
constexpr size_t kHeaderWords = 40;
constexpr size_t kHeaderBytes = kHeaderWords * 4;
constexpr size_t kCrcStart = 16;  // CRC 覆盖 [16, 文件末尾)
constexpr size_t kFileSizeWord = 2;
constexpr size_t kCrcWord = 3;
constexpr size_t kNormalizerWord = 4;
constexpr size_t kPreTokenizerWord = 5;
constexpr size_t kByteFallbackWord = 6;
constexpr size_t kMergePriorityWord = 7;
constexpr size_t kDecoderWord = 8;
constexpr size_t kTokenCountWord = 9;
constexpr size_t kMergeCountWord = 10;
constexpr size_t kSpecialCountWord = 11;
constexpr size_t kEndCountWord = 12;
constexpr size_t kPoolBytesWord = 13;
constexpr size_t kHashSlotsWord = 14;
constexpr size_t kHashEntriesWord = 15;
constexpr size_t kUnitCountWord = 16;
constexpr size_t kTrieNodesWord = 17;
constexpr size_t kTrieEdgesWord = 18;
constexpr size_t kPropertyRowsWord = 19;
constexpr size_t kDecompositionRowsWord = 20;
constexpr size_t kDecompositionPoolWord = 21;
constexpr size_t kCombiningRowsWord = 22;
constexpr size_t kCompositionRowsWord = 23;
constexpr size_t kSectionsWord = 24;
constexpr size_t kSectionCount = 16;
static_assert(kSectionsWord + kSectionCount == kHeaderWords,
              "header words must be fully assigned");

enum Section : size_t {
  kSectionOffsets = 0,
  kSectionPool,
  kSectionKinds,
  kSectionHash,
  kSectionRankOffsets,
  kSectionRankEntries,
  kSectionScores,
  kSectionUnits,
  kSectionTrieNodes,
  kSectionTrieEdges,
  kSectionEndTokens,
  kSectionProperties,
  kSectionDecompositions,
  kSectionDecompositionPool,
  kSectionCombining,
  kSectionCompositions,
};

enum Normalizer : uint32_t {
  kNormalizerNone = 0,
  kNormalizerReplaceSpaceMetaspace = 1,
  kNormalizerNfc = 2,
};
enum PreTokenizer : uint32_t {
  kPreTokenizerNone = 0,
  kPreTokenizerByteLevelQwen2 = 1,
};
enum MergePriority : uint32_t {
  kPriorityRank = 0,
  kPriorityScore = 1,
};
enum Decoder : uint32_t {
  kDecoderMetaspaceByteFallback = 0,
  kDecoderByteLevel = 1,
};

// token 类型：普通、字节回退 <0xXX>（高字节为字节值）、特殊。
constexpr uint8_t kKindNormal = 0;
constexpr uint8_t kKindByteFallback = 1;
constexpr uint8_t kKindSpecial = 2;

constexpr uint32_t kMaxHashSlots = uint32_t{1} << 27;
// rank 模式的打包：高 24 位是排名，低 8 位是左段字节长度。
constexpr uint32_t kRankShift = 8;
constexpr uint32_t kLeftLengthMask = 0xFF;
constexpr uint32_t kMaxRank = (uint32_t{1} << 24) - 1;

constexpr char kMetaspace[] = "\xE2\x96\x81";  // U+2581
constexpr size_t kMetaspaceBytes = 3;

// 规范化后的文本、符号偏移与长度都使用 uint32_t。
static_assert(kMaxEncodeBytes * 4 < std::numeric_limits<uint32_t>::max(),
              "normalized text offsets must fit uint32_t");

uint64_t AlignUp4(uint64_t value) noexcept {
  return (value + 3U) & ~uint64_t{3};
}

// CRC-32/IEEE，与 Python zlib.crc32 一致。只用来发现意外损坏，不是认证手段。
uint32_t Crc32(const unsigned char* bytes, size_t size) noexcept {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t v = i;
      for (int j = 0; j < 8; ++j) {
        v = (v >> 1) ^ ((v & 1) ? 0xEDB88320U : 0U);
      }
      t[i] = v;
    }
    return t;
  }();
  uint32_t crc = ~uint32_t{0};
  for (size_t i = 0; i < size; ++i) {
    crc = table[(crc ^ bytes[i]) & 255U] ^ (crc >> 8);
  }
  return ~crc;
}

bool HostIsLittleEndian() noexcept {
  const uint32_t probe = 1;
  unsigned char first = 0;
  std::memcpy(&first, &probe, 1);
  return first == 1;
}

// FNV-1a 64 位，取低位定位；与生成工具一致。
uint64_t HashBytes(const char* bytes, size_t size) noexcept {
  uint64_t hash = UINT64_C(14695981039346656037);
  for (size_t i = 0; i < size; ++i) {
    hash =
        (hash ^ static_cast<unsigned char>(bytes[i])) * UINT64_C(1099511628211);
  }
  return hash;
}

void SetLoadDiagnostic(std::string* diagnostic, const std::string& path,
                       std::string_view reason) noexcept {
  if (!diagnostic) return;
  try {
    std::string message = path;
    message += ": ";
    message.append(reason);
    diagnostic->assign(message);
  } catch (...) {
    diagnostic->clear();
  }
}

bool IsValidUtf8(std::string_view text) noexcept {
  for (size_t offset = 0; offset < text.size();) {
    if (static_cast<unsigned char>(text[offset]) < 0x80) {
      ++offset;
      continue;
    }
    uint32_t code_point = 0;
    const size_t consumed = utf8::DecodeCodePoint(
        text.data() + offset, text.size() - offset, &code_point);
    if (consumed == 0) return false;
    offset += consumed;
  }
  return true;
}

void AppendCodePoint(std::string* out, uint32_t code_point) {
  if (code_point < 0x80) {
    out->push_back(static_cast<char>(code_point));
  } else if (code_point < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (code_point >> 6)));
    out->push_back(static_cast<char>(0x80 | (code_point & 63)));
  } else if (code_point < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (code_point >> 12)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 63)));
    out->push_back(static_cast<char>(0x80 | (code_point & 63)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (code_point >> 18)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 12) & 63)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 63)));
    out->push_back(static_cast<char>(0x80 | (code_point & 63)));
  }
}

// 读取一个码点。遇到非法序列时返回 false，pos 前进到最大有效前缀之后，
// 出错的下一字节留给下一次读取，与 Rust String::from_utf8_lossy 的替换规则
// 相同。
bool ReadLossyCodePoint(std::string_view text, size_t* pos) noexcept {
  const unsigned char lead = static_cast<unsigned char>(text[(*pos)++]);
  if (lead < 0x80) return true;
  const int continuation = lead >= 0xC2 && lead <= 0xDF   ? 1
                           : lead >= 0xE0 && lead <= 0xEF ? 2
                           : lead >= 0xF0 && lead <= 0xF4 ? 3
                                                          : 0;
  if (continuation == 0) return false;
  for (int j = 0; j < continuation; ++j) {
    if (*pos == text.size()) return false;
    const unsigned char next = static_cast<unsigned char>(text[*pos]);
    if ((next & 0xC0) != 0x80) return false;
    if (j == 0 &&
        ((lead == 0xE0 && next < 0xA0) || (lead == 0xED && next >= 0xA0) ||
         (lead == 0xF0 && next < 0x90) || (lead == 0xF4 && next >= 0x90))) {
      return false;
    }
    ++*pos;
  }
  return true;
}

// 有损 UTF-8 解码：每个最大非法子序列替换为一个 U+FFFD。
void AppendLossyUtf8(std::string_view bytes, std::string* out) {
  out->reserve(out->size() + bytes.size());
  for (size_t pos = 0; pos < bytes.size();) {
    const size_t start = pos;
    if (ReadLossyCodePoint(bytes, &pos)) {
      out->append(bytes.data() + start, pos - start);
    } else {
      out->append("\xEF\xBF\xBD", 3);
    }
  }
}

// 把 token 文本中的 U+2581 换成空格，其余字节原样追加。
void AppendWithSpaces(const char* piece, size_t size, std::string* out) {
  size_t begin = 0;
  for (size_t i = 0; i + kMetaspaceBytes <= size;) {
    if (piece[i] == kMetaspace[0] && piece[i + 1] == kMetaspace[1] &&
        piece[i + 2] == kMetaspace[2]) {
      out->append(piece + begin, i - begin);
      out->append(1, ' ');
      i += kMetaspaceBytes;
      begin = i;
    } else {
      ++i;
    }
  }
  out->append(piece + begin, size - begin);
}

int HexValue(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// 解析 "<0xXX>"（大写十六进制）；格式不符返回 -1。
int ParseByteToken(std::string_view piece) noexcept {
  if (piece.size() != 6 || piece.substr(0, 3) != "<0x" || piece[5] != '>') {
    return -1;
  }
  const int high = HexValue(piece[3]);
  const int low = HexValue(piece[4]);
  return high < 0 || low < 0 ? -1 : high * 16 + low;
}

// 把 float 的位模式映射为保序的 uint32：分数越高值越大；-0.0 与 +0.0 视为相同。
// 直接作用于文件中的位模式，热路径上不需要 float 局部变量。
uint32_t ScorePriority(uint32_t bits) noexcept {
  if (bits == 0x80000000U) bits = 0;
  return (bits & 0x80000000U) ? ~bits : (bits | 0x80000000U);
}

bool MultiplyChecked(size_t a, size_t b, size_t* result) noexcept {
  return !__builtin_mul_overflow(a, b, result);
}

// 只读 mmap。映射在对象存活期间保持有效。
class MappedFile final {
 public:
  static std::unique_ptr<MappedFile> Open(const std::string& path,
                                          std::string* reason) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      *reason = std::string("cannot open file: ") + std::strerror(errno);
      return nullptr;
    }
    struct stat info {};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<uint64_t>(info.st_size) < kHeaderBytes ||
        static_cast<uint64_t>(info.st_size) > kMaxFileBytes) {
      close(fd);
      *reason = "not a regular file or its size is outside the supported range";
      return nullptr;
    }
    const size_t size = static_cast<size_t>(info.st_size);
    void* mapped = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapped == MAP_FAILED) {
      *reason = "cannot map file";
      return nullptr;
    }
    return std::unique_ptr<MappedFile>(
        new MappedFile(static_cast<const unsigned char*>(mapped), size));
  }
  ~MappedFile() {
    if (data_) munmap(const_cast<unsigned char*>(data_), size_);
  }
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;

  const unsigned char* data() const noexcept { return data_; }
  size_t size() const noexcept { return size_; }

 private:
  MappedFile(const unsigned char* data, size_t size) noexcept
      : data_(data), size_(size) {}
  const unsigned char* data_;
  size_t size_;
};

}  // namespace

// 加载后只读的实现。所有指针指向 file_ 的映射。
class BpeTokenizer::Impl final {
 public:
  bool Init(std::unique_ptr<MappedFile> file, std::string* reason);

  int Encode(std::string_view text, std::vector<int32_t>* ids,
             std::string* diagnostic) const;
  int AppendTokenBytes(int32_t id, std::string* bytes,
                       std::string* diagnostic) const;
  int Decode(const std::vector<int32_t>& ids, std::string* text,
             std::string* diagnostic) const;
  const std::vector<int32_t>& end_tokens() const noexcept {
    return end_tokens_;
  }
  size_t vocab_size() const noexcept { return token_count_; }

 private:
  struct Symbol {
    uint32_t start;
    uint32_t size;  // 0 表示已并入左邻符号
    int32_t prev;
    int32_t next;
    int32_t id;  // 负数：词表中没有这个单位，由字节回退处理
    uint32_t chars;  // 包含的 Unicode 码点数，合并时累加（score 优先级用）
  };
  struct Candidate {
    uint32_t priority;  // 越大越先合并
    uint32_t left_chars;  // score：priority 相同时左段码点数多者先合并
    uint32_t right_chars;  // 再比右段码点数
    uint32_t left;
    uint32_t right;
    uint32_t size;  // 合并后的字节数，用来识别已失效的候选
    int32_t id;
  };

  // 候选合并的最大堆：优先级高者在前，同优先级取最左。自己实现而不用
  // std::push_heap / pop_heap，是因为热路径上标准库的堆函数在带
  // AddressSanitizer 的构建里每次调用都很慢。
  class CandidateHeap {
   public:
    bool empty() const noexcept { return size_ == 0; }
    void Push(const Candidate& candidate) {
      if (size_ == items_.size()) items_.resize(items_.size() * 2);
      size_t child = size_++;
      while (child != 0) {
        const size_t parent = (child - 1) / 2;
        if (!Higher(candidate, items_[parent])) break;
        items_[child] = items_[parent];
        child = parent;
      }
      items_[child] = candidate;
    }
    void Pop(Candidate* top) noexcept {
      *top = items_[0];
      const size_t last = --size_;
      if (last == 0) return;
      size_t parent = 0;
      for (size_t child = 1; child < last; child = parent * 2 + 1) {
        if (child + 1 < last && Higher(items_[child + 1], items_[child])) {
          ++child;
        }
        if (!Higher(items_[child], items_[last])) break;
        items_[parent] = items_[child];
        parent = child;
      }
      items_[parent] = items_[last];
    }

   private:
    // 依次比较：优先级、左段码点数、右段码点数（都是越大越先）、合成 token 的
    // ID（越小越先）、位置（越靠左越先）。rank 模式下相同排名意味着相同的
    // 合并规则，后面几项必然相同，所以只有最后的“最左”起作用。
    static bool Higher(const Candidate& a, const Candidate& b) noexcept {
      if (a.priority != b.priority) return a.priority > b.priority;
      if (a.left_chars != b.left_chars) return a.left_chars > b.left_chars;
      if (a.right_chars != b.right_chars) return a.right_chars > b.right_chars;
      if (a.id != b.id) return a.id < b.id;
      return a.left < b.left;
    }
    std::vector<Candidate> items_ = std::vector<Candidate>(16);
    size_t size_ = 0;
  };

  // 热路径用指针和长度，不构造 string_view（原因同 Lookup）。
  const char* PieceData(uint32_t id) const noexcept {
    return pool_ + offsets_[id];
  }
  size_t PieceSize(uint32_t id) const noexcept {
    return offsets_[id + 1] - offsets_[id];
  }
  std::string_view Piece(uint32_t id) const noexcept {
    return std::string_view(pool_ + offsets_[id],
                            offsets_[id + 1] - offsets_[id]);
  }
  uint8_t Kind(uint32_t id) const noexcept { return kinds_[id]; }
  // 字节回退 token 的字节值，由 "<0xXX>" 推出；加载时已确认格式。
  char ByteValue(uint32_t id) const noexcept {
    return static_cast<char>(ParseByteToken(Piece(id)));
  }
  float Score(uint32_t id) const noexcept {
    float value = 0;
    std::memcpy(&value, scores_ + id, sizeof(value));
    return value;
  }
  // 热路径上的函数不接受 string_view 参数：在带 AddressSanitizer 的构建里，
  // 对按值传入的参数调用成员函数会使每次调用都很慢。
  int32_t Lookup(const char* bytes, size_t size) const noexcept;
  int32_t MatchAddedToken(const char* text, size_t size,
                          size_t* length) const noexcept;
  // 给出左右相邻符号合并的候选；没有可用合并时返回 false。
  bool MakeCandidate(const char* word_data, const std::vector<Symbol>& symbols,
                     uint32_t left, uint32_t right,
                     Candidate* candidate) const noexcept;
  // 成功返回 0，失败返回框架错误码并写入 reason。
  int EncodeSegment(std::string_view segment, std::vector<int32_t>* ids,
                    const char** reason) const;
  int MergeWord(std::string_view word, std::vector<int32_t>* ids,
                const char** reason) const;

  std::unique_ptr<MappedFile> file_;

  uint32_t normalizer_ = 0;
  uint32_t pre_tokenizer_ = 0;
  bool byte_fallback_ = false;
  uint32_t merge_priority_ = 0;
  uint32_t decoder_ = 0;
  uint32_t token_count_ = 0;
  uint32_t hash_slots_ = 0;
  uint32_t unit_count_ = 0;
  uint32_t trie_nodes_ = 0;
  size_t max_token_bytes_ = 0;

  const uint32_t* offsets_ = nullptr;
  const char* pool_ = nullptr;
  const uint8_t* kinds_ = nullptr;
  const uint32_t* slots_ = nullptr;
  const uint32_t* rank_offsets_ = nullptr;
  const uint32_t* rank_entries_ = nullptr;
  const uint32_t* scores_ = nullptr;
  const uint32_t* units_ = nullptr;
  const uint32_t* trie_nodes_data_ = nullptr;
  const uint32_t* trie_edges_ = nullptr;

  std::array<uint32_t, 256> trie_root_{};
  std::array<int32_t, 256> byte_ids_{};
  std::vector<int32_t> end_tokens_;
  UnicodeTables unicode_;
};

bool BpeTokenizer::Impl::Init(std::unique_ptr<MappedFile> file,
                              std::string* reason) {
  const auto fail = [reason](const char* message) {
    *reason = message;
    return false;
  };
  file_ = std::move(file);
  const unsigned char* data = file_->data();
  const size_t file_size = file_->size();
  if (file_size < kHeaderBytes) return fail("file is shorter than the header");
  if (std::memcmp(data, kMagic, sizeof(kMagic)) != 0) return fail("bad magic");

  std::array<uint32_t, kHeaderWords> h{};
  std::memcpy(h.data(), data, kHeaderBytes);
  if (h[kFileSizeWord] != file_size) return fail("recorded file size differs");
  if (Crc32(data + kCrcStart, file_size - kCrcStart) != h[kCrcWord]) {
    return fail("CRC mismatch");
  }

  normalizer_ = h[kNormalizerWord];
  pre_tokenizer_ = h[kPreTokenizerWord];
  merge_priority_ = h[kMergePriorityWord];
  decoder_ = h[kDecoderWord];
  if (normalizer_ > kNormalizerNfc) return fail("unknown normalizer");
  if (pre_tokenizer_ > kPreTokenizerByteLevelQwen2) {
    return fail("unknown pre-tokenizer");
  }
  if (h[kByteFallbackWord] > 1) return fail("unknown byte fallback value");
  byte_fallback_ = h[kByteFallbackWord] == 1;
  if (merge_priority_ > kPriorityScore) return fail("unknown merge priority");
  if (decoder_ > kDecoderByteLevel) return fail("unknown decoder");

  token_count_ = h[kTokenCountWord];
  const uint32_t merge_count = h[kMergeCountWord];
  const uint32_t special_count = h[kSpecialCountWord];
  const uint32_t end_count = h[kEndCountWord];
  const uint32_t pool_bytes = h[kPoolBytesWord];
  hash_slots_ = h[kHashSlotsWord];
  const uint32_t hash_entries = h[kHashEntriesWord];
  unit_count_ = h[kUnitCountWord];
  trie_nodes_ = h[kTrieNodesWord];
  const uint32_t trie_edges = h[kTrieEdgesWord];
  if (token_count_ == 0 ||
      token_count_ > std::numeric_limits<int32_t>::max() - 1) {
    return fail("token count is outside the supported range");
  }
  if (merge_priority_ == kPriorityScore && merge_count != 0) {
    return fail("score priority cannot carry merge rules");
  }
  if (hash_slots_ < 2 || (hash_slots_ & (hash_slots_ - 1)) != 0 ||
      hash_slots_ > kMaxHashSlots || hash_entries == 0 ||
      hash_entries >= hash_slots_ || hash_entries > token_count_) {
    return fail("hash table size is invalid");
  }
  if (end_count == 0) return fail("no end token is recorded");
  if (trie_nodes_ == 0 || trie_edges != trie_nodes_ - 1) {
    return fail("special token trie counts are inconsistent");
  }

  // 区段大小；全部用 64 位计算，header 中的计数不会在这里溢出。
  const bool rank = merge_priority_ == kPriorityRank;
  uint64_t sizes[kSectionCount] = {};
  sizes[kSectionOffsets] = (uint64_t{token_count_} + 1) * 4;
  sizes[kSectionPool] = pool_bytes;
  sizes[kSectionKinds] = uint64_t{token_count_};
  sizes[kSectionHash] = uint64_t{hash_slots_} * 4;
  sizes[kSectionRankOffsets] = rank ? (uint64_t{token_count_} + 1) * 4 : 0;
  sizes[kSectionRankEntries] = rank ? uint64_t{merge_count} * 4 : 0;
  sizes[kSectionScores] = rank ? 0 : uint64_t{token_count_} * 4;
  sizes[kSectionUnits] = uint64_t{unit_count_} * 8;
  sizes[kSectionTrieNodes] = uint64_t{trie_nodes_} * 12;
  sizes[kSectionTrieEdges] = uint64_t{trie_edges} * 8;
  sizes[kSectionEndTokens] = uint64_t{end_count} * 4;
  sizes[kSectionProperties] = uint64_t{h[kPropertyRowsWord]} * 12;
  sizes[kSectionDecompositions] = uint64_t{h[kDecompositionRowsWord]} * 12;
  sizes[kSectionDecompositionPool] = uint64_t{h[kDecompositionPoolWord]} * 4;
  sizes[kSectionCombining] = uint64_t{h[kCombiningRowsWord]} * 8;
  sizes[kSectionCompositions] = uint64_t{h[kCompositionRowsWord]} * 12;

  // 区段按顺序紧挨着排列，每个起点 4 字节对齐，最后一个区段结束于文件末尾。
  uint64_t end = kHeaderBytes;
  std::array<uint64_t, kSectionCount> offsets{};
  for (size_t i = 0; i < kSectionCount; ++i) {
    const uint64_t offset = h[kSectionsWord + i];
    if (offset != AlignUp4(end)) {
      return fail("section offsets are inconsistent");
    }
    if (offset > file_size || sizes[i] > file_size - offset) {
      return fail("section exceeds the file");
    }
    offsets[i] = offset;
    end = offset + sizes[i];
  }
  if (end != file_size) return fail("sections do not cover the file");

  const auto words = [&](Section section) {
    return reinterpret_cast<const uint32_t*>(data + offsets[section]);
  };
  offsets_ = words(kSectionOffsets);
  pool_ = reinterpret_cast<const char*>(data + offsets[kSectionPool]);
  kinds_ = data + offsets[kSectionKinds];
  slots_ = words(kSectionHash);
  rank_offsets_ = words(kSectionRankOffsets);
  rank_entries_ = words(kSectionRankEntries);
  scores_ = words(kSectionScores);
  units_ = words(kSectionUnits);
  trie_nodes_data_ = words(kSectionTrieNodes);
  trie_edges_ = words(kSectionTrieEdges);
  const uint32_t* ends = words(kSectionEndTokens);

  // token 字节池：偏移严格递增（没有空 token），末项等于池长度。
  if (offsets_[0] != 0 || offsets_[token_count_] != pool_bytes) {
    return fail("token offsets do not span the byte pool");
  }
  for (uint32_t i = 0; i < token_count_; ++i) {
    if (offsets_[i] >= offsets_[i + 1]) {
      return fail("token offsets are not strictly increasing");
    }
    max_token_bytes_ =
        std::max<size_t>(max_token_bytes_, offsets_[i + 1] - offsets_[i]);
  }

  // token 类型。
  byte_ids_.fill(-1);
  uint32_t specials = 0;
  for (uint32_t i = 0; i < token_count_; ++i) {
    const uint8_t kind = kinds_[i];
    if (kind > kKindSpecial) return fail("token kind is invalid");
    if (kind == kKindSpecial) ++specials;
    if (kind == kKindByteFallback) {
      const int value = ParseByteToken(Piece(i));
      if (value < 0 || byte_ids_[value] != -1) {
        return fail("malformed or duplicate byte-fallback token");
      }
      byte_ids_[value] = static_cast<int32_t>(i);
    }
  }
  if (specials != special_count) return fail("special token count differs");
  if (byte_fallback_ &&
      std::count(byte_ids_.begin(), byte_ids_.end(), -1) != 0) {
    return fail("byte fallback needs all 256 byte tokens");
  }

  // 词表哈希：槽值为 ID+1，0 表示空；每个登记的 token 都能从自己的起点探测到。
  uint32_t occupied = 0;
  for (uint32_t i = 0; i < hash_slots_; ++i) {
    if (slots_[i] > token_count_) return fail("hash slot is out of range");
    occupied += slots_[i] != 0;
  }
  if (occupied != hash_entries) return fail("hash entry count differs");
  for (uint32_t i = 0; i < hash_slots_; ++i) {
    if (slots_[i] != 0 &&
        Lookup(pool_ + offsets_[slots_[i] - 1],
               offsets_[slots_[i]] - offsets_[slots_[i] - 1]) !=
            static_cast<int32_t>(slots_[i] - 1)) {
      return fail("hash table does not index its tokens");
    }
  }

  // 合并优先级。
  if (rank) {
    if (rank_offsets_[0] != 0 || rank_offsets_[token_count_] != merge_count) {
      return fail("merge offsets do not span the merge rules");
    }
    for (uint32_t i = 0; i < token_count_; ++i) {
      if (rank_offsets_[i] > rank_offsets_[i + 1]) {
        return fail("merge offsets are not increasing");
      }
      const size_t token_bytes = offsets_[i + 1] - offsets_[i];
      uint32_t previous_left = 0;
      for (uint32_t j = rank_offsets_[i]; j < rank_offsets_[i + 1]; ++j) {
        const uint32_t left = rank_entries_[j] & kLeftLengthMask;
        if (left == 0 || left >= token_bytes || left <= previous_left) {
          return fail("merge rule left length is invalid");
        }
        previous_left = left;
      }
    }
  } else {
    for (uint32_t i = 0; i < token_count_; ++i) {
      if (!std::isfinite(Score(i))) return fail("a token score is not finite");
    }
  }

  // 初始单位表。
  if (pre_tokenizer_ == kPreTokenizerByteLevelQwen2) {
    if (unit_count_ != 256)
      return fail("byte-level units must cover 256 bytes");
    for (uint32_t b = 0; b < 256; ++b) {
      const uint32_t id = units_[b * 2 + 1];
      const char byte = static_cast<char>(b);
      if (units_[b * 2] != b || id >= token_count_ ||
          Piece(id) != std::string_view(&byte, 1)) {
        return fail("byte-level unit table is invalid");
      }
    }
  } else {
    if (unit_count_ == 0) return fail("character unit table is empty");
    for (uint32_t i = 0; i < unit_count_; ++i) {
      const uint32_t code_point = units_[i * 2];
      const uint32_t id = units_[i * 2 + 1];
      uint32_t decoded = 0;
      if (id >= token_count_ || (i != 0 && units_[i * 2 - 2] >= code_point)) {
        return fail("character unit table is invalid");
      }
      const std::string_view piece = Piece(id);
      if (utf8::DecodeCodePoint(piece.data(), piece.size(), &decoded) !=
              piece.size() ||
          decoded != code_point) {
        return fail("character unit does not match its token");
      }
    }
  }

  // 特殊 token 字节 trie：节点 (首条边, 边数, 终点 ID+1)。
  uint32_t edge_end = 0;
  for (uint32_t i = 0; i < trie_nodes_; ++i) {
    const uint32_t* node = trie_nodes_data_ + size_t{i} * 3;
    if (node[0] != edge_end || node[1] > 256 ||
        node[1] > trie_edges - edge_end || node[2] > token_count_) {
      return fail("special token trie node is invalid");
    }
    for (uint32_t j = 0; j < node[1]; ++j) {
      const uint32_t* edge = trie_edges_ + (size_t{node[0]} + j) * 2;
      if (edge[0] > 255 || edge[1] <= i || edge[1] >= trie_nodes_ ||
          (j != 0 && edge[-2] >= edge[0])) {
        return fail("special token trie edge is invalid");
      }
    }
    edge_end += node[1];
  }
  if (edge_end != trie_edges || trie_nodes_data_[2] != 0) {
    return fail("special token trie is inconsistent");
  }
  trie_root_.fill(0);
  for (uint32_t j = 0; j < trie_nodes_data_[1]; ++j) {
    const uint32_t* edge = trie_edges_ + size_t{j} * 2;
    trie_root_[edge[0]] = edge[1];
  }

  end_tokens_.reserve(end_count);
  for (uint32_t i = 0; i < end_count; ++i) {
    if (ends[i] >= token_count_ || (i != 0 && ends[i - 1] >= ends[i])) {
      return fail("end token list is invalid");
    }
    end_tokens_.push_back(static_cast<int32_t>(ends[i]));
  }

  UnicodeSections unicode;
  unicode.properties = words(kSectionProperties);
  unicode.property_rows = h[kPropertyRowsWord];
  unicode.decompositions = words(kSectionDecompositions);
  unicode.decomposition_rows = h[kDecompositionRowsWord];
  unicode.pool = words(kSectionDecompositionPool);
  unicode.pool_count = h[kDecompositionPoolWord];
  unicode.combining = words(kSectionCombining);
  unicode.combining_rows = h[kCombiningRowsWord];
  unicode.compositions = words(kSectionCompositions);
  unicode.composition_rows = h[kCompositionRowsWord];
  const char* unicode_reason = nullptr;
  if (!unicode_.Bind(unicode, pre_tokenizer_ == kPreTokenizerByteLevelQwen2,
                     normalizer_ == kNormalizerNfc, &unicode_reason)) {
    return fail(unicode_reason);
  }
  return true;
}

int32_t BpeTokenizer::Impl::Lookup(const char* bytes,
                                   size_t size) const noexcept {
  uint32_t position =
      static_cast<uint32_t>(HashBytes(bytes, size)) & (hash_slots_ - 1);
  // 加载时已确认存在空槽，探测次数有上界。
  for (uint32_t probes = 0; probes < hash_slots_;
       ++probes, position = (position + 1) & (hash_slots_ - 1)) {
    const uint32_t value = slots_[position];
    if (value == 0) return -1;
    const uint32_t begin = offsets_[value - 1];
    if (offsets_[value] - begin == size &&
        std::memcmp(pool_ + begin, bytes, size) == 0) {
      return static_cast<int32_t>(value - 1);
    }
  }
  return -1;
}

// 在 text 开头做最长匹配；匹配失败返回 -1。
int32_t BpeTokenizer::Impl::MatchAddedToken(const char* text, size_t size,
                                            size_t* length) const noexcept {
  if (size == 0) return -1;
  uint32_t node = trie_root_[static_cast<unsigned char>(text[0])];
  int32_t id = -1;
  for (size_t depth = 1; node != 0; ++depth) {
    const uint32_t* current = trie_nodes_data_ + size_t{node} * 3;
    if (current[2] != 0) {
      id = static_cast<int32_t>(current[2] - 1);
      *length = depth;
    }
    if (depth == size) break;
    const uint32_t label = static_cast<unsigned char>(text[depth]);
    uint32_t low = current[0];
    uint32_t high = current[0] + current[1];
    while (low < high) {
      const uint32_t middle = low + (high - low) / 2;
      if (trie_edges_[size_t{middle} * 2] < label) {
        low = middle + 1;
      } else {
        high = middle;
      }
    }
    node =
        low < current[0] + current[1] && trie_edges_[size_t{low} * 2] == label
            ? trie_edges_[size_t{low} * 2 + 1]
            : 0;
  }
  return id;
}

bool BpeTokenizer::Impl::MakeCandidate(const char* word_data,
                                       const std::vector<Symbol>& symbols,
                                       uint32_t left, uint32_t right,
                                       Candidate* candidate) const noexcept {
  const Symbol& a = symbols[left];
  const Symbol& b = symbols[right];
  if (a.id < 0 || b.id < 0) return false;
  const uint32_t size = a.size + b.size;
  if (size > max_token_bytes_) return false;
  // 两个相邻符号拼接后的字节正好是 word 中连续的一段。
  const int32_t id = Lookup(word_data + a.start, size);
  if (id < 0) return false;
  uint32_t priority = 0;
  if (merge_priority_ == kPriorityRank) {
    // 该 token 自己的合并规则中，左段长度相同的那一条。
    bool found = false;
    for (uint32_t j = rank_offsets_[id]; j < rank_offsets_[id + 1]; ++j) {
      if ((rank_entries_[j] & kLeftLengthMask) == a.size) {
        priority = kMaxRank - (rank_entries_[j] >> kRankShift);
        found = true;
        break;
      }
    }
    if (!found) return false;
  } else {
    priority = ScorePriority(scores_[id]);
  }
  candidate->priority = priority;
  candidate->left = left;
  candidate->left_chars = a.chars;
  candidate->right_chars = b.chars;
  candidate->right = right;
  candidate->size = size;
  candidate->id = id;
  return true;
}

int BpeTokenizer::Impl::MergeWord(std::string_view word,
                                  std::vector<int32_t>* ids,
                                  const char** reason) const {
  if (word.empty()) return error::kOk;
  // 符号数不超过字节数：先按字节数分配，填完后再收缩。
  // 在带 AddressSanitizer 的构建里逐个 push_back 临时对象很慢，因此按下标写入。
  std::vector<Symbol> symbols(word.size());
  size_t count = 0;
  const auto set_symbol = [&symbols](size_t index, size_t start, size_t size,
                                     int32_t id) {
    Symbol& symbol = symbols[index];
    symbol.start = static_cast<uint32_t>(start);
    symbol.size = static_cast<uint32_t>(size);
    symbol.prev = static_cast<int32_t>(index) - 1;
    symbol.next = static_cast<int32_t>(index) + 1;
    symbol.id = id;
    symbol.chars = 1;
  };
  if (pre_tokenizer_ == kPreTokenizerByteLevelQwen2) {
    for (; count < word.size(); ++count) {
      set_symbol(count, count, 1,
                 static_cast<int32_t>(
                     units_[static_cast<unsigned char>(word[count]) * 2 + 1]));
    }
  } else {
    for (size_t position = 0; position < word.size();) {
      uint32_t code_point = static_cast<unsigned char>(word[position]);
      size_t size = 1;
      if (code_point >= 0x80) {
        size = utf8::DecodeCodePoint(word.data() + position,
                                     word.size() - position, &code_point);
        if (size == 0) {
          *reason = "Tokenizer input is not valid UTF-8";
          return error::kInvalidUtf8;
        }
      }
      // 在有序的 (码点, ID) 表里二分查找。
      size_t low = 0;
      size_t high = unit_count_;
      while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (units_[middle * 2] < code_point) {
          low = middle + 1;
        } else {
          high = middle;
        }
      }
      int32_t id = -1;
      if (low < unit_count_ && units_[low * 2] == code_point) {
        id = static_cast<int32_t>(units_[low * 2 + 1]);
      } else if (!byte_fallback_) {
        *reason = "Character is not in the tokenizer vocabulary";
        return error::kInvalidInput;
      }
      set_symbol(count++, position, size, id);
      position += size;
    }
  }
  symbols.resize(count);
  symbols.back().next = -1;

  CandidateHeap heap;
  Candidate candidate{};
  const auto add_candidate = [&](int32_t left, int32_t right) {
    if (left < 0 || right < 0 ||
        !MakeCandidate(word.data(), symbols, static_cast<uint32_t>(left),
                       static_cast<uint32_t>(right), &candidate)) {
      return;
    }
    heap.Push(candidate);
  };
  for (size_t i = 1; i < symbols.size(); ++i) {
    add_candidate(static_cast<int32_t>(i) - 1, static_cast<int32_t>(i));
  }
  Candidate top{};
  while (!heap.empty()) {
    heap.Pop(&top);
    Symbol& left = symbols[top.left];
    Symbol& right = symbols[top.right];
    // 任何一侧在候选产生后发生过合并，大小之和都会变化。
    if (left.size == 0 || right.size == 0 ||
        left.next != static_cast<int32_t>(top.right) ||
        left.size + right.size != top.size) {
      continue;
    }
    left.size = top.size;
    left.id = top.id;
    left.chars += right.chars;
    left.next = right.next;
    if (right.next >= 0) {
      symbols[static_cast<size_t>(right.next)].prev =
          static_cast<int32_t>(top.left);
    }
    right.size = 0;
    add_candidate(left.prev, static_cast<int32_t>(top.left));
    add_candidate(static_cast<int32_t>(top.left), left.next);
  }
  for (int32_t i = 0; i >= 0; i = symbols[static_cast<size_t>(i)].next) {
    const Symbol& symbol = symbols[static_cast<size_t>(i)];
    if (symbol.id >= 0) {
      ids->push_back(symbol.id);
    } else {
      for (uint32_t j = 0; j < symbol.size; ++j) {
        ids->push_back(
            byte_ids_[static_cast<unsigned char>(word[symbol.start + j])]);
      }
    }
  }
  return error::kOk;
}

int BpeTokenizer::Impl::EncodeSegment(std::string_view segment,
                                      std::vector<int32_t>* ids,
                                      const char** reason) const {
  if (segment.empty()) return error::kOk;
  std::string scratch;
  std::string_view normalized = segment;
  if (normalizer_ == kNormalizerReplaceSpaceMetaspace) {
    // 1 字节的空格变成 3 字节的 U+2581；容量按输入的 3 倍计算。
    size_t capacity = 0;
    if (!MultiplyChecked(segment.size(), kMetaspaceBytes, &capacity)) {
      *reason = "Tokenizer input is too large";
      return error::kInvalidInput;
    }
    scratch.reserve(capacity);
    for (char c : segment) {
      if (c == ' ') {
        scratch.append(kMetaspace, kMetaspaceBytes);
      } else {
        scratch.push_back(c);
      }
    }
    normalized = scratch;
  } else if (normalizer_ == kNormalizerNfc) {
    std::vector<uint32_t> code_points;
    unicode_.NormalizeNfc(segment, &code_points);
    size_t capacity = 0;
    if (!MultiplyChecked(code_points.size(), 4, &capacity)) {
      *reason = "Tokenizer input is too large";
      return error::kInvalidInput;
    }
    scratch.reserve(capacity);
    for (uint32_t code_point : code_points)
      AppendCodePoint(&scratch, code_point);
    normalized = scratch;
  }
  if (normalized.size() > std::numeric_limits<uint32_t>::max()) {
    *reason = "Tokenizer input is too large";
    return error::kInvalidInput;
  }

  if (pre_tokenizer_ == kPreTokenizerNone) {
    return MergeWord(normalized, ids, reason);
  }
  std::vector<std::pair<size_t, size_t>> words;
  if (!SplitByteLevelQwen2(unicode_, normalized, &words)) {
    *reason = "Unhandled pre-tokenizer character";
    return error::kInvalidState;
  }
  for (const auto& word : words) {
    const int code = MergeWord(
        normalized.substr(word.first, word.second - word.first), ids, reason);
    if (code != error::kOk) return code;
  }
  return error::kOk;
}

int BpeTokenizer::Impl::Encode(std::string_view text, std::vector<int32_t>* ids,
                               std::string* diagnostic) const {
  if (!ids) {
    SetDiagnosticNoexcept(diagnostic, "Token output pointer is null");
    return error::kInvalidArgument;
  }
  ids->clear();
  if (text.size() > kMaxEncodeBytes) {
    SetDiagnosticNoexcept(diagnostic, "Tokenizer input exceeds the size limit");
    return error::kInvalidInput;
  }
  if (!IsValidUtf8(text)) {
    SetDiagnosticNoexcept(diagnostic, "Tokenizer input is not valid UTF-8");
    return error::kInvalidUtf8;
  }
  const char* reason = nullptr;
  // 先在原始文本上按最左、最长规则切出特殊 token，其余片段各自走完整流程。
  size_t begin = 0;
  size_t i = 0;
  while (i < text.size()) {
    // 绝大多数字节不是任何特殊 token 的首字节，直接跳过。
    if (trie_root_[static_cast<unsigned char>(text[i])] == 0) {
      ++i;
      continue;
    }
    size_t length = 0;
    const int32_t id =
        MatchAddedToken(text.data() + i, text.size() - i, &length);
    if (id < 0) {
      ++i;
      continue;
    }
    const int code = EncodeSegment(text.substr(begin, i - begin), ids, &reason);
    if (code != error::kOk) {
      ids->clear();
      SetDiagnosticNoexcept(diagnostic, reason);
      return code;
    }
    ids->push_back(id);
    i += length;
    begin = i;
  }
  const int code = EncodeSegment(text.substr(begin), ids, &reason);
  if (code != error::kOk) {
    ids->clear();
    SetDiagnosticNoexcept(diagnostic, reason);
    return code;
  }
  return error::kOk;
}

int BpeTokenizer::Impl::AppendTokenBytes(int32_t id, std::string* bytes,
                                         std::string* diagnostic) const {
  if (!bytes) {
    SetDiagnosticNoexcept(diagnostic, "Token bytes output pointer is null");
    return error::kInvalidArgument;
  }
  if (id < 0 || static_cast<uint32_t>(id) >= token_count_) {
    SetDiagnosticNoexcept(diagnostic, "Token id is out of range");
    return error::kInvalidInput;
  }
  const uint32_t token = static_cast<uint32_t>(id);
  const uint8_t kind = Kind(token);
  if (kind == kKindSpecial) return error::kOk;
  if (decoder_ == kDecoderMetaspaceByteFallback) {
    if (kind == kKindByteFallback) {
      bytes->push_back(ByteValue(token));
    } else {
      AppendWithSpaces(PieceData(token), PieceSize(token), bytes);
    }
  } else {
    bytes->append(PieceData(token), PieceSize(token));
  }
  return error::kOk;
}

int BpeTokenizer::Impl::Decode(const std::vector<int32_t>& ids,
                               std::string* text,
                               std::string* diagnostic) const {
  if (!text) {
    SetDiagnosticNoexcept(diagnostic, "Decoded text output pointer is null");
    return error::kInvalidArgument;
  }
  text->clear();
  for (int32_t id : ids) {
    if (id < 0 || static_cast<uint32_t>(id) >= token_count_) {
      text->clear();
      SetDiagnosticNoexcept(diagnostic, "Token id is out of range");
      return error::kInvalidInput;
    }
  }
  if (decoder_ == kDecoderByteLevel) {
    // 先拼接所有字节，再对整体做有损 UTF-8 解码。
    std::string bytes;
    for (int32_t id : ids) {
      const uint32_t token = static_cast<uint32_t>(id);
      if (Kind(token) != kKindSpecial)
        bytes.append(PieceData(token), PieceSize(token));
    }
    AppendLossyUtf8(bytes, text);
    return error::kOk;
  }

  // 连续的字节回退 token 凑成一段字节：整段是有效 UTF-8 时原样保留，否则每个
  // 字节各替换为一个 U+FFFD，与 HF ByteFallback 一致。被跳过的特殊 token 不会
  // 截断这一段。字节暂存在输出里，不另建缓冲。
  size_t run_start = 0;
  size_t run_bytes = 0;
  const auto flush_run = [&] {
    if (run_bytes == 0) return;
    if (!IsValidUtf8(std::string_view(*text).substr(run_start, run_bytes))) {
      text->resize(run_start);
      for (size_t i = 0; i < run_bytes; ++i) text->append("\xEF\xBF\xBD", 3);
    }
    run_bytes = 0;
  };
  for (int32_t id : ids) {
    const uint32_t token = static_cast<uint32_t>(id);
    const uint8_t kind = Kind(token);
    if (kind == kKindSpecial) continue;
    if (kind == kKindByteFallback) {
      if (run_bytes == 0) run_start = text->size();
      text->push_back(ByteValue(token));
      ++run_bytes;
    } else {
      flush_run();
      AppendWithSpaces(PieceData(token), PieceSize(token), text);
    }
  }
  flush_run();
  return error::kOk;
}

BpeTokenizer::BpeTokenizer(std::unique_ptr<const Impl> impl) noexcept
    : impl_(std::move(impl)) {}

BpeTokenizer::~BpeTokenizer() = default;

std::shared_ptr<const BpeTokenizer> BpeTokenizer::Load(
    const std::string& file, std::string* diagnostic) noexcept {
  try {
    if (!HostIsLittleEndian()) {
      SetLoadDiagnostic(diagnostic, file,
                        "BPE tokenizer files require a little-endian host");
      return nullptr;
    }
    std::string reason;
    auto mapped = MappedFile::Open(file, &reason);
    if (!mapped) {
      SetLoadDiagnostic(diagnostic, file, reason);
      return nullptr;
    }
    std::unique_ptr<Impl> impl(new Impl());
    if (!impl->Init(std::move(mapped), &reason)) {
      SetLoadDiagnostic(diagnostic, file,
                        "invalid or unsupported BPE tokenizer file: " + reason);
      return nullptr;
    }
    return std::shared_ptr<const BpeTokenizer>(
        new BpeTokenizer(std::move(impl)));
  } catch (const std::exception& e) {
    SetLoadDiagnostic(diagnostic, file, e.what());
  } catch (...) {
    SetLoadDiagnostic(diagnostic, file,
                      "unknown exception while loading tokenizer");
  }
  return nullptr;
}

int BpeTokenizer::Encode(std::string_view text, std::vector<int32_t>* ids,
                         std::string* diagnostic) const noexcept {
  try {
    return impl_->Encode(text, ids, diagnostic);
  } catch (const std::exception& e) {
    if (ids) ids->clear();
    SetDiagnosticNoexcept(diagnostic, e.what());
    return error::kInternalException;
  } catch (...) {
    if (ids) ids->clear();
    SetDiagnosticNoexcept(diagnostic, "Unknown tokenizer exception");
    return error::kUnknownException;
  }
}

int BpeTokenizer::AppendTokenBytes(int32_t id, std::string* bytes,
                                   std::string* diagnostic) const noexcept {
  try {
    return impl_->AppendTokenBytes(id, bytes, diagnostic);
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(diagnostic, e.what());
    return error::kInternalException;
  } catch (...) {
    SetDiagnosticNoexcept(diagnostic, "Unknown token decode exception");
    return error::kUnknownException;
  }
}

int BpeTokenizer::Decode(const std::vector<int32_t>& ids, std::string* text,
                         std::string* diagnostic) const noexcept {
  try {
    return impl_->Decode(ids, text, diagnostic);
  } catch (const std::exception& e) {
    if (text) text->clear();
    SetDiagnosticNoexcept(diagnostic, e.what());
    return error::kInternalException;
  } catch (...) {
    if (text) text->clear();
    SetDiagnosticNoexcept(diagnostic, "Unknown token decode exception");
    return error::kUnknownException;
  }
}

bool BpeTokenizer::IsEndToken(int32_t id) const noexcept {
  const auto& end_tokens = impl_->end_tokens();
  return std::binary_search(end_tokens.begin(), end_tokens.end(), id);
}

const std::vector<int32_t>& BpeTokenizer::EndTokens() const noexcept {
  return impl_->end_tokens();
}

size_t BpeTokenizer::VocabSize() const noexcept { return impl_->vocab_size(); }

}  // namespace text
}  // namespace llm_edgeflow
