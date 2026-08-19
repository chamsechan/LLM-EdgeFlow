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

int Alg_Init(void) COMPANY_ALG_NOEXCEPT {
  try {
    // REV2-003: 检测静态注册期是否存在业务 ID/名称冲突，fail-closed 拒绝启动
    if (alg_framework::BusinessAdapterRegistry::Instance()
            .HasRegistrationConflict()) {
      std::cerr << "[Company C Adapter] Alg_Init failed: Registration conflict "
                   "detected in BusinessAdapterRegistry."
                << std::endl;
      for (const auto& err : alg_framework::BusinessAdapterRegistry::Instance()
                                 .GetRegistrationErrors()) {
        std::cerr << "  - " << err << std::endl;
      }
      return -6;
    }

    std::cout
        << "[Company C Adapter] Alg_Init: Global runtime resources initialized."
        << std::endl;
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "[Company C Adapter] Alg_Init exception: " << e.what()
              << std::endl;
    return -99;
  } catch (...) {
    return -100;
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

    // REV2-001: 在 Alg_Create 阶段前置校验业务类型，拒绝 UNKNOWN 与未注册业务
    if (param_create->biz_type == ALG_BIZ_TYPE_UNKNOWN) {
      std::cerr << "[Company C Adapter] Alg_Create failed: Cannot create "
                   "pipeline with ALG_BIZ_TYPE_UNKNOWN."
                << std::endl;
      return -5;
    }

    auto adapter =
        alg_framework::BusinessAdapterRegistry::Instance().GetAdapter(
            param_create->biz_type);
    if (!adapter) {
      std::cerr << "[Company C Adapter] Alg_Create failed: Unsupported or "
                   "unregistered biz_type: "
                << param_create->biz_type << std::endl;
      return -5;
    }

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
    options.has_device_id = (instance->device_id >= 0);
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

    // ADP-006: 校验 Pipeline 声明的业务标识与 Adapter 是否匹配
    if (!adapter->ValidatePipelineBinding(
            instance->pipeline->GetBusinessName())) {
      std::cerr
          << "[Company C Adapter] Alg_Create failed: Pipeline business_name '"
          << instance->pipeline->GetBusinessName()
          << "' does not match adapter '" << adapter->BizName() << "'."
          << std::endl;
      return -5;
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
                int* num_outputs) COMPANY_ALG_NOEXCEPT {
  try {
    if (!hndl) {
      std::cerr << "[Company C Adapter] Alg_Process failed: Null handle."
                << std::endl;
      return -1;
    }

    auto* instance = static_cast<AlgHandleInstance*>(hndl);
    if (instance->biz_type == ALG_BIZ_TYPE_UNKNOWN) {
      std::cerr << "[Company C Adapter] Alg_Process failed: Handle has "
                   "ALG_BIZ_TYPE_UNKNOWN."
                << std::endl;
      return -5;
    }

    auto adapter =
        alg_framework::BusinessAdapterRegistry::Instance().GetAdapter(
            instance->biz_type);
    if (!adapter) {
      std::cerr << "[Company C Adapter] Unsupported biz_type: "
                << instance->biz_type << std::endl;
      return -5;
    }

    // REV2-002 & REV2-005: 严格在 Pipeline Execute
    // 之前完成批大小上限、空槽位与输出缓冲区容量预检
    int preflight_ret =
        adapter->ValidateBatch(inputs, num_inputs, outputs, num_outputs);
    if (preflight_ret != 0) {
      return preflight_ret;
    }

    alg_framework::AlgContext req_ctx;
    alg_framework::AdapterStatus unpack_status;
    int unpack_ret =
        adapter->Unpack(inputs, num_inputs, &req_ctx, &unpack_status);
    if (unpack_ret != 0) {
      std::cerr << "[Company C Adapter] Unpack failed for "
                << adapter->BizName() << " (" << unpack_status.ToString() << ")"
                << std::endl;
      return unpack_ret;
    }

    int exec_ret = instance->pipeline->Execute(&req_ctx);
    if (exec_ret != 0) {
      return exec_ret;
    }

    alg_framework::AdapterStatus pack_status;
    int pack_ret = adapter->Pack(&req_ctx, outputs, num_outputs, &pack_status);
    if (pack_ret != 0) {
      std::cerr << "[Company C Adapter] Pack failed for " << adapter->BizName()
                << " (" << pack_status.ToString() << ")" << std::endl;
      return pack_ret;
    }

    return COMPANY_ALG_SUCCESS;
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
    return instance->pipeline->Control(param_control->control_cmd,
                                       param_control->json_param_str);
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
    std::cout
        << "[Company C Adapter] Alg_DeInit: Global runtime resources released."
        << std::endl;
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "[Company C Adapter] Alg_DeInit exception: " << e.what()
              << std::endl;
    return -99;
  } catch (...) {
    return -100;
  }
}

}  // extern "C"
