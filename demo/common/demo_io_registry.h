#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "demo/common/demo_options.h"
#include "edgeflow/operator/interface.h"
#include "nlohmann/json.hpp"

namespace alg_demo {

struct DemoRequestBatch {
  std::vector<llm_edgeflow::operator_api::NamedIo> requests;
  std::vector<nlohmann::json> request_info;
  std::shared_ptr<void> storage;
};

using BuildRequestsFn =
    int (*)(const DemoOptions&,
            const std::vector<llm_edgeflow::operator_api::OperatorIoEntry>&,
            DemoRequestBatch*);
using ShowResultFn = void (*)(const void*, const nlohmann::json&, uint64_t*,
                              int32_t*, nlohmann::json*);

class DemoIoRegistry {
 public:
  static DemoIoRegistry& Instance();
  bool RegisterInput(std::string types, BuildRequestsFn build);
  bool RegisterOutput(std::string type, ShowResultFn show);
  BuildRequestsFn FindInput(const std::string& types) const;
  ShowResultFn FindOutput(const std::string& type) const;
  std::vector<std::string> ListInputs() const;
  std::vector<std::string> ListOutputs() const;
  bool HasConflict() const;

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, BuildRequestsFn> inputs_;
  std::unordered_map<std::string, ShowResultFn> outputs_;
  bool has_conflict_ = false;
};

#define REGISTER_DEMO_INPUT(types, build_fn)      \
  static const bool registered_input_##build_fn = \
      ::alg_demo::DemoIoRegistry::Instance().RegisterInput(types, build_fn)
#define REGISTER_DEMO_OUTPUT(type, show_fn)       \
  static const bool registered_output_##show_fn = \
      ::alg_demo::DemoIoRegistry::Instance().RegisterOutput(type, show_fn)

}  // namespace alg_demo
