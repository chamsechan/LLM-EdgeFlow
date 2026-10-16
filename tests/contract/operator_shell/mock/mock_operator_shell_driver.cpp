#include <array>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "adapter/operator/mock/operator_error_mapping.h"
#include "contracts/error_codes.h"
#include "edgeflow/operator/interface.h"
#include "tests/contract/operator_shell/operator_shell_driver.h"
#include "tests/support/registry_test_access.h"

namespace llm_edgeflow::operator_shell_test {
namespace {
namespace api = operator_api;

constexpr char kInputKey[] = "source.keyword_in";
constexpr char kOutputKey[] = "response.json_out";
constexpr std::array<int, 2> kUndeclaredCommands{3, 99};

// 违约包装只存在于测试；函数表是无捕获函数指针，状态只能放在静态区。
Violation g_violation = Violation::kNone;
std::vector<api::OpaqueData> g_retained;

int ViolatingProcess(void* handle, const api::NamedIoBatch& inputs,
                     api::NamedIoBatch& outputs) {
  const int result =
      api::Get_LLM_EDGEFLOW_OperatorTable().Process(handle, inputs, outputs);
  switch (g_violation) {
    case Violation::kLeakInternalCode:
      return result == 0 ? result : error::kInvalidInput;
    case Violation::kWriteOutputOnFailure:
      if (result != 0) {
        for (auto& row : outputs)
          for (auto& slot : row) slot.second = std::make_shared<int>(0);
      }
      return result;
    case Violation::kRetainOutputReference:
      if (result == 0) {
        for (const auto& row : outputs)
          for (const auto& slot : row)
            if (slot.second) g_retained.push_back(slot.second);
      }
      return result;
    case Violation::kNone:
      break;
  }
  return result;
}

struct FaultState {
  OutputWriteFault fault = OutputWriteFault::kNone;
  size_t writes = 0;
  size_t resets = 0;
};

class MockOperatorShellDriver final : public OperatorShellDriver {
 public:
  int Init() override { return ops_.Init(); }
  int DeInit() override { return ops_.DeInit(); }

  int Create(void** handle, const CreateRequest& request) override {
    api::CreateParam param;
    param.model_path = request.model_path.c_str();
    param.cfg_file_name = request.config.c_str();
    param.compute_platform = api::ComputePlatform::kCpu;
    param.max_frame_depth = request.max_frame_depth;
    switch (request.defect) {
      case CreateDefect::kNullParam:
        return ops_.Create(handle, nullptr);
      case CreateDefect::kEmptyModelPath:
        param.model_path = "";
        break;
      case CreateDefect::kNullConfig:
        param.cfg_file_name = nullptr;
        break;
      case CreateDefect::kNegativeDevice:
        param.device_id = -1;
        break;
      case CreateDefect::kUnknownPlatform:
        param.compute_platform = static_cast<api::ComputePlatform>(99);
        break;
      case CreateDefect::kNone:
        break;
    }
    return ops_.Create(handle, &param);
  }

  int Process(void* handle, const ProcessRequest& request) override {
    const size_t rows = request.texts.size();
    texts_ = request.texts;
    strings_.assign(rows, {});
    carriers_.assign(rows, {});
    inputs_.assign(rows, {});
    for (size_t i = 0; i < rows; ++i) {
      strings_[i] = {static_cast<int32_t>(texts_[i].size()), texts_[i].data()};
      carriers_[i] = {kMockServiceKeywordMatch, &strings_[i]};
      inputs_[i][kInputKey] = api::MakeBorrowedOperatorInput(&carriers_[i]);
    }
    if (request.invalid_last_input && rows > 0) {
      carriers_.back().sentence_text = nullptr;
    }
    outputs_.assign(request.output_rows, {{kOutputKey, nullptr}});
    sentinel_.reset();
    if (request.occupy_first_output && !outputs_.empty()) {
      sentinel_ = std::make_shared<int>(8);
      outputs_[0][kOutputKey] = sentinel_;
    }
    return ops_.Process(handle, inputs_, outputs_);
  }

  int Control(void* handle, const ControlRequest& request) override {
    switch (request.kind) {
      case ControlKind::kUpdateRules: {
        api::ControlUpdateRulesParam param{request.payload.c_str()};
        return ops_.Control(handle,
                            static_cast<int>(api::ControlCommand::kUpdateRules),
                            &param);
      }
      case ControlKind::kJson: {
        api::ControlJsonParam param{request.cmd_id, request.payload.c_str()};
        return ops_.Control(
            handle, static_cast<int>(api::ControlCommand::kJson), &param);
      }
      case ControlKind::kUndeclaredCommand:
        break;
    }
    api::ControlJsonParam param{request.cmd_id, request.payload.c_str()};
    return ops_.Control(
        handle, kUndeclaredCommands.at(request.undeclared_index), &param);
  }

  int Destroy(void* handle) override { return ops_.Destroy(handle); }

  size_t UndeclaredControlCommandCount() const override {
    return kUndeclaredCommands.size();
  }

  size_t PublishedOutputs() const override {
    size_t published = 0;
    for (const auto& row : outputs_)
      for (const auto& slot : row)
        published += slot.second && slot.second != sentinel_;
    return published;
  }

  bool OccupiedOutputPreserved() const override {
    return sentinel_ && !outputs_.empty() &&
           outputs_[0].at(kOutputKey) == sentinel_;
  }

  std::string OutputJson(size_t row) const override {
    const auto* output = static_cast<const CompanyOperatorOutput*>(
        outputs_.at(row).at(kOutputKey).get());
    if (!output || !output->result_json || !output->result_json->data)
      return {};
    return {output->result_json->data,
            static_cast<size_t>(output->result_json->length)};
  }

  void ReleaseOutputs() override {
    outputs_.clear();
    sentinel_.reset();
  }

  int PlatformCode(Category category) const override {
    return PlatformFailureCode(category);
  }

  void SetOutputWriteFault(OutputWriteFault fault) override {
    if (!saved_values_) InstallOutputWriteFault();
    *fault_ = {fault, 0, 0};
  }
  size_t OutputWrites() const override { return fault_->writes; }
  size_t ReturnedOutputLeases() const override { return fault_->resets; }

  void SetViolation(Violation violation) override {
    g_violation = violation;
    ops_ = api::Get_LLM_EDGEFLOW_OperatorTable();
    if (violation != Violation::kNone) ops_.Process = ViolatingProcess;
  }

  void ReleaseViolationReferences() override { g_retained.clear(); }

  ~MockOperatorShellDriver() override {
    outputs_.clear();
    SetViolation(Violation::kNone);
    ReleaseViolationReferences();
  }

 private:
  // 包装 JSON 输出 binding：计数服务字段写入和租约归还，并按需抛出。
  void InstallOutputWriteFault() {
    saved_values_.emplace();
    auto binding =
        OperatorValueTypeRegistry::Instance().CopyBindingBySuffix("json_out");
    if (!binding) throw std::runtime_error("Missing json_out binding");
    const auto writer = binding->write_service_type;
    const auto reset = binding->reset_external;
    binding->write_service_type = [state = fault_, writer](void* value,
                                                           int32_t service) {
      if (++state->writes == 2) {
        if (state->fault == OutputWriteFault::kStdException)
          throw std::runtime_error(kInjectedOutputWriteFault);
        if (state->fault == OutputWriteFault::kUnknownException) throw 19;
      }
      writer(value, service);
    };
    binding->reset_external = [state = fault_, reset](
                                  void* value,
                                  const ResolvedOutputPoolSpec& spec) {
      ++state->resets;
      reset(value, spec);
    };
    test_support::RegistryTestAccess::SetValueBinding(std::move(*binding));
  }

  api::OperatorFunc ops_ = api::Get_LLM_EDGEFLOW_OperatorTable();
  std::vector<std::string> texts_;
  std::vector<CompanyString> strings_;
  std::vector<CompanyOperatorKeywordInput> carriers_;
  api::NamedIoBatch inputs_;
  api::NamedIoBatch outputs_;
  std::shared_ptr<int> sentinel_;
  std::shared_ptr<FaultState> fault_ = std::make_shared<FaultState>();
  std::optional<test_support::RegistryTestAccess::ScopedValueTypeState>
      saved_values_;
};

}  // namespace

std::unique_ptr<OperatorShellDriver> MakeOperatorShellDriver() {
  return std::make_unique<MockOperatorShellDriver>();
}

}  // namespace llm_edgeflow::operator_shell_test
