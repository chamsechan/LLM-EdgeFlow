#!/bin/bash
set -euo pipefail

REPO_ROOT="${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

if [[ "${1:-}" == "--self-test" ]]; then
  echo "======================================================================"
  echo " [LayerGuard Self-Test] Testing violation detection capability..."
  echo "======================================================================"
  SCRIPT_PATH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
  TMP_TEST_DIR=$(mktemp -d /tmp/layerguard_test_XXXXXX)
  trap 'rm -rf "${TMP_TEST_DIR}"' EXIT

  # Test Case 1: Missing directory must cause script to fail
  mkdir -p "${TMP_TEST_DIR}/empty_repo"
  set +e
  REPO_ROOT="${TMP_TEST_DIR}/empty_repo" bash "${SCRIPT_PATH}" >/dev/null 2>&1
  STATUS_MISSING_DIR=$?
  set -e
  if [ $STATUS_MISSING_DIR -eq 0 ]; then
    echo "❌ [LayerGuard Self-Test FAIL] Script did not fail on missing directory!"
    exit 1
  fi

  # Test Case 2: Injected illegal include must cause script to fail
  mkdir -p "${TMP_TEST_DIR}/violation_repo/src/common_nodes"
  mkdir -p "${TMP_TEST_DIR}/violation_repo/src/adapter/adapters"
  mkdir -p "${TMP_TEST_DIR}/violation_repo/demo"
  mkdir -p "${TMP_TEST_DIR}/violation_repo/include/operator"
  touch "${TMP_TEST_DIR}/violation_repo/include/company_alg_interface.h"
  touch "${TMP_TEST_DIR}/violation_repo/include/operator/company_operator_types.h"
  echo '#include "company_alg_interface.h"' > "${TMP_TEST_DIR}/violation_repo/src/common_nodes/bad_node.cpp"
  set +e
  REPO_ROOT="${TMP_TEST_DIR}/violation_repo" bash "${SCRIPT_PATH}" >/dev/null 2>&1
  STATUS_INJECT_VIOLATION=$?
  set -e
  if [ $STATUS_INJECT_VIOLATION -eq 0 ]; then
    echo "❌ [LayerGuard Self-Test FAIL] Script did not fail on injected architectural violation!"
    exit 1
  fi

  echo "✅ [LayerGuard Self-Test PASS] Successfully verified both missing-directory and violation injection detection."
  exit 0
fi

echo "======================================================================"
echo " [LayerGuard] Checking 4-Tier Architectural Isolation Directives..."
echo "======================================================================"

# Rule 1: Layer 3 (src/common_nodes/) MUST NEVER include Layer 1 header (company_alg_interface.h)
if [ ! -d "$REPO_ROOT/src/common_nodes" ]; then
  echo "❌ [LayerGuard ERROR] src/common_nodes directory not found!"
  exit 1
fi
VIOLATIONS_L3_L1=$(grep -rnE '#include\s*["<]company_alg_interface\.h[">]' "$REPO_ROOT/src/common_nodes" || true)

if [ -n "$VIOLATIONS_L3_L1" ]; then
  echo "❌ [LayerGuard ERROR] Found Layer 3 -> Layer 1 reverse dependency violations:"
  echo "$VIOLATIONS_L3_L1"
  echo "Directive: Layer 3 common nodes must strictly communicate via DTOs and AlgContext (ARCH-002)."
  exit 1
fi
echo "✅ [LayerGuard PASS] Zero Layer 3 -> Layer 1 reverse include violations."

# Rule 2: Layer 1 Adapters (src/adapter/adapters/) MUST NEVER directly include Layer 4 Engine headers
VIOLATIONS_L1_L4=$(grep -rnE '#include\s*["<](engine/|src/engine/)' "$REPO_ROOT/src/adapter/adapters" || true)

if [ -n "$VIOLATIONS_L1_L4" ]; then
  echo "❌ [LayerGuard ERROR] Found Layer 1 -> Layer 4 illegal bypass dependency violations:"
  echo "$VIOLATIONS_L1_L4"
  echo "Directive: Layer 1 adapters must only convert C structs to/from biz DTOs, not bypass Layer 3."
  exit 1
fi
echo "✅ [LayerGuard PASS] Zero Layer 1 -> Layer 4 illegal engine include violations."

# Rule 3: Common Nodes (src/common_nodes/) MUST NEVER depend on biz-specific nodes
VIOLATIONS_COMMON_BIZ=$(grep -rnE '#include\s*["<](biz/|src/biz/|business/|src/business/)' "$REPO_ROOT/src/common_nodes" || true)

if [ -n "$VIOLATIONS_COMMON_BIZ" ]; then
  echo "❌ [LayerGuard ERROR] Found Common Node -> Biz Node dependency violations:"
  echo "$VIOLATIONS_COMMON_BIZ"
  exit 1
fi
echo "✅ [LayerGuard PASS] Zero Common Node -> Biz Node reverse include violations."

# Rule 4: Pure C11 Syntax & ABI Compliance Check via standard C compiler
if command -v gcc >/dev/null 2>&1; then
  gcc -std=c11 -pedantic-errors -fsyntax-only -x c -I"$REPO_ROOT/include" "$REPO_ROOT/include/company_alg_interface.h"
  gcc -std=c11 -pedantic-errors -fsyntax-only -x c -I"$REPO_ROOT/include" "$REPO_ROOT/include/company_alg_log.h"
  gcc -std=c11 -pedantic-errors -fsyntax-only -x c -I"$REPO_ROOT/include" "$REPO_ROOT/include/operator/company_operator_types.h"
  echo "✅ [LayerGuard PASS] GCC pure C11 strict syntax and ABI verification passed."
elif command -v clang >/dev/null 2>&1; then
  clang -std=c11 -pedantic-errors -fsyntax-only -x c -I"$REPO_ROOT/include" "$REPO_ROOT/include/company_alg_interface.h"
  clang -std=c11 -pedantic-errors -fsyntax-only -x c -I"$REPO_ROOT/include" "$REPO_ROOT/include/company_alg_log.h"
  clang -std=c11 -pedantic-errors -fsyntax-only -x c -I"$REPO_ROOT/include" "$REPO_ROOT/include/operator/company_operator_types.h"
  echo "✅ [LayerGuard PASS] Clang pure C11 strict syntax and ABI verification passed."
else
  echo "⚠️ [LayerGuard WARN] Neither gcc nor clang found for C11 syntax-only check."
fi

# Rule 5: Demo Layer (demo/) MUST NEVER directly include internal SDK headers (adapter/, core/, biz/, business/, engine/, src/)
VIOLATIONS_DEMO_INTERNAL=$(grep -rnE '#include\s*["<](adapter/|core/|biz/|business/|engine/|src/)' "$REPO_ROOT/demo" || true)

if [ -n "$VIOLATIONS_DEMO_INTERNAL" ]; then
  echo "❌ [LayerGuard ERROR] Found Demo -> SDK internal header violations:"
  echo "$VIOLATIONS_DEMO_INTERNAL"
  echo "Directive: Demo must strictly behave as external user and only include operator/ and public company_alg_interface.h."
  exit 1
fi
echo "✅ [LayerGuard PASS] Zero Demo -> Internal SDK header violations."

# Rule 6: RFC-0015 LLM vendor/semantic boundary.
LLAMA_VENDOR_OUTSIDE_BACKEND=$(grep -rnE '#include\s*["<]llama\.h[">]' \
  "$REPO_ROOT/include" "$REPO_ROOT/src" \
  --exclude-dir=backends 2>/dev/null || true)
if [ -n "$LLAMA_VENDOR_OUTSIDE_BACKEND" ]; then
  echo "❌ [LayerGuard ERROR] llama.h may only be included by the llama.cpp backend implementation:"
  echo "$LLAMA_VENDOR_OUTSIDE_BACKEND"
  exit 1
fi

LLAMA_BACKEND_SEMANTICS=$(grep -rnE 'ChatML|<\|im_start\|>|top_p|stop_words|AlgContext|Pipeline' \
  "$REPO_ROOT/src/engine/backends/llama_cpp" 2>/dev/null || true)
if [ -n "$LLAMA_BACKEND_SEMANTICS" ]; then
  echo "❌ [LayerGuard ERROR] llama.cpp backend contains model or pipeline semantics:"
  echo "$LLAMA_BACKEND_SEMANTICS"
  exit 1
fi

QWEN_VENDOR_INCLUDE=$(grep -rnE \
  '#include\s*["<](llama\.h|onnxruntime_cxx_api\.h|kitellm_edgeflow_adapter\.h)[">]' \
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

LLM_NODE_LEGACY=$(grep -nE 'ILlmEngine|engine_interface' \
  "$REPO_ROOT/src/common_nodes/llm_generate_node.cpp" 2>/dev/null || true)
if [ -n "$LLM_NODE_LEGACY" ]; then
  echo "❌ [LayerGuard ERROR] LlmGenerateNode still depends on the legacy engine interface:"
  echo "$LLM_NODE_LEGACY"
  exit 1
fi
echo "✅ [LayerGuard PASS] Backend vendor resources and Qwen generation semantics are isolated."

echo "======================================================================"
echo " All LayerGuard architectural isolation checks passed successfully!"
echo "======================================================================"
