#!/usr/bin/env bash
# ==============================================================================
# scripts/run_real_model_e2e.sh
# 独立物理真实模型端到端压测入口 (与日常 CTest 隔离)
# ==============================================================================

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
MODEL_DIR="${PROJECT_ROOT}/models"

MODE="gguf-only"
if [[ $# -gt 1 ]]; then
  echo "Usage: $0 [--all | --gguf-only | --whisper]"
  exit 2
fi
if [[ $# -eq 1 ]]; then
  case "$1" in
    --all) MODE="all" ;;
    --gguf-only) MODE="gguf-only" ;;
    --whisper) MODE="whisper" ;;
    *)
      echo "Usage: $0 [--all | --gguf-only | --whisper]"
      exit 2
      ;;
  esac
fi

echo "=================================================================="
echo "  [LLM-EdgeFlow] 物理真实模型端到端 E2E 压测套件启动 (${MODE})     "
echo "  (独立物理测试：物理加载权重 / 真实推理 / 真实 Profile 验证)        "
echo "=================================================================="

# 1. 拉取或校验固定提交的真实模型权重。
if [[ "${MODE}" == "whisper" ]]; then
  "${PROJECT_ROOT}/scripts/fetch_real_test_models.sh" --whisper
elif [[ "${MODE}" == "all" ]]; then
  "${PROJECT_ROOT}/scripts/fetch_real_test_models.sh" --all
else
  "${PROJECT_ROOT}/scripts/fetch_real_test_models.sh" --gguf-only
fi

# 2. 编译独立物理测试套件
echo ">>> 正在编译 test_real_models_e2e..."
mkdir -p "${BUILD_DIR}"
EXTRA_CMAKE_ARGS=()
if [[ "${MODE}" == "whisper" || "${MODE}" == "all" ]]; then
  EXTRA_CMAKE_ARGS+=(-DENABLE_WHISPERCPP=ON)
fi
cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" \
  -DENABLE_REAL_MODEL_TESTS=ON \
  "${EXTRA_CMAKE_ARGS[@]}"
cmake --build "${BUILD_DIR}" --target test_real_models_e2e alg_demo \
  -j"${LLM_EDGEFLOW_JOBS:-4}"

# 3. 运行物理真实模型端到端测试
echo "=================================================================="
echo ">>> 执行真实模型物理测试 (Google Test)..."
echo "=================================================================="
if [[ "${MODE}" == "whisper" ]]; then
  "${BUILD_DIR}/test_real_models_e2e" --gtest_color=yes \
    --gtest_filter="RealModelE2ETest.RealWhisperAsrTranscribe"
elif [[ "${MODE}" == "gguf-only" ]]; then
  "${BUILD_DIR}/test_real_models_e2e" --gtest_color=yes \
    --gtest_filter="-RealModelE2ETest.RealWhisperAsrTranscribe"
else
  "${BUILD_DIR}/test_real_models_e2e" --gtest_color=yes
fi

# 4. 使用同一固定权重运行公开 real Profile，而非测试专用直连入口。
cd "${PROJECT_ROOT}"
if [[ "${MODE}" == "whisper" || "${MODE}" == "all" ]]; then
  "${BUILD_DIR}/alg_demo" --profile audio_asr_whisper
fi
if [[ "${MODE}" == "gguf-only" || "${MODE}" == "all" ]]; then
  "${BUILD_DIR}/alg_demo" --profile entity_extract_llamacpp
fi

echo "=================================================================="
echo "  🎉 物理真实模型端到端测试 100% 通过 (${MODE})！"
echo "=================================================================="
