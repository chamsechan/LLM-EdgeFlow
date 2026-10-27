#include <filesystem>
#include <iostream>
#include <string>
#include <type_traits>

#include "edgeflow/operator/interface.h"

namespace api = llm_edgeflow::operator_api;

static_assert(std::is_same_v<decltype(api::OperatorFunc::Init), int (*)()>);
static_assert(std::is_same_v<decltype(api::OperatorFunc::Create),
                             int (*)(void**, const void*)>);
static_assert(std::is_same_v<decltype(api::OperatorFunc::Process),
                             int (*)(void*, const api::NamedIoBatch&,
                                     api::NamedIoBatch&)>);
static_assert(std::is_same_v<decltype(api::OperatorFunc::Control),
                             int (*)(void*, int, void*)>);
static_assert(
    std::is_same_v<decltype(api::OperatorFunc::Destroy), int (*)(void*)>);
static_assert(std::is_same_v<decltype(api::OperatorFunc::DeInit), int (*)()>);
static_assert(noexcept(api::Get_LLM_EDGEFLOW_OperatorTable()));
static_assert(noexcept(api::GetOperatorLastError()));

namespace {
class ConsumerSession {
 public:
  api::OperatorFunc ops = api::Get_LLM_EDGEFLOW_OperatorTable();
  void* handle = nullptr;
  bool initialized = false;

  ~ConsumerSession() {
    if (handle) ops.Destroy(handle);
    if (initialized) ops.DeInit();
  }
};

bool Check(bool condition, const char* operation) {
  if (condition) return true;
  std::cerr << operation << ": " << api::GetOperatorLastError() << '\n';
  return false;
}

bool CopyResult(ConsumerSession& session, const api::OperatorIoContract& io,
                const std::string& text, std::string* copied) {
  MockTextInput leaf{sizeof(MockTextInput), text.data(),
                     static_cast<int32_t>(text.size())};
  MockDataPool pool{sizeof(MockDataPool), kMockDataText, &leaf};
  MockInput carrier{sizeof(MockInput), kMockInputMultidata, 1, &pool};
  api::NamedIoBatch inputs(1), outputs(1);
  inputs[0]["client.keyword_in"] = api::MakeBorrowedOperatorInput(&carrier);
  outputs[0]["client.json_out"] = {};
  if (!Check(session.ops.Process(session.handle, inputs, outputs) == 0,
             "Process"))
    return false;
  const auto& slot = outputs[0].at("client.json_out");
  if (!Check(static_cast<bool>(slot), "published output")) return false;
  const auto* result = static_cast<const MockOutput*>(slot.get());
  if (!Check(result->type == kMockOutputOc && result->text &&
                 result->text->value && result->text->length > 0,
             "complete response carrier"))
    return false;
  copied->assign(result->text->value, result->text->length);
  // 宿主先复制字节再归还池租约，随后安全销毁。
  outputs.clear();
  return true;
}
}  // namespace

int main() {
  ConsumerSession session;
  if (!Check(session.ops.Init && session.ops.Create && session.ops.Process &&
                 session.ops.Control && session.ops.Destroy &&
                 session.ops.DeInit,
             "public function table"))
    return 1;
  if (!Check(session.ops.Init() == 0, "Init")) return 1;
  session.initialized = true;
  const std::string root = std::filesystem::current_path().string();
  const char* config = "configs/pipeline_keyword_match_rules.conf";
  api::OperatorIoContract io;
  char diagnostic[512] = {};
  if (!Check(api::ResolveOperatorConfigIo(root.c_str(), config, &io, diagnostic,
                                          sizeof(diagnostic)) == 0,
             diagnostic))
    return 1;
  if (!Check(io.inputs.size() == 1 && io.outputs.size() == 1 &&
                 io.inputs[0].type == "keyword_in" &&
                 io.outputs[0].type == "json_out",
             "resolved host contract"))
    return 1;
  api::CreateParam create;
  create.model_path = root.c_str();
  create.cfg_file_name = config;
  create.compute_platform = api::ComputePlatform::kCpu;
  create.max_frame_depth = 1;
  if (!Check(
          session.ops.Create(&session.handle, &create) == 0 && session.handle,
          "Create"))
    return 1;
  std::string before;
  if (!CopyResult(session, io, "初始化完成", &before)) return 1;
  if (!Check(before.find("SYSTEM_INIT") != std::string::npos,
             "response from configured rule"))
    return 1;
  const std::string control = "{\"categories\":{\"SDK\":[\"宿主\"]}}";
  api::ControlUpdateRulesParam update{control.c_str()};
  if (!Check(session.ops.Control(
                 session.handle,
                 static_cast<int>(api::ControlCommand::kUpdateRules),
                 &update) == 0,
             "Control"))
    return 1;
  std::string after;
  if (!CopyResult(session, io, "宿主调用", &after)) return 1;
  if (!Check(after.find("SDK") != std::string::npos &&
                 before.find("SYSTEM_INIT") != std::string::npos,
             "controlled response and retained host copy"))
    return 1;
  void* handle = session.handle;
  session.handle = nullptr;
  if (!Check(session.ops.Destroy(handle) == 0, "Destroy")) return 1;
  session.initialized = false;
  if (!Check(session.ops.DeInit() == 0, "DeInit")) return 1;
  std::cout << "PASS public SDK lifecycle, control, response copy and lease "
               "release\n";
  return 0;
}
