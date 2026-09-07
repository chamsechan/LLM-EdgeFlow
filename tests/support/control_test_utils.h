#pragma once

#include <filesystem>
#include <fstream>

#include "nlohmann/json.hpp"

namespace llm_edgeflow::test {

// Uses the compiled authoring starter and the existing keyword I/O contract.
inline void WriteControlTestPipeline(const std::filesystem::path& directory) {
  const nlohmann::json pipeline = {
      {"biz_name", "keyword_match_v1"},
      {"models", nlohmann::json::array()},
      {"pipeline",
       {{{"id", "prefix"},
         {"node_type", "TestControlNode"},
         {"depends_on", nlohmann::json::array()},
         {"ports",
          {{"inputs", {{"input", "input_sentences"}}},
           {"outputs", {{"output", "prefixed"}}}}}},
        {{"id", "matcher"},
         {"node_type", "TextRuleMatchNode"},
         {"depends_on", {"prefix"}},
         {"ports",
          {{"inputs", {{"text", "prefixed"}}},
           {"outputs", {{"matches", "rule_matches"}}}}},
         {"config", {{"categories", {{"PREFIX_APPLIED", {"VIP:sample"}}}}}}}}}};
  const nlohmann::json conf = {
      {"data",
       {{"pipe_path", "pipeline.json"},
        {"mem_que",
         {{"type", "keyword_out"},
          {"meta_num", 0},
          {"metadata_type_id", 0},
          {"capacities", {{"match_result_json", 2047}}}}}}}};
  std::ofstream(directory / "pipeline.json") << pipeline.dump(2);
  std::ofstream(directory / "pipeline.conf") << conf.dump(2);
}

}  // namespace llm_edgeflow::test
