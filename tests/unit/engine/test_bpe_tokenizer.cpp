#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "contracts/error_codes.h"
#include "engine/text/bpe_tokenizer.h"

// 两份合成分词器文件由 tools/prepare_bpe_tokenizer.py fixture 在构建时生成。
//
// metaspace：score 优先级、字符级单位、字节回退、Replace(" ", "▁")。
//   ID 0..5 特殊 token，6..261 是 <0xXX>（ID = 6 + 字节值），262
//   起是字符与合并结果。
// byte_level：rank 优先级、字节级单位、NFC、byte_level_qwen2 预分词。
//   ID 0..255 是单字节 token（ID = 字节值），之后按规则顺序编号。
// fixture 内容见工具中的 fixture_metaspace / fixture_byte_level。
namespace llm_edgeflow {
namespace text {
namespace {

using Ids = std::vector<int32_t>;

constexpr int32_t kByteTokenBase = 6;       // metaspace 中 <0x00> 的 ID
constexpr int32_t kFirstNormalToken = 262;  // metaspace 中第一个字符 token

const std::shared_ptr<const BpeTokenizer>& Metaspace() {
  static const auto tokenizer =
      BpeTokenizer::Load(EDGEFLOW_BPE_METASPACE_FIXTURE);
  return tokenizer;
}

const std::shared_ptr<const BpeTokenizer>& ByteLevel() {
  static const auto tokenizer =
      BpeTokenizer::Load(EDGEFLOW_BPE_BYTE_LEVEL_FIXTURE);
  return tokenizer;
}

Ids Encode(const BpeTokenizer& tokenizer, std::string_view text) {
  Ids ids{12345};
  std::string diagnostic;
  EXPECT_EQ(tokenizer.Encode(text, &ids, &diagnostic), 0) << diagnostic;
  return ids;
}

std::string Decode(const BpeTokenizer& tokenizer, const Ids& ids) {
  std::string text = "stale";
  std::string diagnostic;
  EXPECT_EQ(tokenizer.Decode(ids, &text, &diagnostic), 0) << diagnostic;
  return text;
}

std::string Concatenate(const BpeTokenizer& tokenizer, const Ids& ids) {
  std::string bytes;
  for (int32_t id : ids) {
    EXPECT_EQ(tokenizer.AppendTokenBytes(id, &bytes), 0);
  }
  return bytes;
}

// 按 AppendTokenBytes 的结果查找普通 token 的 ID。
int32_t IdOf(const BpeTokenizer& tokenizer, std::string_view bytes,
             int32_t first) {
  for (int32_t id = first; static_cast<size_t>(id) < tokenizer.VocabSize();
       ++id) {
    std::string token;
    if (tokenizer.AppendTokenBytes(id, &token) == 0 && token == bytes) {
      return id;
    }
  }
  ADD_FAILURE() << "token not found: " << bytes;
  return -1;
}

int32_t M(std::string_view bytes) {
  return IdOf(*Metaspace(), bytes, kFirstNormalToken);
}
int32_t B(std::string_view bytes) { return IdOf(*ByteLevel(), bytes, 0); }
int32_t MByte(unsigned byte) {
  return kByteTokenBase + static_cast<int32_t>(byte);
}

// 特殊 token 追加空串，改用编码它自己的文本得到 ID。
int32_t SpecialId(const BpeTokenizer& tokenizer, std::string_view text) {
  const Ids ids = Encode(tokenizer, text);
  EXPECT_EQ(ids.size(), 1U) << text;
  return ids.empty() ? -1 : ids[0];
}

std::string ReadFile(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(stream),
                     std::istreambuf_iterator<char>());
}

// 每个用例实例、每个进程一份临时文件，ctest -j 并行运行时互不干扰；析构时删除。
class ScopedTempFile final {
 public:
  explicit ScopedTempFile(const std::string& stem) {
    std::string test =
        ::testing::UnitTest::GetInstance()->current_test_info()->name();
    std::replace_if(
        test.begin(), test.end(),
        [](char c) { return !std::isalnum(static_cast<unsigned char>(c)); },
        '_');
    path_ = (std::filesystem::path(::testing::TempDir()) /
             (stem + "." + std::to_string(getpid()) + "." + test + ".eftok"))
                .string();
  }
  ~ScopedTempFile() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }
  ScopedTempFile(const ScopedTempFile&) = delete;
  ScopedTempFile& operator=(const ScopedTempFile&) = delete;

  // 以新内容覆盖文件，返回路径。
  const std::string& Write(const std::string& content) const {
    std::ofstream stream(path_, std::ios::binary | std::ios::trunc);
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    return path_;
  }

 private:
  std::string path_;
};

uint32_t Crc32(std::string_view bytes) {
  uint32_t crc = ~uint32_t{0};
  for (unsigned char byte : bytes) {
    crc ^= byte;
    for (int j = 0; j < 8; ++j)
      crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320U : 0);
  }
  return ~crc;
}

uint32_t Word(const std::string& bytes, size_t index) {
  uint32_t value = 0;
  std::memcpy(&value, bytes.data() + index * 4, 4);
  return value;
}

void SetWord(std::string* bytes, size_t index, uint32_t value) {
  std::memcpy(bytes->data() + index * 4, &value, 4);
}

// 重新计算 CRC，使修改只触发结构校验而不是 CRC 校验。
void FixCrc(std::string* bytes) {
  SetWord(bytes, 3, Crc32(std::string_view(*bytes).substr(16)));
}

// 头部字：4..8 五个参数，24..39 区段偏移（按写出顺序）。
constexpr size_t kWordNormalizer = 4;
constexpr size_t kWordPreTokenizer = 5;
constexpr size_t kWordByteFallback = 6;
constexpr size_t kWordPriority = 7;
constexpr size_t kWordDecoder = 8;
constexpr size_t kWordTokenCount = 9;
constexpr size_t kWordCompositionRows = 23;
constexpr size_t kSectionBase = 24;
constexpr size_t kSectionOffsets = 0;
constexpr size_t kSectionKinds = 2;
constexpr size_t kSectionHash = 3;
constexpr size_t kSectionEnds = 10;

size_t SectionWord(const std::string& bytes, size_t section) {
  return Word(bytes, kSectionBase + section) / 4;
}

struct LoadFailure {
  const char* name;
  const char* reason;
  bool fix_crc;
  void (*mutate)(std::string*);
};

const LoadFailure kLoadFailures[] = {
    {"bad_magic", "bad magic", false, [](std::string* b) { (*b)[0] = 'X'; }},
    {"extra_byte", "recorded file size differs", false,
     [](std::string* b) { b->push_back('\0'); }},
    {"short_file", "recorded file size differs", false,
     [](std::string* b) { b->pop_back(); }},
    {"crc", "CRC mismatch", false,
     [](std::string* b) { b->back() = static_cast<char>(b->back() ^ 1); }},
    {"normalizer", "unknown normalizer", true,
     [](std::string* b) { SetWord(b, kWordNormalizer, 9); }},
    {"pre_tokenizer", "unknown pre-tokenizer", true,
     [](std::string* b) { SetWord(b, kWordPreTokenizer, 9); }},
    {"byte_fallback", "unknown byte fallback", true,
     [](std::string* b) { SetWord(b, kWordByteFallback, 2); }},
    {"priority", "unknown merge priority", true,
     [](std::string* b) { SetWord(b, kWordPriority, 9); }},
    {"decoder", "unknown decoder", true,
     [](std::string* b) { SetWord(b, kWordDecoder, 9); }},
    {"token_count", "token count is outside", true,
     [](std::string* b) { SetWord(b, kWordTokenCount, 0x7FFFFFFFU); }},
    {"section_offset", "section offsets are inconsistent", true,
     [](std::string* b) {
       SetWord(b, kSectionBase + kSectionHash,
               Word(*b, kSectionBase + kSectionHash) + 4);
     }},
    {"section_beyond_file", "section exceeds the file", true,
     [](std::string* b) {
       // 最后一个区段的起点不变，但声明的行数超出了文件。
       SetWord(b, kWordCompositionRows, Word(*b, kWordCompositionRows) + 1000);
     }},
    {"token_offsets", "token offsets are not strictly increasing", true,
     [](std::string* b) {
       SetWord(b, SectionWord(*b, kSectionOffsets) + 1, 0);
     }},
    {"token_kind", "token kind is invalid", true,
     [](std::string* b) {
       const size_t at = SectionWord(*b, kSectionKinds) * 4;
       (*b)[at] = 7;
     }},
    {"byte_fallback_kind", "malformed or duplicate byte-fallback token", true,
     [](std::string* b) {
       // 第一个 token 的内容不是 <0xXX>，却被标成字节回退。
       (*b)[SectionWord(*b, kSectionKinds) * 4] = 1;
     }},
    {"hash_slot", "hash slot is out of range", true,
     [](std::string* b) {
       SetWord(b, SectionWord(*b, kSectionHash), 0x7FFFFFFFU);
     }},
    {"end_token", "end token list is invalid", true,
     [](std::string* b) {
       SetWord(b, SectionWord(*b, kSectionEnds), 0xFFFFFF);
     }},
};

class BpeTokenizerLoadTest : public ::testing::TestWithParam<const char*> {};

TEST_P(BpeTokenizerLoadTest, RejectsMissingTruncatedAndCorruptFiles) {
  const std::string source = GetParam();
  const std::string original = ReadFile(source);
  ASSERT_GT(original.size(), 160U);
  ASSERT_NE(BpeTokenizer::Load(source), nullptr);

  std::string diagnostic;
  EXPECT_EQ(BpeTokenizer::Load("/nonexistent/dir/missing.eftok", &diagnostic),
            nullptr);
  EXPECT_NE(diagnostic.find("cannot open file"), std::string::npos)
      << diagnostic;
  EXPECT_NE(diagnostic.find("missing.eftok"), std::string::npos);

  diagnostic.clear();
  EXPECT_EQ(BpeTokenizer::Load(::testing::TempDir(), &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("not a regular file"), std::string::npos)
      << diagnostic;

  const ScopedTempFile temp("bpe_load");
  for (const size_t size : {size_t{0}, size_t{7}, size_t{100}, size_t{159}}) {
    SCOPED_TRACE(size);
    diagnostic.clear();
    const std::string& path = temp.Write(original.substr(0, size));
    EXPECT_EQ(BpeTokenizer::Load(path, &diagnostic), nullptr);
    EXPECT_NE(diagnostic.find(path), std::string::npos) << diagnostic;
  }

  for (const LoadFailure& failure : kLoadFailures) {
    SCOPED_TRACE(failure.name);
    std::string bytes = original;
    failure.mutate(&bytes);
    if (failure.fix_crc) FixCrc(&bytes);
    const std::string& path = temp.Write(bytes);
    diagnostic.clear();
    EXPECT_EQ(BpeTokenizer::Load(path, &diagnostic), nullptr);
    EXPECT_NE(diagnostic.find(path), std::string::npos) << diagnostic;
    EXPECT_NE(diagnostic.find(failure.reason), std::string::npos) << diagnostic;
  }
  // 以上断言都针对副本；原文件仍然可以加载。
  EXPECT_NE(BpeTokenizer::Load(source), nullptr);
}

INSTANTIATE_TEST_SUITE_P(Fixtures, BpeTokenizerLoadTest,
                         ::testing::Values(EDGEFLOW_BPE_METASPACE_FIXTURE,
                                           EDGEFLOW_BPE_BYTE_LEVEL_FIXTURE));

TEST(BpeTokenizerTest, LoadsWithoutDiagnosticPointerAndReportsMetadata) {
  ASSERT_NE(Metaspace(), nullptr);
  ASSERT_NE(ByteLevel(), nullptr);
  EXPECT_EQ(Metaspace()->VocabSize(), 310U);
  EXPECT_EQ(Metaspace()->EndTokens(), (Ids{1, 5}));
  EXPECT_EQ(ByteLevel()->VocabSize(), 287U);
  EXPECT_EQ(ByteLevel()->EndTokens(), (Ids{283, 285}));
  EXPECT_EQ(BpeTokenizer::Load("/nonexistent.eftok"),
            nullptr);  // 无 diagnostic
}

TEST(BpeTokenizerTest, IsEndTokenMatchesTheRecordedList) {
  for (int32_t id = -2; id < 320; ++id) {
    EXPECT_EQ(Metaspace()->IsEndToken(id), id == 1 || id == 5) << id;
    EXPECT_EQ(ByteLevel()->IsEndToken(id), id == 283 || id == 285) << id;
  }
}

// score
// 优先级的组合键：分数高者先；同分时左段码点数多者先，再右段码点数多者先，
// 再合成 token 的 ID 小者先；键完全相同时取最左。
TEST(BpeTokenizerTest, ScoreMergesFollowTheCompositeKeyAndLeftmostOnTies) {
  const BpeTokenizer& t = *Metaspace();
  // abcd：bc(-10) 最先，得到 a|bc|d。之后 abc 与 bcd
  // 同分(-13)：只看分数并取最左会合并 abc，得到
  // abc|d；组合键比较左段码点数，bcd（左段 bc 有 2 个）先于 abc（左段 a 有 1
  // 个）， 尽管 abc 更靠左、ID 也更小。
  ASSERT_LT(M("abc"), M("bcd"));
  EXPECT_EQ(Encode(t, "abcd"), (Ids{M("a"), M("bcd")}));
  // pqr：pq 与 qr 同分，左右段码点数也相同，最后按合成 token 的 ID：qr 的 ID
  // 更小， 所以先合并 qr，得到 p|qr；只取最左会得到 pq|r。
  ASSERT_LT(M("qr"), M("pq"));
  EXPECT_EQ(Encode(t, "pqr"), (Ids{M("p"), M("qr")}));
  EXPECT_EQ(Encode(t, "pq"), (Ids{M("pq")}));
  EXPECT_EQ(Encode(t, "abc"), (Ids{M("abc")}));
  EXPECT_EQ(Encode(t, "ab"), (Ids{M("ab")}));
  // 同分：aaa 的两个候选分数相同，先合并最左的。
  EXPECT_EQ(Encode(t, "aaa"), (Ids{M("aa"), M("a")}));
  EXPECT_EQ(Encode(t, "aaaa"), (Ids{M("aa"), M("aa")}));
  EXPECT_EQ(Encode(t, "aaaaa"), (Ids{M("aa"), M("aa"), M("a")}));
  // 逐级合并。
  EXPECT_EQ(Encode(t, "hello"), (Ids{M("hello")}));
  EXPECT_EQ(Encode(t, " hello"), (Ids{M(" hello")}));
  EXPECT_EQ(Encode(t, "  hello"), (Ids{M(" "), M(" hello")}));
  EXPECT_EQ(Encode(t, "\xE4\xBD\xA0\xE5\xA5\xBD"),
            (Ids{M("\xE4\xBD\xA0\xE5\xA5\xBD")}));
  EXPECT_TRUE(Encode(t, "").empty());
}

// rank 优先级：排名小的先合并，同排名取最左；token 只接受自己登记的切分。
TEST(BpeTokenizerTest, RankMergesLowerRankFirstAndHonoursRecordedSplits) {
  const BpeTokenizer& t = *ByteLevel();
  EXPECT_EQ(Encode(t, "abcd"), (Ids{B("a"), B("bcd")}));
  EXPECT_EQ(Encode(t, "abc"), (Ids{B("abc")}));
  EXPECT_EQ(Encode(t, "aaa"), (Ids{B("aa"), B("a")}));
  EXPECT_EQ(Encode(t, "aaaa"), (Ids{B("aa"), B("aa")}));
  EXPECT_EQ(Encode(t, "hello"), (Ids{B("hello")}));
  // pq 先合并，得到 pq|r；拼接结果 pqr 在词表中，但它登记的切分是 (p, qr)，
  // 左段长度为 2 的切分不存在，所以不能合并。
  EXPECT_EQ(Encode(t, "pqr"), (Ids{B("pq"), B("r")}));
  EXPECT_EQ(Encode(t, "qr"), (Ids{B("qr")}));
  // mno 的第二条登记切分 (mn, o)。
  EXPECT_EQ(Encode(t, "mno"), (Ids{B("mno")}));
  EXPECT_EQ(Encode(t, "\xC3\xA9"), (Ids{B("\xC3\xA9")}));
}

TEST(BpeTokenizerTest, RejectsInvalidInputAndBadArguments) {
  for (const BpeTokenizer* t : {Metaspace().get(), ByteLevel().get()}) {
    for (const std::string_view text :
         {std::string_view("\xFF"), std::string_view("a\xC0\x80"),
          std::string_view("\xED\xA0\x80"), std::string_view("\xE4\xBD"),
          std::string_view("\xF4\x90\x80\x80"), std::string_view("ok\x80")}) {
      Ids ids{1, 2, 3};
      std::string diagnostic;
      EXPECT_EQ(t->Encode(text, &ids, &diagnostic), error::kInvalidUtf8);
      EXPECT_TRUE(ids.empty());
      EXPECT_NE(diagnostic.find("UTF-8"), std::string::npos) << diagnostic;
    }
    std::string diagnostic;
    EXPECT_EQ(t->Encode("a", nullptr, &diagnostic), error::kInvalidArgument);
    EXPECT_FALSE(diagnostic.empty());
    Ids ids;
    EXPECT_EQ(t->Encode("a", &ids), error::kOk);  // diagnostic 可以为空指针
    EXPECT_EQ(t->Encode("\xFF", &ids), error::kInvalidUtf8);
  }
}

// 精确的上限边界：恰好 kMaxEncodeBytes 能编码，多 1
// 字节被拒绝。填充文本没有可合并的 单位，使每个用例在 sanitizer
// 下也能在超时之内完成。
void ExpectExactMaximumInput(const BpeTokenizer& tokenizer, int32_t filler_id) {
  const std::string text(kMaxEncodeBytes, 'x');
  Ids ids;
  std::string diagnostic;
  ASSERT_EQ(tokenizer.Encode(text, &ids, &diagnostic), 0) << diagnostic;
  ASSERT_EQ(ids.size(), kMaxEncodeBytes);
  EXPECT_TRUE(std::all_of(ids.begin(), ids.end(),
                          [filler_id](int32_t id) { return id == filler_id; }));

  const std::string too_long(kMaxEncodeBytes + 1, 'x');
  EXPECT_EQ(tokenizer.Encode(too_long, &ids, &diagnostic),
            error::kInvalidInput);
  EXPECT_TRUE(ids.empty());
  EXPECT_NE(diagnostic.find("size limit"), std::string::npos) << diagnostic;
}

TEST(BpeTokenizerTest, MetaspaceAcceptsExactlyTheMaximumInputSize) {
  ExpectExactMaximumInput(*Metaspace(), M("x"));
}

TEST(BpeTokenizerTest, ByteLevelAcceptsExactlyTheMaximumInputSize) {
  ExpectExactMaximumInput(*ByteLevel(), B("x"));
}

// 合并密集的大输入：每一步都有合并，堆和符号数组按输入长度动态增长。
TEST(BpeTokenizerTest, LargeMergeHeavyInputsMergeEveryPair) {
  const std::string text(kMaxEncodeBytes, 'a');
  for (const BpeTokenizer* t : {Metaspace().get(), ByteLevel().get()}) {
    const int32_t pair = t == Metaspace().get() ? M("aa") : B("aa");
    Ids ids;
    std::string diagnostic;
    ASSERT_EQ(t->Encode(text, &ids, &diagnostic), 0) << diagnostic;
    ASSERT_EQ(ids.size(), kMaxEncodeBytes / 2);
    EXPECT_TRUE(std::all_of(ids.begin(), ids.end(),
                            [pair](int32_t id) { return id == pair; }));
  }
}

// 前 64 KiB 与之后的内容不同：偏移或长度被截断为 16 位时，后半段的查找会读到
// 前半段的字节，结果必然不同。
TEST(BpeTokenizerTest, ResultsAcrossTheSixtyFourKibBoundaryAreExact) {
  std::string text;
  for (int i = 0; i < 32768; ++i) text += "ab";
  text += 'e';
  for (int i = 0; i < 1000; ++i) text += "cd";
  ASSERT_GT(text.size(), size_t{65536});
  {
    const BpeTokenizer& t = *Metaspace();
    Ids expected(32768, M("ab"));
    expected.push_back(M("e"));
    expected.insert(expected.end(), 1000, M("cd"));
    EXPECT_EQ(Encode(t, text), expected);
    // 最后 1000 个 cd 之前的字节长度超过 64 KiB。
    EXPECT_EQ(Decode(t, expected), text);
  }
  {
    const BpeTokenizer& t = *ByteLevel();
    Ids expected(32768, B("ab"));
    expected.push_back(B("e"));
    expected.insert(expected.end(), 1000, B("cd"));
    EXPECT_EQ(Encode(t, text), expected);
    EXPECT_EQ(Decode(t, expected), text);
  }
}

// 空格规范化后每个字节变成 3 个字节，输入的 3 倍都要能处理。
TEST(BpeTokenizerTest, SpaceOnlyInputsExpandToThreeBytesPerSpace) {
  const BpeTokenizer& t = *Metaspace();
  const int32_t one = M(" ");
  const int32_t two = M("  ");
  EXPECT_EQ(Encode(t, " "), (Ids{one}));
  EXPECT_EQ(Encode(t, "  "), (Ids{two}));
  EXPECT_EQ(Encode(t, "   "), (Ids{two, one}));
  EXPECT_EQ(Encode(t, "     "), (Ids{two, two, one}));
  const std::string spaces(kMaxEncodeBytes, ' ');
  Ids ids;
  std::string diagnostic;
  ASSERT_EQ(t.Encode(spaces, &ids, &diagnostic), 0) << diagnostic;
  ASSERT_EQ(ids.size(), kMaxEncodeBytes / 2);
  EXPECT_TRUE(std::all_of(ids.begin(), ids.end(),
                          [two](int32_t id) { return id == two; }));
  EXPECT_EQ(Decode(t, ids), spaces);

  // 字节级预分词：整段空格是一个词，没有合并。
  const BpeTokenizer& b = *ByteLevel();
  ASSERT_EQ(b.Encode(spaces, &ids, &diagnostic), 0) << diagnostic;
  ASSERT_EQ(ids.size(), spaces.size());
  EXPECT_TRUE(
      std::all_of(ids.begin(), ids.end(), [](int32_t id) { return id == 32; }));
}

TEST(BpeTokenizerTest, RecognisesSpecialTokensAndSplitsAdjacentText) {
  const BpeTokenizer& t = *Metaspace();
  const int32_t bos = SpecialId(t, "<bos>");
  const int32_t start = SpecialId(t, "<start_of_turn>");
  const int32_t end = SpecialId(t, "<end_of_turn>");
  EXPECT_EQ(bos, 2);
  EXPECT_EQ(Encode(t, "a<bos>b"), (Ids{M("a"), bos, M("b")}));
  EXPECT_EQ(Encode(t, "<bos><bos>"), (Ids{bos, bos}));
  EXPECT_EQ(Encode(t, "<start_of_turn>hello<end_of_turn>"),
            (Ids{start, M("hello"), end}));
  // 特殊 token 两侧的空格各自规范化，不跨越边界合并。
  EXPECT_EQ(Encode(t, "a <bos> b"), (Ids{M("a"), M(" "), bos, M(" "), M("b")}));
  // 未登记 token 的前缀不是特殊 token；'<' 不在词表中，走字节回退。
  EXPECT_EQ(Encode(t, "<start_of_tur"),
            (Ids{MByte('<'), M("s"), M("t"), M("a"), M("r"), M("t"), MByte('_'),
                 M("o"), M("f"), MByte('_'), M("t"), M("u"), M("r")}));
  // 非特殊的 added token 同样会被识别；词表内的和只在 added_tokens 中的都是。
  const int32_t unused = SpecialId(t, "<unused0>");
  EXPECT_EQ(unused, M("<unused0>"));
  // 最左最长：<ex><ey> 比 <ex> 更长。
  const int32_t ex = SpecialId(t, "<ex>");
  const int32_t ex_long = SpecialId(t, "<ex><ey>");
  EXPECT_NE(ex, ex_long);
  EXPECT_EQ(Encode(t, "x<ex><ey>y"), (Ids{M("x"), ex_long, M("y")}));
  EXPECT_EQ(Encode(t, "<ex><e"), (Ids{ex, MByte('<'), M("e")}));
  EXPECT_EQ(Encode(t, "<ex><ex>"), (Ids{ex, ex}));
}

TEST(BpeTokenizerTest, UnknownCharactersFallBackToByteTokens) {
  const BpeTokenizer& t = *Metaspace();
  EXPECT_EQ(Encode(t, "\xE6\x97\xA5"),
            (Ids{MByte(0xE6), MByte(0x97), MByte(0xA5)}));
  EXPECT_EQ(Encode(t, "\xF0\x9F\x98\x80"),
            (Ids{MByte(0xF0), MByte(0x9F), MByte(0x98), MByte(0x80)}));
  EXPECT_EQ(Encode(t, "\xE4\xBD\xA0\xE6\x97\xA5"),
            (Ids{M("\xE4\xBD\xA0"), MByte(0xE6), MByte(0x97), MByte(0xA5)}));
  // 换行不在词表中。
  EXPECT_EQ(Encode(t, "a\nb"), (Ids{M("a"), MByte('\n'), M("b")}));
  EXPECT_EQ(Decode(t, Encode(t, "a\nb\xF0\x9F\x98\x80")),
            "a\nb\xF0\x9F\x98\x80");
}

TEST(BpeTokenizerTest, ByteLevelNormalizesToNfcBeforeMerging) {
  const BpeTokenizer& t = *ByteLevel();
  const int32_t e_acute = B("\xC3\xA9");
  EXPECT_EQ(Encode(t, "e\xCC\x81"), (Ids{e_acute}));
  EXPECT_EQ(Encode(t, "\xC3\xA9"), (Ids{e_acute}));
  // 组合类排序后再合成：下点 (220) 排在重音 (230) 前，不阻隔 e + U+0301。
  EXPECT_EQ(Encode(t, "e\xCC\x81\xCC\xA3"), Encode(t, "e\xCC\xA3\xCC\x81"));
  // Hangul 由公式合成：U+1100 U+1161 -> U+AC00。
  EXPECT_EQ(Encode(t, "\xE1\x84\x80\xE1\x85\xA1"),
            (Ids{B("\xEA"), B("\xB0"), B("\x80")}));
}

// 规范排序必须是线性对数时间：“a” 加 N 个 U+0301（组合类 230）和 N 个 U+0323
// （组合类 220）曾因逐个插入交换而是平方时间，128 KiB 的输入就要十几秒。
TEST(BpeTokenizerTest, NfcOfLongCombiningMarkRunsIsFast) {
  const BpeTokenizer& t = *ByteLevel();
  // 每个记号 2 字节：1 + 4N 恰好不超过 kMaxEncodeBytes。
  const size_t marks = (kMaxEncodeBytes - 1) / 4;
  std::string text = "a";
  for (size_t i = 0; i < marks; ++i) text += "\xCC\x81";
  for (size_t i = 0; i < marks; ++i) text += "\xCC\xA3";
  ASSERT_LE(text.size(), kMaxEncodeBytes);
  ASSERT_GT(text.size(), kMaxEncodeBytes - 4);

  // 规范排序后 U+0323（220）在前、U+0301（230）在后；这里的 Unicode 表里 a
  // 与它们 都没有合成，所以结果就是排序后文本的逐字节编码。
  std::string sorted = "a";
  for (size_t i = 0; i < marks; ++i) sorted += "\xCC\xA3";
  for (size_t i = 0; i < marks; ++i) sorted += "\xCC\x81";
  Ids expected;
  for (unsigned char byte : sorted) expected.push_back(byte);

  // 不用墙钟断言：平方时间的实现在此规模下远超 CTest 的单用例超时，
  // 由超时兜住，避免慢机器上的偶发失败。
  EXPECT_EQ(Encode(t, text), expected);

  // 较短的输入：不同组合类交错时稳定排序保持同类记号的原有顺序。
  EXPECT_EQ(Encode(t, "a\xCC\x81\xCC\xA3\xCC\x81\xCC\xA3"),
            Encode(t, "a\xCC\xA3\xCC\xA3\xCC\x81\xCC\x81"));
}

// 预分词的边界阻止跨词合并：这些输入在不切分时会得到不同的结果。
TEST(BpeTokenizerTest, ByteLevelPreTokenizerSeparatesWords) {
  const BpeTokenizer& t = *ByteLevel();
  // o1 的排名比 hello 的各步都靠前；数字单独成词，不会和 o 合并。
  EXPECT_EQ(Encode(t, "hello1"), (Ids{B("hello"), B("1")}));
  // n' 排名靠前；缩写 't 与 don 分开。
  EXPECT_EQ(Encode(t, "don't"), (Ids{B("d"), B("o"), B("n"), B("'t")}));
  // 's 是缩写，后面的 x 不并入同一个词（否则 s、x 会合并成 sx）。
  EXPECT_EQ(Encode(t, "'sx"), (Ids{B("'s"), B("x")}));
  EXPECT_EQ(Encode(t, "hello hello"), (Ids{B("hello"), B(" hello")}));
  // \s+(?!\S)：两个空格后接字母时，只有最后一个空格归后面的词。
  EXPECT_EQ(Encode(t, "a  b"), (Ids{B("a"), B(" "), B(" b")}));
  // \s*[\r\n]+：换行连成一个词。
  EXPECT_EQ(Encode(t, "a\n\nb"), (Ids{B("a"), B("\n\n"), B("b")}));
}

TEST(BpeTokenizerTest, ByteLevelRecognisesSpecialAndAddedTokens) {
  const BpeTokenizer& t = *ByteLevel();
  const int32_t end_of_text = SpecialId(t, "<|endoftext|>");
  const int32_t im_start = SpecialId(t, "<|im_start|>");
  const int32_t im_end = SpecialId(t, "<|im_end|>");
  const int32_t tool_call = SpecialId(t, "<tool_call>");
  EXPECT_EQ(Encode(t, "a<|im_end|>b"), (Ids{B("a"), im_end, B("b")}));
  EXPECT_EQ(Encode(t, "<|im_start|><|im_end|>"), (Ids{im_start, im_end}));
  EXPECT_EQ(Encode(t, "<|im_start|>hello\n<|im_end|>"),
            (Ids{im_start, B("hello"), B("\n"), im_end}));
  EXPECT_EQ(Encode(t, "x<tool_call>"), (Ids{B("x"), tool_call}));
  EXPECT_EQ(Encode(t, "<|endoftext|>"), (Ids{end_of_text}));
  // 缺少结尾的 <|im_start| 不是特殊 token，按普通文本逐字节编码。
  EXPECT_EQ(Encode(t, "<|im_start|"),
            (Ids{'<', '|', 'i', 'm', '_', 's', 't', 'a', 'r', 't', '|'}));
}

TEST(BpeTokenizerTest, DecodingValidSequencesMatchesConcatenatedTokenBytes) {
  const std::vector<std::string> texts = {"hello world",
                                          " hello  hello",
                                          "abcd abc",
                                          "aaaaa",
                                          "a\nb",
                                          "\xE4\xBD\xA0\xE5\xA5\xBD",
                                          "\xF0\x9F\x98\x80 x",
                                          "x<bos>y"};
  for (const std::string& text : texts) {
    SCOPED_TRACE(text);
    const Ids ids = Encode(*Metaspace(), text);
    const std::string expected =
        text.find("<bos>") == std::string::npos ? text : "xy";
    EXPECT_EQ(Decode(*Metaspace(), ids), expected);
    EXPECT_EQ(Concatenate(*Metaspace(), ids), expected);
  }
  for (const std::string& text : texts) {
    SCOPED_TRACE(text);
    const Ids ids = Encode(*ByteLevel(), text);
    EXPECT_EQ(Decode(*ByteLevel(), ids), text);
    EXPECT_EQ(Concatenate(*ByteLevel(), ids), text);
  }
}

// 可打印文本的往返：随机文本编码后解码得到原文（metaspace 用 U+2581 做空格，
// 所以文本里不含它；byte_level 先做 NFC，所以文本里不含组合字符）。
TEST(BpeTokenizerTest, RandomTextRoundTrips) {
  uint32_t state = 20261014;
  const auto next = [&state] {
    state = state * 1664525U + 1013904223U;
    return state >> 8;
  };
  const std::vector<std::string> pieces = {"a",
                                           "b",
                                           "c",
                                           "d",
                                           "e",
                                           "h",
                                           "l",
                                           "o",
                                           "n",
                                           "'",
                                           "s",
                                           "t",
                                           "x",
                                           "1",
                                           " ",
                                           "  ",
                                           "\n",
                                           "\r\n",
                                           "\xE4\xBD\xA0",
                                           "\xE5\xA5\xBD",
                                           "\xC3\xA9",
                                           "\xF0\x9F\x98\x80",
                                           "\xE6\x97\xA5",
                                           "hello",
                                           "bcd",
                                           "aa",
                                           ".",
                                           "<bos>"};
  for (int round = 0; round < 300; ++round) {
    std::string text;
    for (uint32_t i = next() % 40; i > 0; --i) {
      text += pieces[next() % pieces.size()];
    }
    SCOPED_TRACE(text);
    const Ids meta = Encode(*Metaspace(), text);
    std::string without_special = text;
    for (size_t at;
         (at = without_special.find("<bos>")) != std::string::npos;) {
      without_special.erase(at, 5);
    }
    EXPECT_EQ(Decode(*Metaspace(), meta), without_special);
    const Ids bytes = Encode(*ByteLevel(), text);
    EXPECT_EQ(Decode(*ByteLevel(), bytes), text);
  }
}

// 非法字节：Decode 与 HF 一致地输出 U+FFFD，AppendTokenBytes 输出原始字节，
// 两者对非法序列不相等。
TEST(BpeTokenizerTest,
     MetaspaceDecoderFollowsByteFallbackRulesForInvalidBytes) {
  const BpeTokenizer& t = *Metaspace();
  const std::string replacement = "\xEF\xBF\xBD";
  const int32_t bos = 2;
  // 有效序列：两种路径相等。
  const Ids valid = {MByte(0xE4), MByte(0xBD), MByte(0xA0)};
  EXPECT_EQ(Decode(t, valid), "\xE4\xBD\xA0");
  EXPECT_EQ(Concatenate(t, valid), Decode(t, valid));
  EXPECT_EQ(Decode(t, {MByte(0xF0), MByte(0x9F), MByte(0x98), MByte(0x80)}),
            "\xF0\x9F\x98\x80");
  EXPECT_EQ(Decode(t, {MByte('A'), MByte('B')}), "AB");
  // 非法序列：整个字节段中的每个字节各替换为一个 U+FFFD，包括有效前缀。
  const Ids truncated = {MByte(0xE4), MByte(0xBD)};
  EXPECT_EQ(Decode(t, truncated), replacement + replacement);
  EXPECT_EQ(Concatenate(t, truncated), "\xE4\xBD");
  EXPECT_NE(Concatenate(t, truncated), Decode(t, truncated));
  EXPECT_EQ(Decode(t, {MByte(0xFF)}), replacement);
  EXPECT_EQ(Decode(t, {MByte(0xE4), M("a"), MByte(0xBD)}),
            replacement + "a" + replacement);
  // 字节段只要整体不是有效 UTF-8，段内每个字节（包括有效的 'A'）都被替换。
  EXPECT_EQ(Decode(t, {MByte('A'), MByte(0xE4), MByte(0xBD)}),
            replacement + replacement + replacement);
  // 被跳过的特殊 token 不打断字节段。
  EXPECT_EQ(Decode(t, {MByte(0xE4), bos, MByte(0xBD), MByte(0xA0)}),
            "\xE4\xBD\xA0");
  // 把 U+2581 换成空格发生在字节处理之前，所以字节编码的 U+2581 保持原样。
  EXPECT_EQ(Decode(t, {MByte(0xE2), MByte(0x96), MByte(0x81)}), "\xE2\x96\x81");
  EXPECT_EQ(Decode(t, {M(" "), M("hello"), M("  ")}), " hello  ");
}

TEST(BpeTokenizerTest, ByteLevelDecoderUsesLossyUtf8OfTheWholeSequence) {
  const BpeTokenizer& t = *ByteLevel();
  const std::string replacement = "\xEF\xBF\xBD";
  EXPECT_EQ(Decode(t, {0xE4, 0xBD, 0xA0}), "\xE4\xBD\xA0");
  EXPECT_EQ(Concatenate(t, {0xE4, 0xBD, 0xA0}), "\xE4\xBD\xA0");
  // 截断的多字节序列整体替换为一个 U+FFFD（最大非法子序列）。
  EXPECT_EQ(Decode(t, {0xE4, 0xBD}), replacement);
  EXPECT_EQ(Concatenate(t, {0xE4, 0xBD}), "\xE4\xBD");
  EXPECT_EQ(Decode(t, {0xE4, 'a'}), replacement + "a");
  EXPECT_EQ(Decode(t, {0xFF}), replacement);
  EXPECT_EQ(Decode(t, {0xC0, 0x80}), replacement + replacement);
  EXPECT_EQ(Decode(t, {0xE0, 0x80}), replacement + replacement);
  EXPECT_EQ(Decode(t, {0xF0, 0x9F, 0x98}), replacement);
  EXPECT_EQ(Decode(t, {0xED, 0xA0, 0x80}),
            replacement + replacement + replacement);
  EXPECT_EQ(Decode(t, {0xF4, 0x90, 0x80, 0x80}),
            replacement + replacement + replacement + replacement);
  // 跨 token 的字节仍然拼成有效字符。
  EXPECT_EQ(Decode(t, {B("\xC3\xA9")}), "\xC3\xA9");
  EXPECT_EQ(Decode(t, {B("\xE4\xBD"), 0xA0}), "\xE4\xBD\xA0");
}

TEST(BpeTokenizerTest, SpecialTokensDecodeToNothingAndAddedTokensToText) {
  {
    const BpeTokenizer& t = *Metaspace();
    std::string bytes = "keep";
    for (int32_t id = 0; id < 6; ++id) {
      EXPECT_EQ(t.AppendTokenBytes(id, &bytes), 0);
      EXPECT_EQ(bytes, "keep") << id;
    }
    EXPECT_EQ(Decode(t, {0, 1, 2, 3, 4, 5}), "");
    EXPECT_EQ(Decode(t, {2, M("hello"), 5}), "hello");
    // 非特殊的 added token 是普通文本。
    EXPECT_EQ(Decode(t, {M("<unused0>")}), "<unused0>");
    EXPECT_EQ(Decode(t, {SpecialId(t, "<ex>")}), "<ex>");
    EXPECT_EQ(Decode(t, {}), "");
  }
  {
    const BpeTokenizer& t = *ByteLevel();
    const int32_t im_start = SpecialId(t, "<|im_start|>");
    EXPECT_EQ(Decode(t, {im_start, B("a")}), "a");
    EXPECT_EQ(Decode(t, {SpecialId(t, "<tool_call>")}), "<tool_call>");
    std::string bytes;
    EXPECT_EQ(t.AppendTokenBytes(im_start, &bytes), 0);
    EXPECT_TRUE(bytes.empty());
    EXPECT_EQ(Decode(t, {im_start, SpecialId(t, "<|endoftext|>")}), "");
  }
}

TEST(BpeTokenizerTest, DecodingRejectsOutOfRangeIdsAndNullOutputs) {
  for (const BpeTokenizer* t : {Metaspace().get(), ByteLevel().get()}) {
    const int32_t count = static_cast<int32_t>(t->VocabSize());
    for (const int32_t bad :
         {-1, count, count + 100, std::numeric_limits<int32_t>::min(),
          std::numeric_limits<int32_t>::max()}) {
      SCOPED_TRACE(bad);
      std::string diagnostic;
      std::string bytes = "x";
      EXPECT_EQ(t->AppendTokenBytes(bad, &bytes, &diagnostic),
                error::kInvalidInput);
      EXPECT_NE(diagnostic.find("out of range"), std::string::npos);
      EXPECT_EQ(bytes, "x");
      diagnostic.clear();
      std::string text = "y";
      EXPECT_EQ(t->Decode({static_cast<int32_t>(0), bad}, &text, &diagnostic),
                error::kInvalidInput);
      EXPECT_TRUE(text.empty());
      EXPECT_NE(diagnostic.find("out of range"), std::string::npos);
    }
    std::string diagnostic;
    EXPECT_EQ(t->AppendTokenBytes(0, nullptr, &diagnostic),
              error::kInvalidArgument);
    EXPECT_FALSE(diagnostic.empty());
    EXPECT_EQ(t->Decode({}, nullptr, &diagnostic), error::kInvalidArgument);
    std::string bytes;
    // diagnostic 可以为空指针
    EXPECT_EQ(t->AppendTokenBytes(-1, &bytes), error::kInvalidInput);
  }
}

TEST(BpeTokenizerTest, ConcurrentEncodeAndDecodeAreConsistent) {
  const std::vector<std::string> inputs = {
      "hello world",        "abcd aaa",   "<bos>x<end_of_turn>",
      "\xE6\x97\xA5 hello", "  hello   ", "a\nb"};
  const BpeTokenizer& meta = *Metaspace();
  const BpeTokenizer& bytes = *ByteLevel();
  std::vector<Ids> expected_meta;
  std::vector<Ids> expected_bytes;
  for (const std::string& input : inputs) {
    expected_meta.push_back(Encode(meta, input));
    expected_bytes.push_back(Encode(bytes, input));
  }
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int worker = 0; worker < 8; ++worker) {
    threads.emplace_back([&] {
      for (int round = 0; round < 200; ++round) {
        for (size_t i = 0; i < inputs.size(); ++i) {
          Ids ids;
          if (meta.Encode(inputs[i], &ids) != 0 || ids != expected_meta[i])
            ++failures;
          std::string text;
          if (meta.Decode(ids, &text) != 0) ++failures;
          if (bytes.Encode(inputs[i], &ids) != 0 || ids != expected_bytes[i])
            ++failures;
        }
      }
    });
  }
  for (std::thread& thread : threads) thread.join();
  EXPECT_EQ(failures.load(), 0);
}

// ---- 真实资产：设置 EDGEFLOW_TEST_BPE_TOKENIZER_DIR 时运行 ----

struct RealModel {
  const char* name;
  size_t vocab_size;
  Ids end_tokens;
};

// 只打印模型名：默认按字节打印会带出指针值，
// 使 CTest 发现的用例名每次构建都不同。
void PrintTo(const RealModel& model, std::ostream* os) { *os << model.name; }

class BpeTokenizerRealAssetTest : public ::testing::TestWithParam<RealModel> {};

TEST_P(BpeTokenizerRealAssetTest, MatchesHuggingFaceGoldenIds) {
  const char* directory = std::getenv("EDGEFLOW_TEST_BPE_TOKENIZER_DIR");
  if (!directory || !*directory) {
    RecordProperty("optional_runtime_asset", "EDGEFLOW_TEST_BPE_TOKENIZER_DIR");
    GTEST_SKIP() << "EDGEFLOW_TEST_BPE_TOKENIZER_DIR is not supplied";
  }
  const RealModel& model = GetParam();
  std::string diagnostic;
  const auto tokenizer = BpeTokenizer::Load(
      std::string(directory) + "/" + model.name + ".eftok", &diagnostic);
  ASSERT_NE(tokenizer, nullptr) << diagnostic;
  EXPECT_EQ(tokenizer->VocabSize(), model.vocab_size);
  EXPECT_EQ(tokenizer->EndTokens(), model.end_tokens);

  const std::string golden_path =
      std::string(EDGEFLOW_BPE_TOKENIZER_GOLDEN_DIR) + "/" + model.name +
      ".golden.json";
  std::ifstream stream(golden_path);
  ASSERT_TRUE(stream.good()) << golden_path;
  const nlohmann::json golden = nlohmann::json::parse(stream);
  ASSERT_GT(golden.at("cases").size(), 50U);
  for (const nlohmann::json& item : golden.at("cases")) {
    const std::string text = item.at("text").get<std::string>();
    const Ids expected = item.at("ids").get<Ids>();
    Ids actual;
    ASSERT_EQ(tokenizer->Encode(text, &actual, &diagnostic), 0)
        << diagnostic << " for " << text.substr(0, 40);
    EXPECT_EQ(actual, expected) << "text: " << text.substr(0, 80);
  }
}

INSTANTIATE_TEST_SUITE_P(
    Models, BpeTokenizerRealAssetTest,
    ::testing::Values(
        RealModel{"qwen2.5-0.5b-instruct", 151665, {151643, 151645}},
        RealModel{"gemma3-270m", 262145, {1, 106}}),
    [](const ::testing::TestParamInfo<RealModel>& info) {
      std::string name = info.param.name;
      std::replace_if(
          name.begin(), name.end(), [](char c) { return !std::isalnum(c); },
          '_');
      return name;
    });

}  // namespace
}  // namespace text
}  // namespace llm_edgeflow
