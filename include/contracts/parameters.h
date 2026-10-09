#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "contracts/config_schema.h"
#include "contracts/config_schema_validation.h"
#include "contracts/diagnostic.h"

namespace llm_edgeflow {

/**
 * @brief Init 期间防御性捕获的输入端口连接事实。
 *
 * 初始化后不可变；用于区分仅做语义解析与按显式输入绑定校验。
 */
struct BindingFacts {
  bool has_bindings = false;
  std::unordered_set<std::string> connected_inputs;

  bool IsConnected(const std::string& port_name) const noexcept {
    return connected_inputs.count(port_name) > 0;
  }
};

template <typename ParamsT>
class Parameters;

template <typename T>
struct FieldTypeTraits {
  static constexpr ConfigValueKind kKind = ConfigValueKind::kObject;
  static bool Extract(const nlohmann::json&, T*, std::string* error) {
    if (error) *error = "Struct elements require Items(Parameters<T>)";
    return false;
  }
};

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
struct FieldTypeTraits<float> {
  static constexpr ConfigValueKind kKind = ConfigValueKind::kNumber;
  static bool Extract(const nlohmann::json& j, float* out, std::string* err) {
    double value;
    if (!FieldTypeTraits<double>::Extract(j, &value, err)) return false;
    if (!(value >= -std::numeric_limits<float>::max() &&
          value <= std::numeric_limits<float>::max())) {
      if (err) *err = "number outside finite float range";
      return false;
    }
    *out = static_cast<float>(value);
    return true;
  }
  static nlohmann::json ToJson(float val) {
    if (!std::isfinite(val)) return val;
    char text[64];
    const auto result = std::to_chars(text, text + sizeof(text), val);
    return nlohmann::json::parse(text, result.ptr);
  }
};

template <>
struct FieldTypeTraits<nlohmann::json> {
  static constexpr ConfigValueKind kKind = ConfigValueKind::kJson;
  static bool Extract(const nlohmann::json& value, nlohmann::json* out,
                      std::string* error) {
    if (value.is_null()) {
      if (error) *error = "JSON value cannot be null";
      return false;
    }
    *out = value;
    return true;
  }
  static nlohmann::json ToJson(const nlohmann::json& value) { return value; }
};

template <typename T>
struct FieldTypeTraits<std::vector<T>> {
  static constexpr ConfigValueKind kKind = ConfigValueKind::kArray;
  static nlohmann::json ToJson(const std::vector<T>& values) {
    auto result = nlohmann::json::array();
    for (const auto& value : values)
      result.push_back(FieldTypeTraits<T>::ToJson(value));
    return result;
  }
};

template <typename T>
struct FieldTypeTraits<std::map<std::string, T>> {
  static constexpr ConfigValueKind kKind = ConfigValueKind::kMap;
  static nlohmann::json ToJson(const std::map<std::string, T>& values) {
    auto result = nlohmann::json::object();
    for (const auto& [name, value] : values)
      result[name] = FieldTypeTraits<T>::ToJson(value);
    return result;
  }
};

namespace detail {
template <typename T>
struct IsOptionalField : std::false_type {};
template <typename T>
struct IsOptionalField<std::optional<T>> : std::true_type {};
}  // namespace detail

template <typename T>
struct FieldTypeTraits<std::optional<T>> {
  static_assert(std::is_same_v<T, std::string> || std::is_same_v<T, bool> ||
                    std::is_same_v<T, int> || std::is_same_v<T, int64_t> ||
                    std::is_same_v<T, float> || std::is_same_v<T, double>,
                "Optional parameters require a supported scalar type");
  static constexpr ConfigValueKind kKind = FieldTypeTraits<T>::kKind;
  static bool Extract(const nlohmann::json& value, std::optional<T>* out,
                      std::string* error) {
    T parsed{};
    if (!FieldTypeTraits<T>::Extract(value, &parsed, error)) return false;
    *out = std::move(parsed);
    return true;
  }
  static nlohmann::json ToJson(const std::optional<T>& value) {
    return value ? FieldTypeTraits<T>::ToJson(*value) : nlohmann::json();
  }
};

namespace detail {

template <typename T>
struct ParameterContainer {
  static constexpr bool kArray = false;
  static constexpr bool kMap = false;
  using Leaf = T;
};
template <typename T>
struct ParameterContainer<std::vector<T>> {
  static constexpr bool kArray = true;
  static constexpr bool kMap = false;
  using Value = T;
  using Leaf = typename ParameterContainer<T>::Leaf;
};
template <typename T>
struct ParameterContainer<std::map<std::string, T>> {
  static constexpr bool kArray = false;
  static constexpr bool kMap = true;
  using Value = T;
  using Leaf = typename ParameterContainer<T>::Leaf;
};

template <typename T, typename Leaf>
ConfigFieldDefinition ParameterValueDefinition(
    const std::shared_ptr<const Parameters<Leaf>>& item_spec,
    std::optional<double> minimum, std::optional<double> maximum,
    const std::vector<std::string>& enums) {
  ConfigFieldDefinition result;
  result.kind = FieldTypeTraits<T>::kKind;
  if constexpr (ParameterContainer<T>::kArray || ParameterContainer<T>::kMap) {
    using Value = typename ParameterContainer<T>::Value;
    result.items = std::make_shared<const ConfigFieldDefinition>(
        ParameterValueDefinition<Value>(item_spec, minimum, maximum, enums));
  } else {
    result.minimum = minimum;
    result.maximum = maximum;
    result.enum_values = enums;
    if constexpr (FieldTypeTraits<T>::kKind == ConfigValueKind::kObject) {
      if (!item_spec)
        throw std::invalid_argument(
            "Struct elements require Items(Parameters<T>)");
      result.fields = item_spec->Fields();
    }
  }
  return result;
}

template <typename T, typename Leaf>
nlohmann::json SerializeParameterValue(
    const T& value, const std::shared_ptr<const Parameters<Leaf>>& item_spec) {
  if constexpr (ParameterContainer<T>::kArray || ParameterContainer<T>::kMap) {
    auto result = ParameterContainer<T>::kArray ? nlohmann::json::array()
                                                : nlohmann::json::object();
    if constexpr (ParameterContainer<T>::kArray) {
      for (const auto& item : value)
        result.push_back(SerializeParameterValue(item, item_spec));
    } else {
      for (const auto& [key, item] : value)
        result[key] = SerializeParameterValue(item, item_spec);
    }
    return result;
  } else if constexpr (FieldTypeTraits<T>::kKind == ConfigValueKind::kObject) {
    return item_spec->Read(value);
  } else {
    return FieldTypeTraits<T>::ToJson(value);
  }
}

template <typename T, typename Leaf>
bool ExtractParameterValue(
    const nlohmann::json& json, T* out, std::string* error,
    const std::shared_ptr<const Parameters<Leaf>>& item_spec) {
  if constexpr (ParameterContainer<T>::kArray || ParameterContainer<T>::kMap) {
    using Value = typename ParameterContainer<T>::Value;
    T result;
    for (auto it = json.begin(); it != json.end(); ++it) {
      Value next{};
      if (!ExtractParameterValue<Value>(*it, &next, error, item_spec)) {
        if (error)
          *error = (ParameterContainer<T>::kArray
                        ? "Array item " + std::to_string(it - json.begin())
                        : "Map key '" + it.key() + "'") +
                   ": " + *error;
        return false;
      }
      if constexpr (ParameterContainer<T>::kArray)
        result.push_back(std::move(next));
      else
        result.emplace(it.key(), std::move(next));
    }
    *out = std::move(result);
    return true;
  } else if constexpr (FieldTypeTraits<T>::kKind == ConfigValueKind::kObject) {
    auto parsed = item_spec->ParseNormalized(json, error);
    if (!parsed) return false;
    *out = std::move(*parsed);
    return true;
  } else {
    return FieldTypeTraits<T>::Extract(json, out, error);
  }
}

}  // namespace detail

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

  virtual std::optional<nlohmann::json> Read(const ParamsT& values) const = 0;
};

template <typename ParamsT, typename MemberT>
class ConcreteFieldBinding final : public ParameterFieldBinding<ParamsT> {
 public:
  using MemberPtr = MemberT ParamsT::*;
  using Leaf = typename detail::ParameterContainer<MemberT>::Leaf;

  ConcreteFieldBinding(std::string name, MemberPtr member_ptr, bool required,
                       bool has_default, MemberT default_val,
                       std::optional<double> minimum,
                       std::optional<double> maximum,
                       std::vector<std::string> enums, std::string semantic,
                       std::shared_ptr<const Parameters<Leaf>> item_spec,
                       bool file)
      : name_(std::move(name)),
        member_ptr_(member_ptr),
        required_(required),
        has_default_(has_default),
        default_val_(std::move(default_val)),
        minimum_(minimum),
        maximum_(maximum),
        enum_values_(std::move(enums)),
        semantic_(std::move(semantic)),
        item_spec_(std::move(item_spec)),
        file_(file) {}

  const std::string& Name() const override { return name_; }
  ConfigFieldDefinition ToFieldDefinition() const override {
    auto result = detail::ParameterValueDefinition<MemberT>(
        item_spec_, minimum_, maximum_, enum_values_);
    result.name = name_;
    result.required = required_;
    result.semantic = semantic_;
    result.file = file_;
    if (has_default_) {
      result.default_value =
          detail::SerializeParameterValue(default_val_, item_spec_);
      if (result.default_value.is_null())
        throw std::invalid_argument("Default cannot be null: " + name_);
    }
    return result;
  }

  bool Assign(const nlohmann::json& normalized, ParamsT* out,
              std::string* error) const override {
    if (!out) return false;
    const auto entry = normalized.find(name_);
    if (entry == normalized.end()) {
      if constexpr (detail::IsOptionalField<MemberT>::value) {
        out->*member_ptr_ = std::nullopt;
        return true;
      }
      if (!has_default_) {
        if (error) *error = "Missing required config field: " + name_;
        return false;
      }
      const auto definition = ToFieldDefinition();
      nlohmann::json value;
      std::vector<ConfigFieldValidationError> errors;
      if (!detail::NormalizeConfigValue(definition, definition.default_value,
                                        &value, &errors, "")) {
        if (error) *error = errors.front().message;
        return false;
      }
      return detail::ExtractParameterValue(value, &(out->*member_ptr_), error,
                                           item_spec_);
    }
    if (!detail::ExtractParameterValue(*entry, &(out->*member_ptr_), error,
                                       item_spec_)) {
      if (error) *error = "Field '" + name_ + "': " + *error;
      return false;
    }
    return true;
  }

  bool ConflictsWithMember(
      const ParameterFieldBinding<ParamsT>& other) const override {
    const auto* casted = dynamic_cast<const ConcreteFieldBinding*>(&other);
    return casted && member_ptr_ == casted->member_ptr_;
  }
  std::unique_ptr<ParameterFieldBinding<ParamsT>> Clone() const override {
    return std::make_unique<ConcreteFieldBinding>(*this);
  }

 private:
  std::optional<nlohmann::json> Read(const ParamsT& values) const override {
    if constexpr (detail::IsOptionalField<MemberT>::value) {
      if (!(values.*member_ptr_)) return std::nullopt;
    }
    return detail::SerializeParameterValue(values.*member_ptr_, item_spec_);
  }
  std::string name_;
  MemberPtr member_ptr_;
  bool required_;
  bool has_default_;
  MemberT default_val_;
  std::optional<double> minimum_, maximum_;
  std::vector<std::string> enum_values_;
  std::string semantic_;
  std::shared_ptr<const Parameters<Leaf>> item_spec_;
  bool file_ = false;
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

  using Leaf = typename detail::ParameterContainer<MemberT>::Leaf;
  FieldBuilder& Items(Parameters<Leaf> spec) {
    static_assert(detail::ParameterContainer<MemberT>::kArray ||
                      detail::ParameterContainer<MemberT>::kMap,
                  "Items is for arrays and maps");
    static_assert(FieldTypeTraits<Leaf>::kKind == ConfigValueKind::kObject,
                  "Items requires a struct parameter declaration");
    item_spec_ = std::make_shared<const Parameters<Leaf>>(std::move(spec));
    return *this;
  }

  FieldBuilder& Description(std::string desc) {
    semantic_ = std::move(desc);
    return *this;
  }

  FieldBuilder& File() {
    static_assert(std::is_same_v<MemberT, std::string> ||
                      std::is_same_v<MemberT, std::optional<std::string>>,
                  "File is only for string and optional string parameters");
    file_ = true;
    return *this;
  }

  std::unique_ptr<ParameterFieldBinding<ParamsT>> Build() const {
    if constexpr (detail::IsOptionalField<MemberT>::value) {
      if (required_ || has_default_) {
        throw std::invalid_argument("Optional field '" + name_ +
                                    "' cannot declare Required or Default");
      }
    } else if (required_ == has_default_) {
      throw std::invalid_argument(
          "Field '" + name_ +
          "' must declare exactly one of Required or Default");
    }
    return std::make_unique<ConcreteFieldBinding<ParamsT, MemberT>>(
        name_, member_ptr_, required_, has_default_, default_val_, minimum_,
        maximum_, enum_values_, semantic_, item_spec_, file_);
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
  std::shared_ptr<const Parameters<Leaf>> item_spec_;
  bool file_ = false;
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

template <typename Outer, typename Inner>
class IncludedFieldBinding final : public ParameterFieldBinding<Outer> {
 public:
  IncludedFieldBinding(Inner Outer::*member,
                       std::unique_ptr<ParameterFieldBinding<Inner>> binding)
      : member_(member), binding_(std::move(binding)) {}
  const std::string& Name() const override { return binding_->Name(); }
  ConfigFieldDefinition ToFieldDefinition() const override {
    return binding_->ToFieldDefinition();
  }
  bool Assign(const nlohmann::json& json, Outer* out,
              std::string* error) const override {
    return out && binding_->Assign(json, &(out->*member_), error);
  }
  bool ConflictsWithMember(
      const ParameterFieldBinding<Outer>& other) const override {
    const auto* included = dynamic_cast<const IncludedFieldBinding*>(&other);
    return included && member_ == included->member_ &&
           binding_->ConflictsWithMember(*included->binding_);
  }
  std::unique_ptr<ParameterFieldBinding<Outer>> Clone() const override {
    return std::make_unique<IncludedFieldBinding>(member_, binding_->Clone());
  }

 private:
  std::optional<nlohmann::json> Read(const Outer& values) const override {
    return binding_->Read(values.*member_);
  }
  Inner Outer::*member_;
  std::unique_ptr<ParameterFieldBinding<Inner>> binding_;
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

    // 实例化配置字段定义，以便提前校验默认值
    definitions_.reserve(bindings_.size());
    for (const auto& binding : bindings_) {
      definitions_.push_back(binding->ToFieldDefinition());
    }
    std::string error;
    if (!ValidateConfigFieldDefinitions(definitions_, &error)) {
      throw std::invalid_argument(error);
    }
  }

  Parameters(const Parameters& other)
      : definitions_(other.definitions_),
        prepare_fn_(other.prepare_fn_),
        included_validators_(other.included_validators_),
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
      prepare_fn_ = other.prepare_fn_;
      included_validators_ = other.included_validators_;
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

  template <typename Inner, typename Outer>
  Parameters& Include(Inner Outer::*member, Parameters<Inner> group) {
    static_assert(std::is_same_v<Outer, ParamsT>,
                  "Include member must belong to the parameter type");
    auto merged = definitions_;
    merged.insert(merged.end(), group.Fields().begin(), group.Fields().end());
    std::string error;
    if (!ValidateConfigFieldDefinitions(merged, &error))
      throw std::invalid_argument(error);
    auto spec = std::make_shared<const Parameters<Inner>>(std::move(group));
    std::vector<std::unique_ptr<ParameterFieldBinding<ParamsT>>> additions;
    for (const auto& binding : spec->Bindings()) {
      auto included = std::make_unique<IncludedFieldBinding<ParamsT, Inner>>(
          member, binding->Clone());
      for (const auto& existing : bindings_) {
        if (included->ConflictsWithMember(*existing))
          throw std::invalid_argument("Same member included more than once: " +
                                      included->Name());
      }
      additions.push_back(std::move(included));
    }
    included_validators_.push_back([member, spec](ParamsT* state,
                                                  const BindingFacts& facts,
                                                  std::string* error) {
      return spec->ValidateState(&(state->*member), facts, error);
    });
    for (auto& binding : additions) bindings_.push_back(std::move(binding));
    definitions_ = std::move(merged);
    return *this;
  }

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
    nlohmann::json normalized;
    std::vector<ConfigFieldValidationError> errors;
    if (!ValidateAndNormalizeFields({binding->ToFieldDefinition()}, obj,
                                    &normalized, &errors)) {
      if (err) *err = errors.front().path + ": " + errors.front().message;
      return false;
    }
    return binding->Assign(normalized, out, err);
  }

  bool ValidateState(ParamsT* state, const BindingFacts& facts,
                     std::string* err) const noexcept {
    try {
      for (const auto& included : included_validators_) {
        if (!included(state, facts, err)) return false;
      }
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
      const std::string_view reason = e.what();
      SetDiagnosticNoexcept(err, reason);
      // 保留原创建工厂在一次临时诊断分配失败后仍能报告原因的行为。
      if (err && err->empty() && !reason.empty()) {
        SetDiagnosticNoexcept(err, reason);
      }
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
        if (validation_errors.empty()) {
          SetDiagnosticNoexcept(error, "Invalid configuration");
        } else {
          const auto& invalid = validation_errors.front();
          SetDiagnosticNoexcept(
              error, invalid.path.empty()
                         ? invalid.message
                         : "Field '" + invalid.path + "': " + invalid.message);
        }
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
      std::string* error = nullptr,
      std::string* field_path = nullptr) const noexcept {
    if (error) error->clear();
    if (field_path) field_path->clear();
    try {
      ParamsT params{};
      for (const auto& binding : bindings_) {
        if (!binding->Assign(normalized, &params, error)) {
          SetDiagnosticNoexcept(
              field_path, "/" + detail::ConfigPointerToken(binding->Name()));
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
      std::string* error = nullptr,
      std::string* field_path = nullptr) const noexcept {
    BindingFacts facts;
    facts.has_bindings = true;
    facts.connected_inputs = connected_inputs;
    auto parsed = ParseNormalized(normalized, facts, error, field_path);
    return parsed.has_value();
  }

  nlohmann::json Read(const ParamsT& values) const {
    auto result = nlohmann::json::object();
    for (const auto& binding : bindings_) {
      auto value = binding->Read(values);
      if (value) result[binding->Name()] = std::move(*value);
    }
    return result;
  }

 private:
  std::vector<std::unique_ptr<ParameterFieldBinding<ParamsT>>> bindings_;
  std::vector<ConfigFieldDefinition> definitions_;
  PrepareFunction prepare_fn_;
  std::vector<PrepareFunction> included_validators_;
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
    try {
      nlohmann::json normalized;
      std::vector<ConfigFieldValidationError> validation_errors;
      if (!ValidateAndNormalizeFields({}, config, &normalized,
                                      &validation_errors)) {
        SetDiagnosticNoexcept(error, validation_errors.empty()
                                         ? "Invalid configuration"
                                         : validation_errors.front().message);
        return std::nullopt;
      }
      return NoParameters{};
    } catch (const std::exception& exception) {
      SetDiagnosticNoexcept(error, exception.what());
    } catch (...) {
      SetDiagnosticNoexcept(error, "Unknown exception parsing parameters");
    }
    return std::nullopt;
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
                            std::string* = nullptr,
                            std::string* field_path = nullptr) const noexcept {
    if (field_path) field_path->clear();
    return true;
  }

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

  nlohmann::json Read(const NoParameters&) const {
    return nlohmann::json::object();
  }
};

}  // namespace llm_edgeflow
