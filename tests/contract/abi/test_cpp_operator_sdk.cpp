/**
 * @file test_cpp_operator_sdk.cpp
 * @brief 公开 C++ Operator SDK 的消费方测试。
 *
 * 验证外部 C++ 消费方只使用公开 SDK 头文件即可链接 llm_edgeflow::sdk，
 * 不依赖任何内部头文件、运行时对象或测试 mock。
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "edgeflow/export.h"
#include "edgeflow/log.h"
#include "edgeflow/operator/interface.h"
#include "edgeflow/operator/types.h"
#include "edgeflow/version.h"
#include "platform_mock/error_codes.h"
#include "platform_mock/operator_data_types.h"
#include "platform_mock/operator_types.h"

// 公开布局与契约断言
static_assert(sizeof(CompanyString) == sizeof(int32_t) + sizeof(char*) +
                                           (sizeof(char*) == 8 ? 4 : 0),
              "CompanyString memory layout check");
static_assert(COMPANY_OPERATOR_MAX_RERANK_CANDIDATES == 8,
              "COMPANY_OPERATOR_MAX_RERANK_CANDIDATES must be 8");
static_assert(E_ALG_BASE_LOG_LEVEL_FATAL == 0, "Fatal log level must remain 0");
static_assert(E_ALG_BASE_LOG_LEVEL_WARNING == 2,
              "Warning log level must remain 2");
static_assert(E_ALG_BASE_LOG_LEVEL_VERBOSE == 5,
              "Verbose log level must remain 5");

int main() {
  // 1. 版本契约检查
  if (std::strcmp(COMPANY_ALG_PRODUCT_VERSION, "11.0.0") != 0) {
    std::fprintf(stderr,
                 "[SDK Consumer Test] Generated version contract drifted\n");
    return 1;
  }

  // 2. 日志 API 检查
  if (AlgBase_getLogLevelByName("LLM_EDGEFLOW") !=
      E_ALG_BASE_LOG_LEVEL_WARNING) {
    std::fprintf(stderr,
                 "[SDK Consumer Test] Public log default must be WARNING\n");
    return 2;
  }
  if (AlgBase_setLogLevelByName("LLM_EDGEFLOW", E_ALG_BASE_LOG_LEVEL_WARNING) !=
          0 ||
      AlgBase_getLogLevelByName("LLM_EDGEFLOW") !=
          E_ALG_BASE_LOG_LEVEL_WARNING) {
    std::fprintf(stderr,
                 "[SDK Consumer Test] Failed to configure public log API\n");
    return 3;
  }
  ALG_LOG_DEBUG("This debug record is filtered\n");
  ALG_LOG_WARNING("[SDK Consumer Test] Public log macro is operational\n");

  // 3. 检查 Operator 函数表
  auto op = llm_edgeflow::operator_api::Get_LLM_EDGEFLOW_OperatorTable();
  if (!op.Init || !op.Create || !op.Process || !op.Control || !op.Destroy ||
      !op.DeInit) {
    std::fprintf(
        stderr,
        "[SDK Consumer Test] Operator function table has null entries\n");
    return 4;
  }

  // 4. Init
  if (op.Init() != 0) {
    std::fprintf(stderr, "[SDK Consumer Test] op.Init failed\n");
    return 5;
  }

  // 定位 configs 目录
  const char* root_candidates[] = {".", "..", "../.."};
  const char* config_rel = "configs/pipeline_keyword_match_rules.conf";
  std::string root_dir;
  for (const char* r : root_candidates) {
    std::string test_p = std::string(r) + "/" + config_rel;
    FILE* fp = std::fopen(test_p.c_str(), "r");
    if (fp) {
      std::fclose(fp);
      root_dir = r;
      break;
    }
  }
  if (root_dir.empty()) {
    std::fprintf(stderr, "[SDK Consumer Test] Could not find %s\n", config_rel);
    return 6;
  }

  // 5. ResolveOperatorConfigIo
  char err_buf[512] = {0};
  llm_edgeflow::operator_api::OperatorIoContract resolved_io;
  int val_ret = llm_edgeflow::operator_api::ResolveOperatorConfigIo(
      root_dir.c_str(), config_rel, &resolved_io, err_buf, sizeof(err_buf));
  if (val_ret != 0 || resolved_io.inputs.size() != 1 ||
      resolved_io.outputs.size() != 1 ||
      resolved_io.inputs[0].type != "keyword_in" ||
      resolved_io.outputs[0].type != "keyword_out" ||
      resolved_io.inputs[0].name != "keyword_match" ||
      resolved_io.outputs[0].name != "keyword_match" ||
      resolved_io.inputs[0].service_type != kMockServiceKeywordMatch ||
      resolved_io.outputs[0].service_type != kMockServiceKeywordMatch) {
    std::fprintf(stderr,
                 "[SDK Consumer Test] ResolveOperatorConfigIo failed: %s\n",
                 err_buf);
    return 7;
  }

  // 6. Create
  llm_edgeflow::operator_api::CreateParam create_param{};
  create_param.model_path = root_dir.c_str();
  create_param.cfg_file_name = config_rel;
  create_param.device_id = 0;
  create_param.compute_platform =
      llm_edgeflow::operator_api::ComputePlatform::kCpu;
  create_param.max_frame_depth = 25;

  void* handle = nullptr;
  if (op.Create(&handle, &create_param) != 0 || !handle) {
    std::fprintf(stderr, "[SDK Consumer Test] op.Create failed: %s\n",
                 llm_edgeflow::operator_api::GetOperatorLastError());
    return 8;
  }

  // 7. Process
  std::string text1 = "客户要求加急处理VIP订单";
  CompanyString cs1{static_cast<int32_t>(text1.size()),
                    const_cast<char*>(text1.data())};
  CompanyOperatorKeywordInput in_req1{};
  in_req1.request_id = 1001;
  in_req1.service_type = kMockServiceKeywordMatch;
  in_req1.sentence_text = &cs1;

  llm_edgeflow::operator_api::NamedIoBatch inputs(1);
  inputs[0]["client_channel.keyword_in"] =
      llm_edgeflow::operator_api::MakeBorrowedOperatorInput(&in_req1);

  llm_edgeflow::operator_api::NamedIoBatch outputs(1);
  outputs[0]["client_channel.keyword_out"] = nullptr;

  if (op.Process(handle, inputs, outputs) != 0) {
    std::fprintf(stderr, "[SDK Consumer Test] op.Process failed: %s\n",
                 llm_edgeflow::operator_api::GetOperatorLastError());
    return 9;
  }

  auto out_sp = outputs[0]["client_channel.keyword_out"];
  if (!out_sp) {
    std::fprintf(
        stderr,
        "[SDK Consumer Test] Output slot client_channel.keyword_out is null\n");
    return 10;
  }
  auto* out_dto = static_cast<CompanyOperatorKeywordOutput*>(out_sp.get());
  if (out_dto->request_id != 1001 || out_dto->status_code != 0 ||
      out_dto->service_type != kMockServiceKeywordMatch) {
    std::fprintf(
        stderr,
        "[SDK Consumer Test] Unexpected output values: req_id=%lu status=%d\n",
        static_cast<unsigned long>(out_dto->request_id), out_dto->status_code);
    return 11;
  }

  // 8. Control：更新规则类别
  llm_edgeflow::operator_api::ControlUpdateRulesParam rules_param{
      "{\"categories\":{\"URGENT\":[\"加急\"]}}"};
  if (op.Control(handle,
                 llm_edgeflow::operator_api::ControlCommand::kUpdateRules,
                 &rules_param) != 0) {
    std::fprintf(stderr, "[SDK Consumer Test] op.Control failed: %s\n",
                 llm_edgeflow::operator_api::GetOperatorLastError());
    return 12;
  }

  // 9. 再次 Process，验证规则变更已生效
  outputs[0]["client_channel.keyword_out"] = nullptr;
  if (op.Process(handle, inputs, outputs) != 0) {
    std::fprintf(stderr, "[SDK Consumer Test] Second op.Process failed: %s\n",
                 llm_edgeflow::operator_api::GetOperatorLastError());
    return 13;
  }
  out_sp = outputs[0]["client_channel.keyword_out"];
  out_dto = static_cast<CompanyOperatorKeywordOutput*>(out_sp.get());
  if (out_dto->is_hit != 1) {
    std::fprintf(
        stderr, "[SDK Consumer Test] Expected keyword hit after rule update\n");
    return 14;
  }

  // 10. 复制字段，并在 Destroy 前释放所有租约
  std::string match_copy;
  if (out_dto->match_result_json && out_dto->match_result_json->data) {
    match_copy.assign(out_dto->match_result_json->data,
                      out_dto->match_result_json->length);
  }
  out_sp.reset();
  outputs.clear();
  inputs.clear();

  // 11. Destroy 句柄
  if (op.Destroy(handle) != 0) {
    std::fprintf(stderr, "[SDK Consumer Test] op.Destroy failed: %s\n",
                 llm_edgeflow::operator_api::GetOperatorLastError());
    return 15;
  }

  // 12. DeInit
  if (op.DeInit() != 0) {
    std::fprintf(stderr, "[SDK Consumer Test] op.DeInit failed\n");
    return 16;
  }

  std::printf(
      "[SDK Consumer Test] All C++ Operator SDK consumer tests passed.\n");
  return 0;
}
