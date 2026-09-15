#pragma once

#include <nlohmann/json.hpp>
#include <string>

#include "core/pipeline_catalog.h"

namespace llm_edgeflow {

/**
 * @brief Integration 层的 Catalog 聚合门面 (Schema Version 4)
 *
 * 聚合 Core 的 PipelineCatalogSnapshot 与 Integration 的
 * IoConverterRegistry 及 IoBindingRegistry，生成对外统一 Schema 4 Catalog JSON。
 */
class IoCatalog {
 public:
  static nlohmann::json ToJson(
      const PipelineCatalogSnapshot& snapshot,
      const std::string& biz_filter = std::string());

  static nlohmann::json ToJson(
      const std::string& biz_filter = std::string());
};

}  // namespace llm_edgeflow
