#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_adapter_registry.h"
#include "adapter/templates/flat_struct_adapter.h"
#include "adapter/templates/nested_array_adapter.h"
#include "adapter/templates/nested_pointer_tree_adapter.h"
#include "adapter/templates/tagged_union_adapter.h"
#include "company_alg_cpp.hpp"
#include "company_alg_interface.h"
#include "core/common_contracts.h"

namespace alg_framework {

static std::string GetConfigPath(const std::string& rel_path) {
  if (std::filesystem::exists(rel_path)) return rel_path;
  if (std::filesystem::exists("../" + rel_path)) return "../" + rel_path;
  return rel_path;
}

class AdapterContractSecurityTest : public ::testing::Test {
 protected:
  void SetUp() override {
    BusinessAdapterRegistry::Instance().ResetConflictForTesting();
    Alg_Init();
  }
  void TearDown() override {
    Alg_DeInit();
    BusinessAdapterRegistry::Instance().ResetConflictForTesting();
  }
};

// ---------------------------------------------------------------------------
// 1. Tagged Union & 模板适配器真实运行与非法枚举拦截 (ADP-001, ADP-010,
// RECHECK-005)
// ---------------------------------------------------------------------------
TEST_F(AdapterContractSecurityTest, TaggedUnionAndEnumValidation) {
  using namespace template_examples;
  TemplateTaggedUnionAdapter adapter;

  // 1.1 Helper 级校验
  AdapterStatus status;
  std::vector<int> valid_enums = {1, 2};
  EXPECT_TRUE(AdapterValidationHelper::RequireEnum(
      "inputs[0].payload_type", 1, valid_enums, 0, "MultiModalBiz", &status));
  EXPECT_TRUE(status.IsOk());

  EXPECT_FALSE(AdapterValidationHelper::RequireEnum(
      "inputs[0].payload_type", 99, valid_enums, 0, "MultiModalBiz", &status));
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.SampleIndex(), 0);
  EXPECT_EQ(status.FieldPath(), "inputs[0].payload_type");

  // 1.2 Tagged Union 适配器真实 Unpack 路径测试 (文本分支)
  TemplateTaggedUnionInput text_in;
  text_in.request_id = 1001;
  text_in.payload_type = TEMPLATE_PAYLOAD_TEXT;
  text_in.data.text.text_content = "Hello Tagged Union";

  const void* inputs[1] = {&text_in};
  AlgContext ctx;
  AdapterStatus unpack_status;
  int ret = adapter.Unpack(inputs, 1, &ctx, &unpack_status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);
  auto* items =
      ctx.Get<std::vector<TemplateUnionItemDto>>("tagged_union_items");
  ASSERT_NE(items, nullptr);
  ASSERT_EQ(items->size(), 1U);
  EXPECT_EQ((*items)[0].text_content, "Hello Tagged Union");

  // 1.3 非法枚举分支直接在真实 Unpack 中被拦截
  TemplateTaggedUnionInput invalid_in;
  invalid_in.request_id = 1002;
  invalid_in.payload_type = 999;  // 非法枚举
  const void* bad_inputs[1] = {&invalid_in};
  AlgContext bad_ctx;
  AdapterStatus bad_status;
  int bad_ret = adapter.Unpack(bad_inputs, 1, &bad_ctx, &bad_status);
  EXPECT_EQ(bad_ret, COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_FALSE(bad_status.IsOk());
  EXPECT_EQ(bad_status.SampleIndex(), 0);
  EXPECT_EQ(bad_status.FieldPath(), "inputs[i].payload_type");
}

// ---------------------------------------------------------------------------
// 2. 嵌套变长数组与乘法溢出/超限测试 (ADP-001, ADP-010, RECHECK-005)
// ---------------------------------------------------------------------------
TEST_F(AdapterContractSecurityTest, NestedArrayAndIntegerOverflowProtection) {
  using namespace template_examples;
  TemplateNestedArrayAdapter adapter;

  // 2.1 Helper 级乘法溢出校验
  AdapterStatus status;
  struct DummyBox {
    float x, y, w, h;
  };
  EXPECT_TRUE(AdapterValidationHelper::CheckedMultiply(
      "inputs[0].boxes", 100, sizeof(DummyBox), 1024 * 1024, 0, "DetectionBiz",
      &status));

  size_t overflow_count = (SIZE_MAX / sizeof(DummyBox)) + 1;
  EXPECT_FALSE(AdapterValidationHelper::CheckedMultiply(
      "inputs[0].boxes", overflow_count, sizeof(DummyBox), 1024 * 1024, 0,
      "DetectionBiz", &status));
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);

  // 2.2 嵌套数组适配器真实 Unpack 路径
  TemplateTagItem tags[2] = {{"tag_a", 0.9f}, {"tag_b", 0.5f}};
  TemplateNestedArrayInput array_in;
  array_in.request_id = 2001;
  array_in.tag_count = 2;
  array_in.tag_array = tags;

  const void* inputs[1] = {&array_in};
  AlgContext ctx;
  AdapterStatus unpack_status;
  int ret = adapter.Unpack(inputs, 1, &ctx, &unpack_status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);
  auto* array_items =
      ctx.Get<std::vector<TemplateNestedArrayItemDto>>("nested_array_items");
  ASSERT_NE(array_items, nullptr);
  ASSERT_EQ(array_items->size(), 1);
  EXPECT_EQ((*array_items)[0].tags.size(), 2);
  EXPECT_EQ((*array_items)[0].tags[0].tag_name, "tag_a");

  // 2.3 异常 count (<0 或 count > max) 拦截
  TemplateNestedArrayInput bad_array_in;
  bad_array_in.request_id = 2002;
  bad_array_in.tag_count = -5;  // 负数
  bad_array_in.tag_array = nullptr;
  const void* bad_inputs[1] = {&bad_array_in};
  AlgContext bad_ctx;
  AdapterStatus bad_status;
  int bad_ret = adapter.Unpack(bad_inputs, 1, &bad_ctx, &bad_status);
  EXPECT_EQ(bad_ret, COMPANY_ALG_ERR_INVALID_INPUT);
}

// ---------------------------------------------------------------------------
// 3. 多级嵌套指针树与最大深度递归栈保护测试 (ADP-001, ADP-010, RECHECK-005)
// ---------------------------------------------------------------------------
TEST_F(AdapterContractSecurityTest, NestedPointerTreeDepthProtection) {
  using namespace template_examples;
  TemplateNestedPointerTreeAdapter adapter;

  // 3.1 正常二层树
  TemplateTreeNode child1{101, "child_node_1", 0, nullptr};
  TemplateTreeNode child2{102, "child_node_2", 0, nullptr};
  const TemplateTreeNode* root_children[2] = {&child1, &child2};
  TemplateTreeNode root{100, "root_node", 2, root_children};

  TemplateNestedTreeInput tree_in{3001, &root};
  const void* inputs[1] = {&tree_in};
  AlgContext ctx;
  AdapterStatus status;
  int ret = adapter.Unpack(inputs, 1, &ctx, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);
  auto* tree_dtos = ctx.Get<std::vector<TemplateTreeNodeDto>>("tree_root_dtos");
  ASSERT_NE(tree_dtos, nullptr);
  ASSERT_EQ(tree_dtos->size(), 1);
  EXPECT_EQ((*tree_dtos)[0].children.size(), 2);

  // 3.2 空子节点指针拦截
  const TemplateTreeNode* bad_children[2] = {&child1, nullptr};
  TemplateTreeNode bad_root{100, "root_node", 2, bad_children};
  TemplateNestedTreeInput bad_tree_in{3002, &bad_root};
  const void* bad_inputs[1] = {&bad_tree_in};
  AlgContext bad_ctx;
  AdapterStatus bad_status;
  int bad_ret = adapter.Unpack(bad_inputs, 1, &bad_ctx, &bad_status);
  EXPECT_EQ(bad_ret, COMPANY_ALG_ERR_INVALID_INPUT);
}

// ---------------------------------------------------------------------------
// 4. COPY_IN 内存所有权深度隔离测试 (ADP-002, RECHECK-006)
// ---------------------------------------------------------------------------
TEST_F(AdapterContractSecurityTest, DirectUnpackMemoryIsolation) {
  auto adapter = BusinessAdapterRegistry::Instance().GetAdapter(
      ALG_BIZ_TYPE_KEYWORD_MATCH);
  ASSERT_NE(adapter, nullptr);

  // 创建动态可修改的原始缓冲区
  char caller_buf[256];
  snprintf(caller_buf, sizeof(caller_buf), "设备系统初始化自检正常");

  CompanyKeywordInputStruct in_struct;
  in_struct.request_id = 9999;
  in_struct.sentence_text = caller_buf;

  const void* inputs[1] = {&in_struct};
  AlgContext ctx;
  AdapterStatus status;
  int unpack_ret = adapter->Unpack(inputs, 1, &ctx, &status);
  ASSERT_EQ(unpack_ret, COMPANY_ALG_SUCCESS);

  // 立即篡改调用方内存 Buffer (例如 memset 覆盖为 'X')
  std::memset(caller_buf, 'X', sizeof(caller_buf) - 1);
  caller_buf[sizeof(caller_buf) - 1] = '\0';

  // 验证 AlgContext 中的 DTO 保持原有数据完全不受外界内存修改影响 (物理深拷贝)
  auto* sentences = ctx.Get(kInputSentences);
  ASSERT_NE(sentences, nullptr);
  ASSERT_EQ(sentences->size(), 1U);
  EXPECT_EQ((*sentences)[0].data, "设备系统初始化自检正常");
  EXPECT_NE((*sentences)[0].data, std::string(caller_buf));
}

// ---------------------------------------------------------------------------
// 5. 输出字符串截断拒绝测试 (RECHECK-001)
// ---------------------------------------------------------------------------
TEST_F(AdapterContractSecurityTest, OutputStringTruncationRejection) {
  using namespace template_examples;
  TemplateFlatStructAdapter adapter;

  AlgContext ctx;
  std::vector<TemplateFlatResultDto> results;
  // 构造长度超过 512 字节的超长 JSON 结果
  std::string huge_json(1024, 'A');
  results.push_back({5001, 0, huge_json});
  ctx.Set("flat_final_outputs", results);

  TemplateFlatOutput out_slot;
  void* outputs[1] = {&out_slot};
  int num_outputs = 1;
  AdapterStatus status;

  // 预期必须返回 COMPANY_ALG_ERR_BUFFER_TOO_SMALL (-4)，拒绝静默假装成功
  int pack_ret = adapter.Pack(&ctx, outputs, &num_outputs, &status);
  EXPECT_EQ(pack_ret, COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_FALSE(status.IsOk());
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
}

// ---------------------------------------------------------------------------
// 6. Pipeline 绑定精确白名单与 Fail-Closed 校验 (RECHECK-002)
// ---------------------------------------------------------------------------
TEST_F(AdapterContractSecurityTest, PipelineBindingFailClosedAndExactMatch) {
  auto adapter = BusinessAdapterRegistry::Instance().GetAdapter(
      ALG_BIZ_TYPE_KEYWORD_MATCH);
  ASSERT_NE(adapter, nullptr);

  // 6.1 精确匹配成功
  EXPECT_TRUE(adapter->ValidatePipelineBinding("keyword_match_v1"));

  // 6.2 包含子串的伪造名称 / 大小写不匹配 / 空白名称均严格拒绝 (Fail-Closed)
  EXPECT_FALSE(adapter->ValidatePipelineBinding("keyword_match_v1_fake"));
  EXPECT_FALSE(adapter->ValidatePipelineBinding("my_keyword_match_v1"));
  EXPECT_FALSE(adapter->ValidatePipelineBinding("KEYWORD_MATCH_V1"));
  EXPECT_FALSE(adapter->ValidatePipelineBinding(""));
  EXPECT_FALSE(
      adapter->ValidatePipelineBinding("dialogue_compliance_audit_v1"));

  // 6.3 Alg_Create 阶段使用串用配置创建句柄立即失败 (-5)
  std::string wrong_cfg = GetConfigPath("configs/pipeline_dialogue_audit.json");
  CompanyAlgParamCreate param;
  param.config_file_path = wrong_cfg.c_str();
  param.model_root_dir = "./models";
  param.device_id = 0;
  param.biz_type = ALG_BIZ_TYPE_KEYWORD_MATCH;  // 业务是 KeywordMatch，但配置是
                                                // ComplianceAudit

  void* handle = nullptr;
  int create_ret = Alg_Create(&handle, &param);
  EXPECT_EQ(create_ret, -5);
  EXPECT_EQ(handle, nullptr);
}

// ---------------------------------------------------------------------------
// 7. Registry 拒绝不支持的 Descriptor 策略组合 (RECHECK-003)
// ---------------------------------------------------------------------------
TEST_F(AdapterContractSecurityTest, RegistryRejectsUnsupportedPolicies) {
  class UnsupportedPolicyAdapter : public IBusinessAdapter {
   public:
    CompanyAlgBizType BizType() const override {
      return static_cast<CompanyAlgBizType>(201);
    }
    const char* BizName() const override { return "UnsupportedPolicy"; }
    const AdapterDescriptor& GetDescriptor() const override {
      static AdapterDescriptor desc{
          static_cast<CompanyAlgBizType>(201),
          "UnsupportedPolicy",
          "2.0.0",
          "In",
          "Out",
          64,
          OwnershipPolicy::kBorrowDuringProcess,  // 当前未开放策略
          ThreadModel::kStatelessThreadSafe,
          OutputCardinality::kOneToOne,
          {BusinessDefinition{"UnsupportedPolicy", "pipeline_v1"}}};
      return desc;
    }
    int Unpack(const void** i, int n, AlgContext* c,
               AdapterStatus* s) const override {
      (void)i;
      (void)n;
      (void)c;
      (void)s;
      return 0;
    }
    int Pack(AlgContext* c, void** o, int* n, AdapterStatus* s) const override {
      (void)c;
      (void)o;
      (void)n;
      (void)s;
      return 0;
    }
  };

  auto bad_adapter = std::make_shared<UnsupportedPolicyAdapter>();
  bool reg_ret =
      BusinessAdapterRegistry::Instance().RegisterAdapter(bad_adapter);
  EXPECT_FALSE(reg_ret);
  EXPECT_TRUE(BusinessAdapterRegistry::Instance().HasRegistrationConflict());
}

// ---------------------------------------------------------------------------
// 8. 结构化诊断工具与有界字符串扫描测试 (RECHECK-004)
// ---------------------------------------------------------------------------
TEST_F(AdapterContractSecurityTest, StructuredStatusAndBoundedStringScan) {
  AdapterStatus status;

  // 正常字符串
  EXPECT_TRUE(AdapterValidationHelper::RequireBoundedString(
      "inputs[0].text", "normal text", 100, 0, "TestBiz", &status));
  EXPECT_TRUE(status.IsOk());

  // 超长字符串拦截
  EXPECT_FALSE(AdapterValidationHelper::RequireBoundedString(
      "inputs[0].text", "a very very long text exceeding limit", 10, 0,
      "TestBiz", &status));
  EXPECT_FALSE(status.IsOk());
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "inputs[0].text");

  // 验证诊断字符串包含丰富定位元数据
  std::string diag = status.ToString();
  EXPECT_NE(diag.find("TestBiz"), std::string::npos);
  EXPECT_NE(diag.find("inputs[0].text"), std::string::npos);
  EXPECT_NE(diag.find("sample [0]"), std::string::npos);
}

// ---------------------------------------------------------------------------
// 9. 多线程共享 Adapter 无状态并发安全性测试 (ADP-003, RECHECK-006)
// ---------------------------------------------------------------------------
TEST_F(AdapterContractSecurityTest, ConcurrentStatelessAdapterExecution) {
  std::string cfg_path = GetConfigPath("configs/pipeline_keyword_match.json");
  CompanyAlgParamCreate param;
  param.config_file_path = cfg_path.c_str();
  param.model_root_dir = "./models";
  param.device_id = 0;
  param.biz_type = ALG_BIZ_TYPE_KEYWORD_MATCH;

  constexpr int kNumThreads = 8;
  constexpr int kNumIters = 10;
  std::vector<std::thread> workers;

  for (int t = 0; t < kNumThreads; ++t) {
    workers.emplace_back([&, t]() {
      void* hndl = nullptr;
      int create_ret = Alg_Create(&hndl, &param);
      ASSERT_EQ(create_ret, 0);
      ASSERT_NE(hndl, nullptr);

      for (int it = 0; it < kNumIters; ++it) {
        CompanyKeywordInputStruct in_req;
        in_req.request_id = t * 1000 + it;
        std::string query = "系统初始化与设备自检请求 #" + std::to_string(t);
        in_req.sentence_text = query.c_str();

        CompanyKeywordOutputStruct out_res;
        const void* in_arr[1] = {&in_req};
        void* out_arr[1] = {&out_res};
        int num_outs = 1;

        int proc_ret = Alg_Process(hndl, in_arr, 1, out_arr, &num_outs);
        EXPECT_EQ(proc_ret, 0);
        EXPECT_EQ(out_res.request_id, in_req.request_id);
        EXPECT_EQ(out_res.is_hit, 1);
      }

      Alg_Destroy(hndl);
    });
  }

  for (auto& w : workers) {
    if (w.joinable()) w.join();
  }
}

}  // namespace alg_framework
