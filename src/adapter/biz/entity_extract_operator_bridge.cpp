#include "adapter/text_carrier.h"

namespace llm_edgeflow {

void RegisterEntityExtractBridge() {
  RegisterOperatorBizBridge(MakeTextCarrierBridge(
      ALG_BIZ_TYPE_ENTITY_EXTRACT, "EntityExtract", "builtin.entity_extract"));
}

REGISTER_OPERATOR_BIZ_BRIDGE(RegisterEntityExtractBridge);

}  // namespace llm_edgeflow
