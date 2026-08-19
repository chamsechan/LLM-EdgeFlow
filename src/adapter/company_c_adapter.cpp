#include <cstring>
#include <iostream>
#include <memory>

#include "adapter/business_adapter_registry.h"
#include "company_alg_interface.h"
#include "core/alg_context.h"
#include "core/pipeline.h"
#include "core/session_context.h"

/**
 * @brief 句柄内部实例数据结构
 */
struct AlgHandleInstance {
  std::unique_ptr<alg_framework::Pipeline> pipeline;
  CompanyAlgBizType biz_type = ALG_BIZ_TYPE_UNKNOWN;
  int device_id = 0;
  std::string model_root_dir;
};

extern "C" {

int Alg_Init(void) {
  std::cout
      << "[Company C Adapter] Alg_Init: Global runtime resources initialized."
      << std::endl;
  return 0;
}

int Alg_Create(void** hndl, const CompanyAlgParamCreate* param_create) {
  if (!hndl || !param_create) {
    std::cerr
        << "[Company C Adapter] Alg_Create failed: Null pointer arguments."
        << std::endl;
    return -1;
  }

  try {
    auto instance = std::make_unique<AlgHandleInstance>();
    instance->biz_type = param_create->biz_type;
    instance->device_id = param_create->device_id;
    instance->model_root_dir =
        param_create->model_root_dir ? param_create->model_root_dir : "";
    instance->pipeline = std::make_unique<alg_framework::Pipeline>();

    alg_framework::RuntimeOptions options;
    options.config_file_path =
        param_create->config_file_path ? param_create->config_file_path : "";
    options.model_root_dir = instance->model_root_dir;
    options.device_id = instance->device_id;
    options.biz_type = static_cast<int>(instance->biz_type);

    instance->pipeline->GetSessionContext().SetRuntimeOptions(options);

    const char* cfg_path =
        param_create->config_file_path ? param_create->config_file_path : "";
    if (strlen(cfg_path) == 0) {
      std::cerr
          << "[Company C Adapter] Alg_Create failed: Empty config_file_path."
          << std::endl;
      return -2;
    }

    std::cout << "[Company C Adapter] Creating Pipeline for BizType ["
              << instance->biz_type << "] with config: " << cfg_path
              << std::endl;
    if (!instance->pipeline->BuildFromConfigFile(cfg_path)) {
      std::cerr << "[Company C Adapter] Failed to build pipeline from config."
                << std::endl;
      return -3;
    }

    *hndl = static_cast<void*>(instance.release());
    std::cout
        << "[Company C Adapter] Alg_Create: Handle created successfully at "
        << *hndl << std::endl;
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "[Company C Adapter] Alg_Create exception: " << e.what()
              << std::endl;
    return -99;
  } catch (...) {
    return -100;
  }
}

int Alg_Process(void* hndl, const void** inputs, int num_inputs, void** outputs,
                int* num_outputs) {
  if (!hndl) {
    std::cerr << "[Company C Adapter] Alg_Process failed: Null handle."
              << std::endl;
    return -1;
  }
  if (!inputs || num_inputs <= 0) {
    std::cerr << "[Company C Adapter] Alg_Process failed: Empty inputs."
              << std::endl;
    return -2;
  }

  auto* instance = static_cast<AlgHandleInstance*>(hndl);

  try {
    CompanyAlgBizType lookup_type = instance->biz_type;
    if (lookup_type == ALG_BIZ_TYPE_UNKNOWN) {
      lookup_type = ALG_BIZ_TYPE_DOC_QA;
    }

    auto adapter =
        alg_framework::BusinessAdapterRegistry::Instance().GetAdapter(
            lookup_type);
    if (!adapter) {
      std::cerr << "[Company C Adapter] Unsupported biz_type: "
                << instance->biz_type << std::endl;
      return -5;
    }

    alg_framework::AlgContext req_ctx;
    int unpack_ret = adapter->Unpack(inputs, num_inputs, &req_ctx);
    if (unpack_ret != 0) {
      std::cerr << "[Company C Adapter] Unpack failed for "
                << adapter->BizName() << " with code: " << unpack_ret
                << std::endl;
      return unpack_ret;
    }

    int exec_ret = instance->pipeline->Execute(&req_ctx);
    if (exec_ret != 0) {
      return exec_ret;
    }

    int pack_ret = adapter->Pack(&req_ctx, outputs, num_outputs);
    if (pack_ret != 0) {
      std::cerr << "[Company C Adapter] Pack failed for " << adapter->BizName()
                << " with code: " << pack_ret << std::endl;
      return pack_ret;
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "[Company C Adapter] Alg_Process exception: " << e.what()
              << std::endl;
    return -99;
  } catch (...) {
    return -100;
  }
}

int Alg_Control(void* hndl, const CompanyAlgParamControl* param_control) {
  if (!hndl || !param_control) return -1;
  if (!param_control->json_param_str) return -2;
  try {
    auto* instance = static_cast<AlgHandleInstance*>(hndl);
    return instance->pipeline->Control(param_control->control_cmd,
                                       param_control->json_param_str);
  } catch (...) {
    return -99;
  }
}

int Alg_Destroy(void* hndl) {
  if (!hndl) return -1;
  try {
    auto* instance = static_cast<AlgHandleInstance*>(hndl);
    std::cout << "[Company C Adapter] Alg_Destroy: Destroying handle at "
              << hndl << std::endl;
    delete instance;
    return 0;
  } catch (...) {
    return -99;
  }
}

int Alg_DeInit(void) {
  std::cout
      << "[Company C Adapter] Alg_DeInit: Global runtime resources released."
      << std::endl;
  return 0;
}

}  // extern "C"
