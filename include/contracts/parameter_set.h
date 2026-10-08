#pragma once

#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

#include "contracts/config_schema.h"
#include "contracts/config_schema_validation.h"
#include "contracts/diagnostic.h"
#include "contracts/parameters.h"

namespace llm_edgeflow {

// 已校验、已补齐默认值的一组参数。Parse 生成之后只读共享；
// 持有者通过 Get<P>() 取回声明时使用的参数结构体。
class ParameterValues {
 public:
  // 类型与声明不符时抛出 std::logic_error。
  template <typename P>
  const P& Get() const {
    if (type_ != std::type_index(typeid(P))) {
      throw std::logic_error(
          "ParameterValues::Get: requested type does not match the declared "
          "parameter type");
    }
    return *static_cast<const P*>(value_.get());
  }

 private:
  friend class ParameterSet;

  ParameterValues(std::type_index type, std::shared_ptr<const void> value)
      : type_(type), value_(std::move(value)) {}

  std::type_index type_;
  std::shared_ptr<const void> value_;
};

// 一个组件（模型、后端）的参数声明：字段列表加上"配置到参数结构体"的
// 完整解析流程。类型擦除，调用方不必知道参数结构体的类型。
class ParameterSet {
 public:
  // 没有参数：配置只能是 {}。
  ParameterSet() : ParameterSet(Parameters<NoParameters>{}) {}

  // 允许 def.params = ParamSpec();
  template <typename P>
  ParameterSet(Parameters<P> spec);  // NOLINT

  const std::vector<ConfigFieldDefinition>& Fields() const noexcept {
    return fields_;
  }

  // 依次执行：按 Fields() 校验并补齐默认值、字段赋值、Prepare、Validate。
  // config 可以是原始配置，也可以是已归一化的配置，两者结果一致。
  // 失败时 values 保持不变，error 写入原因。
  bool Parse(const nlohmann::json& config,
             std::shared_ptr<const ParameterValues>* values,
             std::string* error) const noexcept;

 private:
  using ParseFunction = std::function<bool(
      const nlohmann::json&, std::shared_ptr<const ParameterValues>*,
      std::string*)>;

  template <typename P>
  static std::shared_ptr<const ParameterValues> MakeValues(P&& parameters) {
    using Value = std::decay_t<P>;
    return std::shared_ptr<const ParameterValues>(new ParameterValues(
        std::type_index(typeid(Value)),
        std::make_shared<const Value>(std::forward<P>(parameters))));
  }

  static std::string DescribeFirstError(
      const std::vector<ConfigFieldValidationError>& errors) {
    if (errors.empty()) return "Invalid configuration";
    const auto& first = errors.front();
    if (first.path.empty() ||
        first.message.find(first.path) != std::string::npos) {
      return first.message;
    }
    return "Field '" + first.path + "': " + first.message;
  }

  std::vector<ConfigFieldDefinition> fields_;
  ParseFunction parse_;
};

template <typename P>
ParameterSet::ParameterSet(Parameters<P> spec) : fields_(spec.Fields()) {
  auto shared = std::make_shared<const Parameters<P>>(std::move(spec));
  parse_ = [shared](const nlohmann::json& config,
                    std::shared_ptr<const ParameterValues>* values,
                    std::string* error) {
    nlohmann::json normalized;
    std::vector<ConfigFieldValidationError> errors;
    if (!ValidateAndNormalizeFields(shared->Fields(), config, &normalized,
                                    &errors)) {
      SetDiagnosticNoexcept(error, DescribeFirstError(errors));
      return false;
    }
    auto parsed = shared->ParseNormalized(normalized, error);
    if (!parsed.has_value()) return false;
    if (values) *values = MakeValues(std::move(*parsed));
    return true;
  };
}

inline bool ParameterSet::Parse(const nlohmann::json& config,
                                std::shared_ptr<const ParameterValues>* values,
                                std::string* error) const noexcept {
  if (error) error->clear();
  try {
    return parse_(config, values, error);
  } catch (const std::exception& exception) {
    SetDiagnosticNoexcept(error, exception.what());
  } catch (...) {
    SetDiagnosticNoexcept(error, "Unknown exception parsing parameters");
  }
  return false;
}

}  // namespace llm_edgeflow
