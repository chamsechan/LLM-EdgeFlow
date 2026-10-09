#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "contracts/config_schema_validation.h"
#include "contracts/parameter_set.h"
#include "contracts/parameters.h"

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

struct OptionalParams {
  std::optional<int> count = 99;
  std::optional<int64_t> big_id;
  std::optional<std::string> mode;
  std::optional<bool> enabled;
  std::optional<float> score;
  std::optional<double> ratio;
};

Parameters<OptionalParams> OptionalParamSpec() {
  return Parameters<OptionalParams>{
      Field("count", &OptionalParams::count).Range(1, 100),
      Field("big_id", &OptionalParams::big_id),
      Field("mode", &OptionalParams::mode).Enum({"fast", "slow"}),
      Field("enabled", &OptionalParams::enabled),
      Field("score", &OptionalParams::score).Range(0.0, 1.0),
      Field("ratio", &OptionalParams::ratio).Range(0.0, 1.0)};
}

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

TEST(ParameterBindingTest, OptionalScalarFieldsAreAbsentWithoutDefaults) {
  const auto schema = OptionalParamSpec();
  std::string diagnostic;
  const auto absent = schema.Parse(nlohmann::json::object(), &diagnostic);
  ASSERT_TRUE(absent.has_value()) << diagnostic;
  EXPECT_FALSE(absent->count.has_value());
  EXPECT_FALSE(absent->big_id.has_value());
  EXPECT_FALSE(absent->mode.has_value());
  EXPECT_FALSE(absent->enabled.has_value());
  EXPECT_FALSE(absent->score.has_value());
  EXPECT_FALSE(absent->ratio.has_value());
  ASSERT_EQ(schema.Fields().size(), 6U);
  for (const auto& field : schema.Fields()) {
    EXPECT_FALSE(field.required);
    EXPECT_TRUE(field.default_value.is_null());
    const auto catalog_field = ConfigFieldToJson(field);
    EXPECT_EQ(catalog_field["required"], false);
    EXPECT_FALSE(catalog_field.contains("default"));
  }

  const auto supplied = schema.Parse({{"count", 4},
                                      {"big_id", 99999999999LL},
                                      {"mode", "slow"},
                                      {"enabled", false},
                                      {"score", 0.25},
                                      {"ratio", 0.75}},
                                     &diagnostic);
  ASSERT_TRUE(supplied.has_value()) << diagnostic;
  EXPECT_EQ(supplied->count, 4);
  EXPECT_EQ(supplied->big_id, 99999999999LL);
  EXPECT_EQ(supplied->mode, "slow");
  ASSERT_TRUE(supplied->enabled.has_value());
  EXPECT_FALSE(*supplied->enabled);
  ASSERT_TRUE(supplied->score.has_value());
  EXPECT_FLOAT_EQ(*supplied->score, 0.25f);
  ASSERT_TRUE(supplied->ratio.has_value());
  EXPECT_DOUBLE_EQ(*supplied->ratio, 0.75);
}

TEST(ParameterBindingTest, OptionalFieldsRejectPresenceDeclarations) {
  EXPECT_THROW((Parameters<OptionalParams>{
                   Field("count", &OptionalParams::count).Required()}),
               std::invalid_argument);
  EXPECT_THROW((Parameters<OptionalParams>{
                   Field("count", &OptionalParams::count).Default(1)}),
               std::invalid_argument);
  EXPECT_THROW(
      (Parameters<OptionalParams>{
          Field("count", &OptionalParams::count).Required().Default(1)}),
      std::invalid_argument);
}

TEST(ParameterBindingTest, OptionalFieldsEnforceScalarTypesAndConstraints) {
  const auto schema = OptionalParamSpec();
  for (const auto& invalid : std::vector<nlohmann::json>{
           {{"count", 0}},
           {{"count", 101}},
           {{"count", "4"}},
           {{"count", nullptr}},
           {{"count", uint64_t{4294967298ULL}}},
           {{"big_id", std::numeric_limits<uint64_t>::max()}},
           {{"mode", "other"}},
           {{"enabled", 1}},
           {{"score", -0.01}},
           {{"score", std::numeric_limits<double>::max()}},
           {{"ratio", 1.01}}}) {
    SCOPED_TRACE(invalid.dump());
    std::string diagnostic;
    EXPECT_FALSE(schema.Parse(invalid, &diagnostic).has_value());
    EXPECT_FALSE(diagnostic.empty());
  }
}

TEST(ParameterBindingTest, ParameterSetParsesRawAndNormalizedValuesEqually) {
  auto schema = Parameters<SampleParams>{
      Field("mode", &SampleParams::mode).Default("fast"),
      Field("count", &SampleParams::count).Default(10).Range(1, 100)};
  schema.Prepare([](SampleParams* params, std::string*) {
    params->count *= 2;
    return true;
  });
  ParameterSet set(std::move(schema));
  ASSERT_EQ(set.Fields().size(), 2U);
  const nlohmann::json raw = {{"mode", "slow"}};
  nlohmann::json normalized;
  std::vector<ConfigFieldValidationError> errors;
  ASSERT_TRUE(
      ValidateAndNormalizeFields(set.Fields(), raw, &normalized, &errors));
  EXPECT_EQ(normalized, (nlohmann::json{{"mode", "slow"}, {"count", 10}}));
  std::string diagnostic = "stale";
  std::shared_ptr<const ParameterValues> raw_values, normalized_values;
  ASSERT_TRUE(set.Parse(raw, &raw_values, &diagnostic)) << diagnostic;
  EXPECT_TRUE(diagnostic.empty());
  ASSERT_TRUE(set.Parse(normalized, &normalized_values, &diagnostic))
      << diagnostic;
  EXPECT_EQ(raw_values->Get<SampleParams>().mode, "slow");
  EXPECT_EQ(raw_values->Get<SampleParams>().count, 20);
  EXPECT_EQ(normalized_values->Get<SampleParams>().mode,
            raw_values->Get<SampleParams>().mode);
  EXPECT_EQ(normalized_values->Get<SampleParams>().count,
            raw_values->Get<SampleParams>().count);
  EXPECT_THROW(raw_values->Get<OptionalParams>(), std::logic_error);

  EXPECT_FALSE(set.Parse({{"count", 0}}, &raw_values, &diagnostic));
  EXPECT_EQ(raw_values, nullptr);
  EXPECT_FALSE(diagnostic.empty());
  EXPECT_FALSE(set.Parse(raw, nullptr, &diagnostic));
  EXPECT_FALSE(diagnostic.empty());
}

TEST(ParameterBindingTest, DefaultParameterSetAcceptsOnlyEmptyObject) {
  const ParameterSet set;
  EXPECT_TRUE(set.Fields().empty());
  std::shared_ptr<const ParameterValues> values;
  std::string diagnostic;
  ASSERT_TRUE(set.Parse(nlohmann::json::object(), &values, &diagnostic));
  ASSERT_NE(values, nullptr);
  EXPECT_NO_THROW(values->Get<NoParameters>());
  for (const auto& invalid :
       std::vector<nlohmann::json>{{{"unknown_field", 1}},
                                   nlohmann::json::array(),
                                   nullptr,
                                   "text",
                                   4}) {
    SCOPED_TRACE(invalid.dump());
    EXPECT_FALSE(set.Parse(invalid, &values, &diagnostic));
    EXPECT_EQ(values, nullptr);
    EXPECT_FALSE(diagnostic.empty());
  }
}

TEST(ParameterBindingTest, RejectsComplexParserFieldNameCollisions) {
  ConfigFieldDefinition field;
  field.name = "count";
  field.kind = ConfigValueKind::kInteger;
  field.required = true;
  ConfigParser<SampleParams> parser(
      {field},
      [](const nlohmann::json&, SampleParams*, std::string*) { return true; });
  auto schema =
      Parameters<SampleParams>{Field("count", &SampleParams::count).Default(1)};
  EXPECT_THROW(schema.WithParser(parser), std::invalid_argument);
  auto empty = Parameters<SampleParams>{};
  EXPECT_THROW(empty.WithParser(ConfigParser<SampleParams>(
                   {field, field}, [](const nlohmann::json&, SampleParams*,
                                      std::string*) { return true; })),
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

}  // namespace
}  // namespace llm_edgeflow
