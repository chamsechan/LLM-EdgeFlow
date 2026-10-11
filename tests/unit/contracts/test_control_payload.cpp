#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <vector>

#include "contracts/control_payload.h"

namespace llm_edgeflow {
namespace {
using Json = nlohmann::json;

Json ControlSchema() {
  return Json::parse(R"json({
    "type":"object", "required":["mode","groups"],
    "minProperties":2, "additionalProperties":false,
    "properties":{
      "mode":{"type":"string","enum":["a","b"]},
      "groups":{"type":"array","items":{
        "type":"object","required":["limit"],"additionalProperties":false,
        "properties":{"limit":{"type":"integer","minimum":2,"maximum":5},
                      "enabled":{"type":"boolean"}}}},
      "labels":{"type":"object","additionalProperties":{"type":"integer"}},
      "note":{"type":"string","default":"annotation only"},
      "opaque":{"not":{"type":"null"}}
    }
  })json");
}

TEST(ControlPayloadTest, SchemaDeclarationsAreCheckedBeforeValues) {
  struct Case {
    const char* name;
    Json patch;
    const char* error;
  };
  const std::vector<Case> cases{
      {"unknown type",
       {{{"op", "replace"}, {"path", "/type"}, {"value", "text"}}},
       "supported JSON type"},
      {"unsupported keyword",
       {{{"op", "add"}, {"path", "/pattern"}, {"value", "x"}}},
       "Unsupported Control schema keyword"},
      {"negative property count",
       {{{"op", "replace"}, {"path", "/minProperties"}, {"value", -1}}},
       "nonnegative integer"},
      {"fractional property count",
       {{{"op", "replace"}, {"path", "/minProperties"}, {"value", 1.5}}},
       "nonnegative integer"},
      {"empty enum",
       {{{"op", "replace"},
         {"path", "/properties/mode/enum"},
         {"value", Json::array()}}},
       "nonempty array"},
      {"duplicate enum",
       {{{"op", "replace"},
         {"path", "/properties/mode/enum"},
         {"value", {"a", "a"}}}},
       "duplicate values"},
      {"required is array",
       {{{"op", "replace"}, {"path", "/required"}, {"value", "mode"}}},
       "unique strings"},
      {"required names unique",
       {{{"op", "replace"},
         {"path", "/required"},
         {"value", {"mode", "mode"}}}},
       "unique strings"},
      {"properties is object",
       {{{"op", "replace"}, {"path", "/properties"}, {"value", 1}}},
       "properties must be an object"},
      {"items is schema",
       {{{"op", "replace"},
         {"path", "/properties/groups/items"},
         {"value", true}}},
       "schema must be an object"},
      {"additional is boolean or schema",
       {{{"op", "replace"},
         {"path", "/additionalProperties"},
         {"value", "allow"}}},
       "schema must be an object"},
      {"not is schema",
       {{{"op", "replace"},
         {"path", "/properties/opaque/not"},
         {"value", nullptr}}},
       "schema must be an object"},
      {"bounds numeric",
       {{{"op", "replace"},
         {"path", "/properties/groups/items/properties/limit/minimum"},
         {"value", "2"}}},
       "finite number"},
      {"bounds ordered",
       {{{"op", "replace"},
         {"path", "/properties/groups/items/properties/limit/minimum"},
         {"value", 6}}},
       "minimum cannot exceed maximum"},
      {"annotation string",
       {{{"op", "add"}, {"path", "/description"}, {"value", false}}},
       "annotation must be a string"},
      {"annotation examples array",
       {{{"op", "add"}, {"path", "/examples"}, {"value", 1}}},
       "annotation must be an array"},
      {"annotation boolean",
       {{{"op", "add"}, {"path", "/deprecated"}, {"value", 1}}},
       "annotation must be a boolean"},
  };
  ASSERT_TRUE(ValidateControlSchema(ControlSchema()));
  for (const auto& row : cases) {
    SCOPED_TRACE(row.name);
    const auto schema = ControlSchema().patch(row.patch);
    std::string error;
    EXPECT_FALSE(ValidateControlSchema(schema, &error));
    EXPECT_NE(error.find(row.error), std::string::npos) << error;
  }
  const Json valid = {{"mode", "a"}, {"groups", Json::array()}};
  auto unsupported = ControlSchema();
  unsupported["pattern"] = "x";
  std::string error;
  ASSERT_TRUE(ValidateControlPayload(valid, ControlSchema()));
  EXPECT_FALSE(ValidateControlPayload(valid, unsupported, &error));
  EXPECT_EQ(error, "Unsupported Control schema keyword: pattern");
  Json output = {{"old", 7}};
  EXPECT_FALSE(ParseControlPayload(valid.dump(), unsupported, &output, &error));
  EXPECT_EQ(output, (Json{{"old", 7}}));
  EXPECT_EQ(error, "Unsupported Control schema keyword: pattern");
  auto nonfinite = ControlSchema();
  nonfinite["properties"]["note"]["maximum"] =
      std::numeric_limits<double>::infinity();
  EXPECT_FALSE(ValidateControlSchema(nonfinite));
}

TEST(ControlPayloadTest, NestedValuesHaveLocalDiagnosticsAndDoNotFillDefaults) {
  const auto schema = ControlSchema();
  const auto valid = Json::parse(
      R"({"mode":"a","groups":[{"limit":2,"enabled":true},{"limit":5}],"labels":{"tier":3},"opaque":false})");
  struct Case {
    const char* name;
    Json patch;
    const char* error;
  };
  const std::vector<Case> cases{
      {"missing required",
       {{{"op", "remove"}, {"path", "/mode"}}},
       "Missing required field"},
      {"unknown property",
       {{{"op", "add"}, {"path", "/extra"}, {"value", 1}}},
       "Unknown property"},
      {"enum",
       {{{"op", "replace"}, {"path", "/mode"}, {"value", "c"}}},
       "Property 'mode': Control payload value is not in the allowed enum"},
      {"array type",
       {{{"op", "replace"}, {"path", "/groups"}, {"value", 0}}},
       "Property 'groups': Control payload does not match type 'array'"},
      {"item object",
       {{{"op", "replace"}, {"path", "/groups/1"}, {"value", false}}},
       "Array item 1: Control payload does not match type 'object'"},
      {"nested required",
       {{{"op", "remove"}, {"path", "/groups/1/limit"}}},
       "Array item 1: Missing required field"},
      {"integer excludes float",
       {{{"op", "replace"}, {"path", "/groups/1/limit"}, {"value", 3.0}}},
       "Property 'limit': Control payload does not match type 'integer'"},
      {"minimum",
       {{{"op", "replace"}, {"path", "/groups/1/limit"}, {"value", 1}}},
       "Array item 1: Property 'limit': Control payload number is below "
       "minimum"},
      {"maximum",
       {{{"op", "replace"}, {"path", "/groups/1/limit"}, {"value", 6}}},
       "Array item 1: Property 'limit': Control payload number is above "
       "maximum"},
      {"boolean excludes integer",
       {{{"op", "replace"}, {"path", "/groups/0/enabled"}, {"value", 1}}},
       "Property 'enabled': Control payload does not match type 'boolean'"},
      {"additional schema",
       {{{"op", "replace"}, {"path", "/labels/tier"}, {"value", "3"}}},
       "Property 'labels': Property 'tier'"},
      {"not null",
       {{{"op", "replace"}, {"path", "/opaque"}, {"value", nullptr}}},
       "Property 'opaque': Control payload matches an excluded value type"},
  };
  std::string error = "stale";
  ASSERT_TRUE(ValidateControlPayload(valid, schema, &error));
  EXPECT_TRUE(error.empty());
  EXPECT_FALSE(valid.contains("note"));
  for (const auto& row : cases) {
    SCOPED_TRACE(row.name);
    EXPECT_FALSE(
        ValidateControlPayload(valid.patch(row.patch), schema, &error));
    EXPECT_NE(error.find(row.error), std::string::npos) << error;
  }
  EXPECT_FALSE(ValidateControlPayload({{"mode", "a"}}, schema, &error));
  EXPECT_NE(error.find("fewer properties"), std::string::npos);
  EXPECT_FALSE(
      ValidateControlPayload(Json(std::numeric_limits<double>::infinity()),
                             {{"type", "number"}}, &error));
  EXPECT_NE(error.find("finite"), std::string::npos);
  EXPECT_TRUE(ValidateControlPayload(Json::array({1, "free"}), Json::object()));
  EXPECT_TRUE(ValidateControlPayload(2.5, {{"type", "number"}}));
  EXPECT_TRUE(ValidateControlPayload(nullptr, {{"type", "null"}}));
  EXPECT_FALSE(ValidateControlPayload(1, {{"type", "null"}}));
}

TEST(ControlPayloadTest, ParsingCommitsOnlyValidatedOwnedValues) {
  const auto schema = ControlSchema();
  Json output = {{"old", 7}};
  const auto before = output;
  std::string error;
  for (const std::string text : {"{", "null", R"({"mode":"a"})"}) {
    SCOPED_TRACE(text);
    EXPECT_FALSE(ParseControlPayload(text, schema, &output, &error));
    EXPECT_EQ(output, before);
    EXPECT_FALSE(error.empty());
  }
  EXPECT_FALSE(ParseControlPayload("{}", schema, nullptr, &error));
  EXPECT_EQ(error, "Null control payload output");
  std::string text = R"({"mode":"b","groups":[]})";
  ASSERT_TRUE(ParseControlPayload(text, schema, &output, &error));
  text.clear();
  EXPECT_EQ(output, (Json{{"mode", "b"}, {"groups", Json::array()}}));
  EXPECT_TRUE(error.empty());
}
}  // namespace
}  // namespace llm_edgeflow
