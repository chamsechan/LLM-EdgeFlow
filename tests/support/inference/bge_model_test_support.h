#pragma once

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>

#include "company_alg_interface.h"
#include "engine/backend_interface.h"

namespace alg_framework {

#ifdef HAVE_ONNXRUNTIME
inline std::filesystem::path GetFixturePath(const char* environment_name,
                                            const char* fallback) {
  const char* configured = std::getenv(environment_name);
  return configured && configured[0] != '\0' ? configured : fallback;
}
#endif

inline void WriteTestVocab(const std::filesystem::path& path) {
  std::ofstream out(path);
  out << "[PAD]\n[UNK]\n[CLS]\n[SEP]\nhello\nworld\nword\n";
}

class FaultInjectingTensorBuffer final : public ITensorBuffer {
 public:
  FaultInjectingTensorBuffer(size_t byte_size, size_t data_offset = 0,
                             bool null_data = false)
      : byte_size_(byte_size) {
    const size_t allocation_size =
        byte_size + data_offset + alignof(std::max_align_t);
    allocation_ = std::malloc(allocation_size == 0 ? 1 : allocation_size);
    if (!allocation_ || null_data) return;
    const uintptr_t address = reinterpret_cast<uintptr_t>(allocation_);
    const uintptr_t aligned =
        (address + alignof(std::max_align_t) - 1) &
        ~(static_cast<uintptr_t>(alignof(std::max_align_t)) - 1);
    data_ = reinterpret_cast<void*>(aligned + data_offset);
  }

  ~FaultInjectingTensorBuffer() override { std::free(allocation_); }

  const void* Data() const noexcept override { return data_; }
  void* MutableData() noexcept override { return data_; }
  size_t ByteSize() const noexcept override { return byte_size_; }

 private:
  void* allocation_ = nullptr;
  void* data_ = nullptr;
  size_t byte_size_ = 0;
};

inline Tensor MakeFaultTensor(const TensorDesc& desc, size_t byte_size,
                              size_t data_offset = 0, bool null_data = false) {
  return Tensor{desc, std::make_shared<FaultInjectingTensorBuffer>(
                          byte_size, data_offset, null_data)};
}

class BgeModelTestBase : public ::testing::Test {
 protected:
  void SetUp() override {
    Alg_Init();
    std::random_device entropy;
    for (size_t attempt = 0; attempt < 32; ++attempt) {
      temp_dir_ = std::filesystem::temp_directory_path() /
                  ("test_bge_model_" + std::to_string(entropy()) + "_" +
                   std::to_string(entropy()));
      std::error_code error;
      if (std::filesystem::create_directory(temp_dir_, error)) return;
      ASSERT_FALSE(error) << error.message();
    }
    FAIL() << "Failed to create a unique BGE model test directory";
  }

  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(temp_dir_, error);
    Alg_DeInit();
  }

  std::filesystem::path temp_dir_;
};

}  // namespace alg_framework
