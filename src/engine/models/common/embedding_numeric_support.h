#pragma once

#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace llm_edgeflow {
namespace embedding_support {

/**
 * @brief 以双精度完成 embedding 向量计算，并可选做 L2 归一化。
 *
 * 保证：
 * - 维度非空。
 * - 所有输入和输出必须是有限值。
 * - normalize == true 时，零范数向量失败 (无法归一化为单位方向)。
 * - normalize == false 时，允许有限的零向量。
 * - 双精度累加避免有界 float 模型行溢出。
 */
template <typename T>
inline bool FinalizeEmbeddingVector(const T* values, size_t dim, bool normalize,
                                    std::vector<float>* output) {
  if (!values || dim == 0 || !output) return false;

  double norm_sq = 0.0;
  for (size_t d = 0; d < dim; ++d) {
    double v = static_cast<double>(values[d]);
    if (!std::isfinite(v)) return false;
    norm_sq += v * v;
  }

  if (!std::isfinite(norm_sq)) return false;

  const double norm = std::sqrt(norm_sq);
  if (!std::isfinite(norm)) return false;

  if (normalize && norm == 0.0) {
    return false;
  }

  output->resize(dim);
  for (size_t d = 0; d < dim; ++d) {
    double v = static_cast<double>(values[d]);
    double res = normalize ? (v / norm) : v;
    if (!std::isfinite(res) ||
        std::abs(res) > std::numeric_limits<float>::max())
      return false;
    float f = static_cast<float>(res);
    if (!std::isfinite(f)) return false;
    (*output)[d] = f;
  }
  return true;
}

inline bool FinalizeEmbeddingVector(const std::vector<double>& values,
                                    bool normalize,
                                    std::vector<float>* output) {
  return FinalizeEmbeddingVector(values.data(), values.size(), normalize,
                                 output);
}

inline bool FinalizeEmbeddingVector(const std::vector<float>& values,
                                    bool normalize,
                                    std::vector<float>* output) {
  return FinalizeEmbeddingVector(values.data(), values.size(), normalize,
                                 output);
}

}  // namespace embedding_support
}  // namespace llm_edgeflow
