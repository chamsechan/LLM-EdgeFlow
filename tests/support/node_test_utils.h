#pragma once

#include <nlohmann/json.hpp>

#include "core/node_base.h"
#include "core/session_context.h"

namespace llm_edgeflow {

inline bool InitNodeForTest(INode& node, const nlohmann::json& config,
                            SessionContext* session_ctx) {
  NodeInitContext init_ctx;
  init_ctx.config = &config;
  init_ctx.session_ctx = session_ctx;
  return node.Init(init_ctx);
}

}  // namespace llm_edgeflow
