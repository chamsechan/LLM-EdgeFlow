#include <gtest/gtest.h>

#include <any>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "adapter/io_values.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "contracts/error_codes.h"
#include "platform_mock/operator_data_types.h"
#include "tests/support/mock_text_carrier.h"

namespace llm_edgeflow {
namespace {
const OperatorValueTypeBinding& Native(const char* suffix) {
  const auto* binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix(suffix);
  if (!binding)
    throw std::runtime_error(std::string("Missing native binding: ") + suffix);
  return *binding;
}
void Reject(const OperatorValueTypeBinding& binding, const void* carrier,
            const InputLimits& limits, const char* field) {
  std::string error;
  EXPECT_EQ(binding.validate_external(carrier, limits, &error),
            error::kInvalidInput);
  EXPECT_NE(error.find(field), std::string::npos) << error;
}

TEST(PlatformCarrierContractsTest,
     ExplicitStringLengthRequiresReadableNonNullNonNulBytesAndCopiesOwnership) {
  const auto& binding = Native("string");
  std::array<char, 2> bytes{'h', 'i'};  // 此载体不要求结尾 NUL。
  MockString text{2, bytes.data()};
  InputLimits limits;
  limits.max_text_bytes = 2;
  std::string error;
  ASSERT_EQ(binding.validate_external(&text, limits, &error), 0);
  EXPECT_EQ(binding.value_type, typeid(std::string));
  const auto copied = std::any_cast<std::string>(binding.read_value(&text));
  bytes[0] = 'X';
  EXPECT_EQ(copied, "hi");
  Reject(binding, nullptr, limits, "pointer is null");
  MockString invalid{-1, bytes.data()};
  Reject(binding, &invalid, limits, "negative length");
  invalid = {3, bytes.data()};
  Reject(binding, &invalid, limits, "exceeds max limit");
  invalid = {2, nullptr};
  Reject(binding, &invalid, limits, "data pointer is null");
  bytes[1] = '\0';
  Reject(binding, &text, limits, "embedded NUL at byte offset 1");
  MockString empty{0, nullptr};
  ASSERT_EQ(binding.validate_external(&empty, limits, &error), 0);
  EXPECT_TRUE(std::any_cast<std::string>(binding.read_value(&empty)).empty());
}

TEST(PlatformCarrierContractsTest,
     FrameMetadataValidatesPhysicalLengthAndWhitelistedSizeEquation) {
  const auto& binding = Native("frame");
  std::array<uint8_t, 3> pixels{1, 0, 3};
  std::array<int32_t, 2> values{7, -2};
  MockAny any{2, 2, 8, values.data()};
  MockFrameInput frame{kMockServiceOcrInvoiceQa, 1, 1, 3, pixels.data(), &any};
  InputLimits limits;
  limits.max_any_bytes = 8;
  std::string error;
  EXPECT_EQ(binding.validate_external(&frame, limits, &error), 0);
  for (const auto invalid :
       {MockAny{999, 2, 8, values.data()}, MockAny{2, -1, 8, values.data()},
        MockAny{2, 2, -1, values.data()}, MockAny{2, 2, 4, values.data()},
        MockAny{2, 3, 12, values.data()}, MockAny{2, 2, 8, nullptr},
        MockAny{0, 1, 0, nullptr}}) {
    SCOPED_TRACE(invalid.type_id);
    frame.metadata = &invalid;
    Reject(binding, &frame, limits, "MockFrameInput.metadata");
  }
  MockAny empty{0, 0, 0, nullptr};
  frame.metadata = &empty;
  EXPECT_EQ(binding.validate_external(&frame, limits, &error), 0);
  frame.metadata = nullptr;
  EXPECT_EQ(binding.validate_external(&frame, limits, &error), 0);
}

TEST(PlatformCarrierContractsTest,
     KeywordAndEntityShareTextEnvelopeWithoutInputServiceType) {
  const auto& keyword_binding = Native("keyword_in");
  const auto& entity_binding = Native("entity_in");
  test_support::MockTextCarrier carrier("shared carrier text");
  std::string error;
  ASSERT_EQ(keyword_binding.validate_external(&carrier.root, {}, &error), 0)
      << error;
  ASSERT_EQ(entity_binding.validate_external(&carrier.root, {}, &error), 0);
  EXPECT_EQ(keyword_binding.external_c_type_name, "MockInput");
  EXPECT_EQ(entity_binding.external_c_type_name, "MockInput");
  EXPECT_FALSE(keyword_binding.read_service_type);
  EXPECT_FALSE(entity_binding.read_service_type);
  EXPECT_TRUE(keyword_binding.services.empty());
  EXPECT_TRUE(entity_binding.services.empty());
  const auto copied =
      std::any_cast<TextInputValue>(keyword_binding.read_value(&carrier.root));
  carrier.text.assign(carrier.text.size(), 'X');
  EXPECT_EQ(copied.sentence_text, "shared carrier text");
}

TEST(PlatformCarrierContractsTest,
     TextEnvelopeValidatesRootThenDataPoolThenTextBeforeReading) {
  const auto& binding = Native("keyword_in");
  InputLimits limits;
  limits.max_text_bytes = 5;
  struct Case {
    const char* label;
    void (*corrupt)(test_support::MockTextCarrier*);
    const char* field;
  };
  const Case cases[] = {
      {"root byte_size",
       [](auto* c) { c->root.byte_size = sizeof(MockInput) + 1; },
       "MockInput.byte_size"},
      {"root type", [](auto* c) { c->root.type = 0; }, "kMockInputMultidata"},
      {"frame count", [](auto* c) { c->root.num_frame = 2; }, "num_frame 1"},
      {"null pool", [](auto* c) { c->root.data_pool = nullptr; },
       "MockInput.data_pool"},
      {"pool byte_size", [](auto* c) { c->pool.byte_size = 0; },
       "MockInput.data_pool"},
      {"data type", [](auto* c) { c->pool.data_type = 2; }, "kMockDataText"},
      {"null text", [](auto* c) { c->pool.data_ptr = nullptr; }, "data_ptr"},
      {"text byte_size", [](auto* c) { c->leaf.byte_size = 4; }, "data_ptr"},
      {"negative length", [](auto* c) { c->leaf.length = -1; },
       "negative length"},
      {"over limit", [](auto* c) { c->leaf.length = 6; }, "exceeds max limit"},
      {"null value", [](auto* c) { c->leaf.value = nullptr; },
       "data pointer is null"},
      {"embedded NUL", [](auto* c) { c->text[1] = '\0'; },
       "embedded NUL at byte offset 1"},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.label);
    test_support::MockTextCarrier carrier("hello");
    std::string error;
    ASSERT_EQ(binding.validate_external(&carrier.root, limits, &error), 0);
    test.corrupt(&carrier);
    Reject(binding, &carrier.root, limits, test.field);
  }
  test_support::MockTextCarrier empty;
  empty.leaf.value = nullptr;
  std::string error;
  ASSERT_EQ(binding.validate_external(&empty.root, limits, &error), 0);
  EXPECT_TRUE(std::any_cast<TextInputValue>(binding.read_value(&empty.root))
                  .sentence_text.empty());
}

TEST(PlatformCarrierContractsTest,
     DocumentRequiresQueryButAllowsMissingDocumentAndUsesSeparateByteLimits) {
  const auto& binding = Native("doc_in");
  std::string document = "longdoc!";
  std::string question = "ask";
  MockString doc{8, document.data()}, query{3, question.data()};
  MockDocInput request{kMockServiceDocQa, &doc, &query};
  InputLimits limits;
  limits.max_text_bytes = 3;
  limits.max_doc_text_bytes = 8;
  std::string error;
  ASSERT_EQ(binding.validate_external(&request, limits, &error), 0);
  const auto copied =
      std::any_cast<DocumentInputValue>(binding.read_value(&request));
  document[0] = 'X';
  question[0] = 'X';
  EXPECT_EQ(copied.doc_text, "longdoc!");
  EXPECT_EQ(copied.query_text, "ask");
  EXPECT_EQ(binding.read_service_type(&request), kMockServiceDocQa);
  request.doc_text = nullptr;
  ASSERT_EQ(binding.validate_external(&request, limits, &error), 0);
  EXPECT_TRUE(std::any_cast<DocumentInputValue>(binding.read_value(&request))
                  .doc_text.empty());
  request.query_text = nullptr;
  Reject(binding, &request, limits, "query_text");
  request.query_text = &query;
  query.length = 4;
  Reject(binding, &request, limits, "query_text");
  query.length = 3;
  request.doc_text = &doc;
  doc.length = 9;
  Reject(binding, &request, limits, "doc_text");
}

TEST(PlatformCarrierContractsTest,
     AuditRequiresUserTextAndOptionalChannelHasItsOwnFixedLimit) {
  const auto& binding = Native("audit_in");
  std::string user = "question", channel = "phone";
  MockString text{8, user.data()}, name{5, channel.data()};
  MockAuditInput request{kMockServiceDialogueAudit, &text, &name};
  std::string error;
  ASSERT_EQ(binding.validate_external(&request, {}, &error), 0);
  const auto copied =
      std::any_cast<AuditInputValue>(binding.read_value(&request));
  user[0] = 'X';
  channel[0] = 'X';
  EXPECT_EQ(copied.user_text, "question");
  EXPECT_EQ(copied.channel_name, "phone");
  EXPECT_EQ(binding.read_service_type(&request), kMockServiceDialogueAudit);
  request.channel_name = nullptr;
  ASSERT_EQ(binding.validate_external(&request, {}, &error), 0);
  EXPECT_TRUE(std::any_cast<AuditInputValue>(binding.read_value(&request))
                  .channel_name.empty());
  request.user_text = nullptr;
  Reject(binding, &request, {}, "user_text");
  request.user_text = &text;
  std::string long_channel(257, 'a');
  name = {256, long_channel.data()};
  request.channel_name = &name;
  EXPECT_EQ(binding.validate_external(&request, {}, &error), 0);
  name.length = 257;
  Reject(binding, &request, {}, "channel_name");
}

TEST(PlatformCarrierContractsTest,
     FrameCopiesPaddedRgbStorageAndChecksWideGeometryBeforeReadingPixels) {
  const auto& binding = Native("frame");
  std::array<uint8_t, 8> pixels{1, 2, 3, 99, 4, 5, 6, 98};
  std::array<int32_t, 1> metadata_value{7};
  MockAny metadata{2, 1, 4, metadata_value.data()};
  MockFrameInput frame{
      kMockServiceOcrInvoiceQa, 2, 1, 4, pixels.data(), &metadata};
  InputLimits limits;
  limits.max_image_bytes = 8;
  std::string error;
  ASSERT_EQ(binding.validate_external(&frame, limits, &error), 0);
  const auto copied =
      std::any_cast<ImageInputValue>(binding.read_value(&frame));
  pixels.fill(0);
  EXPECT_EQ(copied.frame.width, 1);
  EXPECT_EQ(copied.frame.height, 2);
  EXPECT_EQ(copied.frame.stride, 4u);
  EXPECT_EQ(copied.frame.data,
            (std::vector<uint8_t>{1, 2, 3, 99, 4, 5, 6, 98}));
  EXPECT_EQ(binding.read_service_type(&frame), kMockServiceOcrInvoiceQa);
  limits.max_image_bytes = 7;
  Reject(binding, &frame, limits, "max_image_bytes");
  limits.max_image_bytes = 8;
  auto invalid = frame;
  invalid.width = 0;
  Reject(binding, &invalid, limits, "positive dimensions");
  invalid = frame;
  invalid.height = -1;
  Reject(binding, &invalid, limits, "positive dimensions");
  invalid = frame;
  invalid.stride = 2;
  Reject(binding, &invalid, limits, "RGB8 stride");
  invalid = frame;
  invalid.width = std::numeric_limits<int32_t>::max();
  invalid.stride = std::numeric_limits<int32_t>::max();
  Reject(binding, &invalid, limits, "RGB8 stride");
  invalid = frame;
  invalid.height = std::numeric_limits<int32_t>::max();
  invalid.stride = std::numeric_limits<int32_t>::max();
  Reject(binding, &invalid, limits, "max_image_bytes");
  invalid = frame;
  invalid.data = nullptr;
  Reject(binding, &invalid, limits, "data is null");
  metadata.type_id = 999;
  Reject(binding, &frame, limits, "MockFrameInput.metadata");
  // 宿主结构没有字节长度字段，校验无法检查分配大小。
}

TEST(PlatformCarrierContractsTest,
     AudioCopiesSamplesAndChecksRateSampleCountAndByteBudgetIndependently) {
  const auto& binding = Native("audio_in");
  std::array<float, 2> pcm{0.25f, -0.5f};
  MockAudioInput audio{kMockServiceAudioAsrIntent, pcm.data(), 2, 16000};
  InputLimits limits;
  limits.max_audio_pcm_samples = 2;
  limits.max_audio_pcm_bytes = 8;
  std::string error;
  ASSERT_EQ(binding.validate_external(&audio, limits, &error), 0);
  const auto copied =
      std::any_cast<AudioInputValue>(binding.read_value(&audio));
  pcm.fill(0);
  EXPECT_EQ(copied.pcm, (std::vector<float>{0.25f, -0.5f}));
  EXPECT_EQ(copied.sample_rate, 16000);
  EXPECT_EQ(binding.read_service_type(&audio), kMockServiceAudioAsrIntent);
  for (int rate : {8000, 192000}) {
    audio.sample_rate = rate;
    EXPECT_EQ(binding.validate_external(&audio, limits, &error), 0);
  }
  for (int rate : {7999, 192001}) {
    audio.sample_rate = rate;
    Reject(binding, &audio, limits, "sample_rate");
  }
  audio.sample_rate = 16000;
  audio.pcm_length = -1;
  Reject(binding, &audio, limits, "pcm_length");
  audio.pcm_length = 3;
  Reject(binding, &audio, limits, "pcm_length");
  audio.pcm_length = 2;
  limits.max_audio_pcm_bytes = 7;
  Reject(binding, &audio, limits, "pcm_length");
  limits.max_audio_pcm_bytes = 8;
  audio.pcm_buffer = nullptr;
  Reject(binding, &audio, limits, "pcm_buffer");
  audio.pcm_length = 0;
  ASSERT_EQ(binding.validate_external(&audio, limits, &error), 0);
  EXPECT_TRUE(
      std::any_cast<AudioInputValue>(binding.read_value(&audio)).pcm.empty());
}

TEST(PlatformCarrierContractsTest,
     RerankOwnsQueryAndCandidatesAndChecksCountBeforeNestedArrayAccess) {
  const auto& binding = Native("rerank_in");
  std::string query = "q", first = "first", second = "second";
  MockString question{1, query.data()}, a{5, first.data()}, b{6, second.data()};
  MockRerankInput request{};
  request.service_type = kMockServiceCrossRerank;
  request.query_text = &question;
  request.candidate_count = 2;
  request.candidate_passages[0] = &a;
  request.candidate_passages[1] = &b;
  std::string error;
  ASSERT_EQ(binding.validate_external(&request, {}, &error), 0);
  const auto copied =
      std::any_cast<RerankInputValue>(binding.read_value(&request));
  query[0] = first[0] = second[0] = 'X';
  EXPECT_EQ(copied.query_text, "q");
  EXPECT_EQ(copied.candidate_passages,
            (std::vector<std::string>{"first", "second"}));
  EXPECT_EQ(binding.read_service_type(&request), kMockServiceCrossRerank);
  for (int count : {-1, 0, 9}) {
    request.candidate_count = count;
    Reject(binding, &request, {}, "candidate_count");
  }
  request.candidate_count = 8;
  for (auto& passage : request.candidate_passages) passage = &a;
  EXPECT_EQ(binding.validate_external(&request, {}, &error), 0);
  request.candidate_count = 2;
  request.candidate_passages[1] = nullptr;
  Reject(binding, &request, {}, "candidate_passages[1]");
  request.candidate_passages[1] = &b;
  request.query_text = nullptr;
  Reject(binding, &request, {}, "query_text");
  request.query_text = &question;
  InputLimits limits;
  limits.max_doc_text_bytes = 5;
  Reject(binding, &request, limits, "candidate_passages[1]");
}

}  // namespace
}  // namespace llm_edgeflow
