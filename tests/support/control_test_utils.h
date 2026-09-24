#pragma once

#include <filesystem>
#include <fstream>

#include "nlohmann/json.hpp"

namespace llm_edgeflow::test {

// Uses the compiled authoring starter and the existing keyword I/O contract.
inline void WriteControlTestPipeline(const std::filesystem::path& directory) {
  const nlohmann::json pipeline = {
      {"deployment",
       {{"io",
         {{"io_binding", "keyword_match.operator.v1"},
          {"out_mem",
           {{"keyword_out",
             {{"meta_num", 0},
              {"metadata_type_id", 0},
              {"capacities", {{"match_result_json", 2047}}}}}}}}}}},
      {"models", nlohmann::json::array()},
      {"pipeline",
       {{{"id", "prefix"},
         {"node_type", "TestControlNode"},
         {"depends_on", nlohmann::json::array()},
         {"inputs", {{"input", "input_sentences"}}},
         {"outputs", {{"output", "prefixed"}}}},
        {{"id", "matcher"},
         {"node_type", "TextRuleMatchNode"},
         {"depends_on", {"prefix"}},
         {"inputs", {{"text", "prefixed"}}},
         {"outputs", {{"matches", "rule_matches"}}},
         {"config", {{"categories", {{"PREFIX_APPLIED", {"VIP:sample"}}}}}}}}}};
  const nlohmann::json conf = {{"pipe_path", "pipeline.json"}};
  std::ofstream(directory / "pipeline.json") << pipeline.dump(2);
  std::ofstream(directory / "pipeline.conf") << conf.dump(2);
}

}  // namespace llm_edgeflow::test
