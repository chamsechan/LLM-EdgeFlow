#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

#include "contracts/parameters.h"

namespace llm_edgeflow {

// 参数在创建时解析，之后只读共享；不要求参数结构可复制。
class ParameterValues {
 public:
  template <typename P>
  const P& Get() const {
    if (type_ != std::type_index(typeid(P))) {
      throw std::logic_error("Parameter type does not match its declaration");
    }
    return *static_cast<const P*>(value_.get());
  }

  const nlohmann::json& Effective() const noexcept { return effective_; }

  std::optional<int64_t> Integer(const std::string& name) const {
    const auto value = effective_.find(name);
    if (value == effective_.end()) return std::nullopt;
    if (value->is_number_unsigned()) {
      const auto number = value->get<uint64_t>();
      if (number > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        return std::nullopt;
      return static_cast<int64_t>(number);
    }
    if (!value->is_number_integer()) return std::nullopt;
    return value->get<int64_t>();
  }

 private:
  friend class ParameterSet;
  template <typename P>
  explicit ParameterValues(P value, nlohmann::json effective)
      : value_(std::make_shared<const P>(std::move(value))),
        type_(typeid(P)),
        effective_(std::move(effective)) {}

  std::shared_ptr<const void> value_;
  std::type_index type_;
  nlohmann::json effective_;
};

class ParameterSet {
 public:
  ParameterSet() : ParameterSet(Parameters<NoParameters>{}) {}

  template <typename P>
  ParameterSet(
      Parameters<P> spec) {  // NOLINT: Definition accepts a typed spec.
    auto owned = std::make_shared<const Parameters<P>>(std::move(spec));
    fields_ = owned->Fields();
    parse_ = [owned](const nlohmann::json& config,
                     std::shared_ptr<const ParameterValues>* values,
                     std::string* error) {
      auto parsed = owned->Parse(config, error);
      if (!parsed) return false;
      auto effective = owned->Read(*parsed);
      *values = std::shared_ptr<const ParameterValues>(
          new ParameterValues(std::move(*parsed), std::move(effective)));
      return true;
    };
  }

  const std::vector<ConfigFieldDefinition>& Fields() const noexcept {
    return fields_;
  }

  bool Parse(const nlohmann::json& config,
             std::shared_ptr<const ParameterValues>* values,
             std::string* error = nullptr) const noexcept {
    if (error) error->clear();
    if (!values) {
      SetDiagnosticNoexcept(error, "Null parameter values output");
      return false;
    }
    values->reset();
    try {
      return parse_(config, values, error);
    } catch (const std::exception& exception) {
      SetDiagnosticNoexcept(error, exception.what());
    } catch (...) {
      SetDiagnosticNoexcept(error, "Unknown exception parsing parameters");
    }
    return false;
  }

 private:
  std::vector<ConfigFieldDefinition> fields_;
  std::function<bool(const nlohmann::json&,
                     std::shared_ptr<const ParameterValues>*, std::string*)>
      parse_;
};

}  // namespace llm_edgeflow
