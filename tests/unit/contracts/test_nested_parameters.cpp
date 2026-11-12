#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "contracts/parameters.h"

namespace llm_edgeflow {
namespace {
using Json = nlohmann::json;

struct StockItem {
  int units{};
  std::string sku;
  int prepared_units{};
};
struct StockConfig {
  std::vector<StockItem> items;
  std::map<std::string, std::vector<StockItem>> warehouses;
  std::vector<bool> flags;
  std::map<std::string, std::vector<int>> sizes;
  Json options;
};

auto ItemSpec() {
  return Parameters<StockItem>{
      Field("units", &StockItem::units).Default(2).Range(2, 5),
      Field("sku", &StockItem::sku).Default("box")}
      .Prepare([](StockItem* item, std::string*) {
        item->prepared_units = item->units * 3;
        return true;
      })
      .Validate([](const StockItem& item, std::string* error) {
        if (item.sku != "banned") return true;
        *error = "sku banned";
        return false;
      });
}

auto StockSpec() {
  return Parameters<StockConfig>{
      Field("items", &StockConfig::items)
          .Default(std::vector<StockItem>{})
          .Items(ItemSpec()),
      Field("warehouses", &StockConfig::warehouses)
          .Default(std::map<std::string, std::vector<StockItem>>{})
          .Items(ItemSpec()),
      Field("flags", &StockConfig::flags).Default(std::vector<bool>{}),
      Field("sizes", &StockConfig::sizes)
          .Default(std::map<std::string, std::vector<int>>{})
          .Range(2, 5),
      Field("options", &StockConfig::options).Default(Json::object())};
}

TEST(NestedParametersTest, NestedDefaultsAndHooksProduceOwnedTypedValues) {
  const auto spec = StockSpec();
  Json raw = {
      {"items", Json::array({Json::object(), {{"units", 5}, {"sku", "kit"}}})},
      {"warehouses", {{"east", Json::array({{{"units", 3}}})}}},
      {"flags", {true, false}},
      {"sizes", {{"bin", {2, 5}}}}};
  Json normalized;
  std::vector<ConfigFieldValidationError> errors;
  ASSERT_TRUE(
      ValidateAndNormalizeFields(spec.Fields(), raw, &normalized, &errors));
  const Json expected = {
      {"items", Json::array({{{"units", 2}, {"sku", "box"}},
                             {{"units", 5}, {"sku", "kit"}}})},
      {"warehouses", {{"east", Json::array({{{"units", 3}, {"sku", "box"}}})}}},
      {"options", Json::object()},
      {"flags", {true, false}},
      {"sizes", {{"bin", {2, 5}}}}};
  EXPECT_EQ(normalized, expected);
  std::string diagnostic;
  auto parsed = spec.Parse(raw, &diagnostic);
  ASSERT_TRUE(parsed) << diagnostic;
  ASSERT_EQ(parsed->items.size(), 2U);
  EXPECT_EQ(parsed->items[0].prepared_units, 6);
  EXPECT_EQ(parsed->items[1].prepared_units, 15);
  EXPECT_EQ(parsed->warehouses.at("east")[0].prepared_units, 9);
  EXPECT_EQ(parsed->flags, (std::vector<bool>{true, false}));
  EXPECT_EQ(parsed->sizes.at("bin"), (std::vector<int>{2, 5}));
  raw.clear();
  EXPECT_EQ(spec.Read(*parsed), expected);
  auto empty = spec.Parse(Json::object());
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty->items.empty());
  EXPECT_TRUE(empty->warehouses.empty());
  for (bool items_first : {false, true}) {
    SCOPED_TRACE(items_first);
    int preparations = 0;
    auto items = ItemSpec();
    items.Prepare([&](StockItem* item, std::string*) {
      ++preparations;
      item->prepared_units = item->units * 3;
      return true;
    });
    auto builder = Field("items", &StockConfig::items);
    if (items_first) builder.Items(items);
    builder.Default(std::vector<StockItem>{{3, "kit", 0}});
    if (!items_first) builder.Items(items);
    const Parameters<StockConfig> spec{builder};
    EXPECT_EQ(preparations, 0);
    EXPECT_EQ(spec.Fields()[0].default_value,
              Json::array({{{"units", 3}, {"sku", "kit"}}}));
    auto parsed = spec.Parse(Json::object());
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->items.at(0).prepared_units, 9);
    EXPECT_EQ(preparations, 1);
    EXPECT_FALSE(spec.ParseNormalized(Json::object()));
    EXPECT_EQ(preparations, 1);
  }
}

TEST(NestedParametersTest, PackedBooleanContainersRoundTripDefaultsAndValues) {
  struct Flags {
    std::vector<bool> values;
    std::vector<std::vector<bool>> rows;
    std::map<std::string, std::vector<bool>> groups;
  };
  const Parameters<Flags> spec{
      Field("values", &Flags::values).Default(std::vector<bool>{true, false}),
      Field("rows", &Flags::rows)
          .Default(std::vector<std::vector<bool>>{{false, true}, {}}),
      Field("groups", &Flags::groups)
          .Default(std::map<std::string, std::vector<bool>>{{"enabled", {true}},
                                                            {"empty", {}}})};
  const Json expected = {
      {"values", Json::array({true, false})},
      {"rows", Json::array({Json::array({false, true}), Json::array()})},
      {"groups", {{"enabled", Json::array({true})}, {"empty", Json::array()}}}};
  std::string diagnostic;
  const auto defaults = spec.Parse(Json::object(), &diagnostic);
  ASSERT_TRUE(defaults) << diagnostic;
  EXPECT_EQ(spec.Read(*defaults), expected);
  Json raw = {
      {"values", Json::array({false})},
      {"rows", Json::array({Json::array({true}), Json::array({false})})},
      {"groups", {{"mixed", Json::array({false, true, false})}}}};
  const auto parsed = spec.Parse(raw, &diagnostic);
  ASSERT_TRUE(parsed) << diagnostic;
  EXPECT_EQ(spec.Read(*parsed), raw);
}

TEST(NestedParametersTest, NestedErrorsIdentifyFieldsAndSemanticFailures) {
  using Kind = ConfigFieldErrorKind;
  struct Case {
    Json input;
    const char* path;
    Kind kind;
  };
  const Case cases[] = {
      {{{"items", Json::array({{{"units", 1}}})}},
       "/items/0/units",
       Kind::kOutOfRange},
      {{{"warehouses",
         {{"a/b~c", Json::array({Json::object(), {{"units", "3"}}})}}}},
       "/warehouses/a~1b~0c/1/units",
       Kind::kTypeMismatch},
      {{{"items", Json::array({{{"extra", true}}})}},
       "/items/0/extra",
       Kind::kUnknownField},
      {{{"items", Json::object()}}, "/items", Kind::kTypeMismatch},
      {{{"flags", {true, 0}}}, "/flags/1", Kind::kTypeMismatch},
      {{{"sizes", {{"bin", {2, 6}}}}}, "/sizes/bin/1", Kind::kOutOfRange},
      {{{"options", nullptr}}, "/options", Kind::kTypeMismatch}};
  const auto spec = StockSpec();
  for (const auto& item : cases) {
    SCOPED_TRACE(item.input.dump());
    Json normalized;
    std::vector<ConfigFieldValidationError> errors;
    EXPECT_FALSE(ValidateAndNormalizeFields(spec.Fields(), item.input,
                                            &normalized, &errors));
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].path, item.path);
    EXPECT_EQ(errors[0].kind, item.kind);
    std::string diagnostic;
    EXPECT_FALSE(spec.Parse(item.input, &diagnostic));
    EXPECT_NE(diagnostic.find(item.path), std::string::npos) << diagnostic;
  }
  std::string diagnostic;
  EXPECT_FALSE(spec.Parse(
      {{"warehouses", {{"dock", Json::array({{{"sku", "banned"}}})}}}},
      &diagnostic));
  EXPECT_NE(diagnostic.find("Map key 'dock'"), std::string::npos);
  EXPECT_NE(diagnostic.find("Array item 0"), std::string::npos);
  EXPECT_NE(diagnostic.find("sku banned"), std::string::npos);
  EXPECT_THROW(
      (Parameters<StockConfig>{Field("items", &StockConfig::items).Required()}),
      std::invalid_argument);
}

TEST(NestedParametersTest, JsonPayloadKeepsAllNonNullJsonKinds) {
  const auto spec = StockSpec();
  const Json values[] = {false,
                         3,
                         0.5,
                         "text",
                         Json::array({nullptr, 1}),
                         Json{{"nested", nullptr}}};
  for (const auto& value : values) {
    SCOPED_TRACE(value.dump());
    auto parsed = spec.Parse({{"options", value}});
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->options, value);
  }
}
}  // namespace
}  // namespace llm_edgeflow
