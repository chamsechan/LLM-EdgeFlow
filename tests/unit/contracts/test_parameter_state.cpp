#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "contracts/parameter_set.h"

namespace llm_edgeflow {
namespace {
using Json = nlohmann::json;

TEST(ParameterStateTest, IncludedFieldsAreFlatAndHooksRunInsideOut) {
  struct Group {
    int size{};
  };
  struct Config {
    Group group;
    bool enabled{};
    std::unique_ptr<int> compiled;
  };
  std::vector<std::string> calls;
  auto group = Parameters<Group>{Field("size", &Group::size).Default(3)}
                   .Prepare([&](Group* value, std::string*) {
                     calls.push_back("inner prepare");
                     value->size *= 2;
                     return true;
                   })
                   .Validate([&](const Group&, std::string*) {
                     calls.push_back("inner validate");
                     return true;
                   });
  auto spec =
      Parameters<Config>{Field("enabled", &Config::enabled).Default(true)};
  spec.Include(&Config::group, std::move(group))
      .Prepare([&](Config* value, std::string*) {
        calls.push_back("outer prepare");
        value->compiled = std::make_unique<int>(value->group.size + 1);
        return true;
      })
      .Validate([&](const Config&, std::string*) {
        calls.push_back("outer validate");
        return true;
      });
  const ParameterSet set(spec);
  std::shared_ptr<const ParameterValues> values;
  std::string diagnostic = "stale";
  ASSERT_TRUE(set.Parse(Json::object(), &values, &diagnostic)) << diagnostic;
  EXPECT_TRUE(diagnostic.empty());
  EXPECT_EQ(calls,
            (std::vector<std::string>{"inner prepare", "inner validate",
                                      "outer prepare", "outer validate"}));
  EXPECT_EQ(values->Effective(), (Json{{"size", 6}, {"enabled", true}}));
  EXPECT_EQ(values->Get<Config>().group.size, 6);
  EXPECT_EQ(*values->Get<Config>().compiled, 7);
  EXPECT_THROW(values->Get<Group>(), std::logic_error);
  EXPECT_EQ(values->Integer("size"), 6);
  EXPECT_FALSE(values->Integer("enabled"));

  calls.clear();
  Json normalized;
  std::vector<ConfigFieldValidationError> errors;
  ASSERT_TRUE(ValidateAndNormalizeFields(set.Fields(), Json::object(),
                                         &normalized, &errors));
  ASSERT_TRUE(set.Parse(normalized, &values, &diagnostic)) << diagnostic;
  EXPECT_EQ(values->Effective(), (Json{{"size", 6}, {"enabled", true}}));
  EXPECT_EQ(calls.size(), 4U);
  auto old_values = values;
  EXPECT_FALSE(set.Parse({{"size", "invalid"}}, &values, &diagnostic));
  EXPECT_EQ(values, nullptr);
  EXPECT_EQ(old_values->Get<Config>().group.size, 6);
  EXPECT_FALSE(diagnostic.empty());
  EXPECT_THROW(
      spec.Include(&Config::group,
                   Parameters<Group>{Field("size", &Group::size).Default(1)}),
      std::invalid_argument);
}

TEST(ParameterStateTest, BindingFactsAndFailuresReachTheStateHooks) {
  struct Config {
    int value{};
  };
  bool saw_session_lifetime = false;
  auto spec = Parameters<Config>{Field("value", &Config::value).Default(1)};
  spec.Prepare([&](Config*, const BindingFacts& facts, std::string*) {
        saw_session_lifetime = facts.InputLifetime("source") == "session";
        return true;
      })
      .ValidateBindings(
          [](const Config&, const auto& connected, std::string* error) {
            if (connected.count("source")) return true;
            *error = "source must be connected";
            return false;
          });
  std::string diagnostic;
  const BindingFacts facts{true, {"source"}, {{"source", "session"}}};
  ASSERT_TRUE(spec.ParseNormalized({{"value", 1}}, facts, &diagnostic));
  EXPECT_TRUE(saw_session_lifetime);
  EXPECT_FALSE(spec.ParseNormalized({{"value", 1}}, BindingFacts{true, {}, {}},
                                    &diagnostic));
  EXPECT_EQ(diagnostic, "source must be connected");
  ASSERT_TRUE(spec.Parse(Json::object(), &diagnostic));

  for (bool standard : {true, false}) {
    SCOPED_TRACE(standard);
    auto throwing = spec;
    throwing.Prepare([standard](Config*, std::string*) -> bool {
      if (standard) throw std::runtime_error("prepare fault");
      throw 7;
    });
    EXPECT_FALSE(throwing.Parse(Json::object(), &diagnostic));
    EXPECT_FALSE(diagnostic.empty());
  }
}

TEST(ParameterStateTest, EmptyAndOptionalStatesHaveNoInventedValues) {
  const ParameterSet empty;
  std::shared_ptr<const ParameterValues> values;
  ASSERT_TRUE(empty.Parse(Json::object(), &values));
  EXPECT_NO_THROW(values->Get<NoParameters>());
  EXPECT_EQ(values->Effective(), Json::object());
  EXPECT_FALSE(empty.Parse({{"extra", 1}}, &values));
  struct Config {
    std::optional<bool> flag;
  };
  const ParameterSet optional(Parameters<Config>{Field("flag", &Config::flag)});
  ASSERT_TRUE(optional.Parse(Json::object(), &values));
  EXPECT_EQ(values->Effective(), Json::object());
  ASSERT_TRUE(optional.Parse({{"flag", false}}, &values));
  EXPECT_EQ(values->Get<Config>().flag, false);
  EXPECT_EQ(values->Effective(), (Json{{"flag", false}}));
}

TEST(ParameterStateTest,
     IntegerProjectionRejectsNonIntegersAndUnsignedOverflow) {
  struct Config {
    Json value;
  };
  const ParameterSet set(
      Parameters<Config>{Field("value", &Config::value).Required()});
  struct Case {
    Json input;
    std::optional<int64_t> expected;
  };
  const Case cases[] = {
      {std::numeric_limits<int64_t>::min(),
       std::numeric_limits<int64_t>::min()},
      {uint64_t{std::numeric_limits<int64_t>::max()},
       std::numeric_limits<int64_t>::max()},
      {uint64_t{std::numeric_limits<int64_t>::max()} + 1, std::nullopt},
      {2.0, std::nullopt},
      {true, std::nullopt},
      {"2", std::nullopt}};
  for (const auto& item : cases) {
    SCOPED_TRACE(item.input.dump());
    std::shared_ptr<const ParameterValues> values;
    ASSERT_TRUE(set.Parse({{"value", item.input}}, &values));
    EXPECT_EQ(values->Integer("value"), item.expected);
    EXPECT_FALSE(values->Integer("missing"));
  }
}
}  // namespace
}  // namespace llm_edgeflow
