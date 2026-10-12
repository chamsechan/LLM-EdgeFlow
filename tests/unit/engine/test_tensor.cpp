#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "engine/tensor.h"
#include "tests/support/scoped_allocation_failure.h"

#ifdef LLM_EDGEFLOW_TEST_WRAP_POSIX_MEMALIGN
namespace {
thread_local bool fail_tensor_allocation = false;
}
extern "C" int __real_posix_memalign(void**, size_t, size_t);
extern "C" int __wrap_posix_memalign(void** pointer, size_t alignment,
                                     size_t bytes) {
  if (fail_tensor_allocation) {
    fail_tensor_allocation = false;
    return ENOMEM;
  }
  return __real_posix_memalign(pointer, alignment, bytes);
}
#endif

namespace llm_edgeflow {
namespace {

class ExternalBuffer final : public ITensorBuffer {
 public:
  ExternalBuffer(size_t size, size_t offset = 0, bool null = false)
      : size_(size), offset_(offset), null_(null) {}
  const void* Data() const noexcept override {
    return null_ ? nullptr : bytes_.data() + offset_;
  }
  void* MutableData() noexcept override {
    return null_ ? nullptr : bytes_.data() + offset_;
  }
  size_t ByteSize() const noexcept override { return size_; }

 private:
  alignas(64) std::array<uint8_t, 64> bytes_{};
  size_t size_, offset_;
  bool null_;
};

TEST(TensorStorageTest, AllocatesZeroedAlignedStorageWithSharedOwnership) {
  Tensor tensor;
  ASSERT_TRUE(CreateHostTensor({ElementType::kFloat32, {2, 3}}, &tensor));
  ASSERT_EQ(tensor.buffer->ByteSize(), 24U);
  auto* data = GetMutableTensorData<float>(&tensor);
  ASSERT_NE(data, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(data) % 64, 0U);
  for (size_t i = 0; i < 6; ++i) {
    EXPECT_EQ(data[i], 0.0F);
    data[i] = static_cast<float>(i + 1);
  }
  Tensor retained = tensor;
  tensor = {};
  ASSERT_NE(GetTensorData<float>(retained), nullptr);
  EXPECT_EQ(std::vector<float>(GetTensorData<float>(retained),
                               GetTensorData<float>(retained) + 6),
            (std::vector<float>{1, 2, 3, 4, 5, 6}));
  ASSERT_TRUE(CreateHostTensor({ElementType::kInt64, {}}, &tensor));
  EXPECT_EQ(tensor.buffer->ByteSize(), 8U);
  ASSERT_NE(GetMutableTensorData<int64_t>(&tensor), nullptr);
  *GetMutableTensorData<int64_t>(&tensor) = 73;
  EXPECT_EQ(*GetTensorData<int64_t>(tensor), 73);
  ASSERT_TRUE(CreateHostTensor({ElementType::kInt32, {2}}, &tensor));
  EXPECT_EQ(tensor.buffer->ByteSize(), 8U);
  ASSERT_NE(GetMutableTensorData<int32_t>(&tensor), nullptr);
  GetMutableTensorData<int32_t>(&tensor)[1] = -7;
  EXPECT_EQ(GetTensorData<int32_t>(tensor)[1], -7);
  ASSERT_TRUE(CreateHostTensor({ElementType::kUInt8, {2}}, &tensor));
  EXPECT_EQ(tensor.buffer->ByteSize(), 2U);
  ASSERT_NE(GetMutableTensorData<uint8_t>(&tensor), nullptr);
  GetMutableTensorData<uint8_t>(&tensor)[1] = 255;
  EXPECT_EQ(GetTensorData<uint8_t>(tensor)[1], 255);
  ASSERT_TRUE(CreateHostTensor({ElementType::kUInt8, {2, 0}}, &tensor));
  EXPECT_EQ(tensor.buffer->ByteSize(), 0U);
  std::string diagnostic;
  GetTensorData<uint8_t>(tensor, &diagnostic);
  EXPECT_TRUE(diagnostic.empty());
}

TEST(TensorStorageTest, InvalidRuntimeShapeAndAllocationClearPriorStorage) {
  const std::vector<std::pair<std::string, TensorDesc>> invalid{
      {"unknown element", {static_cast<ElementType>(99), {1}}},
      {"dynamic dimension", {ElementType::kFloat32, {-1, 3}}},
      {"element count overflow",
       {ElementType::kUInt8, {std::numeric_limits<int64_t>::max(), 3}}},
      {"byte count overflow",
       {ElementType::kInt64, {std::numeric_limits<int64_t>::max()}}}};
  for (const auto& [name, desc] : invalid) {
    SCOPED_TRACE(name);
    Tensor tensor;
    ASSERT_TRUE(CreateHostTensor({ElementType::kFloat32, {1}}, &tensor));
    std::string diagnostic;
    EXPECT_FALSE(CreateHostTensor(desc, &tensor, &diagnostic));
    EXPECT_FALSE(tensor.buffer);
    EXPECT_TRUE(tensor.desc.shape.empty());
    EXPECT_FALSE(diagnostic.empty());
  }
  EXPECT_FALSE(CreateHostTensor({ElementType::kFloat32, {1}}, nullptr));
  Tensor tensor;
  ASSERT_TRUE(CreateHostTensor({ElementType::kFloat32, {1}}, &tensor));
  const TensorDesc desc{ElementType::kFloat32, {3}};
  std::string diagnostic;
  bool success, injected;
  {
    test_support::ScopedAllocationFailure fault(0);
    success = CreateHostTensor(desc, &tensor, &diagnostic);
    injected = fault.Triggered();
  }
  EXPECT_TRUE(injected);
  EXPECT_FALSE(success);
  EXPECT_FALSE(tensor.buffer);
  EXPECT_TRUE(tensor.desc.shape.empty());
  EXPECT_FALSE(diagnostic.empty());
}

TEST(TensorStorageTest, BothAccessorsRejectMalformedExternalStorage) {
  struct Case {
    const char* name;
    Tensor tensor;
  };
  const TensorDesc valid{ElementType::kFloat32, {2}};
  std::vector<Case> cases{
      {"missing buffer", {valid, nullptr}},
      {"wrong type",
       {{ElementType::kInt32, {2}}, std::make_shared<ExternalBuffer>(8)}},
      {"null data", {valid, std::make_shared<ExternalBuffer>(8, 0, true)}},
      {"misaligned", {valid, std::make_shared<ExternalBuffer>(8, 1)}},
      {"too short", {valid, std::make_shared<ExternalBuffer>(4)}},
      {"too long", {valid, std::make_shared<ExternalBuffer>(12)}},
      {"runtime dynamic shape",
       {{ElementType::kFloat32, {-1}}, std::make_shared<ExternalBuffer>(8)}}};
  for (auto& row : cases) {
    SCOPED_TRACE(row.name);
    std::string diagnostic;
    EXPECT_EQ(GetTensorData<float>(row.tensor, &diagnostic), nullptr);
    EXPECT_FALSE(diagnostic.empty());
    diagnostic.clear();
    EXPECT_EQ(GetMutableTensorData<float>(&row.tensor, &diagnostic), nullptr);
    EXPECT_FALSE(diagnostic.empty());
  }
  EXPECT_EQ(GetMutableTensorData<float>(nullptr), nullptr);
  Tensor valid_external{valid, std::make_shared<ExternalBuffer>(8)};
  ASSERT_NE(GetMutableTensorData<float>(&valid_external), nullptr);
  GetMutableTensorData<float>(&valid_external)[1] = 4.5F;
  EXPECT_EQ(GetTensorData<float>(valid_external)[1], 4.5F);
}

#ifdef LLM_EDGEFLOW_TEST_WRAP_POSIX_MEMALIGN
TEST(TensorStorageTest, FailedNativeAlignedAllocationIsNeverAValidEmptyTensor) {
  Tensor tensor;
  ASSERT_TRUE(CreateHostTensor({ElementType::kFloat32, {1}}, &tensor));
  std::weak_ptr<ITensorBuffer> previous = tensor.buffer;
  fail_tensor_allocation = true;
  std::string diagnostic;
  const bool success =
      CreateHostTensor({ElementType::kFloat32, {2}}, &tensor, &diagnostic);
  const bool injected = !fail_tensor_allocation;
  fail_tensor_allocation = false;
  EXPECT_TRUE(injected);
  EXPECT_FALSE(success);
  EXPECT_FALSE(tensor.buffer);
  EXPECT_TRUE(tensor.desc.shape.empty());
  EXPECT_TRUE(previous.expired());
  EXPECT_NE(diagnostic.find("aligned HostTensorBuffer"), std::string::npos);
}
#endif

}  // namespace
}  // namespace llm_edgeflow
