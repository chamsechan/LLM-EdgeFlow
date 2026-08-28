/**
 * @file test_c11_abi_compliance.c
 * @brief Pure C11 compilation and runtime ABI compliance test.
 *
 * This file is compiled with a pure C compiler (C11 standard) to guarantee
 * that include/company_alg_interface.h exposes zero C++ symbols or STL
 * dependencies.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "company_alg_interface.h"
#include "company_alg_log.h"
#include "operator/company_operator_types.h"

_Static_assert(sizeof(CompanyAlgBizType) == sizeof(int32_t),
               "CompanyAlgBizType must remain a 32-bit C ABI type");
_Static_assert(ALG_BIZ_TYPE_MAX_GUARD == INT32_MAX,
               "CompanyAlgBizType ABI guard must remain INT32_MAX");
_Static_assert(sizeof(CompanyString) == sizeof(int32_t) + sizeof(char*) +
                                            (sizeof(char*) == 8 ? 4 : 0),
               "CompanyString memory layout check");
_Static_assert(COMPANY_OPERATOR_MAX_RERANK_CANDIDATES == 8,
               "COMPANY_OPERATOR_MAX_RERANK_CANDIDATES must be 8");
_Static_assert(E_ALG_BASE_LOG_LEVEL_FATAL == 0,
               "Fatal log level must remain 0");
_Static_assert(E_ALG_BASE_LOG_LEVEL_WARNING == 2,
               "Warning log level must remain 2");
_Static_assert(E_ALG_BASE_LOG_LEVEL_VERBOSE == 5,
               "Verbose log level must remain 5");

int main(void) {
  if (AlgBase_getLogLevelByName("LLM_EDGEFLOW") !=
      E_ALG_BASE_LOG_LEVEL_WARNING) {
    fprintf(stderr, "[C11 ABI Test] Public log default must be WARNING\n");
    return 11;
  }
  if (AlgBase_setLogLevelByName("LLM_EDGEFLOW", E_ALG_BASE_LOG_LEVEL_WARNING) !=
          0 ||
      AlgBase_getLogLevelByName("LLM_EDGEFLOW") !=
          E_ALG_BASE_LOG_LEVEL_WARNING) {
    fprintf(stderr, "[C11 ABI Test] Failed to configure public log API\n");
    return 12;
  }
  ALG_LOG_DEBUG("This C11 debug record is filtered\n");
  ALG_LOG_WARNING("[C11 ABI Test] Public log macro is operational\n");

  printf("[C11 ABI Test] Testing pure C ABI lifecycle and safety...\n");

  // 1. Test Alg_Init and Alg_DeInit
  if (Alg_Init() != 0) {
    fprintf(stderr, "[C11 ABI Test] Alg_Init failed\n");
    return 1;
  }

  // 2. Test Null Pointer Safety
  if (Alg_Create(NULL, NULL) == 0) {
    fprintf(stderr, "[C11 ABI Test] Alg_Create should fail on null pointers\n");
    return 2;
  }

  int num_out = 0;
  if (Alg_Process(NULL, NULL, 0, NULL, &num_out) == 0) {
    fprintf(stderr, "[C11 ABI Test] Alg_Process should fail on null handle\n");
    return 3;
  }

  if (Alg_Control(NULL, NULL) == 0) {
    fprintf(stderr, "[C11 ABI Test] Alg_Control should fail on null handle\n");
    return 4;
  }

  if (Alg_Destroy(NULL) == 0) {
    fprintf(stderr, "[C11 ABI Test] Alg_Destroy should fail on null handle\n");
    return 5;
  }

  // 3. Test handle creation and processing with keyword matching
  CompanyAlgParamCreate param;
  memset(&param, 0, sizeof(param));

  // Determine config path
  const char* cfg_candidates[] = {"configs/pipeline_keyword_match.json",
                                  "../configs/pipeline_keyword_match.json",
                                  "../../configs/pipeline_keyword_match.json"};
  const char* cfg_path = NULL;
  for (int i = 0; i < 3; ++i) {
    FILE* f = fopen(cfg_candidates[i], "r");
    if (f) {
      fclose(f);
      cfg_path = cfg_candidates[i];
      break;
    }
  }

  if (!cfg_path) {
    fprintf(stderr,
            "[C11 ABI Test] Could not find pipeline_keyword_match.json\n");
    return 6;
  }

  param.config_file_path = cfg_path;
  param.model_root_dir = "./models";
  param.device_id = 0;
  param.biz_type = ALG_BIZ_TYPE_KEYWORD_MATCH;

  void* handle = NULL;
  int create_ret = Alg_Create(&handle, &param);
  if (create_ret != 0 || !handle) {
    fprintf(stderr, "[C11 ABI Test] Alg_Create failed with code: %d\n",
            create_ret);
    return 7;
  }

  // 4. Test pure C batch processing
  CompanyKeywordInputStruct req0 = {.request_id = 1001,
                                    .sentence_text = "请联系VIP专员办理业务"};
  CompanyKeywordInputStruct req1 = {.request_id = 1002,
                                    .sentence_text = "普通咨询业务"};
  const void* inputs[2] = {&req0, &req1};

  CompanyKeywordOutputStruct out0;
  CompanyKeywordOutputStruct out1;
  memset(&out0, 0, sizeof(out0));
  memset(&out1, 0, sizeof(out1));
  void* outputs[2] = {&out0, &out1};

  int num_outputs = 2;
  int proc_ret = Alg_Process(handle, inputs, 2, outputs, &num_outputs);
  if (proc_ret != 0) {
    fprintf(stderr, "[C11 ABI Test] Alg_Process failed with code: %d\n",
            proc_ret);
    Alg_Destroy(handle);
    return 8;
  }

  printf("[C11 ABI Test] Batch processed %d samples successfully.\n",
         num_outputs);
  printf("[C11 ABI Test] Sample 0: is_hit=%d, match_result=%s\n", out0.is_hit,
         out0.match_result_json);
  printf("[C11 ABI Test] Sample 1: is_hit=%d, match_result=%s\n", out1.is_hit,
         out1.match_result_json);

  if (Alg_Destroy(handle) != 0) {
    fprintf(stderr, "[C11 ABI Test] Alg_Destroy failed\n");
    return 9;
  }

  if (Alg_DeInit() != 0) {
    fprintf(stderr, "[C11 ABI Test] Alg_DeInit failed\n");
    return 10;
  }

  printf("[C11 ABI Test] All pure C11 ABI tests passed successfully!\n");
  return 0;
}
