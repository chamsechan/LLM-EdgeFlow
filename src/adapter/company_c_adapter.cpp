#include <cstring>
#include <iostream>
#include <memory>

#include "adapter/shared_algorithm_runtime.h"
#include "company_alg_interface.h"

/**
 * @brief C ABI 句柄内部实例数据结构 (委托至 SharedAlgorithmRuntime)
 */
struct AlgHandleInstance {
  std::unique_ptr<alg_framework::SharedAlgorithmRuntime> runtime;
};

extern "C" {

int Alg_Init(void) COMPANY_ALG_NOEXCEPT {
  try {
    int ret = alg_framework::SharedAlgorithmRuntime::GlobalInit();
    if (ret == 0) {
      std::cout << "[Company C Adapter] Alg_Init: Global runtime resources "
                   "initialized."
                << std::endl;
    }
    return ret;
  } catch (const std::exception& e) {
    std::cerr << "[Company C Adapter] Alg_Init exception: " << e.what()
              << std::endl;
    return COMPANY_ALG_ERR_EXCEPTION;
  } catch (...) {
    return COMPANY_ALG_ERR_UNKNOWN;
  }
}

int Alg_Create(void** hndl,
               const CompanyAlgParamCreate* param_create) COMPANY_ALG_NOEXCEPT {
  try {
    if (!hndl || !param_create) {
      std::cerr
          << "[Company C Adapter] Alg_Create failed: Null pointer arguments."
          << std::endl;
      return -1;
    }

    const char* cfg_path =
        param_create->config_file_path ? param_create->config_file_path : "";
    if (strlen(cfg_path) == 0) {
      std::cerr
          << "[Company C Adapter] Alg_Create failed: Empty config_file_path."
          << std::endl;
      return -2;
    }

    std::string model_root =
        param_create->model_root_dir ? param_create->model_root_dir : "";
    std::unique_ptr<alg_framework::SharedAlgorithmRuntime> runtime;
    std::string err_msg;

    int ret = alg_framework::SharedAlgorithmRuntime::CreateFromConfigFile(
        cfg_path, param_create->device_id, model_root, param_create->biz_type,
        &runtime, &err_msg);
    if (ret != 0) {
      std::cerr << "[Company C Adapter] Alg_Create failed: " << err_msg
                << std::endl;
      return ret;
    }

    auto instance = std::make_unique<AlgHandleInstance>();
    instance->runtime = std::move(runtime);

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
                int* num_outputs) COMPANY_ALG_NOEXCEPT {
  try {
    if (!hndl) {
      std::cerr << "[Company C Adapter] Alg_Process failed: Null handle."
                << std::endl;
      return -1;
    }

    auto* instance = static_cast<AlgHandleInstance*>(hndl);
    if (!instance->runtime) {
      std::cerr << "[Company C Adapter] Alg_Process failed: Null inner runtime."
                << std::endl;
      return -1;
    }

    std::string err_msg;
    int ret = instance->runtime->ExecuteBatch(inputs, num_inputs, outputs,
                                              num_outputs, &err_msg);
    if (ret != 0 && !err_msg.empty()) {
      std::cerr << "[Company C Adapter] " << err_msg << std::endl;
    }
    return ret;
  } catch (const std::exception& e) {
    std::cerr << "[Company C Adapter] Alg_Process exception: " << e.what()
              << std::endl;
    return -99;
  } catch (...) {
    return -100;
  }
}

int Alg_Control(void* hndl, const CompanyAlgParamControl* param_control)
    COMPANY_ALG_NOEXCEPT {
  try {
    if (!hndl || !param_control) return -1;
    if (!param_control->json_param_str) return -2;

    auto* instance = static_cast<AlgHandleInstance*>(hndl);
    if (!instance->runtime) return -1;

    std::string err_msg;
    return instance->runtime->ExecuteControl(
        param_control->control_cmd, param_control->json_param_str, &err_msg);
  } catch (const std::exception& e) {
    std::cerr << "[Company C Adapter] Alg_Control exception: " << e.what()
              << std::endl;
    return -99;
  } catch (...) {
    return -100;
  }
}

int Alg_Destroy(void* hndl) COMPANY_ALG_NOEXCEPT {
  try {
    if (!hndl) return -1;
    auto* instance = static_cast<AlgHandleInstance*>(hndl);
    std::cout << "[Company C Adapter] Alg_Destroy: Destroying handle at "
              << hndl << std::endl;
    delete instance;
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "[Company C Adapter] Alg_Destroy exception: " << e.what()
              << std::endl;
    return -99;
  } catch (...) {
    return -100;
  }
}

int Alg_DeInit(void) COMPANY_ALG_NOEXCEPT {
  try {
    int ret = alg_framework::SharedAlgorithmRuntime::GlobalDeinit();
    std::cout
        << "[Company C Adapter] Alg_DeInit: Global runtime resources released."
        << std::endl;
    return ret;
  } catch (const std::exception& e) {
    std::cerr << "[Company C Adapter] Alg_DeInit exception: " << e.what()
              << std::endl;
    return COMPANY_ALG_ERR_EXCEPTION;
  } catch (...) {
    return COMPANY_ALG_ERR_UNKNOWN;
  }
}

}  // extern "C"
