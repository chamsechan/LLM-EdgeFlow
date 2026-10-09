#include <map>
#include <string>
#include <utility>
#include <vector>

#include "nodes/authoring.h"
#include "nodes/generate_parameters.h"
#include "nodes/text_template.h"

namespace llm_edgeflow {
namespace {
struct Inputs {
  const TextBatch* input = nullptr;
};
struct Models {
  LlmCall generator;
};
struct Endpoint {
  std::string prompt;
  std::vector<TextTemplateToken> parts;
};
struct Params {
  std::map<std::string, Endpoint> endpoints;
  GenerateOptions generation;
};
struct Outputs {
  TextBatch text;
  StructuredDocumentBatch document;
};

Parameters<Endpoint> EndpointParameters() {
  auto params = Parameters<Endpoint>{
      Field("prompt", &Endpoint::prompt).Default("{{input}}")};
  params.Prepare([](Endpoint* endpoint, std::string* error) {
    if (!ParseTextTemplate(endpoint->prompt, &endpoint->parts, error))
      return false;
    for (const auto& part : endpoint->parts) {
      if (part.type == TextTemplateTokenType::kVariable &&
          part.value != "input") {
        if (error) *error = "Unknown endpoint prompt variable: " + part.value;
        return false;
      }
    }
    return true;
  });
  return params;
}

NodeResult<Outputs> Run(const Inputs& inputs, const Params& params,
                        const Models& models) {
  Outputs output;
  for (const auto& input : *inputs.input) {
    output.document.emplace_back(input.req_id, input.sub_id,
                                 JsonDocumentItem{});
  }
  for (const auto& [name, endpoint] : params.endpoints) {
    TextBatch prompts;
    prompts.reserve(inputs.input->size());
    for (const auto& input : *inputs.input) {
      std::string prompt;
      for (const auto& part : endpoint.parts)
        prompt += part.type == TextTemplateTokenType::kLiteral ? part.value
                                                               : input.data;
      prompts.emplace_back(input.req_id, input.sub_id, std::move(prompt));
    }
    auto result = models.generator.Generate(prompts, params.generation);
    if (!result.ok()) {
      auto failure = result.failure();
      failure.message = "Endpoint '" + name + "': " + failure.message;
      return NodeResult<Outputs>::Failure(std::move(failure));
    }
    for (size_t i = 0; i < result.value().size(); ++i)
      output.document[i].data.structured_data[name] = result.value()[i].data;
    if (params.endpoints.size() == 1) output.text = std::move(result).value();
  }
  for (auto& item : output.document) {
    item.data.is_valid = true;
    try {
      item.data.json_payload = item.data.structured_data.dump();
    } catch (const nlohmann::json::type_error& error) {
      // Keep the single endpoint's raw-text contract; its document consumer
      // rejects a result that cannot be encoded.
      if (params.endpoints.size() != 1 || error.id != 316) throw;
      item.data.is_valid = false;
      item.data.parse_status = JsonParseStatus::kFailed;
      item.data.diagnostic = error.what();
    }
    if (params.endpoints.size() > 1)
      output.text.emplace_back(item.req_id, item.sub_id,
                               item.data.json_payload);
  }
  return NodeResult<Outputs>::Success(std::move(output));
}

auto Spec() {
  auto params = Parameters<Params>{Field("endpoints", &Params::endpoints)
                                       .Required()
                                       .Items(EndpointParameters())};
  params.Include(&Params::generation, GenerateParameters());
  params.Validate([](const Params& params, std::string* error) {
    if (!params.endpoints.empty()) return true;
    if (error) *error = "endpoints must contain at least one endpoint";
    return false;
  });
  return MakeNodeSpec(InputsOf<Inputs>{Required("input", &Inputs::input)},
                      OutputsOf<Outputs>{
                          Produced("text", &Outputs::text, "input"),
                          Produced("document", &Outputs::document, "input")},
                      std::move(params),
                      ModelsOf<Models>{
                          Model("generator", "bind_model", &Models::generator)},
                      &Run)
      .ValidateModels(
          [](const Params& params, const Models& models, std::string* error) {
            if (params.generation.random_seed < 0 ||
                models.generator.SupportsRandomSeed())
              return true;
            if (error) *error = "Bound llm model does not support random_seed";
            return false;
          })
      .Category("common")
      .ParallelSafe(true)
      .Description(
          "Generate endpoint answers as text and a structured document");
}
REGISTER_FUNCTION_NODE(llm_generate, Spec());
}  // namespace
}  // namespace llm_edgeflow
