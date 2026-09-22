#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <vector>

#include "contracts/config_schema_validation.h"
#include "nodes/node_config_parser.h"
#include "nodes/parameter_binding.h"

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

TEST(ParameterBindingTest, RejectsComplexParserFieldNameCollisions) {
  ConfigFieldDefinition field;
  field.name = "count";
  field.kind = ConfigValueKind::kInteger;
  field.required = true;
  NodeConfigParser<SampleParams> parser(
      {field},
      [](const nlohmann::json&, SampleParams*, std::string*) { return true; });
  auto schema =
      Parameters<SampleParams>{Field("count", &SampleParams::count).Default(1)};
  EXPECT_THROW(schema.WithParser(parser), std::invalid_argument);
  auto empty = Parameters<SampleParams>{};
  EXPECT_THROW(empty.WithParser(NodeConfigParser<SampleParams>(
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

  // Empty config -> defaults apply
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

  // Custom values
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
  // Int64 overflow for 32-bit int
  int64_t overflow_val =
      static_cast<int64_t>(std::numeric_limits<int>::max()) + 100LL;
  auto res = schema.Parse({{"count", overflow_val}}, &err);
  EXPECT_FALSE(res.has_value());
}

TEST(ParameterBindingTest, RejectsInvalidDefaultAtConstruction) {
  // Out-of-range default throws during construction
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
  // Semantic validation failure
  auto res = schema.Parse({{"mode", "custom"}, {"count", 5}}, &err);
  EXPECT_FALSE(res.has_value());
  EXPECT_EQ(err, "custom mode requires count >= 10");

  // Semantic validation pass
  res = schema.Parse({{"mode", "custom"}, {"count", 15}}, &err);
  EXPECT_TRUE(res.has_value());

  // Binding validation check
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
  // Connected inputs is empty set {} - must still enforce binding validation!
  bool bind_ok = schema.ValidateWithBindings(norm, {}, &err);
  EXPECT_FALSE(bind_ok);
  EXPECT_NE(err.find("custom mode requires context port"), std::string::npos);
}

}  // namespace
}  // namespace llm_edgeflow
