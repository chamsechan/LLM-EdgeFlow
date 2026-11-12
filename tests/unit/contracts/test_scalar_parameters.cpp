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
  auto count = Field("count", &ScalarConfig::count).Default(2).Range(1, 4);
  auto spec = Parameters<ScalarConfig>{
      Field("name", &ScalarConfig::name).Required(),
      Field("mode", &ScalarConfig::mode).Default("fast").Enum({"fast", "slow"}),
      count,
      Field("id", &ScalarConfig::id).Default(int64_t{0}),
      Field("ratio", &ScalarConfig::ratio).Default(0.25).Range(0, 1),
      Field("enabled", &ScalarConfig::enabled).Default(false),
      Field("limit", &ScalarConfig::limit).Range(1, 8)};
  count.Default(7).Range(5, 9);
  return spec;
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
  const auto spec = ScalarSpec();
  auto reject = [&](const Json& input, const char* path, Kind kind) {
    SCOPED_TRACE(input.dump());
    Json normalized = "unchanged";
    std::vector<ConfigFieldValidationError> errors;
    EXPECT_FALSE(
        ValidateAndNormalizeFields(spec.Fields(), input, &normalized, &errors));
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].path, path);
    EXPECT_EQ(errors[0].kind, kind);
    EXPECT_EQ(normalized, "unchanged");
    std::string diagnostic;
    EXPECT_FALSE(spec.Parse(input, &diagnostic));
    EXPECT_NE(diagnostic.find(path), std::string::npos) << diagnostic;
  };
  struct InvalidField {
    const char* path;
    Json value;
    Kind kind;
  };
  const InvalidField cases[] = {
      {"/a~1b~0c", 1, Kind::kUnknownField},
      {"/name", nullptr, Kind::kTypeMismatch},
      {"/count", 2.0, Kind::kTypeMismatch},
      {"/count", 0, Kind::kOutOfRange},
      {"/ratio", 1.01, Kind::kOutOfRange},
      {"/mode", "other", Kind::kInvalidEnum},
      {"/enabled", 1, Kind::kTypeMismatch},
      {"/limit", nullptr, Kind::kTypeMismatch},
      {"/ratio", std::numeric_limits<double>::infinity(), Kind::kNonFinite}};
  for (const auto& item : cases) {
    Json input = {{"name", "valid"}};
    input[Json::json_pointer(item.path)] = item.value;
    reject(input, item.path, item.kind);
  }
  reject(Json::object(), "/name", Kind::kMissingField);
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
