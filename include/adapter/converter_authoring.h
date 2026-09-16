#pragma once

#include <cstring>
#include <string>

#include "adapter/io_binding.h"
#include "adapter/io_binding_registry.h"
#include "adapter/io_converter.h"
#include "adapter/io_converter_registry.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {

inline int CopyToOperatorString(const char* src, CompanyString* dest,
                                uint32_t capacity, const char* field_name,
                                std::string* err) noexcept {
  try {
    if (!dest || !dest->data) {
      if (err)
        *err = std::string(field_name ? field_name : "string") +
               " in destination pool block is null";
      return -4;
    }
    if (!src) {
      dest->length = 0;
      dest->data[0] = '\0';
      return 0;
    }
    size_t len = std::strlen(src);
    if (len > capacity) {
      if (err)
        *err = std::string(field_name ? field_name : "string") +
               " output length (" + std::to_string(len) +
               ") exceeds pool capacity (" + std::to_string(capacity) + ")";
      return -4;
    }
    std::memcpy(dest->data, src, len);
    dest->data[len] = '\0';
    dest->length = static_cast<int32_t>(len);
    return 0;
  } catch (const std::exception& e) {
    if (err) *err = e.what();
    return -4;
  } catch (...) {
    if (err) *err = "Unknown exception in CopyToOperatorString";
    return -4;
  }
}

#define EDGEFLOW_CONCAT_IMPL(s1, s2) s1##s2
#define EDGEFLOW_CONCAT(s1, s2) EDGEFLOW_CONCAT_IMPL(s1, s2)

#define REGISTER_INPUT_CONVERTER(def_expr)                  \
  static const bool EDGEFLOW_CONCAT(g_reg_input_converter_, \
                                    __COUNTER__) = []() {   \
    return ::llm_edgeflow::IoConverterRegistry::Instance()  \
        .RegisterInputConverter(def_expr);                  \
  }()

#define REGISTER_OUTPUT_CONVERTER(def_expr)                  \
  static const bool EDGEFLOW_CONCAT(g_reg_output_converter_, \
                                    __COUNTER__) = []() {    \
    return ::llm_edgeflow::IoConverterRegistry::Instance()   \
        .RegisterOutputConverter(def_expr);                  \
  }()

#define REGISTER_IO_BINDING(binding_expr)                                    \
  static const bool EDGEFLOW_CONCAT(g_reg_io_binding_, __COUNTER__) = []() { \
    return ::llm_edgeflow::IoBindingRegistry::Instance().RegisterBinding(    \
        binding_expr);                                                       \
  }()

#define REGISTER_BIZ_EXPOSURE(exposure_expr)                                   \
  static const bool EDGEFLOW_CONCAT(g_reg_biz_exposure_, __COUNTER__) = []() { \
    return ::llm_edgeflow::IoBindingRegistry::Instance().RegisterExposure(     \
        exposure_expr);                                                        \
  }()

}  // namespace llm_edgeflow
