#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/business_adapter_registry.h"
#include "company_alg_cpp.hpp"
#include "company_alg_interface.h"

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
// 1. Tagged Union & 非法枚举分支拦截测试 (ADP-001, ADP-011)
// ---------------------------------------------------------------------------
TEST_F(AdapterContractSecurityTest, TaggedUnionAndEnumValidation) {
  AdapterStatus status;
  std::vector<int> valid_enums = {1, 2, 3};

  // 合法枚举
  EXPECT_TRUE(AdapterValidationHelper::RequireEnum(
      "inputs[0].modal_type", 1, valid_enums, 0, "MultiModalBiz", &status));
  EXPECT_TRUE(status.IsOk());

  // 非法枚举 (例如 99)
  EXPECT_FALSE(AdapterValidationHelper::RequireEnum(
      "inputs[0].modal_type", 99, valid_enums, 0, "MultiModalBiz", &status));
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.SampleIndex(), 0);
  EXPECT_EQ(status.FieldPath(), "inputs[0].modal_type");
}

// ---------------------------------------------------------------------------
// 2. 嵌套变长数组与乘法溢出/超限测试 (ADP-001, ADP-011)
// ---------------------------------------------------------------------------
TEST_F(AdapterContractSecurityTest, NestedArrayAndIntegerOverflowProtection) {
  AdapterStatus status;
  struct DummyBox {
    float x, y, w, h;
  };

  // 正常乘法校验
  EXPECT_TRUE(AdapterValidationHelper::CheckedMultiply(
      "inputs[0].boxes", 100, sizeof(DummyBox), 1024 * 1024, 0, "DetectionBiz",
      &status));

  // 乘法溢出 (SIZE_MAX / sizeof(DummyBox) + 1)
  size_t overflow_count = (SIZE_MAX / sizeof(DummyBox)) + 1;
  EXPECT_FALSE(AdapterValidationHelper::CheckedMultiply(
      "inputs[0].boxes", overflow_count, sizeof(DummyBox), 1024 * 1024, 0,
      "DetectionBiz", &status));
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "inputs[0].boxes");

  // 超出最大总字节数限制 (1MB limit vs 2MB payload)
  size_t huge_count = (2 * 1024 * 1024) / sizeof(DummyBox);
  EXPECT_FALSE(AdapterValidationHelper::CheckedMultiply(
      "inputs[0].boxes", huge_count, sizeof(DummyBox), 1024 * 1024, 0,
      "DetectionBiz", &status));
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
}

// ---------------------------------------------------------------------------
// 3. COPY_IN 内存所有权隔离测试 (ADP-002, ADP-011)
// ---------------------------------------------------------------------------
TEST_F(AdapterContractSecurityTest, CopyInMemoryOwnershipIsolation) {
  std::string cfg_path = GetConfigPath("configs/pipeline_keyword_match.json");
  CompanyAlgParamCreate param;
  param.config_file_path = cfg_path.c_str();
  param.model_root_dir = "./models";
  param.device_id = 0;
  param.biz_type = ALG_BIZ_TYPE_KEYWORD_MATCH;

  void* handle = nullptr;
  int create_ret = Alg_Create(&handle, &param);
  ASSERT_EQ(create_ret, 0);
  ASSERT_NE(handle, nullptr);

  // 准备输入缓冲区
  char mutable_sentence[128];
  std::snprintf(mutable_sentence, sizeof(mutable_sentence), "%s",
                "系统正在进行安全初始化流程");

  CompanyKeywordInputStruct req{1001, mutable_sentence};
  CompanyKeywordOutputStruct out{};
  const void* inputs[1] = {&req};
  void* outputs[1] = {&out};
  int num_outputs = 1;

  // 执行算法 (Unpack 会执行 COPY_IN 深拷贝)
  int proc_ret = Alg_Process(handle, inputs, 1, outputs, &num_outputs);
  EXPECT_EQ(proc_ret, COMPANY_ALG_SUCCESS);
  EXPECT_EQ(out.is_hit, 1);  // 命中 "初始化"

  // 在调用后立即污染外部调用方内存
  std::memset(mutable_sentence, 'X', sizeof(mutable_sentence) - 1);
  mutable_sentence[sizeof(mutable_sentence) - 1] = '\0';

  // 再次传入相同 req (但内容已变) 验证独立性
  CompanyKeywordInputStruct req_garbled{1002, mutable_sentence};
  inputs[0] = &req_garbled;
  num_outputs = 1;
  proc_ret = Alg_Process(handle, inputs, 1, outputs, &num_outputs);
  EXPECT_EQ(proc_ret, COMPANY_ALG_SUCCESS);
  EXPECT_EQ(out.is_hit, 0);  // XXXXX 不会命中初始化关键词

  Alg_Destroy(handle);
}

// ---------------------------------------------------------------------------
// 4. 重排序业务候选集严格边界校验 (ADP-005)
// ---------------------------------------------------------------------------
TEST_F(AdapterContractSecurityTest, CrossRerankCandidateStrictValidation) {
  std::string cfg_path = GetConfigPath("configs/pipeline_cross_rerank.json");
  CompanyAlgParamCreate param;
  param.config_file_path = cfg_path.c_str();
  param.model_root_dir = "./models";
  param.device_id = 0;
  param.biz_type = ALG_BIZ_TYPE_CROSS_RERANK;

  void* handle = nullptr;
  int create_ret = Alg_Create(&handle, &param);
  ASSERT_EQ(create_ret, 0);

  const char* candidates[10] = {"p0", "p1", "p2", "p3", "p4",
                                "p5", "p6", "p7", "p8", "p9"};

  // Case A: candidate_count = 0 (非法，拒绝)
  CompanyRerankBatchInputStruct req_zero{};
  req_zero.request_id = 2001;
  req_zero.query_text = "query";
  req_zero.candidate_count = 0;
  const void* in_zero[1] = {&req_zero};
  CompanyRerankBatchOutputStruct out{};
  void* out_arr[1] = {&out};
  int num_out = 1;
  EXPECT_EQ(Alg_Process(handle, in_zero, 1, out_arr, &num_out),
            COMPANY_ALG_ERR_INVALID_INPUT);

  // Case B: candidate_count = 9 (超出 8 上限，必须拒绝而不是静默截断)
  CompanyRerankBatchInputStruct req_overflow{};
  req_overflow.request_id = 2002;
  req_overflow.query_text = "query";
  for (int i = 0; i < 8; ++i) {
    req_overflow.candidate_passages[i] = "p";
  }
  req_overflow.candidate_count = 9;
  const void* in_overflow[1] = {&req_overflow};
  num_out = 1;
  EXPECT_EQ(Alg_Process(handle, in_overflow, 1, out_arr, &num_out),
            COMPANY_ALG_ERR_INVALID_INPUT);

  // Case C: candidate_passages 包含空指针
  CompanyRerankBatchInputStruct req_null_elem{};
  req_null_elem.request_id = 2003;
  req_null_elem.query_text = "query";
  req_null_elem.candidate_passages[0] = "p0";
  req_null_elem.candidate_passages[1] = nullptr;
  req_null_elem.candidate_passages[2] = "p2";
  req_null_elem.candidate_count = 3;
  const void* in_null_elem[1] = {&req_null_elem};
  num_out = 1;
  EXPECT_EQ(Alg_Process(handle, in_null_elem, 1, out_arr, &num_out),
            COMPANY_ALG_ERR_INVALID_INPUT);

  Alg_Destroy(handle);
}

// ---------------------------------------------------------------------------
// 5. 音频业务参数严格校验 (ADP-005)
// ---------------------------------------------------------------------------
TEST_F(AdapterContractSecurityTest, AudioAsrStrictValidation) {
  std::string cfg_path =
      GetConfigPath("configs/pipeline_audio_asr_intent.json");
  CompanyAlgParamCreate param;
  param.config_file_path = cfg_path.c_str();
  param.model_root_dir = "./models";
  param.device_id = 0;
  param.biz_type = ALG_BIZ_TYPE_AUDIO_ASR_INTENT;

  void* handle = nullptr;
  int create_ret = Alg_Create(&handle, &param);
  ASSERT_EQ(create_ret, 0);

  // Case A: 采样率非法 (0 或负数，拒绝)
  float dummy_pcm[16] = {0.0f};
  CompanyAudioInputStruct req_bad_sr{3001, dummy_pcm, 16, -1};
  const void* in_bad_sr[1] = {&req_bad_sr};
  CompanyAudioOutputStruct out{};
  void* out_arr[1] = {&out};
  int num_out = 1;
  EXPECT_EQ(Alg_Process(handle, in_bad_sr, 1, out_arr, &num_out),
            COMPANY_ALG_ERR_INVALID_INPUT);

  // Case B: pcm_length > 0 但 pcm_buffer == nullptr (非法，拒绝)
  CompanyAudioInputStruct req_null_pcm{3002, nullptr, 100, 16000};
  const void* in_null_pcm[1] = {&req_null_pcm};
  num_out = 1;
  EXPECT_EQ(Alg_Process(handle, in_null_pcm, 1, out_arr, &num_out),
            COMPANY_ALG_ERR_INVALID_INPUT);

  // Case C: pcm_length < 0 (非法，拒绝)
  CompanyAudioInputStruct req_neg_len{3003, dummy_pcm, -10, 16000};
  const void* in_neg_len[1] = {&req_neg_len};
  num_out = 1;
  EXPECT_EQ(Alg_Process(handle, in_neg_len, 1, out_arr, &num_out),
            COMPANY_ALG_ERR_INVALID_INPUT);

  Alg_Destroy(handle);
}

// ---------------------------------------------------------------------------
// 6. Pipeline 配置与业务绑定不匹配强拦截 (ADP-006)
// ---------------------------------------------------------------------------
TEST_F(AdapterContractSecurityTest,
       PipelineBusinessBindingMismatchInAlgCreate) {
  // 用语音识别的 biz_type 去加载实体抽取的 Pipeline 配置
  std::string mismatched_cfg =
      GetConfigPath("configs/pipeline_entity_extract.json");
  CompanyAlgParamCreate param;
  param.config_file_path = mismatched_cfg.c_str();
  param.model_root_dir = "./models";
  param.device_id = 0;
  param.biz_type = ALG_BIZ_TYPE_AUDIO_ASR_INTENT;

  void* handle = nullptr;
  int ret = Alg_Create(&handle, &param);
  // 必须在创建阶段立即拦截并返回 -5 (COMPANY_ALG_ERR_UNSUPPORTED_BIZ)
  EXPECT_EQ(ret, COMPANY_ALG_ERR_UNSUPPORTED_BIZ);
  EXPECT_EQ(handle, nullptr);
}

// ---------------------------------------------------------------------------
// 7. Descriptor 单一事实源一致性校验 (ADP-008)
// ---------------------------------------------------------------------------
class InconsistentMockAdapter : public IBusinessAdapter {
 public:
  CompanyAlgBizType BizType() const override { return ALG_BIZ_TYPE_DOC_QA; }
  const char* BizName() const override { return "MockDocQA"; }

  const AdapterDescriptor& GetDescriptor() const override {
    // 故意返回与 BizType/BizName 不一致的 Descriptor
    static AdapterDescriptor desc{
        ALG_BIZ_TYPE_KEYWORD_MATCH, "DifferentName", "2.0.0", "In", "Out", 64};
    return desc;
  }

  int Unpack(const void**, int, AlgContext*) const override { return 0; }
  int Pack(AlgContext*, void**, int*) const override { return 0; }
};

TEST_F(AdapterContractSecurityTest,
       DescriptorSingleSourceOfTruthInconsistency) {
  auto mock_adapter = std::make_shared<InconsistentMockAdapter>();
  bool reg_res =
      BusinessAdapterRegistry::Instance().RegisterAdapter(mock_adapter);
  EXPECT_FALSE(reg_res);
  EXPECT_TRUE(BusinessAdapterRegistry::Instance().HasRegistrationConflict());
  EXPECT_FALSE(
      BusinessAdapterRegistry::Instance().GetRegistrationErrors().empty());
}

// ---------------------------------------------------------------------------
// 8. 适配器跨句柄并发无状态安全性验证 (ADP-003)
// ---------------------------------------------------------------------------
TEST_F(AdapterContractSecurityTest, ConcurrentStatelessAdapterExecution) {
  std::string cfg_path = GetConfigPath("configs/pipeline_keyword_match.json");

  auto run_thread = [&](int thread_id) {
    CompanyAlgParamCreate param;
    param.config_file_path = cfg_path.c_str();
    param.model_root_dir = "./models";
    param.device_id = 0;
    param.biz_type = ALG_BIZ_TYPE_KEYWORD_MATCH;

    void* handle = nullptr;
    int ret = Alg_Create(&handle, &param);
    ASSERT_EQ(ret, 0);

    for (int iter = 0; iter < 10; ++iter) {
      std::string text = "系统自检中初始化线程" + std::to_string(thread_id);
      CompanyKeywordInputStruct in{
          static_cast<uint64_t>(thread_id * 100 + iter), text.c_str()};
      CompanyKeywordOutputStruct out{};
      const void* inputs[1] = {&in};
      void* outputs[1] = {&out};
      int num_out = 1;

      int proc_ret = Alg_Process(handle, inputs, 1, outputs, &num_out);
      EXPECT_EQ(proc_ret, COMPANY_ALG_SUCCESS);
      EXPECT_EQ(out.is_hit, 1);
      EXPECT_EQ(out.request_id, in.request_id);
    }
    Alg_Destroy(handle);
  };

  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back(run_thread, t);
  }
  for (auto& th : threads) {
    th.join();
  }
}

}  // namespace alg_framework
