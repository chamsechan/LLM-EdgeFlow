#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "contracts/config_schema_validation.h"
#include "contracts/control_payload.h"
#include "contracts/parameters.h"
#include "tests/support/config_field_definition.h"

namespace llm_edgeflow {
namespace {

struct SampleParams {
  std::string mode;
  int count = 10;
  int64_t big_id = 1000;
  double ratio = 0.5;
  bool enabled = false;
  std::vector<std::string> tags;
};

TEST(ParameterBindingTest, RequiresExactlyOnePresenceDeclaration) {
  EXPECT_THROW((Parameters<SampleParams>{Field("count", &SampleParams::count)}),
               std::invalid_argument);
  EXPECT_THROW((Parameters<SampleParams>{
                   Field("count", &SampleParams::count).Required().Default(1)}),
               std::invalid_argument);
  EXPECT_THROW((Parameters<SampleParams>{
                   Field("count", &SampleParams::count).Default(1).Required()}),
               std::invalid_argument);
}

TEST(ParameterBindingTest, SuccessfulParseWithDefaultsAndOverrides) {
  auto schema = Parameters<SampleParams>({
      Field("mode", &SampleParams::mode)
          .Default("fast")
          .Enum({"fast", "slow", "balanced"})
          .Description("Execution mode"),
      Field("count", &SampleParams::count)
          .Default(10)
          .Range(1, 100)
          .Description("Item count"),
      Field("big_id", &SampleParams::big_id)
          .Default(static_cast<int64_t>(1000))
          .Description("64-bit ID"),
      Field("ratio", &SampleParams::ratio)
          .Default(0.5)
          .Range(0.0, 1.0)
          .Description("Ratio"),
      Field("enabled", &SampleParams::enabled)
          .Default(false)
          .Description("Flag"),
      Field("tags", &SampleParams::tags)
          .Default(std::vector<std::string>{"default_tag"})
          .Description("List of tags"),
  });

  // 空配置 -> 使用默认值
  std::string err;
  auto default_params = schema.Parse(nlohmann::json::object(), &err);
  ASSERT_TRUE(default_params.has_value()) << err;
  EXPECT_EQ(default_params->mode, "fast");
  EXPECT_EQ(default_params->count, 10);
  EXPECT_EQ(default_params->big_id, 1000);
  EXPECT_DOUBLE_EQ(default_params->ratio, 0.5);
  EXPECT_FALSE(default_params->enabled);
  ASSERT_EQ(default_params->tags.size(), 1u);
  EXPECT_EQ(default_params->tags[0], "default_tag");

  // 自定义值
  nlohmann::json custom = {
      {"mode", "slow"}, {"count", 42},     {"big_id", 99999999999LL},
      {"ratio", 0.75},  {"enabled", true}, {"tags", {"a", "b"}},
  };
  auto custom_params = schema.Parse(custom, &err);
  ASSERT_TRUE(custom_params.has_value()) << err;
  EXPECT_EQ(custom_params->mode, "slow");
  EXPECT_EQ(custom_params->count, 42);
  EXPECT_EQ(custom_params->big_id, 99999999999LL);
  EXPECT_DOUBLE_EQ(custom_params->ratio, 0.75);
  EXPECT_TRUE(custom_params->enabled);
  EXPECT_EQ(custom_params->tags, (std::vector<std::string>{"a", "b"}));
}

TEST(ParameterBindingTest, FloatNumberFieldRejectsOverflowAndNonfiniteValues) {
  struct ScoreParams {
    float min_score = 0.0f;
  };
  auto schema = Parameters<ScoreParams>{
      Field("min_score", &ScoreParams::min_score).Default(0.0f)};
  ASSERT_EQ(schema.Fields().size(), 1u);
  EXPECT_EQ(schema.Fields()[0].kind, ConfigValueKind::kNumber);

  std::string diagnostic;
  auto parsed = schema.Parse({{"min_score", 0.625}}, &diagnostic);
  ASSERT_TRUE(parsed.has_value()) << diagnostic;
  EXPECT_FLOAT_EQ(parsed->min_score, 0.625f);

  const double float_limit = std::numeric_limits<float>::max();
  for (double invalid : {float_limit * 2.0, -float_limit * 2.0,
                         std::numeric_limits<double>::infinity(),
                         -std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()}) {
    SCOPED_TRACE(invalid);
    diagnostic.clear();
    EXPECT_FALSE(
        schema.Parse({{"min_score", invalid}}, &diagnostic).has_value());
    EXPECT_FALSE(diagnostic.empty());
  }
}

TEST(ParameterBindingTest, RejectsNullValues) {
  auto schema = Parameters<SampleParams>({
      Field("mode", &SampleParams::mode).Default("fast"),
      Field("count", &SampleParams::count).Default(10),
  });

  std::string err;
  auto res = schema.Parse({{"mode", nullptr}}, &err);
  EXPECT_FALSE(res.has_value());
}

TEST(ParameterBindingTest, RejectsUnknownFields) {
  auto schema = Parameters<SampleParams>({
      Field("mode", &SampleParams::mode).Default("fast"),
  });

  std::string err;
  auto res = schema.Parse({{"mode", "fast"}, {"unknown_field", 123}}, &err);
  EXPECT_FALSE(res.has_value());
  EXPECT_NE(err.find("unknown_field"), std::string::npos);
}

TEST(ParameterBindingTest, RejectsRequiredFieldMissing) {
  auto schema = Parameters<SampleParams>({
      Field("mode", &SampleParams::mode).Required(),
  });

  std::string err;
  auto res = schema.Parse(nlohmann::json::object(), &err);
  EXPECT_FALSE(res.has_value());
}

TEST(ParameterBindingTest, RejectsIntegerOutOfRange) {
  auto schema = Parameters<SampleParams>({
      Field("count", &SampleParams::count).Default(10).Range(1, 100),
  });

  std::string err;
  auto res = schema.Parse({{"count", 0}}, &err);
  EXPECT_FALSE(res.has_value());

  res = schema.Parse({{"count", 101}}, &err);
  EXPECT_FALSE(res.has_value());
}

TEST(ParameterBindingTest, RejectsIntegerOverflowFor32Bit) {
  auto schema = Parameters<SampleParams>({
      Field("count", &SampleParams::count).Default(10),
  });

  std::string err;
  // Int64 值超出 32 位 int 范围
  int64_t overflow_val =
      static_cast<int64_t>(std::numeric_limits<int>::max()) + 100LL;
  auto res = schema.Parse({{"count", overflow_val}}, &err);
  EXPECT_FALSE(res.has_value());
}

struct OptionalParams {
  std::optional<int64_t> length;
  std::optional<std::string> mode;
  bool flag = false;
};

Parameters<OptionalParams> OptionalSpec() {
  return Parameters<OptionalParams>({
      Field("length", &OptionalParams::length)
          .Range(2, 4096)
          .Description("optional length"),
      Field("mode", &OptionalParams::mode).Enum({"a", "b"}),
      Field("flag", &OptionalParams::flag).Default(false),
  });
}

TEST(ParameterBindingTest, OptionalFieldIsEmptyWhenOmitted) {
  const auto schema = OptionalSpec();
  std::string err;
  auto parsed = schema.Parse(nlohmann::json::object(), &err);
  ASSERT_TRUE(parsed.has_value()) << err;
  EXPECT_FALSE(parsed->length.has_value());
  EXPECT_FALSE(parsed->mode.has_value());
  EXPECT_FALSE(parsed->flag);
}

TEST(ParameterBindingTest, OptionalFieldFollowsTypeRangeAndEnumOfItsValue) {
  const auto schema = OptionalSpec();
  std::string err;
  auto parsed = schema.Parse({{"length", 128}, {"mode", "b"}}, &err);
  ASSERT_TRUE(parsed.has_value()) << err;
  ASSERT_TRUE(parsed->length.has_value());
  EXPECT_EQ(*parsed->length, 128);
  ASSERT_TRUE(parsed->mode.has_value());
  EXPECT_EQ(*parsed->mode, "b");

  EXPECT_FALSE(schema.Parse({{"length", 1}}, &err).has_value());
  EXPECT_FALSE(schema.Parse({{"length", 4097}}, &err).has_value());
  EXPECT_FALSE(schema.Parse({{"length", "128"}}, &err).has_value());
  EXPECT_FALSE(schema.Parse({{"length", 2.5}}, &err).has_value());
  EXPECT_FALSE(schema.Parse({{"mode", "c"}}, &err).has_value());
  EXPECT_FALSE(schema.Parse({{"mode", nullptr}}, &err).has_value());
}

TEST(ParameterBindingTest, OptionalFieldCannotDeclareRequiredOrDefault) {
  EXPECT_THROW((Parameters<OptionalParams>{
                   Field("length", &OptionalParams::length).Required()}),
               std::invalid_argument);
  EXPECT_THROW(
      (Parameters<OptionalParams>{Field("length", &OptionalParams::length)
                                      .Default(std::optional<int64_t>(8))}),
      std::invalid_argument);
}

TEST(ParameterBindingTest, OptionalFieldIsNotRequiredAndHasNoDefaultInSchema) {
  const auto schema = OptionalSpec();
  ASSERT_EQ(schema.Fields().size(), 3u);
  const auto& length = schema.Fields()[0];
  EXPECT_EQ(length.name, "length");
  EXPECT_EQ(length.kind, ConfigValueKind::kInteger);
  EXPECT_FALSE(length.required);
  EXPECT_TRUE(length.default_value.is_null());
  EXPECT_EQ(length.minimum, 2.0);
  EXPECT_EQ(length.maximum, 4096.0);
  const auto& mode = schema.Fields()[1];
  EXPECT_EQ(mode.kind, ConfigValueKind::kString);
  EXPECT_FALSE(mode.required);
  EXPECT_TRUE(mode.default_value.is_null());
  EXPECT_EQ(mode.enum_values, (std::vector<std::string>{"a", "b"}));

  const auto exported = ConfigFieldToJson(length);
  EXPECT_FALSE(exported.at("required").get<bool>());
  EXPECT_FALSE(exported.contains("default"));
}

TEST(ParameterBindingTest, RejectsInvalidDefaultAtConstruction) {
  // 超出范围的默认值在构造时抛异常
  EXPECT_THROW(
      {
        auto schema = Parameters<SampleParams>({
            Field("count", &SampleParams::count).Default(200).Range(1, 100),
        });
      },
      std::invalid_argument);
}

TEST(ParameterBindingTest, RejectsDuplicateFieldName) {
  EXPECT_THROW(
      {
        auto schema = Parameters<SampleParams>({
            Field("count", &SampleParams::count).Default(10),
            Field("count", &SampleParams::count).Default(20),
        });
      },
      std::invalid_argument);
}

TEST(ParameterBindingTest, RejectsDuplicateMemberBinding) {
  EXPECT_THROW(
      {
        auto schema = Parameters<SampleParams>({
            Field("count_a", &SampleParams::count).Default(10),
            Field("count_b", &SampleParams::count).Default(20),
        });
      },
      std::invalid_argument);
}

TEST(ParameterBindingTest, SemanticValidatorAndBindingsHook) {
  auto schema = Parameters<SampleParams>({
      Field("mode", &SampleParams::mode).Default("custom"),
      Field("count", &SampleParams::count).Default(5),
  });

  schema.Validate([](const SampleParams& p, std::string* err) {
    if (p.mode == "custom" && p.count < 10) {
      if (err) *err = "custom mode requires count >= 10";
      return false;
    }
    return true;
  });

  schema.ValidateBindings([](const SampleParams& p,
                             const std::unordered_set<std::string>& conn,
                             std::string* err) {
    if (p.mode == "custom" && conn.find("context") == conn.end()) {
      if (err) *err = "custom mode requires connected context input";
      return false;
    }
    return true;
  });

  std::string err;
  // 语义校验失败
  auto res = schema.Parse({{"mode", "custom"}, {"count", 5}}, &err);
  EXPECT_FALSE(res.has_value());
  EXPECT_EQ(err, "custom mode requires count >= 10");

  // 语义校验通过
  res = schema.Parse({{"mode", "custom"}, {"count", 15}}, &err);
  EXPECT_TRUE(res.has_value());

  // 绑定校验
  nlohmann::json norm = {{"mode", "custom"}, {"count", 15}};
  bool bind_ok = schema.ValidateWithBindings(norm, {"other_input"}, &err);
  EXPECT_FALSE(bind_ok);
  EXPECT_EQ(err, "custom mode requires connected context input");

  bind_ok = schema.ValidateWithBindings(norm, {"context"}, &err);
  EXPECT_TRUE(bind_ok);
}

TEST(ParameterBindingTest, RejectsUint64MaxForIntAndInt64) {
  auto schema = Parameters<SampleParams>({
      Field("count", &SampleParams::count).Default(10),
      Field("big_id", &SampleParams::big_id)
          .Default(static_cast<int64_t>(1000)),
  });

  std::string err;
  uint64_t uint_max = std::numeric_limits<uint64_t>::max();
  auto res_int = schema.Parse({{"count", uint_max}}, &err);
  EXPECT_FALSE(res_int.has_value());
  EXPECT_NE(err.find("range"), std::string::npos);

  err.clear();
  auto res_big = schema.Parse({{"big_id", uint_max}}, &err);
  EXPECT_FALSE(res_big.has_value());
  EXPECT_NE(err.find("range"), std::string::npos);
}

TEST(ParameterBindingTest, BindingValidatorExceptionDoesNotCrash) {
  auto schema = Parameters<SampleParams>({
      Field("mode", &SampleParams::mode).Default("custom"),
  });

  schema.ValidateBindings([](const SampleParams&,
                             const std::unordered_set<std::string>&,
                             std::string*) -> bool {
    throw std::runtime_error("Unexpected failure in validator callback");
  });

  std::string err;
  nlohmann::json norm = {{"mode", "custom"}};
  bool bind_ok = schema.ValidateWithBindings(norm, {"context"}, &err);
  EXPECT_FALSE(bind_ok);
  EXPECT_NE(err.find("Unexpected failure in validator callback"),
            std::string::npos);
}

TEST(ParameterBindingTest,
     ValidateWithBindingsEnforcesConstraintsEvenWhenConnectedInputsIsEmpty) {
  auto schema = Parameters<SampleParams>({
      Field("mode", &SampleParams::mode).Default("custom"),
  });

  schema.ValidateBindings([](const SampleParams& p,
                             const std::unordered_set<std::string>& conn,
                             std::string* err) -> bool {
    if (p.mode == "custom" && conn.count("context") == 0) {
      if (err) *err = "custom mode requires context port";
      return false;
    }
    return true;
  });

  std::string err;
  nlohmann::json norm = {{"mode", "custom"}};
  // 已连接输入为空集 {} 时仍须执行绑定校验！
  bool bind_ok = schema.ValidateWithBindings(norm, {}, &err);
  EXPECT_FALSE(bind_ok);
  EXPECT_NE(err.find("custom mode requires context port"), std::string::npos);
}

// ---------------------------------------------------------------------------
// 元素类型：数组、映射、结构体元素、JSON 值
// ---------------------------------------------------------------------------

struct Rule {
  std::string pattern;
  int weight = 1;
  std::vector<std::string> tags;
};

struct ElementParams {
  std::vector<std::string> names;
  std::vector<int> scores;
  std::map<std::string, std::vector<std::string>> categories;
  std::vector<Rule> rules;
  std::map<std::string, Rule> endpoints;
  nlohmann::json constants;
  std::optional<std::vector<std::string>> hints;
};

Parameters<Rule> RuleParameters() {
  return Parameters<Rule>(
             {
                 Field("pattern", &Rule::pattern).Required(),
                 Field("weight", &Rule::weight).Default(1).Range(1, 10),
                 Field("tags", &Rule::tags).Default(std::vector<std::string>{}),
             })
      .Prepare([](Rule* rule, std::string* error) {
        if (rule->pattern == "bad") {
          if (error) *error = "pattern is rejected";
          return false;
        }
        return true;
      })
      .Validate([](const Rule& rule, std::string* error) {
        if (rule.pattern.empty()) {
          if (error) *error = "pattern is empty";
          return false;
        }
        return true;
      });
}

Parameters<ElementParams> MakeElementParameters() {
  return Parameters<ElementParams>({
      Field("names", &ElementParams::names)
          .Default(std::vector<std::string>{"a"})
          .Enum({"a", "b", "c"}),
      Field("scores", &ElementParams::scores)
          .Default(std::vector<int>{})
          .Range(0, 100),
      Field("categories", &ElementParams::categories)
          .Default(std::map<std::string, std::vector<std::string>>{})
          .Enum({"x", "y"}),
      Field("rules", &ElementParams::rules)
          .Default(std::vector<Rule>{})
          .Items(RuleParameters()),
      Field("endpoints", &ElementParams::endpoints)
          .Required()
          .Items(RuleParameters()),
      Field("constants", &ElementParams::constants)
          .Default(nlohmann::json::object()),
      Field("hints", &ElementParams::hints),
  });
}

const ConfigFieldDefinition* FindField(
    const std::vector<ConfigFieldDefinition>& fields, const std::string& name) {
  for (const auto& field : fields) {
    if (field.name == name) return &field;
  }
  return nullptr;
}

std::vector<ConfigFieldValidationError> ValidationErrors(
    const Parameters<ElementParams>& params, const nlohmann::json& config) {
  std::vector<ConfigFieldValidationError> errors;
  nlohmann::json normalized;
  EXPECT_FALSE(ValidateAndNormalizeFields(params.Fields(), config, &normalized,
                                          &errors));
  return errors;
}

TEST(ParameterBindingTest, ElementTypesAreExportedAsItemsAndFields) {
  const auto params = MakeElementParameters();
  const auto& fields = params.Fields();
  const auto* names = FindField(fields, "names");
  ASSERT_NE(names, nullptr);
  EXPECT_EQ(names->kind, ConfigValueKind::kArray);
  ASSERT_NE(names->items, nullptr);
  EXPECT_EQ(names->items->kind, ConfigValueKind::kString);
  EXPECT_EQ(names->items->enum_values,
            (std::vector<std::string>{"a", "b", "c"}));
  EXPECT_TRUE(names->enum_values.empty());  // 约束落在元素上

  const auto* scores = FindField(fields, "scores");
  ASSERT_NE(scores, nullptr);
  EXPECT_EQ(scores->items->kind, ConfigValueKind::kInteger);
  EXPECT_EQ(scores->items->minimum, 0.0);
  EXPECT_EQ(scores->items->maximum, 100.0);

  const auto* categories = FindField(fields, "categories");
  ASSERT_NE(categories, nullptr);
  EXPECT_EQ(categories->kind, ConfigValueKind::kMap);
  EXPECT_EQ(categories->items->kind, ConfigValueKind::kArray);
  EXPECT_EQ(categories->items->items->kind, ConfigValueKind::kString);
  EXPECT_EQ(categories->items->items->enum_values,
            (std::vector<std::string>{"x", "y"}));

  const auto* rules = FindField(fields, "rules");
  ASSERT_NE(rules, nullptr);
  ASSERT_NE(rules->items, nullptr);
  EXPECT_EQ(rules->items->kind, ConfigValueKind::kObject);
  ASSERT_EQ(rules->items->fields.size(), 3u);
  EXPECT_EQ(rules->items->fields[0].name, "pattern");
  EXPECT_TRUE(rules->items->fields[0].required);
  EXPECT_EQ(rules->items->fields[1].default_value, 1);

  const auto* constants = FindField(fields, "constants");
  ASSERT_NE(constants, nullptr);
  EXPECT_EQ(constants->kind, ConfigValueKind::kJson);

  // Catalog 的 config_fields 由 ConfigFieldToJson 导出
  const auto exported = ConfigFieldToJson(*rules);
  EXPECT_EQ(exported.at("type"), "array");
  EXPECT_EQ(exported.at("items").at("type"), "object");
  EXPECT_EQ(exported.at("items").at("fields").size(), 3u);
  EXPECT_EQ(ConfigFieldToJson(*categories).at("type"), "map");
  EXPECT_EQ(ConfigFieldToJson(*constants).at("type"), "json");

  std::string error;
  EXPECT_TRUE(ValidateConfigFieldDefinitions(fields, &error)) << error;
}

TEST(ParameterBindingTest, ElementsGetDefaultsAndMapsAreOrderedByKey) {
  const auto params = MakeElementParameters();
  std::string error;
  auto parsed = params.Parse({{"endpoints",
                               {{"time", {{"pattern", "t"}}},
                                {"date", {{"pattern", "d"}, {"weight", 5}}}}},
                              {"rules", {{{"pattern", "r"}}}},
                              {"categories", {{"k", {"x"}}}}},
                             &error);
  ASSERT_TRUE(parsed.has_value()) << error;
  ASSERT_EQ(parsed->endpoints.size(), 2u);
  EXPECT_EQ(parsed->endpoints.begin()->first,
            "date");  // 字典序，不保留书写顺序
  EXPECT_EQ(parsed->endpoints.at("time").weight, 1);  // 元素默认值
  ASSERT_EQ(parsed->rules.size(), 1u);
  EXPECT_EQ(parsed->rules[0].weight, 1);
  EXPECT_TRUE(parsed->rules[0].tags.empty());
  EXPECT_EQ(parsed->categories.at("k"), std::vector<std::string>{"x"});
  EXPECT_EQ(parsed->names, std::vector<std::string>{"a"});
  EXPECT_FALSE(parsed->hints.has_value());
  EXPECT_TRUE(parsed->constants.is_object());
}

TEST(ParameterBindingTest, ElementDiagnosticsCarryKeyOrIndexPath) {
  const auto params = MakeElementParameters();
  const nlohmann::json endpoints = {{"time", {{"pattern", "t"}}}};
  struct Case {
    nlohmann::json config;
    std::string path;
    ConfigFieldErrorKind kind;
  };
  const std::vector<Case> cases = {
      // 元素类型错误
      {{{"endpoints", endpoints}, {"names", {"a", 7}}},
       "names/1",
       ConfigFieldErrorKind::kTypeMismatch},
      {{{"endpoints", {{"time", 3}}}},
       "endpoints/time",
       ConfigFieldErrorKind::kTypeMismatch},
      {{{"endpoints", endpoints},
        {"rules", {{{"pattern", "ok"}}, {{"pattern", 5}}}}},
       "rules/1/pattern",
       ConfigFieldErrorKind::kTypeMismatch},
      // 缺必填键、未知键
      {{{"endpoints", {{"time", nlohmann::json::object()}}}},
       "endpoints/time/pattern",
       ConfigFieldErrorKind::kMissingField},
      {{{"endpoints", {{"time", {{"pattern", "t"}, {"patern", "x"}}}}}},
       "endpoints/time/patern",
       ConfigFieldErrorKind::kUnknownField},
      // 范围、枚举作用于元素
      {{{"endpoints", endpoints}, {"scores", {1, 2, 101}}},
       "scores/2",
       ConfigFieldErrorKind::kOutOfRange},
      {{{"endpoints", {{"time", {{"pattern", "t"}, {"weight", 11}}}}}},
       "endpoints/time/weight",
       ConfigFieldErrorKind::kOutOfRange},
      {{{"endpoints", endpoints}, {"names", {"a", "z"}}},
       "names/1",
       ConfigFieldErrorKind::kInvalidEnum},
      // 嵌套：映射的值为数组
      {{{"endpoints", endpoints}, {"categories", {{"k", {"x", "q"}}}}},
       "categories/k/1",
       ConfigFieldErrorKind::kInvalidEnum},
      {{{"endpoints", endpoints}, {"categories", {{"k", "x"}}}},
       "categories/k",
       ConfigFieldErrorKind::kTypeMismatch},
  };
  for (const auto& item : cases) {
    SCOPED_TRACE(item.config.dump());
    const auto errors = ValidationErrors(params, item.config);
    ASSERT_FALSE(errors.empty());
    EXPECT_EQ(errors.front().path, item.path);
    EXPECT_EQ(errors.front().kind, item.kind);
  }
  // 未知键给出该层声明的参数名，用于相近名字建议
  const auto unknown = ValidationErrors(
      params, {{"endpoints", {{"time", {{"pattern", "t"}, {"patern", "x"}}}}}});
  EXPECT_EQ(unknown.front().candidates,
            (std::vector<std::string>{"pattern", "weight", "tags"}));
  // 键名含 '/' 时按 JSON Pointer 转义
  const auto escaped = ValidationErrors(params, {{"endpoints", {{"a/b", 3}}}});
  EXPECT_EQ(escaped.front().path, "endpoints/a~1b");
}

TEST(ParameterBindingTest, ElementPrepareAndValidateFailuresNameKeyOrIndex) {
  const auto params = MakeElementParameters();
  std::string error;
  EXPECT_FALSE(
      params
          .Parse({{"endpoints", {{"time", {{"pattern", "t"}}}}},
                  {"rules",
                   {{{"pattern", "ok"}, {"weight", 2}}, {{"pattern", "bad"}}}}},
                 &error)
          .has_value());
  EXPECT_NE(error.find("rules"), std::string::npos) << error;
  EXPECT_NE(error.find("element 1"), std::string::npos) << error;
  EXPECT_NE(error.find("pattern is rejected"), std::string::npos) << error;

  EXPECT_FALSE(
      params.Parse({{"endpoints", {{"time", {{"pattern", ""}}}}}}, &error)
          .has_value());
  EXPECT_NE(error.find("endpoints"), std::string::npos) << error;
  EXPECT_NE(error.find("key 'time'"), std::string::npos) << error;
  EXPECT_NE(error.find("pattern is empty"), std::string::npos) << error;
}

TEST(ParameterBindingTest, JsonValueAcceptsAnyValueExceptNull) {
  const auto params = MakeElementParameters();
  const nlohmann::json endpoints = {{"time", {{"pattern", "t"}}}};
  for (const auto& value :
       std::vector<nlohmann::json>{{{"a", 1}}, {1, 2}, "text", 3, 2.5, true}) {
    SCOPED_TRACE(value.dump());
    std::string error;
    auto parsed =
        params.Parse({{"endpoints", endpoints}, {"constants", value}}, &error);
    ASSERT_TRUE(parsed.has_value()) << error;
    EXPECT_EQ(parsed->constants, value);
  }
  const auto errors = ValidationErrors(
      params, {{"endpoints", endpoints}, {"constants", nullptr}});
  EXPECT_EQ(errors.front().path, "constants");
  EXPECT_EQ(errors.front().kind, ConfigFieldErrorKind::kTypeMismatch);
}

TEST(ParameterBindingTest, InvalidElementDeclarationsAreRejected) {
  // 元素默认值须满足元素的约束
  ConfigFieldDefinition field;
  field.name = "names";
  field.kind = ConfigValueKind::kArray;
  field.default_value = nlohmann::json::array({"z"});
  auto element = std::make_shared<ConfigFieldDefinition>();
  element->kind = ConfigValueKind::kString;
  element->enum_values = {"a", "b"};
  field.items = element;
  std::string error;
  EXPECT_FALSE(ValidateConfigFieldDefinitions({field}, &error));
  EXPECT_NE(error.find("names"), std::string::npos) << error;

  // 元素定义只能用于数组、映射；fields 只能用于结构体元素
  ConfigFieldDefinition scalar;
  scalar.name = "count";
  scalar.kind = ConfigValueKind::kInteger;
  scalar.items = element;
  EXPECT_FALSE(ValidateConfigFieldDefinitions({scalar}, &error));
  scalar.items = nullptr;
  scalar.fields = {MakeConfigField("x", ConfigValueKind::kString)};
  EXPECT_FALSE(ValidateConfigFieldDefinitions({scalar}, &error));

  // 结构体元素的字段不能重名
  ConfigFieldDefinition object;
  object.name = "rules";
  object.kind = ConfigValueKind::kArray;
  auto struct_element = std::make_shared<ConfigFieldDefinition>();
  struct_element->kind = ConfigValueKind::kObject;
  struct_element->fields = {MakeConfigField("x", ConfigValueKind::kString),
                            MakeConfigField("x", ConfigValueKind::kString)};
  object.items = struct_element;
  EXPECT_FALSE(ValidateConfigFieldDefinitions({object}, &error));

  // Range 不能用在 JSON 值上
  EXPECT_FALSE(ValidateConfigFieldDefinitions(
      Parameters<ElementParams>({Field("constants", &ElementParams::constants)
                                     .Default(nlohmann::json::object())
                                     .Range(0, 1)})
          .Fields(),
      &error));
}

TEST(ParameterBindingTest, FieldJsonSchemaIsGeneratedFromDeclarations) {
  const auto params = MakeElementParameters();
  const auto& fields = params.Fields();
  const auto rules = ConfigFieldJsonSchema(*FindField(fields, "rules"));
  EXPECT_EQ(rules.at("type"), "array");
  EXPECT_EQ(rules.at("items").at("additionalProperties"), false);
  EXPECT_EQ(rules.at("items").at("required"),
            nlohmann::json::array({"pattern"}));
  EXPECT_EQ(rules.at("items").at("properties").at("weight").at("maximum"),
            10.0);

  const auto endpoints = ConfigFieldJsonSchema(*FindField(fields, "endpoints"));
  EXPECT_EQ(endpoints.at("type"), "object");
  EXPECT_EQ(endpoints.at("additionalProperties").at("type"), "object");

  const auto names = ConfigFieldJsonSchema(*FindField(fields, "names"));
  EXPECT_EQ(names.at("items").at("enum"),
            nlohmann::json::array({"a", "b", "c"}));
  EXPECT_FALSE(
      ConfigFieldJsonSchema(*FindField(fields, "constants")).contains("type"));

  // 生成的 schema 可用于校验 payload
  std::string error;
  EXPECT_TRUE(ValidateControlPayload({{{"pattern", "p"}, {"weight", 2}}}, rules,
                                     &error))
      << error;
  EXPECT_FALSE(
      ValidateControlPayload({{{"pattern", "p"}, {"weight", 99}}}, rules));
  EXPECT_FALSE(ValidateControlPayload({{{"weight", 2}}}, rules));
  EXPECT_FALSE(
      ValidateControlPayload({{{"pattern", "p"}, {"extra", 1}}}, rules));
}

// ---------------------------------------------------------------------------
// Include：共享参数组
// ---------------------------------------------------------------------------

struct Shared {
  int limit = 5;
  std::string label;
};

struct Composite {
  std::string own;
  Shared shared;
  std::vector<std::string> events;
};

Parameters<Shared> SharedParameters(
    std::vector<std::string>* events = nullptr) {
  auto params = Parameters<Shared>({
      Field("limit", &Shared::limit).Default(5).Range(1, 50),
      Field("label", &Shared::label).Default("shared"),
  });
  params.Validate([](const Shared& shared, std::string* error) {
    if (shared.label == "forbidden") {
      if (error) *error = "label is forbidden";
      return false;
    }
    return true;
  });
  (void)events;
  return params;
}

TEST(ParameterBindingTest, IncludeFlattensGroupFieldsIntoOwnFields) {
  auto params =
      Parameters<Composite>({Field("own", &Composite::own).Default("o")});
  params.Include(&Composite::shared, SharedParameters());
  std::vector<std::string> names;
  for (const auto& field : params.Fields()) names.push_back(field.name);
  EXPECT_EQ(names, (std::vector<std::string>{"own", "limit", "label"}));

  std::string error;
  auto defaults = params.Parse(nlohmann::json::object(), &error);
  ASSERT_TRUE(defaults.has_value()) << error;
  EXPECT_EQ(defaults->own, "o");
  EXPECT_EQ(defaults->shared.limit, 5);
  EXPECT_EQ(defaults->shared.label, "shared");

  auto configured = params.Parse({{"own", "x"}, {"limit", 9}}, &error);
  ASSERT_TRUE(configured.has_value()) << error;
  EXPECT_EQ(configured->shared.limit, 9);

  // 被并入字段的范围同样生效；未知字段照常报错
  EXPECT_FALSE(params.Parse({{"limit", 51}}, &error).has_value());
  EXPECT_FALSE(params.Parse({{"nested", 1}}, &error).has_value());
}

TEST(ParameterBindingTest, IncludeRejectsDuplicateNamesWhenConstructed) {
  auto params =
      Parameters<Composite>({Field("limit", &Composite::own).Default("o")});
  EXPECT_THROW(params.Include(&Composite::shared, SharedParameters()),
               std::invalid_argument);
}

TEST(ParameterBindingTest, IncludedGroupPreparesAndValidatesBeforeOwnGroup) {
  std::vector<std::string> order;
  auto shared = SharedParameters();
  shared.Prepare([&](Shared*, std::string*) {
    order.push_back("shared");
    return true;
  });
  auto params =
      Parameters<Composite>({Field("own", &Composite::own).Default("o")});
  params.Include(&Composite::shared, shared);
  params.Prepare([&](Composite*, std::string*) {
    order.push_back("own");
    return true;
  });
  std::string error;
  ASSERT_TRUE(params.Parse(nlohmann::json::object(), &error).has_value())
      << error;
  EXPECT_EQ(order, (std::vector<std::string>{"shared", "own"}));

  // 被并入组的 Validate 失败时，整体解析失败并带出原因，本组不再执行
  order.clear();
  EXPECT_FALSE(params.Parse({{"label", "forbidden"}}, &error).has_value());
  EXPECT_NE(error.find("label is forbidden"), std::string::npos) << error;
  EXPECT_EQ(order, std::vector<std::string>{"shared"});
}

TEST(ParameterBindingTest, IncludedFieldsCanBeAssignedIndividually) {
  auto params =
      Parameters<Composite>({Field("own", &Composite::own).Default("o")});
  params.Include(&Composite::shared, SharedParameters());
  Composite value;
  std::string error;
  ASSERT_TRUE(params.AssignField("limit", 7, &value, &error)) << error;
  EXPECT_EQ(value.shared.limit, 7);
  EXPECT_FALSE(params.AssignField("limit", 0, &value, &error));
}

}  // namespace
}  // namespace llm_edgeflow
