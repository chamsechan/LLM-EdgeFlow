#pragma once

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "engine/backend_interface.h"

namespace llm_edgeflow::test_support {

// Model tests declare their own ports and policy; this only records execution.
class TensorGraphProbe : public ITensorGraphSession {
 public:
  explicit TensorGraphProbe(std::string name) : backend(std::move(name)) {}

  const std::string& BackendType() const noexcept override { return backend; }
  ExecutionProtocol Protocol() const noexcept override {
    return ExecutionProtocol::kTensorGraph;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kSerialized;
  }
  BatchPolicy GetBatchPolicy() const noexcept override { return policy; }
  const std::vector<TensorSpec>& Inputs() const noexcept override {
    return inputs;
  }
  const std::vector<TensorSpec>& Outputs() const noexcept override {
    return outputs;
  }
  int Run(const TensorMap& input, TensorMap* output,
          std::string* diagnostic) noexcept override {
    ++calls;
    observed = input;
    *output = response;
    *diagnostic = reason;
    return code;
  }

  const std::string backend;
  BatchPolicy policy{};
  std::vector<TensorSpec> inputs;
  std::vector<TensorSpec> outputs;
  TensorMap observed;
  TensorMap response;
  int calls = 0;
  int code = 0;
  std::string reason;
};

inline Tensor FloatOutput(std::vector<int64_t> shape,
                          const std::vector<float>& values) {
  Tensor tensor;
  EXPECT_TRUE(
      CreateHostTensor({ElementType::kFloat32, std::move(shape)}, &tensor));
  float* data = GetMutableTensorData<float>(&tensor);
  EXPECT_NE(data, nullptr);
  if (data) std::copy(values.begin(), values.end(), data);
  return tensor;
}

inline std::vector<int64_t> Encoded(const TensorMap& tensors,
                                    const std::string& name, size_t count) {
  const auto found = tensors.find(name);
  EXPECT_NE(found, tensors.end());
  if (found == tensors.end()) return {};
  const auto* data = GetTensorData<int64_t>(found->second);
  EXPECT_NE(data, nullptr);
  return data ? std::vector<int64_t>(data, data + count)
              : std::vector<int64_t>{};
}

class WordPieceVocabularyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char path[] = "/tmp/edgeflow-wordpiece-vocab-XXXXXX";
    const int fd = mkstemp(path);
    ASSERT_GE(fd, 0);
    close(fd);
    vocab = path;
    std::ofstream file(vocab);
    file << "[PAD]\n[UNK]\n[CLS]\n[SEP]\nhello\nworld\n##s\n";
    ASSERT_TRUE(file.good());
  }
  void TearDown() override {
    if (!vocab.empty()) std::remove(vocab.c_str());
  }
  std::string vocab;
};

}  // namespace llm_edgeflow::test_support
