#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "contracts/parameter_set.h"

namespace llm_edgeflow {
namespace {

struct SetParams {
  std::string mode;
  int64_t count{};
  std::optional<int64_t> length;
};

struct OtherParams {
  int64_t value{};
};

ParameterSet MakeSet(int* validate_calls = nullptr) {
  auto spec = Parameters<SetParams>({
      Field("mode", &SetParams::mode).Default("fast").Enum({"fast", "slow"}),
      Field("count", &SetParams::count).Default(4).Range(1, 64),
      Field("length", &SetParams::length).Range(2, 4096),
  });
  spec.Validate([validate_calls](const SetParams& p, std::string* error) {
    if (validate_calls) ++*validate_calls;
    if (p.mode == "slow" && p.count > 8) {
      if (error) *error = "slow mode supports at most 8";
      return false;
    }
    return true;
  });
  return spec;
}

TEST(ParameterSetTest, DefaultConstructedAcceptsOnlyAnEmptyObject) {
  const ParameterSet none;
  EXPECT_TRUE(none.Fields().empty());
  std::shared_ptr<const ParameterValues> values;
  std::string error;
  EXPECT_TRUE(none.Parse(nlohmann::json::object(), &values, &error)) << error;
  ASSERT_NE(values, nullptr);
  EXPECT_NO_THROW(values->Get<NoParameters>());
  EXPECT_FALSE(none.Parse({{"unexpected", 1}}, &values, &error));
  EXPECT_NE(error.find("unexpected"), std::string::npos);
}

TEST(ParameterSetTest, FieldsComeFromTheDeclaration) {
  const auto set = MakeSet();
  ASSERT_EQ(set.Fields().size(), 3u);
  EXPECT_EQ(set.Fields()[0].name, "mode");
  EXPECT_EQ(set.Fields()[1].default_value, 4);
  EXPECT_FALSE(set.Fields()[2].required);
  EXPECT_TRUE(set.Fields()[2].default_value.is_null());
}

TEST(ParameterSetTest, ParseFillsDefaultsAndAppliesOverrides) {
  const auto set = MakeSet();
  std::shared_ptr<const ParameterValues> values;
  std::string error;
  ASSERT_TRUE(set.Parse(nlohmann::json::object(), &values, &error)) << error;
  EXPECT_EQ(values->Get<SetParams>().mode, "fast");
  EXPECT_EQ(values->Get<SetParams>().count, 4);
  EXPECT_FALSE(values->Get<SetParams>().length.has_value());

  ASSERT_TRUE(set.Parse({{"mode", "slow"}, {"count", 8}, {"length", 128}},
                        &values, &error))
      << error;
  EXPECT_EQ(values->Get<SetParams>().mode, "slow");
  EXPECT_EQ(values->Get<SetParams>().count, 8);
  ASSERT_TRUE(values->Get<SetParams>().length.has_value());
  EXPECT_EQ(*values->Get<SetParams>().length, 128);
}

TEST(ParameterSetTest, RawAndNormalizedConfigsParseToTheSameValues) {
  const auto set = MakeSet();
  const nlohmann::json raw = {{"count", 6}, {"length", 64}};
  const nlohmann::json normalized = {
      {"mode", "fast"}, {"count", 6}, {"length", 64}};
  std::shared_ptr<const ParameterValues> from_raw;
  std::shared_ptr<const ParameterValues> from_normalized;
  std::string error;
  ASSERT_TRUE(set.Parse(raw, &from_raw, &error)) << error;
  ASSERT_TRUE(set.Parse(normalized, &from_normalized, &error)) << error;
  EXPECT_EQ(from_raw->Get<SetParams>().mode,
            from_normalized->Get<SetParams>().mode);
  EXPECT_EQ(from_raw->Get<SetParams>().count,
            from_normalized->Get<SetParams>().count);
  EXPECT_EQ(from_raw->Get<SetParams>().length,
            from_normalized->Get<SetParams>().length);
}

TEST(ParameterSetTest, RunsFieldChecksBeforeValidateAndValidateOnce) {
  int validate_calls = 0;
  const auto set = MakeSet(&validate_calls);
  std::shared_ptr<const ParameterValues> values;
  std::string error;

  // 字段错误（未知、类型、范围、枚举）不会进入 Validate。
  for (const auto& config :
       {nlohmann::json{{"unknown", 1}}, nlohmann::json{{"count", "4"}},
        nlohmann::json{{"count", 65}}, nlohmann::json{{"mode", "other"}},
        nlohmann::json{{"length", 1}}}) {
    SCOPED_TRACE(config.dump());
    error.clear();
    EXPECT_FALSE(set.Parse(config, &values, &error));
    EXPECT_FALSE(error.empty());
  }
  EXPECT_EQ(validate_calls, 0);

  // 跨字段规则由 Validate 负责，每次解析恰好执行一次。
  EXPECT_FALSE(set.Parse({{"mode", "slow"}, {"count", 9}}, &values, &error));
  EXPECT_EQ(error, "slow mode supports at most 8");
  EXPECT_EQ(validate_calls, 1);
  ASSERT_TRUE(set.Parse(nlohmann::json::object(), &values, &error)) << error;
  EXPECT_EQ(validate_calls, 2);
}

TEST(ParameterSetTest, FieldErrorsNameTheOffendingField) {
  const auto set = MakeSet();
  std::shared_ptr<const ParameterValues> values;
  std::string error;
  EXPECT_FALSE(set.Parse({{"count", 65}}, &values, &error));
  EXPECT_NE(error.find("count"), std::string::npos) << error;
  EXPECT_FALSE(set.Parse({{"mode", 3}}, &values, &error));
  EXPECT_NE(error.find("mode"), std::string::npos) << error;
}

TEST(ParameterSetTest, FailedParseLeavesValuesUntouched) {
  const auto set = MakeSet();
  std::shared_ptr<const ParameterValues> values;
  std::string error;
  ASSERT_TRUE(set.Parse(nlohmann::json::object(), &values, &error));
  const auto previous = values;
  EXPECT_FALSE(set.Parse({{"count", 0}}, &values, &error));
  EXPECT_EQ(values, previous);
}

TEST(ParameterSetTest, GetWithAnotherTypeThrowsLogicError) {
  const auto set = MakeSet();
  std::shared_ptr<const ParameterValues> values;
  std::string error;
  ASSERT_TRUE(set.Parse(nlohmann::json::object(), &values, &error));
  EXPECT_THROW(values->Get<OtherParams>(), std::logic_error);
  EXPECT_NO_THROW(values->Get<SetParams>());
}

TEST(ParameterSetTest, ValidateExceptionsBecomeDiagnostics) {
  auto spec = Parameters<SetParams>(
      {Field("count", &SetParams::count).Default(4).Range(1, 64)});
  spec.Validate([](const SetParams& p, std::string*) -> bool {
    if (p.count == 1) throw std::runtime_error("validator failed");
    if (p.count == 2) throw 2;
    return true;
  });
  const ParameterSet set = spec;
  std::shared_ptr<const ParameterValues> values;
  std::string error;
  EXPECT_FALSE(set.Parse({{"count", 1}}, &values, &error));
  EXPECT_EQ(error, "validator failed");
  EXPECT_FALSE(set.Parse({{"count", 2}}, &values, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_TRUE(set.Parse({{"count", 3}}, &values, &error)) << error;
}

}  // namespace
}  // namespace llm_edgeflow
