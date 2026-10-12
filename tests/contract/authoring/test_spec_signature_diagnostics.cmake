cmake_minimum_required(VERSION 3.19)
if(NOT LAYER_COMPILE_MANIFEST)
  message(FATAL_ERROR "Supply LAYER_COMPILE_MANIFEST from the configured Node target")
endif()
include("${LAYER_COMPILE_MANIFEST}")
set(directory "${layer_test_root}/fresh-spec-signatures")
file(MAKE_DIRECTORY "${directory}")
set(flags ${layer_cxx_flags})
foreach(include IN LISTS capability_nodes_includes)
  list(APPEND flags "-I${include}")
endforeach()
foreach(definition IN LISTS capability_nodes_definitions)
  list(APPEND flags "-D${definition}")
endforeach()
set(preamble [=[
#include "nodes/authoring.h"
using namespace llm_edgeflow;
struct Inputs { const TextBatch* text = nullptr; };
struct Params { std::string prefix; };
struct DifferentParams { int count = 0; };
]=])
function(compile_case name body succeeds diagnostic)
  set(source "${directory}/${name}.cpp")
  file(WRITE "${source}" "${preamble}\n${body}\n")
  execute_process(COMMAND "${layer_cxx}" -std=c++17 ${flags} -fsyntax-only "${source}"
                  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(succeeds AND NOT result EQUAL 0)
    message(FATAL_ERROR "Valid public Node Spec failed: ${name}\n${output}\n${error}")
  elseif(NOT succeeds)
    if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "${diagnostic}")
      message(FATAL_ERROR "Invalid Spec must fail with actionable diagnostic: ${name}\n${output}\n${error}")
    endif()
  endif()
endfunction()
compile_case(valid_parameters [=[
NodeResult<TextBatch> Run(const Inputs& input, const Params&) {
  return NodeResult<TextBatch>::Success(*input.text);
}
auto spec = MakeNodeSpec(InputsOf<Inputs>{Required("text", &Inputs::text)},
                        PreservedOutput<TextBatch>("answer", "text"),
                        Parameters<Params>{Field("prefix", &Params::prefix).Default("")}, &Run);
]=] ON "")
compile_case(valid_model_slot [=[
struct Models { LlmCall generator; };
NodeResult<TextBatch> Run(const Inputs& input, const Models&) {
  return NodeResult<TextBatch>::Success(*input.text);
}
auto spec = MakeNodeSpec(InputsOf<Inputs>{Required("text", &Inputs::text)},
                        PreservedOutput<TextBatch>("answer", "text"),
                        ModelsOf<Models>{Model("generator", "bind_model", &Models::generator)}, &Run);
]=] ON "")
compile_case(config_type_mismatch [=[
NodeResult<TextBatch> Run(const Inputs&, const DifferentParams&) {
  return NodeResult<TextBatch>::Success({});
}
auto spec = MakeNodeSpec(InputsOf<Inputs>{Required("text", &Inputs::text)},
                        PreservedOutput<TextBatch>("answer", "text"),
                        Parameters<Params>{Field("prefix", &Params::prefix)}, &Run);
]=] OFF "Node Run must be callable")
compile_case(raw_batch_return [=[
TextBatch Run(const Inputs& input) { return *input.text; }
auto spec = MakeNodeSpec(InputsOf<Inputs>{Required("text", &Inputs::text)},
                        PreservedOutput<TextBatch>("answer", "text"), &Run);
]=] OFF "Node Run must return NodeResult")
compile_case(non_batch_port_member [=[
struct ScalarInputs { std::string text; };
auto binding = Required("text", &ScalarInputs::text);
]=] OFF "Required")
message(STATUS "PASS current public Spec signatures and actual compiler diagnostics")
