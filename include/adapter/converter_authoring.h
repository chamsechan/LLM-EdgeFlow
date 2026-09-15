#pragma once

#include "adapter/io_binding.h"
#include "adapter/io_binding_registry.h"
#include "adapter/io_converter.h"
#include "adapter/io_converter_registry.h"

namespace llm_edgeflow {

#define EDGEFLOW_CONCAT_IMPL(s1, s2) s1##s2
#define EDGEFLOW_CONCAT(s1, s2) EDGEFLOW_CONCAT_IMPL(s1, s2)

#define REGISTER_INPUT_CONVERTER(def_expr)                                  \
  static const bool EDGEFLOW_CONCAT(g_reg_input_converter_, __COUNTER__) =  \
      []() {                                                                \
        return ::llm_edgeflow::IoConverterRegistry::Instance()              \
            .RegisterInputConverter(def_expr);                              \
      }()

#define REGISTER_OUTPUT_CONVERTER(def_expr)                                 \
  static const bool EDGEFLOW_CONCAT(g_reg_output_converter_, __COUNTER__) = \
      []() {                                                                \
        return ::llm_edgeflow::IoConverterRegistry::Instance()              \
            .RegisterOutputConverter(def_expr);                             \
      }()

#define REGISTER_IO_BINDING(binding_expr)                                   \
  static const bool EDGEFLOW_CONCAT(g_reg_io_binding_, __COUNTER__) =       \
      []() {                                                                \
        return ::llm_edgeflow::IoBindingRegistry::Instance()                \
            .RegisterBinding(binding_expr);                                 \
      }()

#define REGISTER_BIZ_EXPOSURE(exposure_expr)                                \
  static const bool EDGEFLOW_CONCAT(g_reg_biz_exposure_, __COUNTER__) =     \
      []() {                                                                \
        return ::llm_edgeflow::IoBindingRegistry::Instance()                \
            .RegisterExposure(exposure_expr);                               \
      }()

}  // namespace llm_edgeflow
