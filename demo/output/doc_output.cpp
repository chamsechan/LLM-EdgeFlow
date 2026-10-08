#include <iomanip>
#include <iostream>
#include <string>

#include "demo/common/dataset_reader.h"
#include "demo/common/demo_io_registry.h"

namespace alg_demo {
namespace {

void ShowDocResult(const void* output, const nlohmann::json& /*request_info*/,
                   uint64_t* request_id, int32_t* status,
                   nlohmann::json* sample_output) {
  const auto& out = *static_cast<const CompanyOperatorDocOutput*>(output);
  const std::string intent_name = CopyCompanyString(out.intent_name);
  const std::string answer_text = CopyCompanyString(out.answer_text);

  PrintDivider();
  std::cout << "  Request ID    : " << out.request_id << "\n"
            << "  Chunk Count   : " << out.chunk_count
            << " (1-to-N Sub-items)\n"
            << "  Intent Name   : "
            << (!intent_name.empty() ? intent_name : "none")
            << " (Conf: " << std::fixed << std::setprecision(2)
            << out.confidence << ")\n"
            << "  LLM Answer    : " << answer_text << std::endl;

  *request_id = out.request_id;
  *status = out.status_code;
  (*sample_output)["chunk_count"] = out.chunk_count;
  (*sample_output)["intent_name"] = intent_name;
  (*sample_output)["confidence"] = out.confidence;
  (*sample_output)["answer_text"] = answer_text;
}

REGISTER_DEMO_OUTPUT("CompanyOperatorDocOutput", ShowDocResult);

}  // namespace
}  // namespace alg_demo
