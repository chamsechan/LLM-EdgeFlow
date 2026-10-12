#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <optional>
#include <string>

#include "contracts/parameters.h"

namespace llm_edgeflow {
namespace {
using Json = nlohmann::json;

struct ScalarConfig {
  std::string name;
  std::string mode;
  int count{};
  int64_t id{};
  double ratio{};
  bool enabled{};
  std::optional<int> limit;
};

auto ScalarSpec() {
  return Parameters<ScalarConfig>{
      Field("name", &ScalarConfig::name).Required(),
      Field("mode", &ScalarConfig::mode).Default("fast").Enum({"fast", "slow"}),
      Field("count", &ScalarConfig::count).Default(2).Range(1, 4),
      Field("id", &ScalarConfig::id).Default(int64_t{0}),
      Field("ratio", &ScalarConfig::ratio).Default(0.25).Range(0, 1),
      Field("enabled", &ScalarConfig::enabled).Default(false),
      Field("limit", &ScalarConfig::limit).Range(1, 8)};
}

TEST(ScalarParametersTest, DefaultsAndExplicitValuesReachTypedState) {
  const auto spec = ScalarSpec();
  std::string diagnostic = "stale";
  auto defaults = spec.Parse({{"name", "worker"}}, &diagnostic);
  ASSERT_TRUE(defaults) << diagnostic;
  EXPECT_TRUE(diagnostic.empty());
  EXPECT_EQ(spec.Read(*defaults), (Json{{"name", "worker"},
                                        {"mode", "fast"},
                                        {"count", 2},
                                        {"id", 0},
                                        {"ratio", 0.25},
                                        {"enabled", false}}));
  EXPECT_FALSE(defaults->limit);

  const Json explicit_values = {
      {"name", ""}, {"mode", "slow"},  {"count", 4}, {"id", 9007199254740993LL},
      {"ratio", 1}, {"enabled", true}, {"limit", 8}};
  auto parsed = spec.Parse(explicit_values, &diagnostic);
  ASSERT_TRUE(parsed) << diagnostic;
  EXPECT_EQ(parsed->name, "");
  EXPECT_EQ(parsed->mode, "slow");
  EXPECT_EQ(parsed->count, 4);
  EXPECT_EQ(parsed->id, 9007199254740993LL);
  EXPECT_DOUBLE_EQ(parsed->ratio, 1);
  EXPECT_TRUE(parsed->enabled);
  EXPECT_EQ(parsed->limit, 8);
  EXPECT_EQ(spec.Read(*parsed), explicit_values);
  ASSERT_TRUE(
      spec.Parse({{"name", "low"}, {"count", 1}, {"ratio", 0}, {"limit", 1}}));
}

TEST(ScalarParametersTest, InvalidChangesIdentifyTheFieldAndRule) {
  using Kind = ConfigFieldErrorKind;
  struct Case {
    const char* name;
    Json patch;
    const char* path;
    Kind kind;
  };
  const Case cases[] = {
      {"missing required", Json::array({{{"op", "remove"}, {"path", "/name"}}}),
       "/name", Kind::kMissingField},
      {"unknown escaped field",
       Json::array({{{"op", "add"}, {"path", "/a~1b~0c"}, {"value", 1}}}),
       "/a~1b~0c", Kind::kUnknownField},
      {"null string",
       Json::array(
           {{{"op", "replace"}, {"path", "/name"}, {"value", nullptr}}}),
       "/name", Kind::kTypeMismatch},
      {"integer requires integer JSON",
       Json::array({{{"op", "add"}, {"path", "/count"}, {"value", 2.0}}}),
       "/count", Kind::kTypeMismatch},
      {"lower bound",
       Json::array({{{"op", "add"}, {"path", "/count"}, {"value", 0}}}),
       "/count", Kind::kOutOfRange},
      {"upper bound",
       Json::array({{{"op", "add"}, {"path", "/ratio"}, {"value", 1.01}}}),
       "/ratio", Kind::kOutOfRange},
      {"enum",
       Json::array({{{"op", "add"}, {"path", "/mode"}, {"value", "other"}}}),
       "/mode", Kind::kInvalidEnum},
      {"boolean requires boolean JSON",
       Json::array({{{"op", "add"}, {"path", "/enabled"}, {"value", 1}}}),
       "/enabled", Kind::kTypeMismatch},
      {"optional supplied null",
       Json::array({{{"op", "add"}, {"path", "/limit"}, {"value", nullptr}}}),
       "/limit", Kind::kTypeMismatch},
      {"nonfinite",
       Json::array({{{"op", "add"},
                     {"path", "/ratio"},
                     {"value", std::numeric_limits<double>::infinity()}}}),
       "/ratio", Kind::kNonFinite}};
  const auto spec = ScalarSpec();
  for (const auto& item : cases) {
    SCOPED_TRACE(item.name);
    const Json input = Json{{"name", "valid"}}.patch(item.patch);
    Json normalized = "unchanged";
    std::vector<ConfigFieldValidationError> errors;
    EXPECT_FALSE(
        ValidateAndNormalizeFields(spec.Fields(), input, &normalized, &errors));
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].path, item.path);
    EXPECT_EQ(errors[0].kind, item.kind);
    EXPECT_EQ(normalized, "unchanged");
    std::string diagnostic;
    EXPECT_FALSE(spec.Parse(input, &diagnostic));
    EXPECT_NE(diagnostic.find(item.path), std::string::npos) << diagnostic;
  }
  std::string diagnostic;
  EXPECT_FALSE(spec.Parse(Json::array(), &diagnostic));
  EXPECT_FALSE(diagnostic.empty());
}

TEST(ScalarParametersTest, NativeNumericWidthsCannotSilentlyNarrow) {
  struct Widths {
    int small{};
    int64_t large{};
    float fraction{};
  };
  const Parameters<Widths> spec{
      Field("small", &Widths::small).Default(0),
      Field("large", &Widths::large).Default(int64_t{0}),
      Field("fraction", &Widths::fraction).Default(0.1f)};
  for (int64_t number : {int64_t{std::numeric_limits<int>::min()},
                         int64_t{std::numeric_limits<int>::max()}}) {
    auto parsed = spec.Parse({{"small", number}});
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->small, number);
  }
  for (int64_t number : {std::numeric_limits<int64_t>::min(),
                         std::numeric_limits<int64_t>::max()}) {
    auto parsed = spec.Parse({{"large", number}});
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->large, number);
  }
  const Json invalid[] = {
      {{"small", int64_t{std::numeric_limits<int>::min()} - 1}},
      {{"small", uint64_t{std::numeric_limits<int>::max()} + 1}},
      {{"large", uint64_t{std::numeric_limits<int64_t>::max()} + 1}},
      {{"fraction", std::numeric_limits<double>::max()}},
      {{"fraction", std::numeric_limits<double>::quiet_NaN()}}};
  for (const auto& input : invalid) {
    SCOPED_TRACE(input.dump());
    std::string diagnostic;
    EXPECT_FALSE(spec.Parse(input, &diagnostic));
    EXPECT_FALSE(diagnostic.empty());
  }
  const auto fraction = spec.Parse({{"fraction", 0.625}});
  ASSERT_TRUE(fraction);
  EXPECT_FLOAT_EQ(fraction->fraction, 0.625f);
  EXPECT_EQ(spec.Fields()[2].default_value, Json(0.1));
}

TEST(ScalarParametersTest, InvalidDeclarationsFailBeforeUse) {
  struct P {
    int value{};
    std::optional<int> optional;
  };
  EXPECT_THROW((Parameters<P>{Field("value", &P::value)}),
               std::invalid_argument);
  EXPECT_THROW((Parameters<P>{Field("value", &P::value).Required().Default(1)}),
               std::invalid_argument);
  EXPECT_THROW((Parameters<P>{Field("optional", &P::optional).Default(1)}),
               std::invalid_argument);
  EXPECT_THROW(
      (Parameters<P>{Field("value", &P::value).Default(5).Range(1, 4)}),
      std::invalid_argument);
  EXPECT_THROW((Parameters<P>{Field("", &P::value).Default(1)}),
               std::invalid_argument);
  EXPECT_THROW((Parameters<P>{Field("first", &P::value).Default(1),
                              Field("second", &P::value).Default(2)}),
               std::invalid_argument);
}
}  // namespace
}  // namespace llm_edgeflow
