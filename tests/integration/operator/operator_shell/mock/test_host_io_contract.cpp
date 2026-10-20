#include <gtest/gtest.h>

#include "edgeflow/operator/interface.h"

namespace llm_edgeflow {
namespace {

// 只读解析返回 mock 宿主结构名和业务服务码；转换器身份由通用套件覆盖。
TEST(MockHostIoContractTest, ResolutionReturnsNativeHostTypesAndServices) {
  operator_api::OperatorIoContract contract;
  ASSERT_EQ(operator_api::ResolveOperatorConfigIo(
                ".", "configs/pipeline_keyword_match_rules.conf", &contract),
            0);
  ASSERT_EQ(contract.inputs.size(), 1u);
  ASSERT_EQ(contract.outputs.size(), 1u);
  EXPECT_EQ(contract.inputs[0].type, "keyword_in");
  EXPECT_EQ(contract.inputs[0].name, "keyword_match");
  EXPECT_EQ(contract.inputs[0].type_name, "MockInput");
  // 文本 envelope 不带 service_type。
  EXPECT_FALSE(contract.inputs[0].service_type);
  EXPECT_EQ(contract.outputs[0].type, "json_out");
  EXPECT_EQ(contract.outputs[0].type_name, "MockOutput");
  EXPECT_EQ(contract.outputs[0].service_type, kMockServiceKeywordMatch);
}

}  // namespace
}  // namespace llm_edgeflow
