#pragma once

#include <nlohmann/json.hpp>

#include "core/pipeline_catalog.h"

namespace llm_edgeflow {

/**
 * @brief Integration 层的 Catalog 聚合门面
 *
 * 聚合 Core 的 PipelineCatalogSnapshot 与 Integration 的
 * IoConverterRegistry，生成对外统一 Catalog JSON。
 */
class IoCatalog {
 public:
  static nlohmann::json ToJson(const PipelineCatalogSnapshot& snapshot);

  static nlohmann::json ToJson();
};

}  // namespace llm_edgeflow
