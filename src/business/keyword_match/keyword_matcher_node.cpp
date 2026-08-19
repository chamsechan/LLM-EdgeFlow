#include <cstring>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "business/keyword_match/keyword_match_dto.h"
#include "core/node_base.h"
#include "core/node_registry.h"

namespace alg_framework {

/**
 * @brief 关注词匹配算子 (无需加载模型，纯规则与内存字典)
 *
 * 业务特点：
 * 1. 纯 CPU / 规则处理，零模型依赖；
 * 2. 支持通过 Control 接口动态下发并热更新关注词表；
 * 3. 读写安全保护 (std::shared_mutex 读读并发、写写互斥)；
 * 4. 输入固定/可变长度句子 vector，输出命中类别与词汇 JSON。
 */
class KeywordMatcherNode : public INode {
 public:
  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    (void)session_ctx;
    // 从初始化配置加载默认词表 (可选)
    if (config.contains("default_categories") &&
        config["default_categories"].is_object()) {
      UpdateCategoryKeywords(config["default_categories"]);
    }
    std::cout << "[KeywordMatcherNode] Initialized with "
              << category_keywords_map_.size() << " initial categories."
              << std::endl;
    return true;
  }

  int Control(int cmd, const std::string& json_param) override {
    if (cmd == 1) {  // 1: 更新关注词表
      try {
        nlohmann::json root = nlohmann::json::parse(json_param);
        if (root.contains("categories") && root["categories"].is_object()) {
          UpdateCategoryKeywords(root["categories"]);
          std::cout << "[KeywordMatcherNode] Dynamically updated categories "
                       "via Control! Total categories: "
                    << category_keywords_map_.size() << std::endl;
          return 0;
        }
      } catch (const std::exception& e) {
        std::cerr << "[KeywordMatcherNode] Control JSON parse error: "
                  << e.what() << std::endl;
        return -1;
      }
    }
    return 0;
  }

  int Process(AlgContext* req_ctx) override {
    auto* sentences = req_ctx->Get<std::vector<std::string>>("input_sentences");
    auto* req_ids = req_ctx->Get<std::vector<uint64_t>>("raw_request_ids");

    if (!sentences || !req_ids) {
      req_ctx->SetError(
          -5001,
          "KeywordMatcherNode: Missing input_sentences or raw_request_ids");
      return -5001;
    }

    size_t batch_size = sentences->size();
    std::vector<KeywordMatchResult> outputs(batch_size);

    std::shared_lock<std::shared_mutex> lock(rw_mutex_);

    for (size_t i = 0; i < batch_size; ++i) {
      outputs[i].request_id = (*req_ids)[i];
      outputs[i].status_code = 0;

      const std::string& sentence = (*sentences)[i];
      nlohmann::json matches_array = nlohmann::json::array();

      // 在当前词表中遍历匹配
      for (const auto& [category, words] : category_keywords_map_) {
        for (const auto& w : words) {
          if (sentence.find(w) != std::string::npos) {
            nlohmann::json match_item;
            match_item["category"] = category;
            match_item["matched_word"] = w;
            matches_array.push_back(match_item);
          }
        }
      }

      nlohmann::json result_json;
      if (matches_array.empty()) {
        // 未命中：输出空列表 JSON
        outputs[i].is_hit = 0;
        result_json["matches"] = nlohmann::json::array();
      } else {
        // 命中：输出命中详情
        outputs[i].is_hit = 1;
        result_json["matches"] = matches_array;
      }

      outputs[i].match_result_json = result_json.dump();
    }

    std::cout << "[KeywordMatcherNode] Processed batch of " << batch_size
              << " sentences." << std::endl;
    req_ctx->Set("keyword_match_outputs", std::move(outputs));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = "KeywordMatcherNode";
    return name;
  }

 private:
  void UpdateCategoryKeywords(const nlohmann::json& categories_json) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    category_keywords_map_.clear();
    for (auto it = categories_json.begin(); it != categories_json.end(); ++it) {
      if (it.value().is_array()) {
        std::vector<std::string> words;
        for (const auto& w : it.value()) {
          if (w.is_string()) {
            words.push_back(w.get<std::string>());
          }
        }
        category_keywords_map_[it.key()] = std::move(words);
      }
    }
  }

 private:
  mutable std::shared_mutex rw_mutex_;
  // 节点私有词表映射：类别 -> 词列表
  std::unordered_map<std::string, std::vector<std::string>>
      category_keywords_map_;
};

REGISTER_NODE(KeywordMatcherNode);

}  // namespace alg_framework
