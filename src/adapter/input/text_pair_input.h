#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "adapter/converter_authoring.h"
#include "core/common_contracts.h"

namespace llm_edgeflow {

template <typename Value>
int DecodeTextPair(const ExternalInputBatchView& source,
                   const InputDecodeOptions& options, AlgContext* context,
                   AdapterStatus* status, const char* slot,
                   const BlackboardKey<TextBatch>& first_key,
                   std::string Value::*first,
                   const BlackboardKey<TextBatch>& second_key,
                   std::string Value::*second) {
  if (!ValidateDecodeRequest(source, options, context, status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  TextBatch first_texts;
  TextBatch second_texts;
  first_texts.reserve(source.count);
  second_texts.reserve(source.count);

  for (size_t i = 0; i < source.count; ++i) {
    auto input = ReadInputSlot<Value>(source, slot, i, options, status);
    if (!input) return COMPANY_ALG_ERR_INVALID_INPUT;

    std::string first_text = (*input).*first;
    std::string second_text = (*input).*second;
    first_texts.emplace_back(static_cast<uint32_t>(i), 0,
                             std::move(first_text));
    second_texts.emplace_back(static_cast<uint32_t>(i), 0,
                              std::move(second_text));
  }

  if (!AdapterValidationHelper::PublishContextValue(
          *context, options.Port(first_key.name), std::move(first_texts),
          options.Label().c_str(), status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, options.Port(second_key.name), std::move(second_texts),
          options.Label().c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  return COMPANY_ALG_SUCCESS;
}

}  // namespace llm_edgeflow
