#pragma once

#include <cstdint>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "adapter/adapter_result.h"
#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/result_validation.h"
#include "adapter/text_carrier.h"
#include "core/alg_context.h"
#include "core/blackboard_key.h"
#include "platform_mock/error_codes.h"

namespace llm_edgeflow {

/**
 * @brief 类型化文本输入批次执行骨架 (ADP-002, RFC-0053)
 *
 * 保证执行顺序：
 * 1. 批信封预检与非空 ctx 检查
 * 2. 整批 carrier 校验与 copy-in (保证错误优先级)
 * 3. 逐项调用业务 Decode，构建批内编号 (req_id=i, sub_id=0) 与外部 ID 映射
 * 4. 全部样本转换成功后才原子发布 BlackboardKey
 */
template <typename DecodeFn>
inline int UnpackTextBatchSkeleton(const void** inputs, int num_inputs,
                                   int max_batch_size, const char* adapter_name,
                                   AlgContext* ctx,
                                   const BlackboardKey<TextBatch>& text_key,
                                   DecodeFn&& decode_fn,
                                   AdapterStatus* out_status = nullptr,
                                   const char* carrier_adapter_name = nullptr) {
  if (!ctx) {
    return AdapterValidationHelper::ReturnInvalidInput(
        out_status, "Batch envelope validation failed or null AlgContext",
        "inputs", adapter_name);
  }

  const char* carrier_name =
      (carrier_adapter_name && carrier_adapter_name[0] != '\0')
          ? carrier_adapter_name
          : adapter_name;

  std::vector<OwnedTextRequest> requests;
  int carrier_ret = UnpackTextCarrierBatch(inputs, num_inputs, max_batch_size,
                                           carrier_name, &requests, out_status);
  if (carrier_ret != COMPANY_ALG_SUCCESS) {
    return carrier_ret;
  }

  std::vector<uint64_t> raw_req_ids;
  TextBatch sentences;
  raw_req_ids.reserve(num_inputs);
  sentences.reserve(num_inputs);

  for (int i = 0; i < num_inputs; ++i) {
    auto res = decode_fn(requests[i]);
    if (!res.IsOk()) {
      if (out_status) {
        *out_status = res.Status();
      }
      return res.ReturnCode();
    }
    raw_req_ids.push_back(requests[i].request_id);
    sentences.emplace_back(static_cast<uint32_t>(i), 0, res.TakeValue());
  }

  if (!AdapterValidationHelper::PublishContextValue(*ctx, kRawRequestIds,
                                                    std::move(raw_req_ids),
                                                    adapter_name, out_status) ||
      !AdapterValidationHelper::PublishContextValue(
          *ctx, text_key, std::move(sentences), adapter_name, out_status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  return COMPANY_ALG_SUCCESS;
}

/**
 * @brief 多路一对一结果对齐视图 (RFC-0053 Section 2.2)
 */
template <typename PrimaryBatch, typename... SecondaryBatches>
class RequestResults {
 public:
  RequestResults() = default;

  RequestResults(
      const std::vector<uint64_t>* raw_req_ids,
      std::vector<const typename PrimaryBatch::value_type*> primary,
      std::tuple<std::vector<const typename SecondaryBatches::value_type*>...>
          secondary)
      : raw_req_ids_(raw_req_ids),
        primary_(std::move(primary)),
        secondary_(std::move(secondary)) {}

  size_t Size() const noexcept { return primary_.size(); }

  uint64_t RequestId(size_t index) const {
    return (raw_req_ids_ && index < raw_req_ids_->size())
               ? (*raw_req_ids_)[index]
               : primary_[index]->req_id;
  }

  const typename PrimaryBatch::value_type& Primary(size_t index) const {
    return *primary_[index];
  }

  template <size_t N>
  const auto& Secondary(size_t index) const {
    return *std::get<N>(secondary_)[index];
  }

 private:
  const std::vector<uint64_t>* raw_req_ids_{nullptr};
  std::vector<const typename PrimaryBatch::value_type*> primary_;
  std::tuple<std::vector<const typename SecondaryBatches::value_type*>...>
      secondary_;
};

/**
 * @brief 多路一对一结果绑定规格 (RFC-0053 Section 2.2)
 *
 * 封装各路内部结果的带类型 BlackboardKey、字段名（用于错误诊断）、
 * 索引名（用于 IndexResults 错误诊断）、必需性、自定义错误消息与返回码映射。
 */
template <typename Batch>
struct ResultBindingSpec {
  BlackboardKey<Batch> key;
  std::string field_name{""};
  std::string index_name{""};
  bool required{true};
  std::string missing_message{""};
  int missing_error_code{COMPANY_ALG_ERR_INVALID_INPUT};

  ResultBindingSpec() = default;

  ResultBindingSpec(BlackboardKey<Batch> k, std::string field,
                    std::string idx = "", bool req = true, std::string msg = "",
                    int err = COMPANY_ALG_ERR_INVALID_INPUT)
      : key(std::move(k)),
        field_name(std::move(field)),
        index_name(idx.empty() ? field_name : std::move(idx)),
        required(req),
        missing_message(std::move(msg)),
        missing_error_code(err) {}
};

namespace detail {

inline int ReportMissingContextKey(AdapterStatus* out_status, int error_code,
                                   std::string msg, std::string field_name,
                                   const char* adapter_name) {
  if (out_status) {
    *out_status =
        AdapterStatus(error_code, std::move(msg), std::move(field_name), -1,
                      adapter_name ? adapter_name : "");
  }
  return error_code;
}

template <typename PrimaryBatch, typename... SecondaryBatches>
inline int ValidateRequiredSpecs(
    const char* adapter_name, AdapterStatus* out_status,
    const ResultBindingSpec<PrimaryBatch>& primary_spec,
    const ResultBindingSpec<std::vector<uint64_t>>& raw_req_ids_spec,
    const ResultBindingSpec<SecondaryBatches>&... secondary_specs) {
  if (!primary_spec.required) {
    return ReportMissingContextKey(
        out_status, COMPANY_ALG_ERR_INVALID_INPUT,
        "Optional bindings not supported in current phase",
        primary_spec.field_name, adapter_name);
  }
  if (!raw_req_ids_spec.required) {
    return ReportMissingContextKey(
        out_status, COMPANY_ALG_ERR_INVALID_INPUT,
        "Optional bindings not supported in current phase",
        raw_req_ids_spec.field_name, adapter_name);
  }
  if constexpr (sizeof...(SecondaryBatches) > 0) {
    int ret = COMPANY_ALG_SUCCESS;
    auto check_spec = [&](const auto& spec) {
      if (ret != COMPANY_ALG_SUCCESS) return;
      if (!spec.required) {
        ret = ReportMissingContextKey(
            out_status, COMPANY_ALG_ERR_INVALID_INPUT,
            "Optional bindings not supported in current phase", spec.field_name,
            adapter_name);
      }
    };
    (check_spec(secondary_specs), ...);
    if (ret != COMPANY_ALG_SUCCESS) return ret;
  }
  return COMPANY_ALG_SUCCESS;
}

template <size_t Index = 0, typename PrimaryBatch, typename... SecondaryBatches,
          typename Tuple>
inline int IndexSecondaryBatches(
    const AlgContext& ctx, const std::vector<uint64_t>* raw_req_ids,
    const char* adapter_name, AdapterStatus* out_status, Tuple& tuple,
    const std::tuple<ResultBindingSpec<SecondaryBatches>...>& specs) {
  if constexpr (Index < sizeof...(SecondaryBatches)) {
    const auto& spec = std::get<Index>(specs);
    const auto* batch_ptr = ctx.Read(spec.key);
    if (batch_ptr) {
      if (!IndexResults(batch_ptr, raw_req_ids, &std::get<Index>(tuple),
                        spec.index_name.c_str(), adapter_name, out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }
    }
    return IndexSecondaryBatches<Index + 1, PrimaryBatch, SecondaryBatches...>(
        ctx, raw_req_ids, adapter_name, out_status, tuple, specs);
  }
  return COMPANY_ALG_SUCCESS;
}

}  // namespace detail

/**
 * @brief 通用多路 1:1 结果读取与按请求索引对齐核心模板 (RFC-0053 Section 2.2)
 *
 * 执行语义：
 * 1. 检查 AlgContext 是否非空
 * 2. 检查所有 spec 是否为 required（首期拒绝 optional binding）
 * 3. 读取主结果批次并获取 count（若缺失则按 primary_spec 诊断并返回）
 * 4. 逐路校验次要结果批次的大小匹配（按 secondary_specs 顺序校验与诊断）
 * 5. 校验原始请求 ID 列表的大小匹配（按 raw_req_ids_spec 诊断）
 * 6. 对主结果与各路次要结果调用 IndexResults 按请求构建单项索引视图
 * 7. 装配并输出 RequestResults<PrimaryBatch, SecondaryBatches...>
 */
namespace detail {

template <typename PrimaryBatch, typename... SecondaryBatches>
inline int ValidatePrimaryAndSpecs(
    AlgContext* ctx, const char* adapter_name, AdapterStatus* out_status,
    const PrimaryBatch** out_primary,
    const ResultBindingSpec<PrimaryBatch>& primary_spec,
    const ResultBindingSpec<std::vector<uint64_t>>& raw_req_ids_spec,
    const ResultBindingSpec<SecondaryBatches>&... secondary_specs) {
  if (!ctx) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        out_status, "Null AlgContext passed to Pack", "ctx", adapter_name);
  }

  int spec_ret =
      detail::ValidateRequiredSpecs(adapter_name, out_status, primary_spec,
                                    raw_req_ids_spec, secondary_specs...);
  if (spec_ret != COMPANY_ALG_SUCCESS) return spec_ret;

  const auto* primary = ctx->Read(primary_spec.key);
  if (!primary) {
    std::string msg = primary_spec.missing_message.empty()
                          ? ("Missing required context value for " +
                             std::string(adapter_name ? adapter_name : ""))
                          : primary_spec.missing_message;
    return detail::ReportMissingContextKey(
        out_status, primary_spec.missing_error_code, std::move(msg),
        primary_spec.field_name, adapter_name);
  }

  *out_primary = primary;
  return COMPANY_ALG_SUCCESS;
}

template <typename PrimaryBatch, typename... SecondaryBatches>
inline int AlignAndIndexResults(
    AlgContext* ctx, const char* adapter_name, AdapterStatus* out_status,
    const PrimaryBatch* primary,
    RequestResults<PrimaryBatch, SecondaryBatches...>* out_results,
    const ResultBindingSpec<PrimaryBatch>& primary_spec,
    const ResultBindingSpec<std::vector<uint64_t>>& raw_req_ids_spec,
    const ResultBindingSpec<SecondaryBatches>&... secondary_specs) {
  const int count = static_cast<int>(primary->size());

  // 逐项校验次要结果批次
  if constexpr (sizeof...(SecondaryBatches) > 0) {
    int check_ret = COMPANY_ALG_SUCCESS;
    auto check_secondary = [&](const auto& spec) {
      if (check_ret != COMPANY_ALG_SUCCESS) return;
      const auto* batch_ptr = ctx->Read(spec.key);
      if (!batch_ptr || batch_ptr->size() != static_cast<size_t>(count)) {
        std::string msg =
            spec.missing_message.empty()
                ? (spec.field_name + " missing or count mismatch in AlgContext")
                : spec.missing_message;
        check_ret = detail::ReportMissingContextKey(
            out_status, spec.missing_error_code, std::move(msg),
            spec.field_name, adapter_name);
      }
    };
    (check_secondary(secondary_specs), ...);
    if (check_ret != COMPANY_ALG_SUCCESS) return check_ret;
  }

  // 校验 raw_req_ids
  const auto* raw_req_ids = ctx->Read(raw_req_ids_spec.key);
  if (!raw_req_ids || raw_req_ids->size() != static_cast<size_t>(count)) {
    std::string msg = raw_req_ids_spec.missing_message.empty()
                          ? (raw_req_ids_spec.field_name +
                             " missing or count mismatch in AlgContext")
                          : raw_req_ids_spec.missing_message;
    return detail::ReportMissingContextKey(
        out_status, raw_req_ids_spec.missing_error_code, std::move(msg),
        raw_req_ids_spec.field_name, adapter_name);
  }

  // 索引主结果
  std::vector<const typename PrimaryBatch::value_type*> primary_ptrs;
  if (primary) {
    if (!IndexResults(primary, raw_req_ids, &primary_ptrs,
                      primary_spec.index_name.c_str(), adapter_name,
                      out_status)) {
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }
  }

  // 索引各路次要结果
  std::tuple<std::vector<const typename SecondaryBatches::value_type*>...>
      secondary_tuple;
  const auto specs_tuple = std::make_tuple(secondary_specs...);
  int index_ret =
      detail::IndexSecondaryBatches<0, PrimaryBatch, SecondaryBatches...>(
          *ctx, raw_req_ids, adapter_name, out_status, secondary_tuple,
          specs_tuple);
  if (index_ret != COMPANY_ALG_SUCCESS) return index_ret;

  if (out_results) {
    *out_results = RequestResults<PrimaryBatch, SecondaryBatches...>(
        raw_req_ids, std::move(primary_ptrs), std::move(secondary_tuple));
  }

  return COMPANY_ALG_SUCCESS;
}

}  // namespace detail

template <typename PrimaryBatch, typename... SecondaryBatches>
inline int ReadMultiWayResults(
    AlgContext* ctx, const char* adapter_name, AdapterStatus* out_status,
    RequestResults<PrimaryBatch, SecondaryBatches...>* out_results,
    const ResultBindingSpec<PrimaryBatch>& primary_spec,
    const ResultBindingSpec<std::vector<uint64_t>>& raw_req_ids_spec,
    const ResultBindingSpec<SecondaryBatches>&... secondary_specs) {
  const PrimaryBatch* primary = nullptr;
  int ret = detail::ValidatePrimaryAndSpecs(
      ctx, adapter_name, out_status, &primary, primary_spec, raw_req_ids_spec,
      secondary_specs...);
  if (ret != COMPANY_ALG_SUCCESS) return ret;

  return detail::AlignAndIndexResults(ctx, adapter_name, out_status, primary,
                                      out_results, primary_spec,
                                      raw_req_ids_spec, secondary_specs...);
}

/**
 * @brief 带输出容量与槽位校验的多路 1:1 结果读取对齐模板 (RFC-0053 Section 2.2)
 *
 * 在读取对齐前无条件校验 outputs 与 num_outputs（防止空指针解引用及容量越界）。
 */
template <typename PrimaryBatch, typename... SecondaryBatches>
inline int ReadMultiWayResults(
    AlgContext* ctx, void** outputs, int* num_outputs, const char* adapter_name,
    AdapterStatus* out_status,
    RequestResults<PrimaryBatch, SecondaryBatches...>* out_results,
    const ResultBindingSpec<PrimaryBatch>& primary_spec,
    const ResultBindingSpec<std::vector<uint64_t>>& raw_req_ids_spec,
    const ResultBindingSpec<SecondaryBatches>&... secondary_specs) {
  const PrimaryBatch* primary = nullptr;
  int ret = detail::ValidatePrimaryAndSpecs(
      ctx, adapter_name, out_status, &primary, primary_spec, raw_req_ids_spec,
      secondary_specs...);
  if (ret != COMPANY_ALG_SUCCESS) return ret;

  const int count = static_cast<int>(primary->size());

  int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
      outputs, num_outputs, count, adapter_name, out_status);
  if (valid_ret != 0) return valid_ret;

  return detail::AlignAndIndexResults(ctx, adapter_name, out_status, primary,
                                      out_results, primary_spec,
                                      raw_req_ids_spec, secondary_specs...);
}

}  // namespace llm_edgeflow
