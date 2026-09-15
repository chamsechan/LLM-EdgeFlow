#pragma once

#include "adapter/io_binding.h"
#include "adapter/io_converter.h"

namespace llm_edgeflow {

class IoConverterRegistry;
class IoBindingRegistry;

#define REGISTER_INPUT_CONVERTER(def_expr)                                  \
  static const bool g_reg_input_converter_##__LINE__ = []() {               \
    return ::llm_edgeflow::IoConverterRegistry::Instance()                  \
        .RegisterInputConverter(def_expr);                                  \
  }()

#define REGISTER_OUTPUT_CONVERTER(def_expr)                                 \
  static const bool g_reg_output_converter_##__LINE__ = []() {              \
    return ::llm_edgeflow::IoConverterRegistry::Instance()                  \
        .RegisterOutputConverter(def_expr);                                 \
  }()

#define REGISTER_IO_BINDING(binding_expr)                                   \
  static const bool g_reg_io_binding_##__LINE__ = []() {                    \
    return ::llm_edgeflow::IoBindingRegistry::Instance().RegisterBinding(   \
        binding_expr);                                                      \
  }()

#define REGISTER_BIZ_EXPOSURE(exposure_expr)                                \
  static const bool g_reg_biz_exposure_##__LINE__ = []() {                  \
    return ::llm_edgeflow::IoBindingRegistry::Instance().RegisterExposure(  \
        exposure_expr);                                                     \
  }()

}  // namespace llm_edgeflow
