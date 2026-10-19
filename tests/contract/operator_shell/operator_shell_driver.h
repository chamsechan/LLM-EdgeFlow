#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "adapter/operator/operator_failure_category.h"

// 壳无关一致性套件与具体壳之间的测试边界。场景只使用本头声明的请求、
// 类别和清单；平台载体、参数结构、函数表和数字错误码留在各壳的驱动中。
namespace llm_edgeflow::operator_shell_test {

using Category = OperatorFailureCategory;

inline constexpr Category kAllCategories[] = {
    Category::kSuccess,          Category::kInvalidHandle,
    Category::kInvalidParam,     Category::kInvalidInput,
    Category::kOutputCapacity,   Category::kUnsupportedBiz,
    Category::kRegistryConflict, Category::kUnsupportedControl,
    Category::kException,        Category::kUnknown};

// ---------------------------------------------------------------------------
// 错误码镜像清单
// ---------------------------------------------------------------------------

enum class ManifestStatus {
  kExternalPlaceholder,  // 外网占位：固定当前回归预期，尚未与真实平台核对
  kInternalVerified,  // 内网已核对：按已确认的宿主契约登记
};

struct ErrorCodeManifestEntry {
  Category category;
  const char* symbol;  // 平台符号名
  int value;           // 独立写定的登记数值
  ManifestStatus status;
  const char* basis;  // 核对依据
};

// 由所选壳的测试目录提供。数值不得从被测映射或平台常量现场计算。
const std::vector<ErrorCodeManifestEntry>& ErrorCodeManifest();

// ---------------------------------------------------------------------------
// 驱动请求
// ---------------------------------------------------------------------------

enum class CreateDefect {
  kNone,
  kNullParam,
  kEmptyModelPath,
  kNullConfig,
  kNegativeDevice,
  kUnknownPlatform,
};

struct CreateRequest {
  std::string model_path = ".";
  std::string config = "configs/pipeline_keyword_match_rules.conf";
  uint32_t max_frame_depth = 2;
  CreateDefect defect = CreateDefect::kNone;
};

// 关键词业务的一次 Process：驱动按行装配输入载体和空输出槽位。
struct ProcessRequest {
  std::vector<std::string> texts;  // 每项一行输入
  size_t output_rows = 0;
  bool occupy_first_output = false;  // 第一行输出槽位预置调用方的非空值
  bool invalid_last_input = false;  // 最后一行输入载体缺少必填文本
};

enum class ControlKind {
  kUpdateRules,        // 规则更新命令
  kJson,               // 通用 JSON 命令，使用 cmd_id
  kUndeclaredCommand,  // 平台未声明的命令值，按 undeclared_index 选择
};

struct ControlRequest {
  ControlKind kind = ControlKind::kUpdateRules;
  int32_t cmd_id = 0;
  std::string payload;
  size_t undeclared_index = 0;
};

// 下一次输出写入从第二个服务字段写起抛出的异常类型。
enum class OutputWriteFault { kNone, kStdException, kUnknownException };
inline constexpr char kInjectedOutputWriteFault[] =
    "injected output write failure";

// 违约自测的函数表包装；生产壳没有注入开关。
enum class Violation {
  kNone,
  kLeakInternalCode,       // 失败时透传内部码
  kWriteOutputOnFailure,   // 失败时写入输出槽位
  kRetainOutputReference,  // 成功后额外保留一份输出引用
};

class OperatorShellDriver {
 public:
  virtual ~OperatorShellDriver() = default;

  // 公共入口。驱动原样返回入口的状态值，不折叠、不清理、不吞异常。
  virtual int Init() = 0;
  virtual int DeInit() = 0;
  virtual int Create(void** handle, const CreateRequest& request) = 0;
  virtual int Process(void* handle, const ProcessRequest& request) = 0;
  virtual int Control(void* handle, const ControlRequest& request) = 0;
  virtual int Destroy(void* handle) = 0;
  virtual size_t UndeclaredControlCommandCount() const = 0;

  // 最近一次 Process 的输出槽位。
  virtual size_t PublishedOutputs() const = 0;  // 不含调用方预置值
  virtual bool OccupiedOutputPreserved() const = 0;
  virtual std::string OutputJson(size_t row) const = 0;  // 复制响应字节
  virtual void ReleaseOutputs() = 0;

  // 壳内映射表查询，只用于独立核验镜像清单。
  virtual int PlatformCode(Category category) const = 0;

  // 输出写入故障。首次设置须在 Init 与 Create 之前，用于安装故障注入；
  // 之后可在已有句柄上切换故障类型。计数从每次设置时清零。
  virtual void SetOutputWriteFault(OutputWriteFault fault) = 0;
  virtual size_t OutputWrites() const = 0;
  virtual size_t ReturnedOutputLeases() const = 0;

  virtual void SetViolation(Violation violation) = 0;
  virtual void ReleaseViolationReferences() = 0;
};

std::unique_ptr<OperatorShellDriver> MakeOperatorShellDriver();

}  // namespace llm_edgeflow::operator_shell_test
