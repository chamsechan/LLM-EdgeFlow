#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "contracts/config_schema.h"

namespace alg_framework {

/**
 * @brief 底层执行协议枚举
 */
enum class ExecutionProtocol {
  kTensorGraph,
  kTextGeneration,
};

/**
 * @brief 推理并发模型
 */
enum class InferenceConcurrency {
  kSerialized,
  kConcurrent,
};

inline bool IsValidExecutionProtocol(ExecutionProtocol protocol) noexcept {
  switch (protocol) {
    case ExecutionProtocol::kTensorGraph:
    case ExecutionProtocol::kTextGeneration:
      return true;
    default:
      return false;
  }
}

inline bool IsValidInferenceConcurrency(
    InferenceConcurrency concurrency) noexcept {
  switch (concurrency) {
    case InferenceConcurrency::kSerialized:
    case InferenceConcurrency::kConcurrent:
      return true;
    default:
      return false;
  }
}

inline bool IsConcurrencyCompatible(InferenceConcurrency declared,
                                    InferenceConcurrency actual) noexcept {
  if (!IsValidInferenceConcurrency(declared) ||
      !IsValidInferenceConcurrency(actual)) {
    return false;
  }
  return declared != InferenceConcurrency::kConcurrent ||
         actual == InferenceConcurrency::kConcurrent;
}

/**
 * @brief 批处理调度策略
 */
struct BatchPolicy {
  size_t max_batch_size = 1;
  size_t fixed_batch_size = 0;  // 0 表示可变执行 batch，>0 表示必须为固定 batch
};

/**
 * @brief 中性 Tensor 元素类型枚举
 */
enum class ElementType {
  kFloat32,
  kInt32,
  kInt64,
  kUInt8,
};

/**
 * @brief 中性 Tensor 描述元数据
 */
struct TensorDesc {
  ElementType element_type = ElementType::kFloat32;
  std::vector<int64_t> shape;
};

/**
 * @brief 中性 Tensor 内存缓冲区纯虚抽象
 */
class ITensorBuffer {
 public:
  virtual ~ITensorBuffer() = default;
  virtual const void* Data() const noexcept = 0;
  virtual void* MutableData() noexcept = 0;
  virtual size_t ByteSize() const noexcept = 0;
};

/**
 * @brief 中性 Tensor 结构体
 */
struct Tensor {
  TensorDesc desc;
  std::shared_ptr<ITensorBuffer> buffer;
};

/**
 * @brief 中性 Tensor 端口规格说明
 */
struct TensorSpec {
  std::string name;
  ElementType element_type = ElementType::kFloat32;
  std::vector<int64_t> shape;
};

using TensorMap = std::unordered_map<std::string, Tensor>;

/**
 * @brief 获取元素类型字节大小 (未知类型严格 fail-closed 返回 0)
 */
inline size_t ElementTypeByteSize(ElementType type) noexcept {
  switch (type) {
    case ElementType::kFloat32:
      return sizeof(float);
    case ElementType::kInt32:
      return sizeof(int32_t);
    case ElementType::kInt64:
      return sizeof(int64_t);
    case ElementType::kUInt8:
      return sizeof(uint8_t);
    default:
      return 0;
  }
}

inline const char* ElementTypeToString(ElementType type) noexcept {
  switch (type) {
    case ElementType::kFloat32:
      return "float32";
    case ElementType::kInt32:
      return "int32";
    case ElementType::kInt64:
      return "int64";
    case ElementType::kUInt8:
      return "uint8";
    default:
      return "unknown";
  }
}

/**
 * @brief 原生 C++ 类型到 ElementType 映射特化
 */
template <typename T>
struct NativeTypeTraits {
  static constexpr bool kSupported = false;
};

template <>
struct NativeTypeTraits<float> {
  static constexpr bool kSupported = true;
  static constexpr ElementType kElementType = ElementType::kFloat32;
};

template <>
struct NativeTypeTraits<int32_t> {
  static constexpr bool kSupported = true;
  static constexpr ElementType kElementType = ElementType::kInt32;
};

template <>
struct NativeTypeTraits<int64_t> {
  static constexpr bool kSupported = true;
  static constexpr ElementType kElementType = ElementType::kInt64;
};

template <>
struct NativeTypeTraits<uint8_t> {
  static constexpr bool kSupported = true;
  static constexpr ElementType kElementType = ElementType::kUInt8;
};

/**
 * @brief 具 64 字节硬件对齐保证的 Host 内存 Buffer 实现
 */
class HostTensorBuffer : public ITensorBuffer {
 public:
  explicit HostTensorBuffer(size_t byte_size) {
    if (byte_size > 0) {
      size_t alignment = 64;
      void* ptr = nullptr;
      int ret = posix_memalign(&ptr, alignment, byte_size);
      if (ret == 0 && ptr != nullptr) {
        data_ = ptr;
        size_ = byte_size;
        std::memset(data_, 0, size_);
      } else {
        data_ = nullptr;
        size_ = 0;
      }
    }
  }

  HostTensorBuffer(const HostTensorBuffer&) = delete;
  HostTensorBuffer& operator=(const HostTensorBuffer&) = delete;
  HostTensorBuffer(HostTensorBuffer&&) = delete;
  HostTensorBuffer& operator=(HostTensorBuffer&&) = delete;

  ~HostTensorBuffer() override {
    if (data_) {
      std::free(data_);
      data_ = nullptr;
    }
  }

  const void* Data() const noexcept override { return data_; }
  void* MutableData() noexcept override { return data_; }
  size_t ByteSize() const noexcept override { return size_; }
  bool IsValid() const noexcept { return data_ != nullptr || size_ == 0; }

 private:
  void* data_ = nullptr;
  size_t size_ = 0;
};

namespace inference_detail {

inline void SetDiagnostic(std::string* diagnostic,
                          const char* message) noexcept {
  if (!diagnostic) return;
  try {
    *diagnostic = message;
  } catch (...) {
  }
}

inline bool ComputeTensorByteSize(const TensorDesc& desc, size_t element_size,
                                  size_t* byte_size,
                                  std::string* diagnostic) noexcept {
  if (!byte_size || element_size == 0) {
    SetDiagnostic(diagnostic, "Invalid tensor byte-size calculation request");
    return false;
  }

  try {
    size_t element_count = 1;
    for (int64_t dim : desc.shape) {
      if (dim < 0) {
        SetDiagnostic(diagnostic,
                      "Negative or unresolved dynamic dimension is not allowed "
                      "at runtime");
        return false;
      }
      const size_t extent = static_cast<size_t>(dim);
      if (extent != 0 &&
          element_count > std::numeric_limits<size_t>::max() / extent) {
        SetDiagnostic(diagnostic,
                      "Shape element count multiplication overflow");
        return false;
      }
      element_count *= extent;
    }
    if (element_count != 0 &&
        element_count > std::numeric_limits<size_t>::max() / element_size) {
      SetDiagnostic(diagnostic, "Tensor byte size overflow");
      return false;
    }
    *byte_size = element_count * element_size;
    return true;
  } catch (...) {
    SetDiagnostic(diagnostic,
                  "Exception while validating tensor shape and byte size");
    return false;
  }
}

}  // namespace inference_detail

/**
 * @brief 分配并创建 Host 端 Tensor 实例 (严格 fail-closed)
 */
inline bool CreateHostTensor(const TensorDesc& desc, Tensor* tensor,
                             std::string* diagnostic = nullptr) noexcept {
  try {
    if (!tensor) {
      inference_detail::SetDiagnostic(diagnostic,
                                      "Output tensor pointer is null");
      return false;
    }
    tensor->buffer.reset();
    tensor->desc = {};

    const size_t element_size = ElementTypeByteSize(desc.element_type);
    if (element_size == 0) {
      inference_detail::SetDiagnostic(diagnostic,
                                      "Unknown or unsupported ElementType");
      return false;
    }

    size_t total_bytes = 0;
    if (!inference_detail::ComputeTensorByteSize(desc, element_size,
                                                 &total_bytes, diagnostic)) {
      return false;
    }

    auto buf = std::make_shared<HostTensorBuffer>(total_bytes);
    if (!buf->IsValid()) {
      inference_detail::SetDiagnostic(
          diagnostic, "Failed to allocate aligned HostTensorBuffer");
      return false;
    }
    Tensor staged{desc, std::move(buf)};
    *tensor = std::move(staged);
    return true;
  } catch (...) {
    if (tensor) {
      try {
        tensor->buffer.reset();
        tensor->desc = {};
      } catch (...) {
      }
    }
    inference_detail::SetDiagnostic(diagnostic,
                                    "Exception creating host tensor");
    return false;
  }
}

/**
 * @brief 安全类型检查 Tensor 常量数据指针访问器
 */
template <typename T>
inline const T* GetTensorData(const Tensor& tensor,
                              std::string* diagnostic = nullptr) noexcept {
  static_assert(NativeTypeTraits<T>::kSupported,
                "Unsupported native type for GetTensorData");
  try {
    if (!tensor.buffer) {
      inference_detail::SetDiagnostic(diagnostic, "Tensor buffer is null");
      return nullptr;
    }
    if (tensor.desc.element_type != NativeTypeTraits<T>::kElementType) {
      inference_detail::SetDiagnostic(diagnostic,
                                      "Tensor element type mismatch");
      return nullptr;
    }
    const void* raw_data = tensor.buffer->Data();
    if (!raw_data && tensor.buffer->ByteSize() > 0) {
      inference_detail::SetDiagnostic(diagnostic, "Tensor buffer data is null");
      return nullptr;
    }
    if (reinterpret_cast<uintptr_t>(raw_data) % alignof(T) != 0) {
      inference_detail::SetDiagnostic(
          diagnostic, "Tensor buffer is misaligned for requested type");
      return nullptr;
    }
    size_t expected_bytes = 0;
    if (!inference_detail::ComputeTensorByteSize(tensor.desc, sizeof(T),
                                                 &expected_bytes, diagnostic)) {
      return nullptr;
    }
    if (tensor.buffer->ByteSize() != expected_bytes) {
      inference_detail::SetDiagnostic(diagnostic,
                                      "Tensor buffer byte size mismatch");
      return nullptr;
    }
    return static_cast<const T*>(raw_data);
  } catch (...) {
    inference_detail::SetDiagnostic(diagnostic,
                                    "Exception accessing tensor data");
    return nullptr;
  }
}

/**
 * @brief 安全类型检查 Tensor 可变数据指针访问器
 */
template <typename T>
inline T* GetMutableTensorData(Tensor* tensor,
                               std::string* diagnostic = nullptr) noexcept {
  static_assert(NativeTypeTraits<T>::kSupported,
                "Unsupported native type for GetMutableTensorData");
  try {
    if (!tensor || !tensor->buffer) {
      inference_detail::SetDiagnostic(diagnostic,
                                      "Tensor or tensor buffer is null");
      return nullptr;
    }
    if (tensor->desc.element_type != NativeTypeTraits<T>::kElementType) {
      inference_detail::SetDiagnostic(diagnostic,
                                      "Tensor element type mismatch");
      return nullptr;
    }
    void* raw_data = tensor->buffer->MutableData();
    if (!raw_data && tensor->buffer->ByteSize() > 0) {
      inference_detail::SetDiagnostic(diagnostic,
                                      "Tensor buffer mutable data is null");
      return nullptr;
    }
    if (reinterpret_cast<uintptr_t>(raw_data) % alignof(T) != 0) {
      inference_detail::SetDiagnostic(
          diagnostic, "Tensor buffer is misaligned for requested type");
      return nullptr;
    }
    size_t expected_bytes = 0;
    if (!inference_detail::ComputeTensorByteSize(tensor->desc, sizeof(T),
                                                 &expected_bytes, diagnostic)) {
      return nullptr;
    }
    if (tensor->buffer->ByteSize() != expected_bytes) {
      inference_detail::SetDiagnostic(diagnostic,
                                      "Tensor buffer byte size mismatch");
      return nullptr;
    }
    return static_cast<T*>(raw_data);
  } catch (...) {
    inference_detail::SetDiagnostic(diagnostic,
                                    "Exception accessing mutable tensor data");
    return nullptr;
  }
}

/**
 * @brief 模型语义定义元数据 (ModelDefinition)
 */
struct ModelDefinition {
  std::string model_type;
  std::string capability;
  std::string description;
  ExecutionProtocol required_protocol = ExecutionProtocol::kTensorGraph;
  std::vector<ConfigFieldDefinition> config_fields;
  InferenceConcurrency concurrency = InferenceConcurrency::kSerialized;
};

/**
 * @brief 推理后端定义元数据 (BackendDefinition)
 */
struct BackendDefinition {
  std::string backend_type;
  std::string description;
  std::vector<ExecutionProtocol> supported_protocols;
  std::vector<ConfigFieldDefinition> config_fields;
  InferenceConcurrency concurrency = InferenceConcurrency::kSerialized;
};

inline const char* ExecutionProtocolName(ExecutionProtocol protocol) noexcept {
  switch (protocol) {
    case ExecutionProtocol::kTensorGraph:
      return "tensor_graph";
    case ExecutionProtocol::kTextGeneration:
      return "text_generation";
    default:
      return "unknown";
  }
}

inline const char* InferenceConcurrencyName(
    InferenceConcurrency conc) noexcept {
  switch (conc) {
    case InferenceConcurrency::kSerialized:
      return "serialized";
    case InferenceConcurrency::kConcurrent:
      return "concurrent";
    default:
      return "unknown";
  }
}

inline const char* ElementTypeName(ElementType type) noexcept {
  switch (type) {
    case ElementType::kFloat32:
      return "float32";
    case ElementType::kInt32:
      return "int32";
    case ElementType::kInt64:
      return "int64";
    case ElementType::kUInt8:
      return "uint8";
    default:
      return "unknown";
  }
}

}  // namespace alg_framework
