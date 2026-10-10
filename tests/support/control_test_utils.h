#pragma once

#include <filesystem>
#include <fstream>

#include "nlohmann/json.hpp"

namespace llm_edgeflow::test {

// 使用已编译的编写入门模板和现有的 keyword I/O 契约。
inline void WriteControlTestPipeline(const std::filesystem::path& directory) {
  const nlohmann::json pipeline = {
      {"io",
       {{"input", {{{"type", "keyword_in"}, {"name", "keyword_match"}}}},
        {"output",
         {{{"type", "keyword_out"},
           {"name", "keyword_match"},
           {"inputs", {{"matches", "matcher.matches"}}}}}}}},
      {"models", nlohmann::json::array()},
      {"pipeline",
       {{{"name", "prefix"},
         {"type", "test_control"},
         {"depends_on", nlohmann::json::array()},
         {"inputs", {{"input", "input.sentence_text"}}}},
        {{"name", "matcher"},
         {"type", "text_rule_match"},
         {"depends_on", {"prefix"}},
         {"inputs", {{"text", "prefix.output"}}},
         {"params", {{"categories", {{"PREFIX_APPLIED", {"VIP:sample"}}}}}}}}}};
  const nlohmann::json conf = {{"pipe_path", "pipeline.json"}};
  std::ofstream(directory / "pipeline.json") << pipeline.dump(2);
  std::ofstream(directory / "pipeline.conf") << conf.dump(2);
}

}  // namespace llm_edgeflow::test
