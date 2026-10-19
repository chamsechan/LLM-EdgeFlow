#include "tests/contract/operator_shell/operator_shell_driver.h"

namespace llm_edgeflow::operator_shell_test {

// mock 壳的平台码镜像。数值按 include/platform_mock/error_codes.h 独立写定，
// 只固定外网回归预期；真实值在授权内网按宿主契约核对后另行登记。
const std::vector<ErrorCodeManifestEntry>& ErrorCodeManifest() {
  static const auto* const manifest = new std::vector<ErrorCodeManifestEntry>{
      {Category::kSuccess, "COMPANY_ALG_SUCCESS", 0,
       ManifestStatus::kExternalPlaceholder, "外网 mock 夹具声明"},
      {Category::kInvalidHandle, "COMPANY_ALG_ERR_INVALID_HANDLE", -1,
       ManifestStatus::kExternalPlaceholder, "外网 mock 夹具声明"},
      {Category::kInvalidParam, "COMPANY_ALG_ERR_INVALID_PARAM", -2,
       ManifestStatus::kExternalPlaceholder, "外网 mock 夹具声明"},
      {Category::kInvalidInput, "COMPANY_ALG_ERR_INVALID_INPUT", -3,
       ManifestStatus::kExternalPlaceholder, "外网 mock 夹具声明"},
      {Category::kOutputCapacity, "COMPANY_ALG_ERR_BUFFER_TOO_SMALL", -4,
       ManifestStatus::kExternalPlaceholder, "外网 mock 夹具声明"},
      {Category::kUnsupportedBiz, "COMPANY_ALG_ERR_UNSUPPORTED_BIZ", -5,
       ManifestStatus::kExternalPlaceholder, "外网 mock 夹具声明"},
      {Category::kRegistryConflict, "COMPANY_ALG_ERR_REGISTRY_CONFLICT", -6,
       ManifestStatus::kExternalPlaceholder, "外网 mock 夹具声明"},
      {Category::kUnsupportedControl, "COMPANY_ALG_ERR_UNSUPPORTED_CONTROL", -7,
       ManifestStatus::kExternalPlaceholder, "外网 mock 夹具声明"},
      {Category::kException, "COMPANY_ALG_ERR_EXCEPTION", -99,
       ManifestStatus::kExternalPlaceholder, "外网 mock 夹具声明"},
      {Category::kUnknown, "COMPANY_ALG_ERR_UNKNOWN", -100,
       ManifestStatus::kExternalPlaceholder, "外网 mock 夹具声明"},
  };
  return *manifest;
}

}  // namespace llm_edgeflow::operator_shell_test
