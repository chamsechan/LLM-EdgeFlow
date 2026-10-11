#include <gtest/gtest.h>

#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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
  EXPECT_EQ(raw_values->Effective(),
            (nlohmann::json{{"mode", "slow"}, {"count", 20}}));
  EXPECT_EQ(normalized_values->Effective(), raw_values->Effective());
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
  EXPECT_EQ(values->Effective(), nlohmann::json::object());
  EXPECT_FALSE(values->Integer("missing").has_value());
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

TEST(ParameterBindingTest, EffectiveValuesOmitUnsetOptionalFields) {
  const ParameterSet set(OptionalParamSpec());
  std::shared_ptr<const ParameterValues> values;
  std::string diagnostic;
  ASSERT_TRUE(set.Parse(nlohmann::json::object(), &values, &diagnostic))
      << diagnostic;
  EXPECT_EQ(values->Effective(), nlohmann::json::object());
  EXPECT_FALSE(values->Integer("count").has_value());

  const nlohmann::json config = {
      {"count", 4}, {"enabled", false}, {"mode", "slow"}, {"ratio", 0.75}};
  ASSERT_TRUE(set.Parse(config, &values, &diagnostic)) << diagnostic;
  EXPECT_EQ(values->Effective(), config);
  EXPECT_EQ(values->Integer("count"), 4);
  EXPECT_FALSE(values->Effective().contains("big_id"));
  EXPECT_FALSE(values->Effective().contains("score"));
}

TEST(ParameterBindingTest, EffectiveValuesReflectPrepareAndKeepMoveOnlyState) {
  struct Params {
    int count = 0;
    std::unique_ptr<int> derived;
    std::string prepared;
  };
  int prepare_calls = 0;
  ParameterSet set(
      Parameters<Params>{Field("count", &Params::count).Default(3)}.Prepare(
          [&prepare_calls](Params* params, std::string*) {
            ++prepare_calls;
            params->count *= 2;
            params->derived = std::make_unique<int>(params->count);
            params->prepared = "compiled";
            return true;
          }));
  std::shared_ptr<const ParameterValues> values;
  std::string diagnostic;
  ASSERT_TRUE(set.Parse(nlohmann::json::object(), &values, &diagnostic))
      << diagnostic;
  EXPECT_EQ(prepare_calls, 1);
  EXPECT_EQ(values->Effective(), (nlohmann::json{{"count", 6}}));
  EXPECT_EQ(values->Integer("count"), 6);
  const auto& params = values->Get<Params>();
  EXPECT_EQ(params.count, 6);
  ASSERT_NE(params.derived, nullptr);
  EXPECT_EQ(*params.derived, 6);
  EXPECT_EQ(params.prepared, "compiled");
  EXPECT_EQ(prepare_calls, 1);
}

TEST(ParameterBindingTest, IntegerReadsOnlyRepresentableIntegerJsonValues) {
  struct Params {
    nlohmann::json value;
  };
  const ParameterSet set(
      Parameters<Params>{Field("value", &Params::value).Required()});
  const std::vector<std::pair<nlohmann::json, std::optional<int64_t>>> cases = {
      {std::numeric_limits<int64_t>::min(),
       std::numeric_limits<int64_t>::min()},
      {std::numeric_limits<int64_t>::max(),
       std::numeric_limits<int64_t>::max()},
      {uint64_t{7}, int64_t{7}},
      {static_cast<uint64_t>(std::numeric_limits<int64_t>::max()),
       std::numeric_limits<int64_t>::max()},
      {static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1,
       std::nullopt},
      {std::numeric_limits<uint64_t>::max(), std::nullopt},
      {7.0, std::nullopt},
      {"7", std::nullopt},
      {true, std::nullopt},
      {nlohmann::json::array(), std::nullopt},
      {nlohmann::json::object(), std::nullopt}};
  for (const auto& [input, expected] : cases) {
    SCOPED_TRACE(input.dump());
    std::shared_ptr<const ParameterValues> values;
    std::string diagnostic;
    ASSERT_TRUE(set.Parse({{"value", input}}, &values, &diagnostic))
        << diagnostic;
    EXPECT_EQ(values->Integer("value"), expected);
    EXPECT_FALSE(values->Integer("missing").has_value());
  }
}

TEST(ParameterBindingTest, IncludeFlattensFieldsAndRunsInnerHooksBeforeOuter) {
  struct Inner {
    std::string prefix;
    std::string prepared;
  };
  struct Outer {
    Inner inner;
    int count = 0;
    std::string prepared;
  };
  std::vector<std::string> events;
  auto inner =
      Parameters<Inner>{Field("prefix", &Inner::prefix).Default("default:")}
          .Prepare([&events](Inner* value, std::string*) {
            events.push_back("inner prepare");
            value->prepared = value->prefix;
            return true;
          })
          .Validate([&events](const Inner& value, std::string* error) {
            events.push_back("inner validate");
            if (value.prefix == "reject") {
              if (error) *error = "inner rejected";
              return false;
            }
            return value.prepared == value.prefix;
          });
  auto outer = Parameters<Outer>{Field("count", &Outer::count).Default(2)}
                   .Include(&Outer::inner, inner)
                   .Prepare([&events](Outer* value, std::string*) {
                     events.push_back("outer prepare");
                     value->prepared = value->inner.prepared;
                     return true;
                   })
                   .Validate([&events](const Outer& value, std::string*) {
                     events.push_back("outer validate");
                     return value.prepared == value.inner.prefix;
                   });
  auto copy = outer;
  std::string diagnostic;
  const auto parsed = copy.Parse({{"prefix", "provided:"}}, &diagnostic);
  ASSERT_TRUE(parsed.has_value()) << diagnostic;
  EXPECT_EQ(parsed->inner.prefix, "provided:");
  EXPECT_EQ(parsed->count, 2);
  EXPECT_EQ(parsed->prepared, "provided:");
  EXPECT_EQ(events,
            (std::vector<std::string>{"inner prepare", "inner validate",
                                      "outer prepare", "outer validate"}));
  ASSERT_EQ(copy.Fields().size(), 2U);
  EXPECT_EQ(copy.Fields()[1].name, "prefix");
  EXPECT_EQ(copy.Fields()[1].default_value, "default:");
  events.clear();
  EXPECT_FALSE(copy.Parse({{"prefix", "reject"}}, &diagnostic).has_value());
  EXPECT_EQ(diagnostic, "inner rejected");
  EXPECT_EQ(events,
            (std::vector<std::string>{"inner prepare", "inner validate"}));
  EXPECT_FALSE(copy.Parse({{"inner", {{"prefix", "nested:"}}}}, &diagnostic)
                   .has_value());
  auto duplicate =
      Parameters<Outer>{Field("prefix", &Outer::prepared).Default("")};
  EXPECT_THROW(duplicate.Include(&Outer::inner, inner), std::invalid_argument);
  EXPECT_TRUE(
      duplicate.Parse(nlohmann::json::object(), &diagnostic).has_value())
      << diagnostic;
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
  bool bind_ok = schema.ValidateWithBindings(
      norm, BindingFacts{true, {"other_input"}, {}}, &err);
  EXPECT_FALSE(bind_ok);
  EXPECT_EQ(err, "custom mode requires connected context input");

  bind_ok = schema.ValidateWithBindings(
      norm, BindingFacts{true, {"context"}, {}}, &err);
  EXPECT_TRUE(bind_ok);
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
  bool bind_ok = schema.ValidateWithBindings(
      norm, BindingFacts{true, {"context"}, {}}, &err);
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
  bool bind_ok =
      schema.ValidateWithBindings(norm, BindingFacts{true, {}, {}}, &err);
  EXPECT_FALSE(bind_ok);
  EXPECT_NE(err.find("custom mode requires context port"), std::string::npos);
}

}  // namespace
}  // namespace llm_edgeflow
