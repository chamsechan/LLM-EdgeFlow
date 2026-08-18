#!/usr/bin/env bash
# ==============================================================================
# scripts/fetch_real_test_models.sh
# 自动拉取轻量级真实开源模型权重 (用于阶段性物理端到端压测)
# ==============================================================================

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODEL_DIR="${PROJECT_ROOT}/models"
mkdir -p "${MODEL_DIR}"

echo "=================================================="
echo "  [LLM-EdgeFlow] 真实模型权重自动下载与校验器     "
echo "  Target Directory: ${MODEL_DIR}"
echo "=================================================="

# 1. 下载 Qwen2.5-0.5B-Instruct GGUF 真实大语言模型 (约 350MB)
QWEN_GGUF_FILE="${MODEL_DIR}/qwen2.5-0.5b-instruct-q4_k_m.gguf"
QWEN_URL="https://modelscope.cn/models/qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/master/qwen2.5-0.5b-instruct-q4_k_m.gguf"

if [ -f "${QWEN_GGUF_FILE}" ] && [ -s "${QWEN_GGUF_FILE}" ]; then
  echo "✓ [OK] Qwen2.5-0.5B GGUF model already exists: ${QWEN_GGUF_FILE}"
else
  echo "⬇️ Downloading Qwen2.5-0.5B-Instruct GGUF from ModelScope mirror..."
  if command -v curl >/dev/null 2>&1; then
    curl -L --retry 3 "${QWEN_URL}" -o "${QWEN_GGUF_FILE}"
  elif command -v wget >/dev/null 2>&1; then
    wget -c "${QWEN_URL}" -O "${QWEN_GGUF_FILE}"
  else
    echo "❌ Error: Neither curl nor wget found. Please download manually: ${QWEN_URL}"
    exit 1
  fi
  echo "✓ [OK] Downloaded Qwen2.5-0.5B GGUF successfully."
fi

# 2. 建立常用模型别名软链接，便于测试与配置无缝读取
ln -sf "qwen2.5-0.5b-instruct-q4_k_m.gguf" "${MODEL_DIR}/qwen_0.6b_llamacpp.gguf"
ln -sf "qwen2.5-0.5b-instruct-q4_k_m.gguf" "${MODEL_DIR}/qwen_0.6b.gguf"

echo "=================================================="
echo "  🎉 真实模型权重准备完毕！"
ls -lh "${MODEL_DIR}"/*.gguf 2>/dev/null || true
echo "=================================================="
