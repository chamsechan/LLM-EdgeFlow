#include "adapter/operator/mock/operator_builtin_value_types.h"

#include "adapter/io_values.h"
#include "adapter/operator/mock/platform_value_binding.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "contracts/error_codes.h"

namespace llm_edgeflow {
namespace {
std::string ReadString(const MockString* value) {
  return !value || value->length == 0 ? std::string{}
                                      : std::string(value->data, value->length);
}

// 文本 envelope 不带 service_type，因此不声明业务映射；业务由 Pipeline
// 输入 name 区分。
OperatorValueTypeBinding MakeTextInputBinding(const char* suffix) {
  auto binding = MakeTypedInputBinding<MockInput>(
      suffix,
      [](const MockInput& in, const InputLimits& limits, std::string* err) {
        return ValidateMockInputText(in, limits.max_text_bytes, "MockInput",
                                     err);
      });
  SetInputValue<MockInput, TextInputValue>(&binding, [](const MockInput& in) {
    return TextInputValue{std::string(MockInputText(in))};
  });
  return binding;
}

AdapterStatus WriteText(MockTextOutput* output, const std::string& text,
                        const char* field, const ResolvedOutputPoolSpec& spec) {
  const auto it = spec.capacities.find(field);
  if (it == spec.capacities.end())
    return AdapterStatus::BufferTooSmall(
        "Missing output capacity specification", field);
  std::string error;
  const int code =
      CopyToMockTextOutput(text, output, it->second, field, &error);
  return AdapterStatus(code, std::move(error), code == 0 ? "" : field);
}
}  // namespace

void RegisterMockOperatorBindings(OperatorValueTypeRegistry& registry) {
  // string -> MockString
  {
    auto binding = MakeTypedInputBinding<MockString>(
        "string",
        [](const MockString& in, const InputLimits& limits,
           std::string* err) -> int {
          return ValidateMockString(&in, limits.max_text_bytes, "string", err);
        });
    SetInputValue<MockString, std::string>(
        &binding, [](const MockString& in) { return ReadString(&in); });
    registry.RegisterBinding(std::move(binding));
  }

  // frame -> MockFrameInput
  {
    auto binding = MakeTypedInputBinding<MockFrameInput>(
        "frame",
        [](const MockFrameInput& in, const InputLimits& limits,
           std::string* err) -> int {
          if (in.width <= 0 || in.height <= 0 || in.stride <= 0 ||
              static_cast<uint64_t>(in.stride) <
                  static_cast<uint64_t>(in.width) * 3) {
            if (err)
              *err =
                  "MockFrameInput requires positive dimensions and RGB8 stride "
                  ">= width * 3";
            return error::kInvalidInput;
          }
          if (!in.data) {
            if (err) *err = "MockFrameInput.data is null";
            return error::kInvalidInput;
          }
          if (static_cast<uint64_t>(in.stride) * in.height >
              limits.max_image_bytes) {
            if (err) *err = "MockFrameInput pixels exceed max_image_bytes";
            return error::kInvalidInput;
          }
          int ret = 0;
          if (in.metadata) {
            ret = ValidateMockAnyPayload(in.metadata, limits.max_any_bytes,
                                         "MockFrameInput.metadata", err);
            if (ret != 0) return ret;
          }
          return 0;
        });
    SetServiceTypeMember(&binding, &MockFrameInput::service_type);
    SetInputValue<MockFrameInput, ImageInputValue>(
        &binding, [](const MockFrameInput& in) {
          ImageFrame frame;
          frame.width = in.width;
          frame.height = in.height;
          frame.stride = static_cast<size_t>(in.stride);
          const auto* pixels = static_cast<const uint8_t*>(in.data);
          frame.data.assign(pixels, pixels + frame.stride * frame.height);
          return ImageInputValue{std::move(frame)};
        });
    binding.services = {{"ocr_invoice_qa", kMockServiceOcrInvoiceQa}};
    registry.RegisterBinding(std::move(binding));
  }

  // json_out -> MockOutput
  {
    auto binding = MakePooledOutputBinding<MockOutput>(
        "json_out", {{"result_json", &MockOutput::text, {65536}}},
        [](MockOutput& out) noexcept {
          out.byte_size = static_cast<int32_t>(sizeof(MockOutput));
          out.type = kMockOutputOc;
          out.service_type = 0;
        });
    SetServiceTypeMember(&binding, &MockOutput::service_type);
    SetOutputValue<MockOutput, JsonOutputValue>(
        &binding, [](MockOutput& out, const JsonOutputValue& value,
                     const ResolvedOutputPoolSpec& spec) {
          if (out.byte_size != static_cast<int32_t>(sizeof(MockOutput)) ||
              out.type != kMockOutputOc)
            return AdapterStatus::BufferTooSmall(
                "MockOutput pool block has invalid layout", "result_json");
          return WriteText(out.text, value.result_json, "result_json", spec);
        });
    binding.services = {{"keyword_match", kMockServiceKeywordMatch},
                        {"entity_extract", kMockServiceEntityExtract},
                        {"doc_qa", kMockServiceDocQa},
                        {"dialogue_audit", kMockServiceDialogueAudit},
                        {"ocr_invoice_qa", kMockServiceOcrInvoiceQa},
                        {"audio_asr_intent", kMockServiceAudioAsrIntent},
                        {"cross_rerank", kMockServiceCrossRerank},
                        {"translate", kMockServiceTranslate}};
    registry.RegisterBinding(std::move(binding));
  }

  // keyword_in / entity_in -> MockInput
  registry.RegisterBinding(MakeTextInputBinding("keyword_in"));
  registry.RegisterBinding(MakeTextInputBinding("entity_in"));

  // doc_in -> MockDocInput
  {
    auto binding = MakeTypedInputBinding<MockDocInput>(
        "doc_in",
        [](const MockDocInput& in, const InputLimits& limits,
           std::string* err) -> int {
          int ret = ValidateMockString(in.query_text, limits.max_text_bytes,
                                       "query_text", err);
          if (ret != 0) return ret;
          if (in.doc_text) {
            ret = ValidateMockString(in.doc_text, limits.max_doc_text_bytes,
                                     "doc_text", err);
            if (ret != 0) return ret;
          }
          return 0;
        });
    SetServiceTypeMember(&binding, &MockDocInput::service_type);
    SetInputValue<MockDocInput, DocumentInputValue>(
        &binding, [](const MockDocInput& in) {
          return DocumentInputValue{ReadString(in.doc_text),
                                    ReadString(in.query_text)};
        });
    binding.services = {{"doc_qa", kMockServiceDocQa}};
    registry.RegisterBinding(std::move(binding));
  }

  // audit_in -> MockAuditInput
  {
    auto binding = MakeTypedInputBinding<MockAuditInput>(
        "audit_in",
        [](const MockAuditInput& in, const InputLimits& limits,
           std::string* err) -> int {
          int ret = ValidateMockString(in.user_text, limits.max_text_bytes,
                                       "user_text", err);
          if (ret != 0) return ret;
          if (in.channel_name) {
            ret = ValidateMockString(in.channel_name,
                                     input_limits::kMaxChannelNameBytes,
                                     "channel_name", err);
            if (ret != 0) return ret;
          }
          return 0;
        });
    SetServiceTypeMember(&binding, &MockAuditInput::service_type);
    SetInputValue<MockAuditInput, AuditInputValue>(
        &binding, [](const MockAuditInput& in) {
          return AuditInputValue{ReadString(in.user_text),
                                 ReadString(in.channel_name)};
        });
    binding.services = {{"dialogue_audit", kMockServiceDialogueAudit}};
    registry.RegisterBinding(std::move(binding));
  }

  // audio_in -> MockAudioInput
  {
    auto binding = MakeTypedInputBinding<MockAudioInput>(
        "audio_in",
        [](const MockAudioInput& in, const InputLimits& limits,
           std::string* err) -> int {
          if (in.sample_rate < limits.min_sample_rate ||
              in.sample_rate > limits.max_sample_rate) {
            if (err) {
              *err = "sample_rate " + std::to_string(in.sample_rate) +
                     " out of valid range [" +
                     std::to_string(limits.min_sample_rate) + ", " +
                     std::to_string(limits.max_sample_rate) + "]";
            }
            return error::kInvalidInput;
          }
          if (in.pcm_length < 0 ||
              in.pcm_length > limits.max_audio_pcm_samples ||
              static_cast<size_t>(in.pcm_length) >
                  limits.max_audio_pcm_bytes / sizeof(float)) {
            if (err)
              *err = "pcm_length " + std::to_string(in.pcm_length) +
                     " invalid or exceeds limit " +
                     std::to_string(limits.max_audio_pcm_samples);
            return error::kInvalidInput;
          }
          if (in.pcm_length > 0 && !in.pcm_buffer) {
            if (err) *err = "pcm_buffer pointer is null";
            return error::kInvalidInput;
          }
          return 0;
        });
    SetServiceTypeMember(&binding, &MockAudioInput::service_type);
    SetInputValue<MockAudioInput, AudioInputValue>(
        &binding, [](const MockAudioInput& in) {
          AudioInputValue value;
          value.sample_rate = in.sample_rate;
          if (in.pcm_length)
            value.pcm.assign(in.pcm_buffer, in.pcm_buffer + in.pcm_length);
          return value;
        });
    binding.services = {{"audio_asr_intent", kMockServiceAudioAsrIntent}};
    registry.RegisterBinding(std::move(binding));
  }

  // rerank_in -> MockRerankInput
  {
    auto binding = MakeTypedInputBinding<MockRerankInput>(
        "rerank_in",
        [](const MockRerankInput& in, const InputLimits& limits,
           std::string* err) -> int {
          int ret = ValidateMockString(in.query_text, limits.max_text_bytes,
                                       "query_text", err);
          if (ret != 0) return ret;
          if (in.candidate_count <= 0 ||
              in.candidate_count > MOCK_MAX_RERANK_CANDIDATES) {
            if (err) {
              *err = "candidate_count " + std::to_string(in.candidate_count) +
                     " out of valid range [1, " +
                     std::to_string(MOCK_MAX_RERANK_CANDIDATES) + "]";
            }
            return error::kInvalidInput;
          }
          for (int i = 0; i < in.candidate_count; ++i) {
            if (!in.candidate_passages[i]) {
              if (err)
                *err = "candidate_passages[" + std::to_string(i) + "] is null";
              return error::kInvalidInput;
            }
            ret = ValidateMockString(
                in.candidate_passages[i], limits.max_doc_text_bytes,
                ("candidate_passages[" + std::to_string(i) + "]").c_str(), err);
            if (ret != 0) return ret;
          }
          return 0;
        });
    SetServiceTypeMember(&binding, &MockRerankInput::service_type);
    SetInputValue<MockRerankInput, RerankInputValue>(
        &binding, [](const MockRerankInput& in) {
          RerankInputValue value;
          value.query_text = ReadString(in.query_text);
          for (int i = 0; i < in.candidate_count; ++i)
            value.candidate_passages.push_back(
                ReadString(in.candidate_passages[i]));
          return value;
        });
    binding.services = {{"cross_rerank", kMockServiceCrossRerank}};
    registry.RegisterBinding(std::move(binding));
  }
}

namespace {
const bool kMockBindingsRegistered = [] {
  RegisterMockOperatorBindings(OperatorValueTypeRegistry::Instance());
  return true;
}();
}  // namespace

}  // namespace llm_edgeflow
