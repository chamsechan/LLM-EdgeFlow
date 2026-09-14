#include "adapter/text_carrier.h"

namespace llm_edgeflow {

void RegisterTranslateBridge() {
  RegisterOperatorBizBridge(MakeTextCarrierBridge(
      ALG_BIZ_TYPE_TRANSLATE, "Translate", "builtin.translate"));
}

REGISTER_OPERATOR_BIZ_BRIDGE(RegisterTranslateBridge);

}  // namespace llm_edgeflow
