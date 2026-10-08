#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
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

// 可选的编写辅助：为普通参数 struct 提供一份字段列表和一个语义解析器。
// 不读文件，也不序列化 JSON。
template <typename Parameters>
class ConfigParser {
 public:
  using ParseFn =
      std::function<bool(const nlohmann::json&, Parameters*, std::string*)>;

  ConfigParser(std::vector<ConfigFieldDefinition> fields, ParseFn parse)
      : fields_(std::move(fields)), parse_(std::move(parse)) {}

  const std::vector<ConfigFieldDefinition>& Fields() const noexcept {
    return fields_;
  }

  // 用于原始 Node 配置或已解码的 Control payload。复用与 PipelineValidator
  // 和 AuthorNode 相同的字段校验与默认值。
  std::optional<Parameters> Parse(const nlohmann::json& config,
                                  std::string* error = nullptr) const noexcept {
    if (error) error->clear();
    try {
      nlohmann::json normalized;
      std::vector<ConfigFieldValidationError> errors;
      if (!ValidateAndNormalizeFields(fields_, config, &normalized, &errors)) {
        SetDiagnosticNoexcept(error, errors.empty()
                                         ? "Invalid Node configuration"
                                         : errors.front().message);
        return std::nullopt;
      }
      return ParseNormalized(normalized, error);
    } catch (const std::exception& exception) {
      SetDiagnosticNoexcept(error, exception.what());
    } catch (...) {
      SetDiagnosticNoexcept(error,
                            "Unknown exception parsing Node configuration");
    }
    return std::nullopt;
  }

  // 仅用于已按 Fields() 校验并填充默认值的输入，如 Definition 的
  // validate_config 回调或 AuthorNode 初始化。直接转发现有 JSON 对象，
  // 不再复制或归一化。解析器负责语义检查，并须返回自有的参数数据。
  std::optional<Parameters> ParseNormalized(
      const nlohmann::json& config,
      std::string* error = nullptr) const noexcept {
    if (error) error->clear();
    try {
      if (!parse_) {
        SetDiagnosticNoexcept(error, "Missing Node configuration parser");
        return std::nullopt;
      }
      Parameters next{};
      if (!parse_(config, &next, error)) {
        if (error && error->empty())
          SetDiagnosticNoexcept(error, "Invalid Node configuration");
        return std::nullopt;
      }
      return std::optional<Parameters>(std::move(next));
    } catch (const std::exception& exception) {
      SetDiagnosticNoexcept(error, exception.what());
    } catch (...) {
      SetDiagnosticNoexcept(error,
                            "Unknown exception parsing Node configuration");
    }
    return std::nullopt;
  }

 private:
  std::vector<ConfigFieldDefinition> fields_;
  ParseFn parse_;
};

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
  // 以 float 能往返的最短十进制写出，避免 0.7f 变成 0.699999988...
  static nlohmann::json ToJson(float val) {
    char buffer[32];
    for (int precision = 1; precision <= 9; ++precision) {
      std::snprintf(buffer, sizeof(buffer), "%.*g", precision,
                    static_cast<double>(val));
      if (std::strtof(buffer, nullptr) == val) {
        return std::strtod(buffer, nullptr);
      }
    }
    return static_cast<double>(val);
  }
};

namespace detail {

// FieldTypeTraits<T> 已定义时为真。结构体元素没有特化，由 Items(Parameters<T>)
// 提供定义和解析。
template <typename T, typename = void>
struct HasFieldTraits : std::false_type {};
template <typename T>
struct HasFieldTraits<T, std::void_t<decltype(sizeof(FieldTypeTraits<T>))>>
    : std::true_type {};

template <typename T, typename = void>
struct HasDescribe : std::false_type {};
template <typename T>
struct HasDescribe<T, std::void_t<decltype(T::Describe())>> : std::true_type {};

// 成员类型对应的无名定义：类型，加数组、映射的元素定义。
template <typename T>
ConfigFieldDefinition DescribeType() {
  if constexpr (HasFieldTraits<T>::value) {
    if constexpr (HasDescribe<FieldTypeTraits<T>>::value) {
      return FieldTypeTraits<T>::Describe();
    } else {
      ConfigFieldDefinition def;
      def.kind = FieldTypeTraits<T>::kKind;
      return def;
    }
  } else {
    ConfigFieldDefinition def;  // 结构体元素
    def.kind = ConfigValueKind::kObject;
    return def;
  }
}

template <typename T>
bool ExtractElement(const nlohmann::json& j, T* out, std::string* err) {
  if constexpr (HasFieldTraits<T>::value) {
    return FieldTypeTraits<T>::Extract(j, out, err);
  } else {
    if (err) *err = "struct elements require Items(Parameters<T>)";
    return false;
  }
}

template <typename T>
nlohmann::json ElementToJson(const T& value) {
  if constexpr (HasFieldTraits<T>::value) {
    return FieldTypeTraits<T>::ToJson(value);
  } else {
    (void)value;
    throw std::invalid_argument(
        "Struct elements cannot be written as a default value");
  }
}

}  // namespace detail

template <typename T>
struct FieldTypeTraits<std::vector<T>> {
  static constexpr ConfigValueKind kKind = ConfigValueKind::kArray;
  static ConfigFieldDefinition Describe() {
    ConfigFieldDefinition def;
    def.kind = kKind;
    def.items = std::make_shared<const ConfigFieldDefinition>(
        detail::DescribeType<T>());
    return def;
  }
  static bool Extract(const nlohmann::json& j, std::vector<T>* out,
                      std::string* err) {
    if (!j.is_array()) {
      if (err) *err = "expected array";
      return false;
    }
    std::vector<T> res;
    res.reserve(j.size());
    for (size_t i = 0; i < j.size(); ++i) {
      T element{};
      std::string element_error;
      if (!detail::ExtractElement(j[i], &element, &element_error)) {
        if (err) *err = "element " + std::to_string(i) + ": " + element_error;
        return false;
      }
      res.push_back(std::move(element));
    }
    *out = std::move(res);
    return true;
  }
  static nlohmann::json ToJson(const std::vector<T>& val) {
    nlohmann::json array = nlohmann::json::array();
    for (const auto& element : val) {
      array.push_back(detail::ElementToJson(element));
    }
    return array;
  }
};

template <typename T>
struct FieldTypeTraits<std::map<std::string, T>> {
  static constexpr ConfigValueKind kKind = ConfigValueKind::kMap;
  static ConfigFieldDefinition Describe() {
    ConfigFieldDefinition def;
    def.kind = kKind;
    def.items = std::make_shared<const ConfigFieldDefinition>(
        detail::DescribeType<T>());
    return def;
  }
  static bool Extract(const nlohmann::json& j, std::map<std::string, T>* out,
                      std::string* err) {
    if (!j.is_object()) {
      if (err) *err = "expected object";
      return false;
    }
    std::map<std::string, T> res;
    for (auto it = j.begin(); it != j.end(); ++it) {
      T element{};
      std::string element_error;
      if (!detail::ExtractElement(it.value(), &element, &element_error)) {
        if (err) *err = "key '" + it.key() + "': " + element_error;
        return false;
      }
      res.emplace(it.key(), std::move(element));
    }
    *out = std::move(res);
    return true;
  }
  static nlohmann::json ToJson(const std::map<std::string, T>& val) {
    nlohmann::json object = nlohmann::json::object();
    for (const auto& [key, element] : val) {
      object[key] = detail::ElementToJson(element);
    }
    return object;
  }
};

// 任意 JSON 值，不能为 null。
template <>
struct FieldTypeTraits<nlohmann::json> {
  static constexpr ConfigValueKind kKind = ConfigValueKind::kJson;
  static bool Extract(const nlohmann::json& j, nlohmann::json* out,
                      std::string* err) {
    if (j.is_null()) {
      if (err) *err = "expected a non-null JSON value";
      return false;
    }
    *out = j;
    return true;
  }
  static nlohmann::json ToJson(const nlohmann::json& val) { return val; }
};

// std::optional<T> 成员表示"可以不写"：类型、范围、枚举规则与 T 相同，
// 不写时成员保持为空。可选参数不能声明 Required() 或 Default()。
template <typename T>
struct FieldTypeTraits<std::optional<T>> {
  static constexpr ConfigValueKind kKind = FieldTypeTraits<T>::kKind;
  static ConfigFieldDefinition Describe() { return detail::DescribeType<T>(); }
  static bool Extract(const nlohmann::json& j, std::optional<T>* out,
                      std::string* err) {
    T value{};
    if (!detail::ExtractElement(j, &value, err)) return false;
    *out = std::move(value);
    return true;
  }
  static nlohmann::json ToJson(const std::optional<T>& val) {
    return val.has_value() ? detail::ElementToJson(*val) : nlohmann::json();
  }
};

namespace detail {

template <typename T>
struct OptionalMember : std::false_type {
  using Value = T;
};
template <typename T>
struct OptionalMember<std::optional<T>> : std::true_type {
  using Value = T;
};

// 约束落在最内层的标量元素上；非容器时就是字段本身。
inline void ApplyConstraints(ConfigFieldDefinition* def,
                             const std::optional<double>& minimum,
                             const std::optional<double>& maximum,
                             const std::vector<std::string>& enum_values) {
  if (def->items) {
    ConfigFieldDefinition element = *def->items;
    ApplyConstraints(&element, minimum, maximum, enum_values);
    def->items =
        std::make_shared<const ConfigFieldDefinition>(std::move(element));
    return;
  }
  def->minimum = minimum;
  def->maximum = maximum;
  def->enum_values = enum_values;
}

template <typename T>
struct StructContainer : std::false_type {};
template <typename E>
struct StructContainer<std::vector<E>> : std::true_type {
  using Element = E;
};
template <typename E>
struct StructContainer<std::map<std::string, E>> : std::true_type {
  using Element = E;
};

template <typename T>
const T& MemberValue(const T& value) noexcept {
  return value;
}
template <typename T>
const T& MemberValue(const std::optional<T>& value) noexcept {
  return *value;
}

}  // namespace detail

template <typename ParamsT>
class Parameters;

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
  using ValueType = typename detail::OptionalMember<MemberT>::Value;

  // 结构体元素的容器没有 FieldTypeTraits，由 Items(Parameters<E>) 提供解析。
  using Extractor =
      std::function<bool(const nlohmann::json&, MemberT*, std::string*)>;

  ConcreteFieldBinding(std::string name, MemberPtr member_ptr, bool required,
                       bool has_default, MemberT default_val,
                       std::optional<double> min_val,
                       std::optional<double> max_val,
                       std::vector<std::string> enum_vals, std::string semantic,
                       std::vector<ConfigFieldDefinition> element_fields = {},
                       Extractor extractor = nullptr)
      : name_(std::move(name)),
        member_ptr_(member_ptr),
        required_(required),
        has_default_(has_default),
        default_val_(std::move(default_val)),
        minimum_(min_val),
        maximum_(max_val),
        enum_values_(std::move(enum_vals)),
        semantic_(std::move(semantic)),
        element_fields_(std::move(element_fields)),
        extractor_(std::move(extractor)) {}

  const std::string& Name() const override { return name_; }

  ConfigFieldDefinition ToFieldDefinition() const override {
    ConfigFieldDefinition def = detail::DescribeType<MemberT>();
    def.name = name_;
    def.required = required_;
    def.semantic = semantic_;
    if (!element_fields_.empty() && def.items) {
      ConfigFieldDefinition element = *def.items;
      element.fields = element_fields_;
      def.items = std::make_shared<const ConfigFieldDefinition>(element);
    }
    // 数组、映射的 Range、Enum 约束其中的标量元素。
    detail::ApplyConstraints(&def, minimum_, maximum_, enum_values_);

    if (has_default_) {
      def.default_value = FieldTypeTraits<MemberT>::ToJson(default_val_);
      // default_value 必须满足 minimum、maximum 和 enum_values
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
    const bool extracted_ok = extractor_ ? extractor_(val_json, &extracted, err)
                                         : FieldTypeTraits<MemberT>::Extract(
                                               val_json, &extracted, err);
    if (!extracted_ok) {
      if (err) {
        *err = "Field '" + name_ + "' extraction error: " + *err;
      }
      return false;
    }
    // 再校验取值边界
    // 以配置中的数值比较，避免 float 成员的舍入误差越过边界。
    if (minimum_.has_value()) {
      if constexpr (std::is_arithmetic_v<ValueType>) {
        if (val_json.get<double>() < *minimum_) {
          if (err) *err = "Field '" + name_ + "' is below minimum";
          return false;
        }
      }
    }
    if (maximum_.has_value()) {
      if constexpr (std::is_arithmetic_v<ValueType>) {
        if (val_json.get<double>() > *maximum_) {
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
  std::vector<ConfigFieldDefinition> element_fields_;
  Extractor extractor_;
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

  // 元素是结构体的数组、映射（std::vector<E>、std::map<std::string, E>）
  // 用 E 的参数声明校验、补默认值、解析每个元素，再执行 E 的 Prepare 和
  // Validate。E 须可默认构造。
  template <typename E>
  FieldBuilder& Items(Parameters<E> element) {
    using Container = typename detail::OptionalMember<MemberT>::Value;
    static_assert(std::is_same_v<Container, std::vector<E>> ||
                      std::is_same_v<Container, std::map<std::string, E>>,
                  "Items(Parameters<E>) requires a std::vector<E> or "
                  "std::map<std::string, E> member");
    auto shared = std::make_shared<const Parameters<E>>(std::move(element));
    element_fields_ = shared->Fields();
    if (element_fields_.empty()) {
      throw std::invalid_argument("Items for field '" + name_ +
                                  "' must declare at least one field");
    }
    extractor_ = [shared, name = name_](const nlohmann::json& j, MemberT* out,
                                        std::string* err) {
      Container result;
      const auto parse_element = [&](const nlohmann::json& value,
                                     const std::string& where, E* element_out) {
        std::string element_error;
        auto parsed = shared->ParseNormalized(value, &element_error);
        if (!parsed.has_value()) {
          if (err) *err = where + ": " + element_error;
          return false;
        }
        *element_out = std::move(*parsed);
        return true;
      };
      if constexpr (std::is_same_v<Container, std::vector<E>>) {
        if (!j.is_array()) {
          if (err) *err = "expected array";
          return false;
        }
        for (size_t i = 0; i < j.size(); ++i) {
          E element;
          if (!parse_element(j[i], "element " + std::to_string(i), &element))
            return false;
          result.push_back(std::move(element));
        }
      } else {
        if (!j.is_object()) {
          if (err) *err = "expected object";
          return false;
        }
        for (auto it = j.begin(); it != j.end(); ++it) {
          E element;
          if (!parse_element(it.value(), "key '" + it.key() + "'", &element))
            return false;
          result.emplace(it.key(), std::move(element));
        }
      }
      *out = std::move(result);
      return true;
    };
    return *this;
  }

  std::unique_ptr<ParameterFieldBinding<ParamsT>> Build() const {
    if constexpr (detail::OptionalMember<MemberT>::value) {
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
        maximum_, enum_values_, semantic_, element_fields_, extractor_);
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
  std::vector<ConfigFieldDefinition> element_fields_;
  typename ConcreteFieldBinding<ParamsT, MemberT>::Extractor extractor_;
};

template <typename ParamsT, typename MemberT>
inline FieldBuilder<ParamsT, MemberT> Field(std::string name,
                                            MemberT ParamsT::*member_ptr) {
  return FieldBuilder<ParamsT, MemberT>(std::move(name), member_ptr);
}

// Include 并入的字段：在 JSON 中与自有字段平铺，值写进 ParamsT 的一个成员。
template <typename ParamsT, typename GroupT>
class IncludedFieldBinding final : public ParameterFieldBinding<ParamsT> {
 public:
  IncludedFieldBinding(GroupT ParamsT::*member,
                       std::unique_ptr<ParameterFieldBinding<GroupT>> inner)
      : member_(member), inner_(std::move(inner)) {}
  IncludedFieldBinding(const IncludedFieldBinding& other)
      : member_(other.member_), inner_(other.inner_->Clone()) {}

  const std::string& Name() const override { return inner_->Name(); }
  ConfigFieldDefinition ToFieldDefinition() const override {
    return inner_->ToFieldDefinition();
  }
  bool Assign(const nlohmann::json& normalized_json, ParamsT* out,
              std::string* err) const override {
    if (!out) return false;
    return inner_->Assign(normalized_json, &(out->*member_), err);
  }
  bool ConflictsWithMember(
      const ParameterFieldBinding<ParamsT>& other) const override {
    const auto* casted =
        dynamic_cast<const IncludedFieldBinding<ParamsT, GroupT>*>(&other);
    return casted && member_ == casted->member_ &&
           inner_->ConflictsWithMember(*casted->inner_);
  }
  std::unique_ptr<ParameterFieldBinding<ParamsT>> Clone() const override {
    return std::make_unique<IncludedFieldBinding>(*this);
  }

 private:
  GroupT ParamsT::*member_;
  std::unique_ptr<ParameterFieldBinding<GroupT>> inner_;
};

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

    // 实例化配置字段定义，以便提前校验默认值
    definitions_.reserve(bindings_.size());
    for (const auto& binding : bindings_) {
      definitions_.push_back(binding->ToFieldDefinition());
    }
  }

  Parameters(const Parameters& other)
      : definitions_(other.definitions_),
        complex_parser_(other.complex_parser_),
        includes_(other.includes_),
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
      includes_ = other.includes_;
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

  // 并入另一组参数：其字段与自有字段平铺在同一层，值写进 member。重名时
  // 立即报错；被并入组的 Prepare、Validate 先于本组执行。
  template <typename GroupT>
  Parameters& Include(GroupT ParamsT::*member,
                      const Parameters<GroupT>& group) {
    if (group.HasParser()) {
      throw std::invalid_argument(
          "Include does not support a parameter group with WithParser");
    }
    for (const auto& incoming : group.Bindings()) {
      if (!incoming) continue;
      for (const auto& existing : bindings_) {
        if (existing->Name() == incoming->Name()) {
          throw std::invalid_argument("Duplicate config field name: " +
                                      incoming->Name());
        }
      }
    }
    std::vector<std::unique_ptr<ParameterFieldBinding<ParamsT>>> added;
    for (const auto& incoming : group.Bindings()) {
      if (!incoming) continue;
      added.push_back(std::make_unique<IncludedFieldBinding<ParamsT, GroupT>>(
          member, incoming->Clone()));
    }
    for (auto& binding : added) {
      for (const auto& existing : bindings_) {
        if (binding->ConflictsWithMember(*existing)) {
          throw std::invalid_argument(
              "Same struct member bound to multiple config fields (" +
              existing->Name() + ", " + binding->Name() + ")");
        }
      }
      definitions_.push_back(binding->ToFieldDefinition());
      bindings_.push_back(std::move(binding));
    }
    auto shared = std::make_shared<const Parameters<GroupT>>(group);
    includes_.push_back([member, shared](ParamsT* state,
                                         const BindingFacts& facts,
                                         std::string* err) {
      return shared->ValidateState(&(state->*member), facts, err);
    });
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
      for (const auto& include : includes_) {
        if (!include(state, facts, err)) {
          if (err && err->empty()) *err = "Included parameters are invalid";
          return false;
        }
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

  // 解析器提供复杂的自有成员；之后由类型化绑定赋值简单成员，
  // Validate 再检查完整的参数对象。
  Parameters& WithParser(ConfigParser<ParamsT> parser) {
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
  std::optional<ConfigParser<ParamsT>> complex_parser_;
  std::vector<PrepareFunction> includes_;
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
