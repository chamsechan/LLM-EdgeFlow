#!/bin/bash
set -euo pipefail

REPO_ROOT="${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

if [[ $# -ne 0 ]]; then
  echo "Usage: $0"
  exit 2
fi

echo "======================================================================"
echo " [LayerGuard] Checking Integration / Orchestration / Capability Nodes / Model Execution Boundaries..."
echo "======================================================================"

# 先确认必需目录存在，依赖规则由解析器统一维护。
for SOURCE_DIRECTORY in \
  "$REPO_ROOT/src/common_nodes" "$REPO_ROOT/src/custom_nodes" \
  "$REPO_ROOT/src/adapter/input" "$REPO_ROOT/src/adapter/output"; do
  if [ ! -d "$SOURCE_DIRECTORY" ]; then
    echo "❌ [LayerGuard ERROR] ${SOURCE_DIRECTORY#"$REPO_ROOT/"} directory not found!"
    exit 1
  fi
done

python3 "$(dirname "${BASH_SOURCE[0]}")/check_layer_dependencies.py" --root "${REPO_ROOT}"

# CMake 中的源码归属必须保持四个编译期分层和显式的组合根。
for OWNERSHIP in \
  "src/engine/CMakeLists.txt:edgeflow_model_execution_objects" \
  "src/common_nodes/CMakeLists.txt:edgeflow_capability_nodes_objects" \
  "src/custom_nodes/CMakeLists.txt:edgeflow_capability_nodes_objects" \
  "src/core/CMakeLists.txt:edgeflow_orchestration_objects" \
  "src/adapter/CMakeLists.txt:edgeflow_integration_objects"; do
  OWNERSHIP_FILE="${OWNERSHIP%%:*}"
  OWNERSHIP_TARGET="${OWNERSHIP#*:}"
  if [ ! -f "$REPO_ROOT/$OWNERSHIP_FILE" ] || \
     ! grep -q "target_sources(${OWNERSHIP_TARGET}" "$REPO_ROOT/$OWNERSHIP_FILE"; then
    echo "❌ [LayerGuard ERROR] $OWNERSHIP_FILE does not assign sources to $OWNERSHIP_TARGET."
    exit 1
  fi
done
AGGREGATE_SOURCE_OWNERSHIP=$(grep -rn 'target_sources(edgeflow_runtime_objects' \
  "$REPO_ROOT/src" 2>/dev/null || true)
if [ -n "$AGGREGATE_SOURCE_OWNERSHIP" ]; then
  echo "❌ [LayerGuard ERROR] Layer sources must not be attached to the aggregate runtime target:"
  echo "$AGGREGATE_SOURCE_OWNERSHIP"
  exit 1
fi
if ! grep -q 'target_sources(edgeflow_composition_objects' \
     "$REPO_ROOT/src/CMakeLists.txt" || \
   ! grep -q 'target_sources(edgeflow_composition_objects' \
     "$REPO_ROOT/src/adapter/CMakeLists.txt" || \
   ! grep -q 'shared_algorithm_runtime.cpp' \
     "$REPO_ROOT/src/adapter/CMakeLists.txt"; then
  echo "❌ [LayerGuard ERROR] Composition-root source ownership is incomplete."
  exit 1
fi
echo "✅ [LayerGuard PASS] CMake source ownership preserves all four layers and the composition root."

# 用标准 C 编译器检查纯 C11 语法与 ABI 合规性
GENERATED_VERSION_INCLUDE="$(mktemp -d "${TMPDIR:-/tmp}/edgeflow-version-header.XXXXXX")"
cleanup_generated_version() {
  rm -rf "${GENERATED_VERSION_INCLUDE}"
}
trap cleanup_generated_version EXIT INT TERM
mkdir -p "${GENERATED_VERSION_INCLUDE}/edgeflow"
PRODUCT_VERSION="$(
  sed -nE 's/^project\(LLMEdgeFlow VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES C CXX\)$/\1/p' \
    "${REPO_ROOT}/CMakeLists.txt" | head -n 1
)"
if [[ -z "${PRODUCT_VERSION}" ]]; then
  echo "❌ [LayerGuard ERROR] Unable to derive public version header values from CMakeLists.txt"
  exit 1
fi
sed \
  -e "s/@PROJECT_VERSION@/${PRODUCT_VERSION}/g" \
  "${REPO_ROOT}/cmake_ext/edgeflow_version.h.in" > \
  "${GENERATED_VERSION_INCLUDE}/edgeflow/version.h"

C11_COMPILER=""
if command -v gcc >/dev/null 2>&1; then
  C11_COMPILER=gcc
elif command -v clang >/dev/null 2>&1; then
  C11_COMPILER=clang
fi
if [[ -n "${C11_COMPILER}" ]]; then
  for C11_HEADER in \
    edgeflow/log.h edgeflow/operator/types.h \
    platform_mock/error_codes.h \
    platform_mock/operator_data_types.h; do
    # 纯宏头文件也合法；为 -pedantic 提供一个翻译单元。
    printf '#include "%s"\nint main(void) { return 0; }\n' "${C11_HEADER}" | \
      "${C11_COMPILER}" -std=c11 -pedantic-errors -fsyntax-only -x c \
        -I"${GENERATED_VERSION_INCLUDE}" -I"$REPO_ROOT/include" -
  done
  echo "✅ [LayerGuard PASS] ${C11_COMPILER} pure C11 strict syntax and ABI verification passed."
else
  echo "⚠️ [LayerGuard WARN] Neither gcc nor clang found for C11 syntax-only check."
fi

# LLM 厂商运行时与模型语义边界。
LLAMA_BACKEND_SEMANTICS=$(grep -rnE 'ChatML|<\|im_start\|>|top_p|stop_words|AlgContext|Pipeline' \
  "$REPO_ROOT/src/engine/backends/llama_cpp" 2>/dev/null || true)
if [ -n "$LLAMA_BACKEND_SEMANTICS" ]; then
  echo "❌ [LayerGuard ERROR] llama.cpp backend contains model or pipeline semantics:"
  echo "$LLAMA_BACKEND_SEMANTICS"
  exit 1
fi

QWEN_VENDOR_INCLUDE=$(grep -rnE \
  '#include\s*["<]kitellm_edgeflow_adapter\.h[">]' \
  "$REPO_ROOT/src/engine/models/qwen_causal_lm" 2>/dev/null || true)
if [ -n "$QWEN_VENDOR_INCLUDE" ]; then
  echo "❌ [LayerGuard ERROR] Qwen model must not include Backend vendor headers:"
  echo "$QWEN_VENDOR_INCLUDE"
  exit 1
fi

QWEN_GENERATION_LOOP=$(grep -rnE \
  'IAutoregressiveDecoder|CommonAutoregressiveGenerator|SampleNextToken|ApplyRepetitionPenalty' \
  "$REPO_ROOT/src/engine/models/qwen_causal_lm" 2>/dev/null || true)
if [ -n "$QWEN_GENERATION_LOOP" ]; then
  echo "❌ [LayerGuard ERROR] Qwen model must delegate generation through ITextGenerationSession:"
  echo "$QWEN_GENERATION_LOOP"
  exit 1
fi

if ! grep -rq 'ITextGenerationSession' \
  "$REPO_ROOT/src/engine/models/qwen_causal_lm"; then
  echo "❌ [LayerGuard ERROR] Qwen model does not depend on the text_generation protocol."
  exit 1
fi

echo "✅ [LayerGuard PASS] Backend vendor resources and Qwen generation semantics are isolated."

if [[ -n "${LLM_EDGEFLOW_LAYER_COMPILE_MANIFEST:-}" ]]; then
  cmake "-DLAYER_COMPILE_MANIFEST=${LLM_EDGEFLOW_LAYER_COMPILE_MANIFEST}" \
    -P "${REPO_ROOT}/tests/contract/architecture/test_layer_header_views.cmake"
fi

echo "======================================================================"
echo " All LayerGuard architectural isolation checks passed successfully!"
echo "======================================================================"
