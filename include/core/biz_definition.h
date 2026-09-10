#pragma once

#include <string>
#include <utility>
#include <vector>

#include "core/port_definition.h"

namespace llm_edgeflow {

struct BizDefinition {
  std::string biz_name;
  std::string demo_biz;
  std::string display_name;
  std::vector<BizPortDefinition> ingress;
  std::vector<BizPortDefinition> egress;

  BizDefinition() = default;
  BizDefinition(std::string name, std::string demo, std::string display = {},
                std::vector<BizPortDefinition> in = {},
                std::vector<BizPortDefinition> out = {})
      : biz_name(std::move(name)),
        demo_biz(std::move(demo)),
        display_name(std::move(display)),
        ingress(std::move(in)),
        egress(std::move(out)) {}
};

}  // namespace llm_edgeflow
