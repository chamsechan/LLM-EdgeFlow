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

# 1. 检查并拉取真实开源小模型 (Qwen2.5-0.5B GGUF)
if [ ! -f "${MODEL_DIR}/qwen2.5-0.5b-instruct-q4_k_m.gguf" ]; then
  echo ">>> 未检测到本地真实模型权重，正在自动拉取官方轻量模型..."
  "${PROJECT_ROOT}/scripts/fetch_real_test_models.sh"
fi

# 2. 编译独立物理测试套件
echo ">>> 正在编译 test_real_models_e2e..."
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"
cmake -DENABLE_REAL_MODEL_TESTS=ON ..
make -j4 test_real_models_e2e

# 3. 运行物理真实模型端到端测试
echo "=================================================================="
echo ">>> 执行真实模型物理测试 (Google Test)..."
echo "=================================================================="
./test_real_models_e2e --gtest_color=yes

echo "=================================================================="
echo "  🎉 物理真实模型端到端测试 100% 通过！"
echo "=================================================================="
