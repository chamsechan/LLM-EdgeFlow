#include "engine/backends/onnxruntime/onnxruntime_backend.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "company_alg_log.h"

#ifdef HAVE_ONNXRUNTIME
#include "onnxruntime_cxx_api.h"
#endif

namespace alg_framework {

#ifdef HAVE_ONNXRUNTIME

namespace {

ElementType OnnxTypeToElementType(ONNXTensorElementDataType onnx_type) {
  switch (onnx_type) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
      return ElementType::kFloat32;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:
      return ElementType::kInt32;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:
      return ElementType::kInt64;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:
      return ElementType::kUInt8;
    default:
      return ElementType::kFloat32;
  }
}

ONNXTensorElementDataType ElementTypeToOnnxType(ElementType type) {
  switch (type) {
    case ElementType::kFloat32:
      return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    case ElementType::kInt32:
      return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32;
    case ElementType::kInt64:
      return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
    case ElementType::kUInt8:
      return ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8;
  }
  return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
}

class OnnxTensorGraphSession : public ITensorGraphSession {
 public:
  OnnxTensorGraphSession(std::shared_ptr<Ort::Env> env,
                         std::unique_ptr<Ort::Session> session,
                         std::vector<TensorSpec> inputs,
                         std::vector<TensorSpec> outputs, BatchPolicy policy)
      : env_(std::move(env)),
        session_(std::move(session)),
        inputs_(std::move(inputs)),
        outputs_(std::move(outputs)),
        policy_(policy) {}

  const std::string& BackendType() const noexcept override {
    static const std::string type = OnnxRuntimeBackend::kBackendType;
    return type;
  }

  ExecutionProtocol Protocol() const noexcept override {
    return ExecutionProtocol::kTensorGraph;
  }

  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }

  BatchPolicy GetBatchPolicy() const noexcept override { return policy_; }

  const std::vector<TensorSpec>& Inputs() const noexcept override {
    return inputs_;
  }

  const std::vector<TensorSpec>& Outputs() const noexcept override {
    return outputs_;
  }

  int Run(const TensorMap& inputs, TensorMap* outputs,
          std::string* diagnostic = nullptr) noexcept override {
    if (!outputs) {
      if (diagnostic) *diagnostic = "Output TensorMap pointer is null";
      return -1;
    }
    outputs->clear();

    if (!session_) {
      if (diagnostic) *diagnostic = "ONNX Runtime session is not initialized";
      return -1;
    }

    try {
      Ort::MemoryInfo mem_info =
          Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

      std::vector<const char*> input_names;
      std::vector<Ort::Value> ort_inputs;
      input_names.reserve(inputs_.size());
      ort_inputs.reserve(inputs_.size());

      for (const auto& spec : inputs_) {
        auto it = inputs.find(spec.name);
        if (it == inputs.end()) {
          if (diagnostic) {
            *diagnostic = "Missing required input tensor: " + spec.name;
          }
          outputs->clear();
          return -1;
        }

        const Tensor& tensor = it->second;
        if (!tensor.buffer || tensor.buffer->Data() == nullptr) {
          if (diagnostic) {
            *diagnostic = "Input tensor buffer is null for: " + spec.name;
          }
          outputs->clear();
          return -1;
        }

        if (tensor.desc.element_type != spec.element_type) {
          if (diagnostic) {
            *diagnostic =
                "Input tensor element type mismatch for: " + spec.name;
          }
          outputs->clear();
          return -1;
        }

        if (tensor.desc.shape.size() != spec.shape.size()) {
          if (diagnostic) {
            *diagnostic = "Input tensor rank mismatch for: " + spec.name;
          }
          outputs->clear();
          return -1;
        }

        for (size_t d = 0; d < spec.shape.size(); ++d) {
          if (spec.shape[d] >= 0 && tensor.desc.shape[d] != spec.shape[d]) {
            if (diagnostic) {
              *diagnostic = "Input tensor dimension " + std::to_string(d) +
                            " mismatch for: " + spec.name;
            }
            outputs->clear();
            return -1;
          }
        }

        ONNXTensorElementDataType onnx_type =
            ElementTypeToOnnxType(tensor.desc.element_type);
        Ort::Value ort_val = Ort::Value::CreateTensor(
            mem_info, const_cast<void*>(tensor.buffer->Data()),
            tensor.buffer->ByteSize(), tensor.desc.shape.data(),
            tensor.desc.shape.size(), onnx_type);

        input_names.push_back(spec.name.c_str());
        ort_inputs.push_back(std::move(ort_val));
      }

      std::vector<const char*> output_names;
      output_names.reserve(outputs_.size());
      for (const auto& spec : outputs_) {
        output_names.push_back(spec.name.c_str());
      }

      Ort::RunOptions run_options;
      auto ort_outputs = session_->Run(
          run_options, input_names.data(), ort_inputs.data(), ort_inputs.size(),
          output_names.data(), output_names.size());

      if (ort_outputs.size() != outputs_.size()) {
        if (diagnostic) {
          *diagnostic = "ONNX Runtime returned unexpected output count";
        }
        outputs->clear();
        return -1;
      }

      for (size_t i = 0; i < outputs_.size(); ++i) {
        const auto& spec = outputs_[i];
        Ort::Value& ort_out = ort_outputs[i];

        if (!ort_out.IsTensor()) {
          if (diagnostic) {
            *diagnostic = "Output is not a tensor: " + spec.name;
          }
          outputs->clear();
          return -1;
        }

        auto type_info = ort_out.GetTensorTypeAndShapeInfo();
        TensorDesc out_desc;
        out_desc.element_type =
            OnnxTypeToElementType(type_info.GetElementType());
        out_desc.shape = type_info.GetShape();

        Tensor host_tensor;
        if (!CreateHostTensor(out_desc, &host_tensor, diagnostic)) {
          outputs->clear();
          return -1;
        }

        const void* src_data = ort_out.GetTensorData<void>();
        if (src_data && host_tensor.buffer) {
          std::memcpy(host_tensor.buffer->MutableData(), src_data,
                      host_tensor.buffer->ByteSize());
        }

        (*outputs)[spec.name] = std::move(host_tensor);
      }

      return 0;
    } catch (const Ort::Exception& e) {
      if (diagnostic) {
        *diagnostic = std::string("ONNX Runtime Exception: ") + e.what();
      }
      outputs->clear();
      return -1;
    } catch (const std::exception& e) {
      if (diagnostic) {
        *diagnostic = std::string("Standard Exception: ") + e.what();
      }
      outputs->clear();
      return -1;
    } catch (...) {
      if (diagnostic) {
        *diagnostic = "Unknown error during ONNX Runtime execution";
      }
      outputs->clear();
      return -1;
    }
  }

 private:
  std::shared_ptr<Ort::Env> env_;
  std::unique_ptr<Ort::Session> session_;
  std::vector<TensorSpec> inputs_;
  std::vector<TensorSpec> outputs_;
  BatchPolicy policy_;
};

}  // namespace

#endif

OnnxRuntimeBackend::OnnxRuntimeBackend() = default;
OnnxRuntimeBackend::~OnnxRuntimeBackend() = default;

const std::string& OnnxRuntimeBackend::BackendType() const noexcept {
  static const std::string type = kBackendType;
  return type;
}

std::shared_ptr<IBackendSession> OnnxRuntimeBackend::Load(
    const BackendLoadSpec& spec, std::string* diagnostic) noexcept {
#ifndef HAVE_ONNXRUNTIME
  if (diagnostic) {
    *diagnostic =
        "ONNX Runtime backend was not compiled into this build "
        "(HAVE_ONNXRUNTIME missing)";
  }
  return nullptr;
#else
  if (spec.model_path.empty()) {
    if (diagnostic) *diagnostic = "Model path is empty";
    return nullptr;
  }

  std::error_code ec;
  if (!std::filesystem::exists(spec.model_path, ec) || ec) {
    if (diagnostic) {
      *diagnostic = "Model file does not exist on disk: " + spec.model_path;
    }
    return nullptr;
  }

  try {
    auto env = std::make_shared<Ort::Env>(ORT_LOGGING_LEVEL_WARNING,
                                          "OnnxRuntimeBackend");

    Ort::SessionOptions session_options;
    int intra_threads = spec.backend_config.value("intra_op_num_threads", 2);
    int inter_threads = spec.backend_config.value("inter_op_num_threads", 1);
    std::string opt_level_str =
        spec.backend_config.value("graph_optimization_level", "all");

    session_options.SetIntraOpNumThreads(intra_threads);
    session_options.SetInterOpNumThreads(inter_threads);

    if (opt_level_str == "none") {
      session_options.SetGraphOptimizationLevel(
          GraphOptimizationLevel::ORT_DISABLE_ALL);
    } else if (opt_level_str == "basic") {
      session_options.SetGraphOptimizationLevel(
          GraphOptimizationLevel::ORT_ENABLE_BASIC);
    } else if (opt_level_str == "extended") {
      session_options.SetGraphOptimizationLevel(
          GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
    } else {
      session_options.SetGraphOptimizationLevel(
          GraphOptimizationLevel::ORT_ENABLE_ALL);
    }

    auto session = std::make_unique<Ort::Session>(*env, spec.model_path.c_str(),
                                                  session_options);

    Ort::AllocatorWithDefaultOptions allocator;
    size_t num_inputs = session->GetInputCount();
    std::vector<TensorSpec> inputs;
    inputs.reserve(num_inputs);

    for (size_t i = 0; i < num_inputs; ++i) {
      auto name_allocated = session->GetInputNameAllocated(i, allocator);
      std::string name(name_allocated.get());

      auto type_info = session->GetInputTypeInfo(i);
      auto tensor_info = type_info.GetTensorTypeAndShapeInfo();

      TensorSpec in_spec;
      in_spec.name = name;
      in_spec.element_type =
          OnnxTypeToElementType(tensor_info.GetElementType());
      in_spec.shape = tensor_info.GetShape();
      inputs.push_back(std::move(in_spec));
    }

    size_t num_outputs = session->GetOutputCount();
    std::vector<TensorSpec> outputs;
    outputs.reserve(num_outputs);

    for (size_t i = 0; i < num_outputs; ++i) {
      auto name_allocated = session->GetOutputNameAllocated(i, allocator);
      std::string name(name_allocated.get());

      auto type_info = session->GetOutputTypeInfo(i);
      auto tensor_info = type_info.GetTensorTypeAndShapeInfo();

      TensorSpec out_spec;
      out_spec.name = name;
      out_spec.element_type =
          OnnxTypeToElementType(tensor_info.GetElementType());
      out_spec.shape = tensor_info.GetShape();
      outputs.push_back(std::move(out_spec));
    }

    BatchPolicy policy;
    policy.max_batch_size = spec.backend_config.value("max_batch_size", 4);
    policy.fixed_batch_size = 0;

    return std::make_shared<OnnxTensorGraphSession>(
        std::move(env), std::move(session), std::move(inputs),
        std::move(outputs), policy);
  } catch (const Ort::Exception& e) {
    if (diagnostic) {
      *diagnostic = std::string("ONNX Runtime Load Exception: ") + e.what();
    }
    return nullptr;
  } catch (const std::exception& e) {
    if (diagnostic) {
      *diagnostic = std::string("Standard Exception: ") + e.what();
    }
    return nullptr;
  } catch (...) {
    if (diagnostic) {
      *diagnostic = "Unknown error during ONNX Runtime Load";
    }
    return nullptr;
  }
#endif
}

static const BackendDefinition kOnnxRuntimeBackendDefinition = [] {
  BackendDefinition def;
  def.backend_type = OnnxRuntimeBackend::kBackendType;
  def.description = "Microsoft ONNX Runtime TensorGraph inference backend";
  def.supported_protocols = {ExecutionProtocol::kTensorGraph};
  def.concurrency = InferenceConcurrency::kConcurrent;
  def.config_fields = {
      {"intra_op_num_threads", ConfigValueKind::kInteger, false, 2, 1.0, 64.0},
      {"inter_op_num_threads", ConfigValueKind::kInteger, false, 1, 1.0, 64.0},
      {"graph_optimization_level",
       ConfigValueKind::kString,
       false,
       "all",
       std::nullopt,
       std::nullopt,
       {"none", "basic", "extended", "all"}},
      {"device_id", ConfigValueKind::kInteger, false, -1, -1.0, 16.0},
  };
  return def;
}();

REGISTER_BACKEND_WITH_DEFINITION(OnnxRuntimeBackend,
                                 kOnnxRuntimeBackendDefinition);

}  // namespace alg_framework
