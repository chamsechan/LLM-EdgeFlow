#pragma once

#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace llm_edgeflow {
namespace embedding_support {

/**
 * @brief Finalize and optionally L2-normalize an embedding vector in double
 * precision.
 *
 * Enforces:
 * - Non-empty dimensions.
 * - All inputs and outputs must be finite.
 * - When normalize == true, zero-norm vectors fail (cannot normalize to unit
 * direction).
 * - When normalize == false, finite zero vectors are allowed.
 * - Double accumulation avoids float overflow for bounded float model rows.
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
