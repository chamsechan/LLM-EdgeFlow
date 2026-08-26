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

_Static_assert(sizeof(CompanyAlgBizType) == sizeof(int32_t),
               "CompanyAlgBizType must remain a 32-bit C ABI type");
_Static_assert(ALG_BIZ_TYPE_MAX_GUARD == INT32_MAX,
               "CompanyAlgBizType ABI guard must remain INT32_MAX");

int main(void) {
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

  // Verify Slot Struct Mappings mapping contract
  int map_count = 0;
  const CompanySlotStructMapping* mappings =
      Alg_GetSlotStructMappings(&map_count);
  if (!mappings || map_count <= 0) {
    fprintf(stderr, "[C11 ABI Test] Alg_GetSlotStructMappings failed\n");
    Alg_Destroy(handle);
    return 8;
  }
  printf("[C11 ABI Test] Alg_GetSlotStructMappings verified (%d mappings).\n",
         map_count);

  // 4. Test pure C batch processing
  CompanyString str0, str1;
  CompanyString_FromCString(&str0, "请联系VIP专员办理业务");
  CompanyString_FromCString(&str1, "普通咨询业务");

  CompanyKeywordInputStruct req0 = {.request_id = 1001, .sentence_text = &str0};
  CompanyKeywordInputStruct req1 = {.request_id = 1002, .sentence_text = &str1};
  const void* inputs[2] = {&req0, &req1};

  char buf0[2048] = {0};
  char buf1[2048] = {0};
  CompanyString out_str0, out_str1;
  CompanyString_Init(&out_str0, buf0, sizeof(buf0));
  CompanyString_Init(&out_str1, buf1, sizeof(buf1));

  CompanyKeywordOutputStruct out0 = {.match_result_json = &out_str0};
  CompanyKeywordOutputStruct out1 = {.match_result_json = &out_str1};
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
  printf("[C11 ABI Test] Sample 0: is_hit=%d, match_result=%s (len=%zu)\n",
         out0.is_hit, out0.match_result_json->data,
         out0.match_result_json->length);
  printf("[C11 ABI Test] Sample 1: is_hit=%d, match_result=%s (len=%zu)\n",
         out1.is_hit, out1.match_result_json->data,
         out1.match_result_json->length);

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
