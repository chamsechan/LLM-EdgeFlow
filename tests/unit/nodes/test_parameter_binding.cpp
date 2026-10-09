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

struct ContainerParams {
  std::vector<int> counts;
  std::vector<bool> flags;
  std::map<std::string, double> scores;
  std::map<std::string, std::vector<std::string>> groups;
  nlohmann::json payload;
};

Parameters<ContainerParams> ContainerParamSpec() {
  return Parameters<ContainerParams>{
      Field("counts", &ContainerParams::counts).Default({}).Range(1, 9),
      Field("flags", &ContainerParams::flags).Default({}),
      Field("scores", &ContainerParams::scores).Default({}).Range(0, 1),
      Field("groups", &ContainerParams::groups)
          .Default({})
          .Enum({"fast", "slow"}),
      Field("payload", &ContainerParams::payload)
          .Default(nlohmann::json::object())};
}

TEST(ParameterBindingTest,
     ContainersUseScalarLeafConstraintsAndNestedMetadata) {
  const auto schema = ContainerParamSpec();
  std::string diagnostic;
  const auto parsed =
      schema.Parse({{"counts", {1, 9}},
                    {"flags", {true, false}},
                    {"scores", {{"second", 0.75}, {"first", 0.25}}},
                    {"groups", {{"first", {"fast", "slow"}}}},
                    {"payload", 42}},
                   &diagnostic);
  ASSERT_TRUE(parsed.has_value()) << diagnostic;
  EXPECT_EQ(parsed->counts, (std::vector<int>{1, 9}));
  EXPECT_EQ(parsed->flags, (std::vector<bool>{true, false}));
  EXPECT_EQ(parsed->scores.begin()->first, "first");
  EXPECT_EQ(parsed->groups.at("first"),
            (std::vector<std::string>{"fast", "slow"}));
  EXPECT_EQ(parsed->payload, 42);
  const auto defaults = schema.Parse(nlohmann::json::object(), &diagnostic);
  ASSERT_TRUE(defaults.has_value()) << diagnostic;
  EXPECT_TRUE(defaults->counts.empty());
  EXPECT_TRUE(defaults->flags.empty());
  EXPECT_TRUE(defaults->scores.empty());
  EXPECT_TRUE(defaults->groups.empty());
  EXPECT_EQ(defaults->payload, nlohmann::json::object());

  const auto groups = ConfigFieldToJson(schema.Fields()[3]);
  EXPECT_EQ(groups["type"], "map");
  EXPECT_EQ(groups["items"]["type"], "array");
  EXPECT_EQ(groups["items"]["items"]["type"], "string");
  EXPECT_EQ(groups["items"]["items"]["enum"], (nlohmann::json{"fast", "slow"}));
  const auto group_schema = ConfigFieldJsonSchema(schema.Fields()[3]);
  EXPECT_EQ(group_schema["type"], "object");
  EXPECT_EQ(group_schema["additionalProperties"]["type"], "array");
  EXPECT_EQ(group_schema["additionalProperties"]["items"]["enum"],
            groups["items"]["items"]["enum"]);
  const auto count_schema = ConfigFieldJsonSchema(schema.Fields()[0]);
  EXPECT_EQ(count_schema["items"]["minimum"], 1);
  EXPECT_EQ(count_schema["items"]["maximum"], 9);
  EXPECT_FALSE(count_schema.contains("minimum"));
}

TEST(ParameterBindingTest, ContainerErrorsCarryEscapedJsonPointerPaths) {
  const auto schema = ContainerParamSpec();
  struct Case {
    nlohmann::json config;
    const char* path;
    ConfigFieldErrorKind kind;
  };
  const Case cases[] = {
      {{{"counts", {1, 10}}}, "/counts/1", ConfigFieldErrorKind::kOutOfRange},
      {{{"counts", {1, "bad"}}},
       "/counts/1",
       ConfigFieldErrorKind::kTypeMismatch},
      {{{"flags", {true, 0}}}, "/flags/1", ConfigFieldErrorKind::kTypeMismatch},
      {{{"scores", {{"a/b~c", 1.01}}}},
       "/scores/a~1b~0c",
       ConfigFieldErrorKind::kOutOfRange},
      {{{"groups", {{"a/b~c", {"fast", "other"}}}}},
       "/groups/a~1b~0c/1",
       ConfigFieldErrorKind::kInvalidEnum},
      {{{"groups", {{"empty", nullptr}}}},
       "/groups/empty",
       ConfigFieldErrorKind::kTypeMismatch}};
  for (const auto& entry : cases) {
    SCOPED_TRACE(entry.config.dump());
    nlohmann::json normalized;
    std::vector<ConfigFieldValidationError> errors;
    EXPECT_FALSE(ValidateAndNormalizeFields(schema.Fields(), entry.config,
                                            &normalized, &errors));
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].path, entry.path);
    EXPECT_EQ(errors[0].kind, entry.kind);
    std::string diagnostic;
    EXPECT_FALSE(schema.Parse(entry.config, &diagnostic).has_value());
    EXPECT_NE(diagnostic.find(entry.path), std::string::npos) << diagnostic;
  }
}

TEST(ParameterBindingTest, JsonParametersAcceptEveryNonNullJsonValue) {
  struct JsonParams {
    nlohmann::json value;
  };
  const auto schema =
      Parameters<JsonParams>{Field("value", &JsonParams::value).Required()};
  for (const auto& value : std::vector<nlohmann::json>{nlohmann::json::object(),
                                                       nlohmann::json::array(),
                                                       {{"nested", nullptr}},
                                                       {1, "two", nullptr},
                                                       "text",
                                                       4,
                                                       0.5,
                                                       false}) {
    SCOPED_TRACE(value.dump());
    std::string diagnostic;
    const auto parsed = schema.Parse({{"value", value}}, &diagnostic);
    ASSERT_TRUE(parsed.has_value()) << diagnostic;
    EXPECT_EQ(parsed->value, value);
  }
  std::string diagnostic;
  EXPECT_FALSE(schema.Parse({{"value", nullptr}}, &diagnostic).has_value());
  EXPECT_FALSE(diagnostic.empty());
  EXPECT_EQ(ConfigFieldToJson(schema.Fields()[0])["type"], "json");
  EXPECT_EQ(ConfigFieldJsonSchema(schema.Fields()[0])["not"],
            (nlohmann::json{{"type", "null"}}));
  EXPECT_THROW((Parameters<JsonParams>{
                   Field("value", &JsonParams::value).Default(nullptr)}),
               std::invalid_argument);
}

struct ElementParams {
  std::string name;
  int weight = 0;
  std::string prepared;
};

Parameters<ElementParams> ElementParamSpec() {
  return Parameters<ElementParams>{
      Field("name", &ElementParams::name).Required(),
      Field("weight", &ElementParams::weight).Default(2).Range(1, 9)}
      .Prepare([](ElementParams* value, std::string* error) {
        if (value->name == "fail_prepare") {
          if (error) *error = "element prepare rejected";
          return false;
        }
        value->prepared = "prepared:" + value->name;
        return true;
      })
      .Validate([](const ElementParams& value, std::string* error) {
        if (value.name == "fail_validate") {
          if (error) *error = "element validate rejected";
          return false;
        }
        return true;
      });
}

struct ElementsParams {
  std::vector<std::vector<ElementParams>> rows;
  std::map<std::string, ElementParams> endpoints;
};

Parameters<ElementsParams> ElementsParamSpec() {
  return Parameters<ElementsParams>{
      Field("rows", &ElementsParams::rows)
          .Default({})
          .Items(ElementParamSpec()),
      Field("endpoints", &ElementsParams::endpoints)
          .Default({})
          .Items(ElementParamSpec())};
}

TEST(ParameterBindingTest,
     ReadProjectsCurrentIncludedAndNestedDeclaredMembers) {
  struct Common {
    int capacity = 0;
    std::optional<std::string> label;
    std::string derived;
  };
  struct Params {
    Common common;
    std::vector<std::vector<ElementParams>> rows;
    std::map<std::string, std::vector<ElementParams>> endpoints;
    std::string derived;
  };
  const auto schema =
      Parameters<Params>{
          Field("rows", &Params::rows).Default({}).Items(ElementParamSpec()),
          Field("endpoints", &Params::endpoints)
              .Default({})
              .Items(ElementParamSpec())}
          .Include(&Params::common,
                   Parameters<Common>{
                       Field("capacity", &Common::capacity).Default(1),
                       Field("label", &Common::label)});
  Params values;
  values.common = {7, std::nullopt, "hidden common state"};
  values.rows = {{ElementParams{"row", 3, "hidden row state"}}};
  values.endpoints = {
      {"a/b~c", {ElementParams{"clock", 4, "hidden map state"}}}};
  values.derived = "hidden outer state";
  const nlohmann::json expected = {
      {"capacity", 7},
      {"rows", nlohmann::json::array({nlohmann::json::array(
                   {{{"name", "row"}, {"weight", 3}}})})},
      {"endpoints",
       {{"a/b~c",
         nlohmann::json::array({{{"name", "clock"}, {"weight", 4}}})}}}};
  EXPECT_EQ(schema.Read(values), expected);
  values.common.label = "supplied";
  values.rows[0][0].weight = 8;
  auto updated = expected;
  updated["label"] = "supplied";
  updated["rows"][0][0]["weight"] = 8;
  EXPECT_EQ(schema.Read(values), updated);
}

TEST(ParameterBindingTest, StructItemsNormalizeDefaultsAndRunElementHooks) {
  const auto schema = ElementsParamSpec();
  const nlohmann::json config = {
      {"rows",
       nlohmann::json::array({nlohmann::json::array({{{"name", "row"}}})})},
      {"endpoints", {{"time", {{"name", "clock"}}}}}};
  nlohmann::json normalized;
  std::vector<ConfigFieldValidationError> errors;
  ASSERT_TRUE(ValidateAndNormalizeFields(schema.Fields(), config, &normalized,
                                         &errors));
  EXPECT_EQ(normalized["rows"][0][0]["weight"], 2);
  EXPECT_EQ(normalized["endpoints"]["time"]["weight"], 2);
  std::string diagnostic;
  const auto parsed = schema.Parse(config, &diagnostic);
  ASSERT_TRUE(parsed.has_value()) << diagnostic;
  ASSERT_EQ(parsed->rows.size(), 1U);
  ASSERT_EQ(parsed->rows[0].size(), 1U);
  EXPECT_EQ(parsed->rows[0][0].weight, 2);
  EXPECT_EQ(parsed->rows[0][0].prepared, "prepared:row");
  EXPECT_EQ(parsed->endpoints.at("time").prepared, "prepared:clock");
  const auto metadata = ConfigFieldToJson(schema.Fields()[1]);
  EXPECT_EQ(metadata["items"]["type"], "object");
  ASSERT_EQ(metadata["items"]["fields"].size(), 2U);
  EXPECT_EQ(metadata["items"]["fields"][0]["name"], "name");
  const auto item_schema =
      ConfigFieldJsonSchema(schema.Fields()[1])["additionalProperties"];
  EXPECT_EQ(item_schema["additionalProperties"], false);
  EXPECT_EQ(item_schema["required"], (nlohmann::json{"name"}));
  EXPECT_EQ(item_schema["properties"]["weight"]["default"], 2);
}

TEST(ParameterBindingTest, StructItemFieldErrorsPreserveCodesAndNestedPaths) {
  const auto schema = ElementsParamSpec();
  struct Case {
    nlohmann::json element;
    const char* suffix;
    ConfigFieldErrorKind kind;
  };
  const Case cases[] = {
      {nlohmann::json::object(), "/name", ConfigFieldErrorKind::kMissingField},
      {{{"name", 4}}, "/name", ConfigFieldErrorKind::kTypeMismatch},
      {{{"name", "ok"}, {"unknown/~", 1}},
       "/unknown~1~0",
       ConfigFieldErrorKind::kUnknownField},
      {{{"name", "ok"}, {"weight", 0}},
       "/weight",
       ConfigFieldErrorKind::kOutOfRange}};
  for (const auto& entry : cases) {
    nlohmann::json normalized;
    std::vector<ConfigFieldValidationError> errors;
    const nlohmann::json config = {{"endpoints", {{"a/b~c", entry.element}}}};
    EXPECT_FALSE(ValidateAndNormalizeFields(schema.Fields(), config,
                                            &normalized, &errors));
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].path, std::string("/endpoints/a~1b~0c") + entry.suffix);
    EXPECT_EQ(errors[0].kind, entry.kind);
  }
}

TEST(ParameterBindingTest, StructItemHookFailuresIdentifyElementIndexAndKey) {
  const auto schema = ElementsParamSpec();
  for (const char* rejected : {"fail_prepare", "fail_validate"}) {
    std::string diagnostic;
    EXPECT_FALSE(schema
                     .Parse({{"endpoints", {{"time", {{"name", rejected}}}}}},
                            &diagnostic)
                     .has_value());
    EXPECT_NE(diagnostic.find("Map key 'time'"), std::string::npos)
        << diagnostic;
    EXPECT_NE(diagnostic.find("element"), std::string::npos) << diagnostic;
    const nlohmann::json rows = nlohmann::json::array(
        {nlohmann::json::array({{{"name", "ok"}}, {{"name", rejected}}})});
    EXPECT_FALSE(schema.Parse({{"rows", rows}}, &diagnostic).has_value());
    EXPECT_NE(diagnostic.find("Array item 0"), std::string::npos) << diagnostic;
    EXPECT_NE(diagnostic.find("Array item 1"), std::string::npos) << diagnostic;
  }
}

TEST(ParameterBindingTest, NonemptyPlainStructDefaultsUseDeclaredItemFields) {
  struct Params {
    std::vector<ElementParams> rules;
  };
  std::optional<Parameters<Params>> schema;
  ASSERT_NO_THROW(schema.emplace(Parameters<Params>{
      Field("rules", &Params::rules)
          .Default({ElementParams{"initial", 3, "ignored derived value"}})
          .Items(ElementParamSpec())}));
  ASSERT_TRUE(schema.has_value());
  EXPECT_EQ(schema->Fields()[0].default_value,
            (nlohmann::json::array({{{"name", "initial"}, {"weight", 3}}})));
  std::string diagnostic;
  const auto parsed = schema->Parse(nlohmann::json::object(), &diagnostic);
  ASSERT_TRUE(parsed.has_value()) << diagnostic;
  ASSERT_EQ(parsed->rules.size(), 1U);
  EXPECT_EQ(parsed->rules[0].prepared, "prepared:initial");
}

TEST(ParameterBindingTest,
     MissingItemDeclarationsAndInvalidLeafDefaultsFailAtConstruction) {
  struct JsonElement {
    nlohmann::json payload;
    std::optional<int> count;
  };
  struct JsonParams {
    std::vector<JsonElement> elements;
  };
  const auto items = Parameters<JsonElement>{
      Field("payload", &JsonElement::payload).Default(nlohmann::json::object()),
      Field("count", &JsonElement::count)};
  EXPECT_THROW(
      (Parameters<JsonParams>{Field("elements", &JsonParams::elements)
                                  .Default({JsonElement{nullptr, std::nullopt}})
                                  .Items(items)}),
      std::invalid_argument);
  const auto optional_default = Parameters<JsonParams>{
      Field("elements", &JsonParams::elements)
          .Default({JsonElement{nlohmann::json::object(), std::nullopt}})
          .Items(items)};
  EXPECT_EQ(optional_default.Fields()[0].default_value,
            (nlohmann::json::array({{{"payload", nlohmann::json::object()}}})));
  EXPECT_THROW((Parameters<ElementsParams>{
                   Field("rows", &ElementsParams::rows).Default({})}),
               std::invalid_argument);
  EXPECT_THROW(
      (Parameters<ContainerParams>{
          Field("counts", &ContainerParams::counts).Default({10}).Range(1, 9)}),
      std::invalid_argument);
  EXPECT_THROW(
      (Parameters<ContainerParams>{Field("groups", &ContainerParams::groups)
                                       .Default({{"invalid", {"other"}}})
                                       .Enum({"fast", "slow"})}),
      std::invalid_argument);
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

TEST(ParameterBindingTest, FloatDefaultMetadataUsesShortestDecimalValues) {
  struct Params {
    float temperature = 0;
    float top_p = 0;
  };
  const auto schema = Parameters<Params>{
      Field("temperature", &Params::temperature).Default(0.7f),
      Field("top_p", &Params::top_p).Default(0.9f)};
  EXPECT_EQ(schema.Fields()[0].default_value.dump(), "0.7");
  EXPECT_EQ(schema.Fields()[1].default_value.dump(), "0.9");
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
  bool bind_ok = schema.ValidateWithBindings(
      norm, BindingFacts{true, {"other_input"}, {}}, &err);
  EXPECT_FALSE(bind_ok);
  EXPECT_EQ(err, "custom mode requires connected context input");

  bind_ok = schema.ValidateWithBindings(
      norm, BindingFacts{true, {"context"}, {}}, &err);
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
