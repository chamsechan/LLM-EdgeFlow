#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "contracts/config_schema.h"
#include "contracts/config_schema_validation.h"
#include "contracts/diagnostic.h"
#include "nodes/configuration_snapshot.h"
#include "nodes/node_config_parser.h"

namespace llm_edgeflow {

template <typename T>
struct FieldTypeTraits;

template <>
struct FieldTypeTraits<std::string> {
  static constexpr ConfigValueKind kKind = ConfigValueKind::kString;
  static bool Extract(const nlohmann::json& j, std::string* out,
                      std::string* err) {
    if (!j.is_string()) {
      if (err) *err = "expected string";
      return false;
    }
    *out = j.get<std::string>();
    return true;
  }
  static nlohmann::json ToJson(const std::string& val) { return val; }
};

template <>
struct FieldTypeTraits<bool> {
  static constexpr ConfigValueKind kKind = ConfigValueKind::kBoolean;
  static bool Extract(const nlohmann::json& j, bool* out, std::string* err) {
    if (!j.is_boolean()) {
      if (err) *err = "expected boolean";
      return false;
    }
    *out = j.get<bool>();
    return true;
  }
  static nlohmann::json ToJson(bool val) { return val; }
};

template <>
struct FieldTypeTraits<int> {
  static constexpr ConfigValueKind kKind = ConfigValueKind::kInteger;
  static bool Extract(const nlohmann::json& j, int* out, std::string* err) {
    if (!j.is_number_integer() && !j.is_number_unsigned()) {
      if (err) *err = "expected integer";
      return false;
    }
    if (j.is_number_unsigned()) {
      uint64_t uval = j.get<uint64_t>();
      if (uval > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        if (err) *err = "integer value out of 32-bit signed range";
        return false;
      }
      *out = static_cast<int>(uval);
      return true;
    }
    int64_t val = j.get<int64_t>();
    if (val < static_cast<int64_t>(std::numeric_limits<int>::min()) ||
        val > static_cast<int64_t>(std::numeric_limits<int>::max())) {
      if (err) *err = "integer value out of 32-bit signed range";
      return false;
    }
    *out = static_cast<int>(val);
    return true;
  }
  static nlohmann::json ToJson(int val) { return val; }
};

template <>
struct FieldTypeTraits<int64_t> {
  static constexpr ConfigValueKind kKind = ConfigValueKind::kInteger;
  static bool Extract(const nlohmann::json& j, int64_t* out, std::string* err) {
    if (!j.is_number_integer() && !j.is_number_unsigned()) {
      if (err) *err = "expected integer";
      return false;
    }
    if (j.is_number_unsigned()) {
      uint64_t uval = j.get<uint64_t>();
      if (uval > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        if (err) *err = "integer value out of 64-bit signed range";
        return false;
      }
      *out = static_cast<int64_t>(uval);
      return true;
    }
    *out = j.get<int64_t>();
    return true;
  }
  static nlohmann::json ToJson(int64_t val) { return val; }
};

template <>
struct FieldTypeTraits<double> {
  static constexpr ConfigValueKind kKind = ConfigValueKind::kNumber;
  static bool Extract(const nlohmann::json& j, double* out, std::string* err) {
    if (!j.is_number()) {
      if (err) *err = "expected number";
      return false;
    }
    *out = j.get<double>();
    return true;
  }
  static nlohmann::json ToJson(double val) { return val; }
};

template <>
struct FieldTypeTraits<std::vector<std::string>> {
  static constexpr ConfigValueKind kKind = ConfigValueKind::kArray;
  static bool Extract(const nlohmann::json& j, std::vector<std::string>* out,
                      std::string* err) {
    if (!j.is_array()) {
      if (err) *err = "expected array";
      return false;
    }
    std::vector<std::string> res;
    res.reserve(j.size());
    for (const auto& elem : j) {
      if (!elem.is_string()) {
        if (err) *err = "expected array of strings";
        return false;
      }
      res.push_back(elem.get<std::string>());
    }
    *out = std::move(res);
    return true;
  }
  static nlohmann::json ToJson(const std::vector<std::string>& val) {
    return val;
  }
};

template <typename ParamsT>
class ParameterFieldBinding {
 public:
  virtual ~ParameterFieldBinding() = default;
  virtual const std::string& Name() const = 0;
  virtual ConfigFieldDefinition ToFieldDefinition() const = 0;
  virtual bool Assign(const nlohmann::json& normalized_json, ParamsT* out,
                      std::string* err) const = 0;
  virtual bool ConflictsWithMember(
      const ParameterFieldBinding<ParamsT>& other) const = 0;
  virtual std::unique_ptr<ParameterFieldBinding<ParamsT>> Clone() const = 0;
};

template <typename ParamsT, typename MemberT>
class ConcreteFieldBinding final : public ParameterFieldBinding<ParamsT> {
 public:
  using MemberPtr = MemberT ParamsT::*;

  ConcreteFieldBinding(std::string name, MemberPtr member_ptr, bool required,
                       bool has_default, MemberT default_val,
                       std::optional<double> min_val,
                       std::optional<double> max_val,
                       std::vector<std::string> enum_vals, std::string semantic)
      : name_(std::move(name)),
        member_ptr_(member_ptr),
        required_(required),
        has_default_(has_default),
        default_val_(std::move(default_val)),
        minimum_(min_val),
        maximum_(max_val),
        enum_values_(std::move(enum_vals)),
        semantic_(std::move(semantic)) {}

  const std::string& Name() const override { return name_; }

  ConfigFieldDefinition ToFieldDefinition() const override {
    ConfigFieldDefinition def;
    def.name = name_;
    def.kind = FieldTypeTraits<MemberT>::kKind;
    def.required = required_;
    def.minimum = minimum_;
    def.maximum = maximum_;
    def.enum_values = enum_values_;
    def.semantic = semantic_;

    if (has_default_) {
      def.default_value = FieldTypeTraits<MemberT>::ToJson(default_val_);
      // Validate that default_value satisfies minimum, maximum, and enum_values
      if (minimum_.has_value()) {
        if constexpr (std::is_arithmetic_v<MemberT>) {
          if (static_cast<double>(default_val_) < *minimum_) {
            throw std::invalid_argument("Default value for field '" + name_ +
                                        "' is below minimum");
          }
        }
      }
      if (maximum_.has_value()) {
        if constexpr (std::is_arithmetic_v<MemberT>) {
          if (static_cast<double>(default_val_) > *maximum_) {
            throw std::invalid_argument("Default value for field '" + name_ +
                                        "' exceeds maximum");
          }
        }
      }
      if (!enum_values_.empty()) {
        if constexpr (std::is_same_v<MemberT, std::string>) {
          bool found = false;
          for (const auto& ev : enum_values_) {
            if (ev == default_val_) {
              found = true;
              break;
            }
          }
          if (!found) {
            throw std::invalid_argument("Default value for field '" + name_ +
                                        "' is not in allowed enum values");
          }
        }
      }
    }
    return def;
  }

  bool Assign(const nlohmann::json& normalized_json, ParamsT* out,
              std::string* err) const override {
    if (!out) return false;
    if (!normalized_json.contains(name_)) {
      if (required_) {
        if (err) *err = "Missing required config field: " + name_;
        return false;
      }
      if (has_default_) {
        out->*member_ptr_ = default_val_;
        return true;
      }
      return true;
    }
    const auto& val_json = normalized_json[name_];
    if (val_json.is_null()) {
      if (err) *err = "Field '" + name_ + "' cannot be null";
      return false;
    }
    MemberT extracted{};
    if (!FieldTypeTraits<MemberT>::Extract(val_json, &extracted, err)) {
      if (err) {
        *err = "Field '" + name_ + "' extraction error: " + *err;
      }
      return false;
    }
    // Additional bounds validation
    if (minimum_.has_value()) {
      if constexpr (std::is_arithmetic_v<MemberT>) {
        if (static_cast<double>(extracted) < *minimum_) {
          if (err) *err = "Field '" + name_ + "' is below minimum";
          return false;
        }
      }
    }
    if (maximum_.has_value()) {
      if constexpr (std::is_arithmetic_v<MemberT>) {
        if (static_cast<double>(extracted) > *maximum_) {
          if (err) *err = "Field '" + name_ + "' exceeds maximum";
          return false;
        }
      }
    }
    out->*member_ptr_ = std::move(extracted);
    return true;
  }

  bool ConflictsWithMember(
      const ParameterFieldBinding<ParamsT>& other) const override {
    const auto* casted =
        dynamic_cast<const ConcreteFieldBinding<ParamsT, MemberT>*>(&other);
    if (!casted) return false;
    return member_ptr_ == casted->member_ptr_;
  }

  std::unique_ptr<ParameterFieldBinding<ParamsT>> Clone() const override {
    return std::make_unique<ConcreteFieldBinding<ParamsT, MemberT>>(*this);
  }

 private:
  std::string name_;
  MemberPtr member_ptr_;
  bool required_ = false;
  bool has_default_ = false;
  MemberT default_val_{};
  std::optional<double> minimum_;
  std::optional<double> maximum_;
  std::vector<std::string> enum_values_;
  std::string semantic_;
};

template <typename ParamsT, typename MemberT>
class FieldBuilder {
 public:
  FieldBuilder(std::string name, MemberT ParamsT::*member_ptr)
      : name_(std::move(name)), member_ptr_(member_ptr) {}

  FieldBuilder& Default(MemberT default_val) {
    has_default_ = true;
    default_val_ = std::move(default_val);
    return *this;
  }

  FieldBuilder& Required() {
    required_ = true;
    return *this;
  }

  FieldBuilder& Range(double min_val, double max_val) {
    minimum_ = min_val;
    maximum_ = max_val;
    return *this;
  }

  FieldBuilder& Minimum(double min_val) {
    minimum_ = min_val;
    return *this;
  }

  FieldBuilder& Maximum(double max_val) {
    maximum_ = max_val;
    return *this;
  }

  FieldBuilder& Enum(std::vector<std::string> allowed) {
    enum_values_ = std::move(allowed);
    return *this;
  }

  FieldBuilder& Description(std::string desc) {
    semantic_ = std::move(desc);
    return *this;
  }

  FieldBuilder& Semantic(std::string sem) {
    semantic_ = std::move(sem);
    return *this;
  }

  std::unique_ptr<ParameterFieldBinding<ParamsT>> Build() const {
    if (required_ == has_default_) {
      throw std::invalid_argument(
          "Field '" + name_ +
          "' must declare exactly one of Required or Default");
    }
    return std::make_unique<ConcreteFieldBinding<ParamsT, MemberT>>(
        name_, member_ptr_, required_, has_default_, default_val_, minimum_,
        maximum_, enum_values_, semantic_);
  }

 private:
  std::string name_;
  MemberT ParamsT::*member_ptr_;
  bool required_ = false;
  bool has_default_ = false;
  MemberT default_val_{};
  std::optional<double> minimum_;
  std::optional<double> maximum_;
  std::vector<std::string> enum_values_;
  std::string semantic_;
};

template <typename ParamsT, typename MemberT>
inline FieldBuilder<ParamsT, MemberT> Field(std::string name,
                                            MemberT ParamsT::*member_ptr) {
  return FieldBuilder<ParamsT, MemberT>(std::move(name), member_ptr);
}

template <typename ParamsT>
class ParameterFieldBindingHolder {
 public:
  template <typename MemberT>
  ParameterFieldBindingHolder(
      const FieldBuilder<ParamsT, MemberT>& builder)  // NOLINT
      : binding_(builder.Build()) {}

  ParameterFieldBindingHolder(
      std::unique_ptr<ParameterFieldBinding<ParamsT>> binding)
      : binding_(std::move(binding)) {}

  ParameterFieldBindingHolder(const ParameterFieldBindingHolder& other)
      : binding_(other.binding_ ? other.binding_->Clone() : nullptr) {}

  ParameterFieldBindingHolder(ParameterFieldBindingHolder&&) noexcept = default;
  ParameterFieldBindingHolder& operator=(
      const ParameterFieldBindingHolder& other) {
    if (this != &other) {
      binding_ = other.binding_ ? other.binding_->Clone() : nullptr;
    }
    return *this;
  }
  ParameterFieldBindingHolder& operator=(
      ParameterFieldBindingHolder&&) noexcept = default;

  const ParameterFieldBinding<ParamsT>* get() const noexcept {
    return binding_.get();
  }
  const ParameterFieldBinding<ParamsT>& operator*() const noexcept {
    return *binding_;
  }
  const ParameterFieldBinding<ParamsT>* operator->() const noexcept {
    return binding_.get();
  }

 private:
  std::unique_ptr<ParameterFieldBinding<ParamsT>> binding_;
};

struct NoParameters {};

template <typename ParamsT>
class Parameters {
 public:
  using SemanticValidator = std::function<bool(const ParamsT&, std::string*)>;
  using BindingValidator = std::function<bool(
      const ParamsT&, const std::unordered_set<std::string>&, std::string*)>;
  using PrepareFunction =
      std::function<bool(ParamsT*, const BindingFacts&, std::string*)>;

  Parameters() = default;

  Parameters(
      std::initializer_list<ParameterFieldBindingHolder<ParamsT>> fields) {
    std::unordered_set<std::string> seen_names;
    bindings_.reserve(fields.size());
    for (const auto& holder : fields) {
      if (!holder.get()) continue;
      const auto& name = holder->Name();
      if (name.empty()) {
        throw std::invalid_argument("Config field name cannot be empty");
      }
      if (seen_names.count(name) > 0) {
        throw std::invalid_argument("Duplicate config field name: " + name);
      }
      for (const auto& existing : bindings_) {
        if (holder->ConflictsWithMember(*existing)) {
          throw std::invalid_argument(
              "Same struct member bound to multiple config fields (" +
              existing->Name() + ", " + name + ")");
        }
      }
      seen_names.insert(name);
      bindings_.push_back(holder->Clone());
    }

    // Materialize config field definitions to validate defaults eagerly
    definitions_.reserve(bindings_.size());
    for (const auto& binding : bindings_) {
      definitions_.push_back(binding->ToFieldDefinition());
    }
  }

  Parameters(const Parameters& other)
      : definitions_(other.definitions_),
        complex_parser_(other.complex_parser_),
        prepare_fn_(other.prepare_fn_),
        semantic_validator_(other.semantic_validator_),
        binding_validator_(other.binding_validator_) {
    bindings_.reserve(other.bindings_.size());
    for (const auto& b : other.bindings_) {
      bindings_.push_back(b ? b->Clone() : nullptr);
    }
  }

  Parameters(Parameters&&) noexcept = default;
  Parameters& operator=(const Parameters& other) {
    if (this != &other) {
      definitions_ = other.definitions_;
      complex_parser_ = other.complex_parser_;
      prepare_fn_ = other.prepare_fn_;
      semantic_validator_ = other.semantic_validator_;
      binding_validator_ = other.binding_validator_;
      bindings_.clear();
      bindings_.reserve(other.bindings_.size());
      for (const auto& b : other.bindings_) {
        bindings_.push_back(b ? b->Clone() : nullptr);
      }
    }
    return *this;
  }
  Parameters& operator=(Parameters&&) noexcept = default;

  Parameters& Prepare(PrepareFunction prepare) {
    prepare_fn_ = std::move(prepare);
    return *this;
  }

  Parameters& Prepare(std::function<bool(ParamsT*, std::string*)> prepare) {
    prepare_fn_ = [fn = std::move(prepare)](ParamsT* p, const BindingFacts&,
                                            std::string* diag) {
      return fn(p, diag);
    };
    return *this;
  }

  bool HasPrepare() const noexcept { return static_cast<bool>(prepare_fn_); }

  bool HasParser() const noexcept { return complex_parser_.has_value(); }

  const std::vector<std::unique_ptr<ParameterFieldBinding<ParamsT>>>& Bindings()
      const noexcept {
    return bindings_;
  }

  const ParameterFieldBinding<ParamsT>* FindBinding(
      const std::string& name) const noexcept {
    for (const auto& b : bindings_) {
      if (b && b->Name() == name) return b.get();
    }
    return nullptr;
  }

  bool AssignField(const std::string& name, const nlohmann::json& val,
                   ParamsT* out, std::string* err) const {
    const auto* binding = FindBinding(name);
    if (!binding) {
      if (err) *err = "Field '" + name + "' not found in parameter bindings";
      return false;
    }
    nlohmann::json obj = nlohmann::json::object();
    obj[name] = val;
    return binding->Assign(obj, out, err);
  }

  bool ValidateState(ParamsT* state, const BindingFacts& facts,
                     std::string* err) const noexcept {
    try {
      if (prepare_fn_) {
        if (!prepare_fn_(state, facts, err)) {
          if (err && err->empty()) *err = "Prepare failed";
          return false;
        }
      }
      if (semantic_validator_) {
        if (!semantic_validator_(*state, err)) {
          if (err && err->empty()) *err = "Semantic validation failed";
          return false;
        }
      }
      if (binding_validator_ && facts.has_bindings) {
        if (!binding_validator_(*state, facts.connected_inputs, err)) {
          if (err && err->empty()) *err = "Binding validation failed";
          return false;
        }
      }
      return true;
    } catch (const std::exception& e) {
      SetDiagnosticNoexcept(err, e.what());
      return false;
    } catch (...) {
      SetDiagnosticNoexcept(err, "Unknown exception validating parameters");
      return false;
    }
  }

  Parameters& Validate(SemanticValidator validator) {
    semantic_validator_ = std::move(validator);
    return *this;
  }

  Parameters& ValidateBindings(BindingValidator validator) {
    binding_validator_ = std::move(validator);
    return *this;
  }

  // The parser supplies complex owned members; typed bindings then assign
  // simple members, and Validate checks the complete parameter object.
  Parameters& WithParser(NodeConfigParser<ParamsT> parser) {
    if (complex_parser_) {
      throw std::invalid_argument(
          "Complex parameter parser is already configured");
    }
    auto merged = definitions_;
    merged.insert(merged.end(), parser.Fields().begin(), parser.Fields().end());
    std::string error;
    if (!ValidateConfigFieldDefinitions(merged, &error)) {
      throw std::invalid_argument(error);
    }
    complex_parser_ = std::move(parser);
    definitions_ = std::move(merged);
    return *this;
  }

  const std::vector<ConfigFieldDefinition>& Fields() const noexcept {
    return definitions_;
  }

  std::optional<ParamsT> Parse(const nlohmann::json& config,
                               std::string* error = nullptr) const noexcept {
    if (error) error->clear();
    try {
      nlohmann::json normalized;
      std::vector<ConfigFieldValidationError> validation_errors;
      if (!ValidateAndNormalizeFields(definitions_, config, &normalized,
                                      &validation_errors)) {
        SetDiagnosticNoexcept(error, validation_errors.empty()
                                         ? "Invalid configuration"
                                         : validation_errors.front().message);
        return std::nullopt;
      }
      return ParseNormalized(normalized, error);
    } catch (const std::exception& e) {
      SetDiagnosticNoexcept(error, e.what());
      return std::nullopt;
    } catch (...) {
      SetDiagnosticNoexcept(error, "Unknown exception parsing configuration");
      return std::nullopt;
    }
  }

  std::optional<ParamsT> ParseNormalized(
      const nlohmann::json& normalized, const BindingFacts& facts,
      std::string* error = nullptr) const noexcept {
    if (error) error->clear();
    try {
      ParamsT params{};
      if (complex_parser_) {
        auto parsed = complex_parser_->ParseNormalized(normalized, error);
        if (!parsed) return std::nullopt;
        params = std::move(*parsed);
      }
      for (const auto& binding : bindings_) {
        if (!binding->Assign(normalized, &params, error)) {
          return std::nullopt;
        }
      }
      if (!ValidateState(&params, facts, error)) {
        return std::nullopt;
      }
      return params;
    } catch (const std::exception& e) {
      SetDiagnosticNoexcept(error, e.what());
      return std::nullopt;
    } catch (...) {
      SetDiagnosticNoexcept(error, "Unknown exception parsing configuration");
      return std::nullopt;
    }
  }

  std::optional<ParamsT> ParseNormalized(
      const nlohmann::json& normalized,
      std::string* error = nullptr) const noexcept {
    BindingFacts facts;
    return ParseNormalized(normalized, facts, error);
  }

  bool ValidateWithBindings(
      const nlohmann::json& normalized,
      const std::unordered_set<std::string>& connected_inputs,
      std::string* error = nullptr) const noexcept {
    BindingFacts facts;
    facts.has_bindings = true;
    facts.connected_inputs = connected_inputs;
    auto parsed = ParseNormalized(normalized, facts, error);
    return parsed.has_value();
  }

 private:
  std::vector<std::unique_ptr<ParameterFieldBinding<ParamsT>>> bindings_;
  std::vector<ConfigFieldDefinition> definitions_;
  std::optional<NodeConfigParser<ParamsT>> complex_parser_;
  PrepareFunction prepare_fn_;
  SemanticValidator semantic_validator_;
  BindingValidator binding_validator_;
};

template <>
class Parameters<NoParameters> {
 public:
  const std::vector<ConfigFieldDefinition>& Fields() const noexcept {
    static const std::vector<ConfigFieldDefinition> empty_fields{};
    return empty_fields;
  }

  std::optional<NoParameters> Parse(
      const nlohmann::json& config,
      std::string* error = nullptr) const noexcept {
    if (error) error->clear();
    if (!config.empty()) {
      nlohmann::json normalized;
      std::vector<ConfigFieldValidationError> validation_errors;
      if (!ValidateAndNormalizeFields({}, config, &normalized,
                                      &validation_errors)) {
        SetDiagnosticNoexcept(error, validation_errors.empty()
                                         ? "Invalid configuration"
                                         : validation_errors.front().message);
        return std::nullopt;
      }
    }
    return NoParameters{};
  }

  std::optional<NoParameters> ParseNormalized(
      const nlohmann::json&, std::string* = nullptr) const noexcept {
    return NoParameters{};
  }

  std::optional<NoParameters> ParseNormalized(
      const nlohmann::json&, const BindingFacts&,
      std::string* = nullptr) const noexcept {
    return NoParameters{};
  }

  bool ValidateWithBindings(const nlohmann::json&,
                            const std::unordered_set<std::string>&,
                            std::string* = nullptr) const noexcept {
    return true;
  }

  bool HasPrepare() const noexcept { return false; }
  bool HasParser() const noexcept { return false; }

  const ParameterFieldBinding<NoParameters>* FindBinding(
      const std::string&) const noexcept {
    return nullptr;
  }

  bool AssignField(const std::string&, const nlohmann::json&, NoParameters*,
                   std::string*) const {
    return false;
  }

  bool ValidateState(NoParameters*, const BindingFacts&,
                     std::string*) const noexcept {
    return true;
  }
};

}  // namespace llm_edgeflow
