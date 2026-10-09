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

TEST(ParameterSetTest, EffectiveValuesAndIntegerReadTheValuesAfterPrepare) {
  auto spec = Parameters<SetParams>({
      Field("mode", &SetParams::mode).Default("fast").Enum({"fast", "slow"}),
      Field("count", &SetParams::count).Default(4).Range(1, 64),
      Field("length", &SetParams::length).Range(2, 4096),
  });
  // Prepare 改写已声明字段：生效值和 Integer() 读到的是改写之后的值。
  spec.Prepare([](SetParams* p, std::string*) {
    p->count *= 2;
    p->length = p->count * 10;
    return true;
  });
  const ParameterSet set = spec;
  std::shared_ptr<const ParameterValues> values;
  std::string error;
  ASSERT_TRUE(set.Parse({{"count", 5}}, &values, &error)) << error;
  EXPECT_EQ(values->Get<SetParams>().count, 10);
  EXPECT_EQ(values->Effective(),
            (nlohmann::json{{"mode", "fast"}, {"count", 10}, {"length", 100}}));
  EXPECT_EQ(values->Integer("count"), 10);
  EXPECT_EQ(values->Integer("length"), 100);
  // 不是整数的参数和没有声明的参数都读不到。
  EXPECT_EQ(values->Integer("mode"), std::nullopt);
  EXPECT_EQ(values->Integer("missing"), std::nullopt);
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

struct InnerElement {
  std::string text;
  int64_t score = 10;
  // 派生成员，不属于配置字段
  std::string derived_marker;
};

struct StructContainerParams {
  std::vector<InnerElement> rows;
  std::map<std::string, InnerElement> mapped;
  std::optional<std::vector<InnerElement>> optional_rows;
  std::optional<std::map<std::string, InnerElement>> optional_mapped;
};

TEST(ParameterSetTest, StructElementsCanBeParsedAndExportedInEffective) {
  auto element_spec = Parameters<InnerElement>({
      Field("text", &InnerElement::text).Required(),
      Field("score", &InnerElement::score).Default(10).Range(1, 100),
  });
  element_spec.Prepare([](InnerElement* e, std::string*) {
    e->derived_marker = "derived_" + e->text;
    e->score *= 2;  // Prepare 翻倍 score
    return true;
  });

  auto spec = Parameters<StructContainerParams>({
      Field("rows", &StructContainerParams::rows)
          .Required()
          .Items(element_spec),
      Field("mapped", &StructContainerParams::mapped)
          .Default({})
          .Items(element_spec),
      Field("optional_rows", &StructContainerParams::optional_rows)
          .Items(element_spec),
      Field("optional_mapped", &StructContainerParams::optional_mapped)
          .Items(element_spec),
  });

  ParameterSet set(spec);
  std::shared_ptr<const ParameterValues> values;
  std::string error;

  nlohmann::json config = {
      {"rows", {{{"text", "hello"}}}},
      {"mapped", {{"key1", {{"text", "world"}, {"score", 20}}}}},
      {"optional_rows", {{{"text", "opt1"}}}},
  };

  ASSERT_TRUE(set.Parse(config, &values, &error)) << error;
  ASSERT_NE(values, nullptr);

  const auto& p = values->Get<StructContainerParams>();
  ASSERT_EQ(p.rows.size(), 1U);
  EXPECT_EQ(p.rows[0].text, "hello");
  EXPECT_EQ(p.rows[0].score, 20);  // 默认值 10 * 2 = 20
  EXPECT_EQ(p.rows[0].derived_marker, "derived_hello");

  ASSERT_EQ(p.mapped.size(), 1U);
  EXPECT_EQ(p.mapped.at("key1").text, "world");
  EXPECT_EQ(p.mapped.at("key1").score, 40);  // 显式 20 * 2 = 40
  EXPECT_EQ(p.mapped.at("key1").derived_marker, "derived_world");

  ASSERT_TRUE(p.optional_rows.has_value());
  ASSERT_EQ(p.optional_rows->size(), 1U);
  EXPECT_EQ((*p.optional_rows)[0].text, "opt1");
  EXPECT_EQ((*p.optional_rows)[0].score, 20);

  EXPECT_FALSE(p.optional_mapped.has_value());

  // Effective 检查：包含 Prepare 后的字段和默认值，不包含派生成员
  // derived_marker，未设置的可选容器不出现
  const auto& effective = values->Effective();
  const auto expected_rows =
      nlohmann::json::array({nlohmann::json{{"text", "hello"}, {"score", 20}}});
  const auto expected_mapped =
      nlohmann::json{{"key1", {{"text", "world"}, {"score", 40}}}};
  const auto expected_optional_rows =
      nlohmann::json::array({nlohmann::json{{"text", "opt1"}, {"score", 20}}});
  EXPECT_EQ(effective["rows"], expected_rows);
  EXPECT_EQ(effective["mapped"], expected_mapped);
  EXPECT_EQ(effective["optional_rows"], expected_optional_rows);
  EXPECT_FALSE(effective.contains("optional_mapped"));
  EXPECT_FALSE(effective["rows"][0].contains("derived_marker"));

  // 进一步验证：当 optional_mapped 显式提供时，能够正确解析并导出生效值
  nlohmann::json config_with_opt_mapped = {
      {"rows", {{{"text", "hello2"}}}},
      {"optional_mapped", {{"opt_k", {{"text", "opt_val"}, {"score", 30}}}}},
  };
  std::shared_ptr<const ParameterValues> values2;
  ASSERT_TRUE(set.Parse(config_with_opt_mapped, &values2, &error)) << error;
  ASSERT_NE(values2, nullptr);
  const auto& p2 = values2->Get<StructContainerParams>();
  ASSERT_TRUE(p2.optional_mapped.has_value());
  EXPECT_EQ(p2.optional_mapped->at("opt_k").text, "opt_val");
  EXPECT_EQ(p2.optional_mapped->at("opt_k").score, 60);  // 30 * 2 = 60
  const auto& effective2 = values2->Effective();
  EXPECT_TRUE(effective2.contains("optional_mapped"));
  EXPECT_EQ(effective2["optional_mapped"]["opt_k"]["score"], 60);
  EXPECT_EQ(effective2["optional_mapped"]["opt_k"]["text"], "opt_val");
}

TEST(ParameterSetTest, StructContainerDefaultsAreReadThroughDeclaredFields) {
  const auto element = Parameters<InnerElement>({
      Field("text", &InnerElement::text).Default("default text"),
      Field("score", &InnerElement::score).Default(10),
  });
  const auto spec = Parameters<StructContainerParams>({
      Field("rows", &StructContainerParams::rows)
          .Default({InnerElement{"row", 7, "excluded"}})
          .Items(element),
      Field("mapped", &StructContainerParams::mapped)
          .Default({{"key", InnerElement{"mapped", 8, "excluded"}}})
          .Items(element),
  });
  std::shared_ptr<const ParameterValues> values;
  std::string error;
  ASSERT_TRUE(
      ParameterSet(spec).Parse(nlohmann::json::object(), &values, &error))
      << error;
  EXPECT_EQ(values->Effective(),
            (nlohmann::json{
                {"rows", {{{"text", "row"}, {"score", 7}}}},
                {"mapped", {{"key", {{"text", "mapped"}, {"score", 8}}}}}}));
}

}  // namespace
}  // namespace llm_edgeflow
