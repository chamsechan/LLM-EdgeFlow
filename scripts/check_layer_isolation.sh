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

  # Test Case 3: Lower layers must not regain business-owned Blackboard keys.
  : > "${TMP_TEST_DIR}/violation_repo/src/common_nodes/bad_node.cpp"
  mkdir -p "${TMP_TEST_DIR}/violation_repo/src/core"
  echo '#include "adapter/biz_blackboard_keys.h"' > \
    "${TMP_TEST_DIR}/violation_repo/src/core/bad_core.cpp"
  set +e
  BUSINESS_KEY_OUTPUT=$(REPO_ROOT="${TMP_TEST_DIR}/violation_repo" \
    bash "${SCRIPT_PATH}" 2>&1)
  STATUS_BUSINESS_KEY=$?
  set -e
  if [ $STATUS_BUSINESS_KEY -eq 0 ] || \
     ! grep -q "business Blackboard key" <<<"${BUSINESS_KEY_OUTPUT}"; then
    echo "❌ [LayerGuard Self-Test FAIL] Business-key ownership violation was not detected!"
    exit 1
  fi

  echo "✅ [LayerGuard Self-Test PASS] Missing paths and injected dependency violations were detected."
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

# Rule 4: Layer 1 owns business-facing Blackboard key names. Layers 2-4 may
# depend only on neutral value contracts and resolved logical port bindings.
LOWER_LAYER_PATHS=(
  "$REPO_ROOT/include/core" "$REPO_ROOT/src/core"
  "$REPO_ROOT/include/nodes" "$REPO_ROOT/src/common_nodes"
  "$REPO_ROOT/include/engine" "$REPO_ROOT/src/engine"
)
VIOLATIONS_BIZ_KEYS=$(grep -rnE \
  '#include\s*["<]adapter/biz_blackboard_keys\.h[">]' \
  "${LOWER_LAYER_PATHS[@]}" 2>/dev/null || true)
if [ -n "$VIOLATIONS_BIZ_KEYS" ]; then
  echo "❌ [LayerGuard ERROR] Found lower-layer dependency on Layer 1 business Blackboard key ownership:"
  echo "$VIOLATIONS_BIZ_KEYS"
  exit 1
fi
echo "✅ [LayerGuard PASS] Business Blackboard keys remain owned by Layer 1."

# Rule 5: The neutral TraceableItem contract has one canonical include path.
LEGACY_TRACEABLE_HEADER="$REPO_ROOT/include/core/traceable_item.h"
LEGACY_TRACEABLE_INCLUDES=$(grep -rnE \
  '#include\s*["<]core/traceable_item\.h[">]' \
  "$REPO_ROOT/include" "$REPO_ROOT/src" "$REPO_ROOT/demo" \
  "$REPO_ROOT/dev_support" "$REPO_ROOT/tests" 2>/dev/null || true)
if [ -e "$LEGACY_TRACEABLE_HEADER" ] || [ -n "$LEGACY_TRACEABLE_INCLUDES" ]; then
  echo "❌ [LayerGuard ERROR] Legacy core/traceable_item.h compatibility path remains:"
  echo "$LEGACY_TRACEABLE_INCLUDES"
  exit 1
fi
echo "✅ [LayerGuard PASS] TraceableItem uses the neutral contracts include path."

# Rule 6: Node support consumes the extracted validated-node plan, not the full
# Layer 2 validator implementation contract.
NODE_SUPPORT_HEADER="$REPO_ROOT/include/nodes/node_support.h"
if [ ! -f "$NODE_SUPPORT_HEADER" ] || \
   ! grep -q 'core/validated_node_plan.h' "$NODE_SUPPORT_HEADER" || \
   grep -q 'core/pipeline_validator.h' "$NODE_SUPPORT_HEADER"; then
  echo "❌ [LayerGuard ERROR] node_support.h must depend only on validated_node_plan.h."
  exit 1
fi
echo "✅ [LayerGuard PASS] Node support is decoupled from PipelineValidator."

# Rule 7: Source ownership in CMake must preserve the four compile-time layers
# and the explicit composition root.
for OWNERSHIP in \
  "src/engine/CMakeLists.txt:edgeflow_layer4_engine_objects" \
  "src/common_nodes/CMakeLists.txt:edgeflow_layer3_node_objects" \
  "src/core/CMakeLists.txt:edgeflow_layer2_core_objects" \
  "src/adapter/CMakeLists.txt:edgeflow_layer1_adapter_objects"; do
  OWNERSHIP_FILE="${OWNERSHIP%%:*}"
  OWNERSHIP_TARGET="${OWNERSHIP#*:}"
  if [ ! -f "$REPO_ROOT/$OWNERSHIP_FILE" ] || \
     ! grep -q "target_sources(${OWNERSHIP_TARGET}" "$REPO_ROOT/$OWNERSHIP_FILE"; then
    echo "❌ [LayerGuard ERROR] $OWNERSHIP_FILE does not assign sources to $OWNERSHIP_TARGET."
    exit 1
  fi
done
LEGACY_SOURCE_OWNERSHIP=$(grep -rn 'target_sources(edgeflow_runtime_objects' \
  "$REPO_ROOT/src" 2>/dev/null || true)
if [ -n "$LEGACY_SOURCE_OWNERSHIP" ]; then
  echo "❌ [LayerGuard ERROR] Layer sources still use the legacy aggregate target:"
  echo "$LEGACY_SOURCE_OWNERSHIP"
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

# Rule 8: Pure C11 Syntax & ABI Compliance Check via standard C compiler
GENERATED_VERSION_INCLUDE="$(mktemp -d "${TMPDIR:-/tmp}/edgeflow-version-header.XXXXXX")"
cleanup_generated_version() {
  rm -rf "${GENERATED_VERSION_INCLUDE}"
}
trap cleanup_generated_version EXIT INT TERM
PRODUCT_VERSION="$(
  sed -nE 's/^project\(LLMEdgeFlow VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES C CXX\)$/\1/p' \
    "${REPO_ROOT}/CMakeLists.txt" | head -n 1
)"
ABI_VERSION="$(
  sed -nE 's/^set\(LLM_EDGEFLOW_ABI_VERSION "([0-9]+\.[0-9]+\.[0-9]+)"\)$/\1/p' \
    "${REPO_ROOT}/CMakeLists.txt" | head -n 1
)"
ABI_MAJOR="$(
  sed -nE 's/^set\(LLM_EDGEFLOW_ABI_VERSION_MAJOR ([0-9]+)\)$/\1/p' \
    "${REPO_ROOT}/CMakeLists.txt" | head -n 1
)"
if [[ -z "${PRODUCT_VERSION}" || -z "${ABI_VERSION}" || -z "${ABI_MAJOR}" ]]; then
  echo "❌ [LayerGuard ERROR] Unable to derive public version header values from CMakeLists.txt"
  exit 1
fi
sed \
  -e "s/@PROJECT_VERSION@/${PRODUCT_VERSION}/g" \
  -e "s/@LLM_EDGEFLOW_ABI_VERSION@/${ABI_VERSION}/g" \
  -e "s/@LLM_EDGEFLOW_ABI_VERSION_MAJOR@/${ABI_MAJOR}/g" \
  "${REPO_ROOT}/cmake/company_alg_version.h.in" > \
  "${GENERATED_VERSION_INCLUDE}/company_alg_version.h"

if command -v gcc >/dev/null 2>&1; then
  gcc -std=c11 -pedantic-errors -fsyntax-only -x c \
    -I"${GENERATED_VERSION_INCLUDE}" -I"$REPO_ROOT/include" \
    "$REPO_ROOT/include/company_alg_interface.h"
  gcc -std=c11 -pedantic-errors -fsyntax-only -x c \
    -I"${GENERATED_VERSION_INCLUDE}" -I"$REPO_ROOT/include" \
    "$REPO_ROOT/include/company_alg_log.h"
  gcc -std=c11 -pedantic-errors -fsyntax-only -x c -I"$REPO_ROOT/include" "$REPO_ROOT/include/operator/company_operator_types.h"
  echo "✅ [LayerGuard PASS] GCC pure C11 strict syntax and ABI verification passed."
elif command -v clang >/dev/null 2>&1; then
  clang -std=c11 -pedantic-errors -fsyntax-only -x c \
    -I"${GENERATED_VERSION_INCLUDE}" -I"$REPO_ROOT/include" \
    "$REPO_ROOT/include/company_alg_interface.h"
  clang -std=c11 -pedantic-errors -fsyntax-only -x c \
    -I"${GENERATED_VERSION_INCLUDE}" -I"$REPO_ROOT/include" \
    "$REPO_ROOT/include/company_alg_log.h"
  clang -std=c11 -pedantic-errors -fsyntax-only -x c -I"$REPO_ROOT/include" "$REPO_ROOT/include/operator/company_operator_types.h"
  echo "✅ [LayerGuard PASS] Clang pure C11 strict syntax and ABI verification passed."
else
  echo "⚠️ [LayerGuard WARN] Neither gcc nor clang found for C11 syntax-only check."
fi

# Rule 9: Demo Layer (demo/) MUST NEVER directly include internal SDK headers (adapter/, core/, biz/, business/, engine/, src/)
VIOLATIONS_DEMO_INTERNAL=$(grep -rnE '#include\s*["<](adapter/|core/|biz/|business/|engine/|src/)' "$REPO_ROOT/demo" || true)

if [ -n "$VIOLATIONS_DEMO_INTERNAL" ]; then
  echo "❌ [LayerGuard ERROR] Found Demo -> SDK internal header violations:"
  echo "$VIOLATIONS_DEMO_INTERNAL"
  echo "Directive: Demo must strictly behave as external user and only include operator/ and public company_alg_interface.h."
  exit 1
fi
echo "✅ [LayerGuard PASS] Zero Demo -> Internal SDK header violations."

# Rule 10: RFC-0015 LLM vendor/semantic boundary.
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
