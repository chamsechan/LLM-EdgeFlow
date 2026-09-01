#!/usr/bin/env bash
# ==============================================================================
# scripts/run_real_model_e2e.sh
# 独立物理真实模型端到端压测入口 (与日常 CTest 隔离)
# ==============================================================================

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
MODEL_DIR="${PROJECT_ROOT}/models"

echo "=================================================================="
echo "  [LLM-EdgeFlow] 物理真实模型端到端 E2E 压测套件启动               "
echo "  (独立物理测试：物理加载权重 / 真实 llama.cpp 自回归解码 / 性能评测) "
echo "=================================================================="

# 1. 拉取或校验固定提交的 Qwen2.5-0.5B GGUF。
"${PROJECT_ROOT}/scripts/fetch_real_test_models.sh" --gguf-only

# 2. 编译独立物理测试套件
echo ">>> 正在编译 test_real_models_e2e..."
mkdir -p "${BUILD_DIR}"
cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" -DENABLE_REAL_MODEL_TESTS=ON
cmake --build "${BUILD_DIR}" --target test_real_models_e2e alg_demo \
  -j"${LLM_EDGEFLOW_JOBS:-4}"

# 3. 运行物理真实模型端到端测试
echo "=================================================================="
echo ">>> 执行真实模型物理测试 (Google Test)..."
echo "=================================================================="
"${BUILD_DIR}/test_real_models_e2e" --gtest_color=yes

# 4. 使用同一固定权重运行公开 real Profile，而非测试专用直连入口。
cd "${PROJECT_ROOT}"
"${BUILD_DIR}/alg_demo" --profile entity_extract_llamacpp

echo "=================================================================="
echo "  🎉 物理真实模型端到端测试 100% 通过！"
echo "=================================================================="
