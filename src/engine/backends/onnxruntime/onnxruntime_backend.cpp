#include "engine/backends/onnxruntime/onnxruntime_backend.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "company_alg_log.h"

#ifdef HAVE_ONNXRUNTIME
#include "onnxruntime_cxx_api.h"
#endif

namespace alg_framework {

namespace onnxruntime_detail {

bool ValidateInputTensor(const Tensor& tensor, const TensorSpec& spec,
                         const BatchPolicy& policy,
                         std::string* diagnostic) noexcept {
  if (!tensor.buffer || tensor.buffer->Data() == nullptr) {
    if (diagnostic) *diagnostic = "Input tensor buffer is null: " + spec.name;
    return false;
  }

  if (tensor.desc.element_type != spec.element_type) {
    if (diagnostic) {
      *diagnostic = "Input tensor element type mismatch for: " + spec.name;
    }
    return false;
  }

  if (tensor.desc.shape.size() != spec.shape.size()) {
    if (diagnostic) {
      *diagnostic = "Input tensor rank mismatch for: " + spec.name +
                    ". Expected rank: " + std::to_string(spec.shape.size()) +
                    ", got: " + std::to_string(tensor.desc.shape.size());
    }
    return false;
  }

  for (size_t d = 0; d < spec.shape.size(); ++d) {
    if (tensor.desc.shape[d] < 0) {
      if (diagnostic) {
        *diagnostic = "Input tensor has negative dimension " +
                      std::to_string(d) + " for: " + spec.name;
      }
      return false;
    }
    if (spec.shape[d] >= 0 && tensor.desc.shape[d] != spec.shape[d]) {
      if (diagnostic) {
        *diagnostic = "Input tensor static dimension " + std::to_string(d) +
                      " mismatch for: " + spec.name +
                      ". Expected: " + std::to_string(spec.shape[d]) +
                      ", got: " + std::to_string(tensor.desc.shape[d]);
      }
      return false;
    }
  }

  if (!tensor.desc.shape.empty()) {
    int64_t batch = tensor.desc.shape[0];
    if (batch <= 0) {
      if (diagnostic) {
        *diagnostic = "Input tensor batch must be positive for: " + spec.name;
      }
      return false;
    }
    if (policy.fixed_batch_size > 0 &&
        static_cast<size_t>(batch) != policy.fixed_batch_size) {
      if (diagnostic) {
        *diagnostic =
            "Input tensor fixed batch size mismatch for: " + spec.name +
            ". Expected: " + std::to_string(policy.fixed_batch_size) +
            ", got: " + std::to_string(batch);
      }
      return false;
    }
    if (static_cast<size_t>(batch) > policy.max_batch_size) {
      if (diagnostic) {
        *diagnostic =
            "Input tensor batch exceeds max_batch_size for: " + spec.name +
            ". Max: " + std::to_string(policy.max_batch_size) +
            ", got: " + std::to_string(batch);
      }
      return false;
    }
  }

  size_t elem_size = ElementTypeByteSize(tensor.desc.element_type);
  size_t expected_bytes = 0;
  if (!inference_detail::ComputeTensorByteSize(tensor.desc, elem_size,
                                               &expected_bytes, diagnostic)) {
    return false;
  }

  if (tensor.buffer->ByteSize() != expected_bytes) {
    if (diagnostic) {
      *diagnostic = "Input tensor buffer byte size mismatch for: " + spec.name +
                    ". Expected: " + std::to_string(expected_bytes) +
                    ", got: " + std::to_string(tensor.buffer->ByteSize());
    }
    return false;
  }

  return true;
}

bool ValidateOutputMetadata(ElementType element_type,
                            const std::vector<int64_t>& shape,
                            size_t runtime_element_count,
                            const TensorSpec& spec, size_t expected_batch,
                            std::string* diagnostic) noexcept {
  if (element_type != spec.element_type) {
    if (diagnostic) {
      *diagnostic = "Output tensor element type mismatch for: " + spec.name;
    }
    return false;
  }

  if (shape.size() != spec.shape.size()) {
    if (diagnostic) {
      *diagnostic = "Output tensor rank mismatch for: " + spec.name;
    }
    return false;
  }

  for (size_t d = 0; d < spec.shape.size(); ++d) {
    if (shape[d] < 0) {
      if (diagnostic) {
        *diagnostic =
            "Output tensor has negative runtime dimension: " + spec.name;
      }
      return false;
    }
    if (spec.shape[d] >= 0 && shape[d] != spec.shape[d]) {
      if (diagnostic) {
        *diagnostic =
            "Output tensor static dimension mismatch for: " + spec.name;
      }
      return false;
    }
  }

  if (expected_batch > 0 &&
      (shape.empty() || static_cast<size_t>(shape[0]) != expected_batch)) {
    if (diagnostic) {
      *diagnostic = "Output tensor batch dimension mismatch for: " + spec.name;
    }
    return false;
  }

  TensorDesc desc{element_type, shape};
  const size_t element_size = ElementTypeByteSize(element_type);
  size_t expected_bytes = 0;
  if (!inference_detail::ComputeTensorByteSize(desc, element_size,
                                               &expected_bytes, diagnostic)) {
    return false;
  }
  if (element_size == 0 ||
      expected_bytes / element_size != runtime_element_count) {
    if (diagnostic) {
      *diagnostic = "Output tensor element count mismatch for: " + spec.name;
    }
    return false;
  }
  return true;
}

bool InferBatchPolicy(const std::vector<TensorSpec>& inputs,
                      const std::vector<TensorSpec>& outputs,
                      size_t configured_max_batch, BatchPolicy* policy,
                      std::string* diagnostic) noexcept {
  if (!policy) {
    if (diagnostic) *diagnostic = "Output BatchPolicy pointer is null";
    return false;
  }
  *policy = {};
  if (configured_max_batch == 0) {
    if (diagnostic) *diagnostic = "Configured max_batch_size must be positive";
    return false;
  }

  try {
    std::optional<size_t> static_batch;
    const auto inspect = [&](const std::vector<TensorSpec>& specs,
                             const char* kind) -> bool {
      for (const auto& spec : specs) {
        if (spec.shape.empty()) {
          if (diagnostic) {
            *diagnostic = std::string("ONNX ") + kind +
                          " tensor has rank 0: " + spec.name;
          }
          return false;
        }
        const int64_t batch_dimension = spec.shape.front();
        if (batch_dimension == 0) {
          if (diagnostic) {
            *diagnostic = std::string("ONNX ") + kind +
                          " tensor batch dimension cannot be 0: " + spec.name;
          }
          return false;
        }
        if (batch_dimension < 0) continue;

        const size_t candidate = static_cast<size_t>(batch_dimension);
        if (static_batch.has_value() && *static_batch != candidate) {
          if (diagnostic) {
            *diagnostic =
                "Conflicting static ONNX batch dimensions. Expected " +
                std::to_string(*static_batch) + ", got " +
                std::to_string(candidate) + " for " + kind +
                " tensor: " + spec.name;
          }
          return false;
        }
        static_batch = candidate;
      }
      return true;
    };

    if (!inspect(inputs, "input") || !inspect(outputs, "output")) {
      return false;
    }

    if (static_batch.has_value()) {
      *policy = BatchPolicy{*static_batch, *static_batch};
    } else {
      *policy = BatchPolicy{configured_max_batch, 0};
    }
    return true;
  } catch (...) {
    *policy = {};
    if (diagnostic) *diagnostic = "Exception inferring ONNX BatchPolicy";
    return false;
  }
}

}  // namespace onnxruntime_detail

namespace {

std::string NormalizePlatform(std::string platform) {
  std::transform(
      platform.begin(), platform.end(), platform.begin(),
      [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return platform;
}

bool ValidateExecutionTarget(const ExecutionTarget& target,
                             std::string* diagnostic) {
  const std::string platform = NormalizePlatform(target.platform);
  if (!platform.empty() && platform != "UNKNOWN" && platform != "CPU" &&
      platform != "CPU_GENERIC") {
    if (diagnostic) {
      *diagnostic =
          "ONNX Runtime backend only supports CPU execution in this "
          "build; requested platform: " +
          target.platform;
    }
    return false;
  }
  if (target.device_id.has_value() && *target.device_id != 0) {
    if (diagnostic) {
      *diagnostic = "ONNX Runtime CPU backend only accepts device_id 0; got: " +
                    std::to_string(*target.device_id);
    }
    return false;
  }
  return true;
}

}  // namespace

#ifdef HAVE_ONNXRUNTIME

namespace {

std::optional<ElementType> TryMapOnnxElementType(
    ONNXTensorElementDataType onnx_type) noexcept {
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
      return std::nullopt;
  }
}

ONNXTensorElementDataType ElementTypeToOnnxType(ElementType type) noexcept {
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
      size_t expected_batch = 0;

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
        if (!onnxruntime_detail::ValidateInputTensor(tensor, spec, policy_,
                                                     diagnostic)) {
          outputs->clear();
          return -1;
        }
        const size_t input_batch =
            static_cast<size_t>(tensor.desc.shape.front());
        if (expected_batch == 0) {
          expected_batch = input_batch;
        } else if (input_batch != expected_batch) {
          if (diagnostic) {
            *diagnostic = "Input tensor batch dimensions are inconsistent";
          }
          outputs->clear();
          return -1;
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
        auto elem_type_opt = TryMapOnnxElementType(type_info.GetElementType());
        if (!elem_type_opt.has_value()) {
          if (diagnostic) {
            *diagnostic =
                "Output tensor has unsupported element type for: " + spec.name;
          }
          outputs->clear();
          return -1;
        }

        auto shape = type_info.GetShape();
        if (!onnxruntime_detail::ValidateOutputMetadata(
                *elem_type_opt, shape, type_info.GetElementCount(), spec,
                expected_batch, diagnostic)) {
          outputs->clear();
          return -1;
        }

        TensorDesc out_desc;
        out_desc.element_type = *elem_type_opt;
        out_desc.shape = shape;

        Tensor host_tensor;
        if (!CreateHostTensor(out_desc, &host_tensor, diagnostic)) {
          outputs->clear();
          return -1;
        }

        const void* src_data = ort_out.GetTensorData<void>();
        if (!host_tensor.buffer ||
            (host_tensor.buffer->ByteSize() > 0 && !src_data)) {
          if (diagnostic) {
            *diagnostic = "Output tensor data is null for: " + spec.name;
          }
          outputs->clear();
          return -1;
        }
        if (host_tensor.buffer->ByteSize() > 0) {
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
  if (spec.requested_protocol.has_value() &&
      *spec.requested_protocol != ExecutionProtocol::kTensorGraph) {
    if (diagnostic) {
      *diagnostic =
          "ONNX Runtime backend does not support requested protocol: " +
          std::string(ExecutionProtocolName(*spec.requested_protocol));
    }
    return nullptr;
  }
  if (!ValidateExecutionTarget(spec.execution_target, diagnostic)) {
    return nullptr;
  }
#ifndef HAVE_ONNXRUNTIME
  static_cast<void>(spec);
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
  if (!std::filesystem::is_regular_file(spec.model_path, ec) || ec) {
    if (diagnostic) {
      *diagnostic = "Model file does not exist or is not a regular file: " +
                    spec.model_path;
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
      if (name.empty()) {
        if (diagnostic) *diagnostic = "Model has input with empty name";
        return nullptr;
      }

      auto type_info = session->GetInputTypeInfo(i);
      if (type_info.GetONNXType() != ONNX_TYPE_TENSOR) {
        if (diagnostic) {
          *diagnostic = "Model input is not a tensor: " + name;
        }
        return nullptr;
      }
      auto tensor_info = type_info.GetTensorTypeAndShapeInfo();
      auto elem_type_opt = TryMapOnnxElementType(tensor_info.GetElementType());
      if (!elem_type_opt.has_value()) {
        if (diagnostic) {
          *diagnostic =
              "Model input has unsupported ONNX data type for: " + name;
        }
        return nullptr;
      }

      auto shape = tensor_info.GetShape();
      if (shape.empty()) {
        if (diagnostic) *diagnostic = "Model input has rank 0: " + name;
        return nullptr;
      }

      TensorSpec in_spec;
      in_spec.name = name;
      in_spec.element_type = *elem_type_opt;
      in_spec.shape = std::move(shape);
      inputs.push_back(std::move(in_spec));
    }

    size_t num_outputs = session->GetOutputCount();
    std::vector<TensorSpec> outputs;
    outputs.reserve(num_outputs);

    for (size_t i = 0; i < num_outputs; ++i) {
      auto name_allocated = session->GetOutputNameAllocated(i, allocator);
      std::string name(name_allocated.get());
      if (name.empty()) {
        if (diagnostic) *diagnostic = "Model has output with empty name";
        return nullptr;
      }

      auto type_info = session->GetOutputTypeInfo(i);
      if (type_info.GetONNXType() != ONNX_TYPE_TENSOR) {
        if (diagnostic) {
          *diagnostic = "Model output is not a tensor: " + name;
        }
        return nullptr;
      }
      auto tensor_info = type_info.GetTensorTypeAndShapeInfo();
      auto elem_type_opt = TryMapOnnxElementType(tensor_info.GetElementType());
      if (!elem_type_opt.has_value()) {
        if (diagnostic) {
          *diagnostic =
              "Model output has unsupported ONNX data type for: " + name;
        }
        return nullptr;
      }

      auto shape = tensor_info.GetShape();
      if (shape.empty()) {
        if (diagnostic) *diagnostic = "Model output has rank 0: " + name;
        return nullptr;
      }

      TensorSpec out_spec;
      out_spec.name = name;
      out_spec.element_type = *elem_type_opt;
      out_spec.shape = std::move(shape);
      outputs.push_back(std::move(out_spec));
    }

    const size_t config_max_batch =
        spec.backend_config.value("max_batch_size", 4);
    BatchPolicy policy;
    if (!onnxruntime_detail::InferBatchPolicy(inputs, outputs, config_max_batch,
                                              &policy, diagnostic)) {
      return nullptr;
    }

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

#ifdef HAVE_ONNXRUNTIME
static const BackendDefinition kOnnxRuntimeBackendDefinition = [] {
  BackendDefinition def;
  def.backend_type = OnnxRuntimeBackend::kBackendType;
  def.description = "Microsoft ONNX Runtime TensorGraph inference backend";
  def.supported_protocols = {ExecutionProtocol::kTensorGraph};
  def.concurrency = InferenceConcurrency::kConcurrent;
  def.config_fields = {
      {"max_batch_size", ConfigValueKind::kInteger, false, 4, 1.0, 1024.0},
      {"intra_op_num_threads", ConfigValueKind::kInteger, false, 2, 1.0, 64.0},
      {"inter_op_num_threads", ConfigValueKind::kInteger, false, 1, 1.0, 64.0},
      {"graph_optimization_level",
       ConfigValueKind::kString,
       false,
       "all",
       std::nullopt,
       std::nullopt,
       {"none", "basic", "extended", "all"}},
  };
  return def;
}();

REGISTER_BACKEND_WITH_DEFINITION(OnnxRuntimeBackend,
                                 kOnnxRuntimeBackendDefinition);
#endif

}  // namespace alg_framework
