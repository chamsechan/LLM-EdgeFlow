#include <string>
#include <vector>

#include "core/common_contracts.h"
#include "nodes/authoring.h"

namespace llm_edgeflow {

namespace {

struct Inputs {};
struct Params {
  std::vector<std::string> corpus;
};

NodeResult<TextBatch> Run(const Inputs&, const Params& params) {
  TextBatch output;
  output.reserve(params.corpus.size());
  for (size_t i = 0; i < params.corpus.size(); ++i) {
    output.emplace_back(0, static_cast<uint32_t>(i), params.corpus[i]);
  }
  return NodeResult<TextBatch>::Success(std::move(output));
}

auto Spec() {
  auto params = Parameters<Params>{
      Field("corpus", &Params::corpus)
          .Required()
          .Description("静态语料字符串数组，例如 [\"开户步骤\", "
                       "\"退款政策\"]；数组顺序对应 "
                       "sub_id，使用共享语料 req_id=0。")};
  return MakeNodeSpec(
             InputsOf<Inputs>{},
             ProducedBatch<TextBatch>(
                 "corpus", PortFlow{"1:N", "generate_sub_id", "session"}),
             std::move(params), &Run)
      .Category("common")
      .Description("Static text corpus and knowledge database source node")
      .ParallelSafe(true);
}
}  // namespace
REGISTER_FUNCTION_NODE(text_corpus_source, Spec());
}  // namespace llm_edgeflow
