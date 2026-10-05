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

  # 用例 1：缺少目录时脚本必须失败
  mkdir -p "${TMP_TEST_DIR}/empty_repo"
  set +e
  REPO_ROOT="${TMP_TEST_DIR}/empty_repo" bash "${SCRIPT_PATH}" >/dev/null 2>&1
  STATUS_MISSING_DIR=$?
  set -e
  if [ $STATUS_MISSING_DIR -eq 0 ]; then
    echo "❌ [LayerGuard Self-Test FAIL] Script did not fail on missing directory!"
    exit 1
  fi

  # 用例 2：注入非法 include 时脚本必须失败
  mkdir -p "${TMP_TEST_DIR}/violation_repo/src/common_nodes"
  mkdir -p "${TMP_TEST_DIR}/violation_repo/src/custom_nodes"
  mkdir -p "${TMP_TEST_DIR}/violation_repo/src/adapter/biz"
  mkdir -p "${TMP_TEST_DIR}/violation_repo/demo"
  mkdir -p "${TMP_TEST_DIR}/violation_repo/include/edgeflow/operator"
  touch "${TMP_TEST_DIR}/violation_repo/include/edgeflow/operator/interface.h"
  touch "${TMP_TEST_DIR}/violation_repo/include/edgeflow/operator/types.h"
  echo '#include "edgeflow/operator/interface.h"' > "${TMP_TEST_DIR}/violation_repo/src/common_nodes/bad_node.cpp"
  set +e
  REPO_ROOT="${TMP_TEST_DIR}/violation_repo" bash "${SCRIPT_PATH}" >/dev/null 2>&1
  STATUS_INJECT_VIOLATION=$?
  set -e
  if [ $STATUS_INJECT_VIOLATION -eq 0 ]; then
    echo "❌ [LayerGuard Self-Test FAIL] Script did not fail on injected architectural violation!"
    exit 1
  fi

  # 用例 3：下层不得重新引入业务专属的 Blackboard 键。
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

  # 用例 4：Kite SDK 头文件只能直接出现在其自身的 Backend 中。
  : > "${TMP_TEST_DIR}/violation_repo/src/core/bad_core.cpp"
  for KITE_INJECTION_PATH in \
    "include/engine/bad_model.h" \
    "src/engine/models/qwen_causal_lm/bad_model.cpp" \
    "src/engine/backends/onnxruntime/bad_backend.cpp" \
    "src/core/bad_core.cpp" \
    "src/common_nodes/bad_node.cpp" \
    "src/custom_nodes/bad_node.cpp" \
    "src/adapter/biz/bad_adapter.cpp" \
    "demo/bad_demo.cpp"; do
    KITE_INJECTION_FILE="${TMP_TEST_DIR}/violation_repo/${KITE_INJECTION_PATH}"
    mkdir -p "$(dirname "${KITE_INJECTION_FILE}")"
    for KITE_INCLUDE in '#include <kiteLLM.h>' '# include "vendor/kiteLLM.h"'; do
      echo "${KITE_INCLUDE}" > "${KITE_INJECTION_FILE}"
      set +e
      KITE_GUARD_OUTPUT=$(REPO_ROOT="${TMP_TEST_DIR}/violation_repo" \
        bash "${SCRIPT_PATH}" 2>&1)
      STATUS_KITE_GUARD=$?
      set -e
      if [ $STATUS_KITE_GUARD -eq 0 ] || \
         ! grep -q "kiteLLM vendor header" <<<"${KITE_GUARD_OUTPUT}"; then
        echo "❌ [LayerGuard Self-Test FAIL] Kite header escape was not detected: ${KITE_INJECTION_PATH}"
        exit 1
      fi
    done
    : > "${KITE_INJECTION_FILE}"
  done

  # 用例 5：whisper.h 头文件只能直接出现在其自身的 Backend 中。
  for WHISPER_INJECTION_PATH in \
    "include/engine/bad_model.h" \
    "src/engine/models/whisper_asr/bad_model.cpp" \
    "src/engine/backends/onnxruntime/bad_backend.cpp" \
    "src/core/bad_core.cpp" \
    "src/common_nodes/bad_node.cpp" \
    "src/custom_nodes/bad_node.cpp" \
    "src/adapter/biz/bad_adapter.cpp" \
    "demo/bad_demo.cpp"; do
    WHISPER_INJECTION_FILE="${TMP_TEST_DIR}/violation_repo/${WHISPER_INJECTION_PATH}"
    mkdir -p "$(dirname "${WHISPER_INJECTION_FILE}")"
    for WHISPER_INCLUDE in '#include <whisper.h>' '# include "vendor/whisper.h"'; do
      echo "${WHISPER_INCLUDE}" > "${WHISPER_INJECTION_FILE}"
      set +e
      WHISPER_GUARD_OUTPUT=$(REPO_ROOT="${TMP_TEST_DIR}/violation_repo" \
        bash "${SCRIPT_PATH}" 2>&1)
      STATUS_WHISPER_GUARD=$?
      set -e
      if [ $STATUS_WHISPER_GUARD -eq 0 ] || \
         ! grep -q "whisper.h vendor header" <<<"${WHISPER_GUARD_OUTPUT}"; then
        echo "❌ [LayerGuard Self-Test FAIL] Whisper header escape was not detected: ${WHISPER_INJECTION_PATH}"
        exit 1
      fi
    done
    : > "${WHISPER_INJECTION_FILE}"
  done

  # 用例 6：自定义 Node 与通用 Node 遵守相同的平台边界。
  for CUSTOM_INCLUDE in \
    '#include "edgeflow/operator/interface.h"' \
    '# include "../adapter/biz_blackboard_keys.h"' \
    '#include "adapter/io_converter.h"' \
    '#include "edgeflow/operator/types.h"' \
    '#include "platform_mock/operator_types.h"'; do
    echo "${CUSTOM_INCLUDE}" > "${TMP_TEST_DIR}/violation_repo/src/custom_nodes/bad_node.cpp"
    set +e
    CUSTOM_GUARD_OUTPUT=$(REPO_ROOT="${TMP_TEST_DIR}/violation_repo" \
      bash "${SCRIPT_PATH}" 2>&1)
    STATUS_CUSTOM_GUARD=$?
    set -e
    if [ $STATUS_CUSTOM_GUARD -eq 0 ] || \
       ! grep -q "Capability Nodes -> Integration" <<<"${CUSTOM_GUARD_OUTPUT}"; then
      echo "❌ [LayerGuard Self-Test FAIL] Custom Node platform dependency was not detected: ${CUSTOM_INCLUDE}"
      exit 1
    fi
  done
  : > "${TMP_TEST_DIR}/violation_repo/src/custom_nodes/bad_node.cpp"

  # 用例 7：可复用的框架代码不得依赖自定义实现。
  for CUSTOM_CONSUMER in \
    "src/common_nodes/bad_node.cpp" \
    "include/nodes/bad_support.h" \
    "src/core/bad_core.cpp" \
    "include/core/bad_contract.h" \
    "src/engine/models/bad_model.cpp" \
    "include/engine/bad_model.h"; do
    CUSTOM_CONSUMER_FILE="${TMP_TEST_DIR}/violation_repo/${CUSTOM_CONSUMER}"
    mkdir -p "$(dirname "${CUSTOM_CONSUMER_FILE}")"
    for CUSTOM_INCLUDE in \
      '#include "custom_nodes/domain_node.h"' \
      '# include "../custom_nodes/domain_node.h"'; do
      echo "${CUSTOM_INCLUDE}" > "${CUSTOM_CONSUMER_FILE}"
      set +e
      CUSTOM_GUARD_OUTPUT=$(REPO_ROOT="${TMP_TEST_DIR}/violation_repo" \
        bash "${SCRIPT_PATH}" 2>&1)
      STATUS_CUSTOM_GUARD=$?
      set -e
      if [ $STATUS_CUSTOM_GUARD -eq 0 ] || \
         ! grep -q "dependency on custom Node" <<<"${CUSTOM_GUARD_OUTPUT}"; then
        echo "❌ [LayerGuard Self-Test FAIL] Framework dependency on custom Node was not detected: ${CUSTOM_CONSUMER}"
        exit 1
      fi
    done
    : > "${CUSTOM_CONSUMER_FILE}"
  done

  # 用例 8：即使尚无任何 Node，也要检查自定义源码的归属。
  mkdir -p "${TMP_TEST_DIR}/violation_repo/include/nodes"
  cp "${REPO_ROOT}/include/nodes/node_base.h" \
    "${TMP_TEST_DIR}/violation_repo/include/nodes/node_base.h"
  for CUSTOM_FIXTURE_LAYER in engine common_nodes core adapter; do
    cp "${REPO_ROOT}/src/${CUSTOM_FIXTURE_LAYER}/CMakeLists.txt" \
      "${TMP_TEST_DIR}/violation_repo/src/${CUSTOM_FIXTURE_LAYER}/CMakeLists.txt"
  done
  echo 'target_sources(edgeflow_orchestration_objects PRIVATE misplaced.cpp)' > \
    "${TMP_TEST_DIR}/violation_repo/src/custom_nodes/CMakeLists.txt"
  set +e
  CUSTOM_GUARD_OUTPUT=$(REPO_ROOT="${TMP_TEST_DIR}/violation_repo" \
    bash "${SCRIPT_PATH}" 2>&1)
  STATUS_CUSTOM_GUARD=$?
  set -e
  if [ $STATUS_CUSTOM_GUARD -eq 0 ] || \
     ! grep -q "src/custom_nodes/CMakeLists.txt does not assign sources" <<<"${CUSTOM_GUARD_OUTPUT}"; then
    echo "❌ [LayerGuard Self-Test FAIL] Custom Node source ownership violation was not detected!"
    exit 1
  fi

  python3 "$(dirname "${SCRIPT_PATH}")/check_layer_dependencies.py" --self-test

  echo "✅ [LayerGuard Self-Test PASS] Missing paths and injected dependency violations were detected."
  exit 0
fi

echo "======================================================================"
echo " [LayerGuard] Checking Integration / Orchestration / Capability Nodes / Model Execution Boundaries..."
echo "======================================================================"

# 规则 1：所有能力节点都须把平台结构及其转换留在接入适配层。
NODE_SOURCE_PATHS=("$REPO_ROOT/src/common_nodes" "$REPO_ROOT/src/custom_nodes")
for NODE_SOURCE_PATH in "${NODE_SOURCE_PATHS[@]}"; do
  if [ ! -d "$NODE_SOURCE_PATH" ]; then
    echo "❌ [LayerGuard ERROR] ${NODE_SOURCE_PATH#"$REPO_ROOT/"} directory not found!"
    exit 1
  fi
done
VIOLATIONS_NODES_INTEGRATION=$(grep -rnE \
  '^[[:space:]]*#[[:space:]]*include[[:space:]]*["<]([^">]*/)?(company_alg_interface\.h[">]|edgeflow/(c_api\.(h|hpp)[">]|operator/)|(adapter|operator|platform_mock)/)' \
  "${NODE_SOURCE_PATHS[@]}" || true)

if [ -n "$VIOLATIONS_NODES_INTEGRATION" ]; then
  echo "❌ [LayerGuard ERROR] Found Capability Nodes -> Integration reverse dependency violations:"
  echo "$VIOLATIONS_NODES_INTEGRATION"
  echo "Directive: All capability nodes must communicate via internal values and AlgContext."
  exit 1
fi
echo "✅ [LayerGuard PASS] Zero Capability Nodes -> Integration reverse include violations."

# 规则 2：接入适配层 (src/adapter/biz/) 绝不能直接 include 模型执行层头文件
VIOLATIONS_INTEGRATION_EXECUTION=$(grep -rnE '#include\s*["<](engine/|src/engine/)' "$REPO_ROOT/src/adapter/biz" || true)

if [ -n "$VIOLATIONS_INTEGRATION_EXECUTION" ]; then
  echo "❌ [LayerGuard ERROR] Found Integration -> Model Execution illegal bypass dependency violations:"
  echo "$VIOLATIONS_INTEGRATION_EXECUTION"
  echo "Directive: Integration adapters must only convert C structs to/from biz DTOs, not bypass Capability Nodes."
  exit 1
fi
echo "✅ [LayerGuard PASS] Zero Integration -> Model Execution illegal engine include violations."

# 规则 3：通用 Node (src/common_nodes/) 绝不能依赖业务专属 Node
VIOLATIONS_COMMON_BIZ=$(grep -rnE '#include\s*["<](biz/|src/biz/|business/|src/business/)' "$REPO_ROOT/src/common_nodes" || true)

if [ -n "$VIOLATIONS_COMMON_BIZ" ]; then
  echo "❌ [LayerGuard ERROR] Found Common Node -> Biz Node dependency violations:"
  echo "$VIOLATIONS_COMMON_BIZ"
  exit 1
fi
echo "✅ [LayerGuard PASS] Zero Common Node -> Biz Node reverse include violations."

# 规则 3b：框架代码不得依赖自定义 Node 实现。
VIOLATIONS_CUSTOM_DEPENDENCY=$(grep -rnE \
  '^[[:space:]]*#[[:space:]]*include[[:space:]]*["<]([^">]*/)?custom_nodes/' \
  "$REPO_ROOT/src/common_nodes" "$REPO_ROOT/include/nodes" \
  "$REPO_ROOT/src/core" "$REPO_ROOT/include/core" \
  "$REPO_ROOT/src/engine" "$REPO_ROOT/include/engine" 2>/dev/null || true)
if [ -n "$VIOLATIONS_CUSTOM_DEPENDENCY" ]; then
  echo "❌ [LayerGuard ERROR] Found framework dependency on custom Node implementations:"
  echo "$VIOLATIONS_CUSTOM_DEPENDENCY"
  exit 1
fi
echo "✅ [LayerGuard PASS] Framework code does not depend on custom Node implementations."

# 规则 4：业务 Blackboard 键名归接入适配层所有。流程编排层、能力节点层和
# 模型执行层只能依赖中性值契约和已解析的逻辑端口绑定。
LOWER_LAYER_PATHS=(
  "$REPO_ROOT/include/core" "$REPO_ROOT/src/core"
  "$REPO_ROOT/include/nodes" "${NODE_SOURCE_PATHS[@]}"
  "$REPO_ROOT/include/engine" "$REPO_ROOT/src/engine"
)
VIOLATIONS_BIZ_KEYS=$(grep -rnE \
  '#include\s*["<]adapter/biz_blackboard_keys\.h[">]' \
  "${LOWER_LAYER_PATHS[@]}" 2>/dev/null || true)
if [ -n "$VIOLATIONS_BIZ_KEYS" ]; then
  echo "❌ [LayerGuard ERROR] Found lower-layer dependency on Integration business Blackboard key ownership:"
  echo "$VIOLATIONS_BIZ_KEYS"
  exit 1
fi
echo "✅ [LayerGuard PASS] Business Blackboard keys remain owned by Integration."

# 规则 4b：Kite SDK 只能直接出现在其具体 Backend 中。在构建相关的守卫之前
# 检查，使隔离自测无需 SDK 或构建。
KITE_VENDOR_OUTSIDE_BACKEND=$(grep -rnE \
  '^[[:space:]]*#[[:space:]]*include[[:space:]]*["<]([^">]*/)?kiteLLM\.h[">]' \
  "$REPO_ROOT/include" "$REPO_ROOT/src" "$REPO_ROOT/demo" 2>/dev/null | \
  grep -vF "$REPO_ROOT/src/engine/backends/kite_llm/" || true)
if [ -n "$KITE_VENDOR_OUTSIDE_BACKEND" ]; then
  echo "❌ [LayerGuard ERROR] kiteLLM vendor header may only be included inside its concrete Backend:"
  echo "$KITE_VENDOR_OUTSIDE_BACKEND"
  exit 1
fi
echo "✅ [LayerGuard PASS] The kiteLLM vendor header stays inside its concrete Backend."

# 规则 4c：whisper.h 头文件只能直接出现在其具体 Backend 中。
WHISPER_VENDOR_OUTSIDE_BACKEND=$(grep -rnE \
  '^[[:space:]]*#[[:space:]]*include[[:space:]]*["<]([^">]*/)?whisper\.h[">]' \
  "$REPO_ROOT/include" "$REPO_ROOT/src" "$REPO_ROOT/demo" 2>/dev/null | \
  grep -vF "$REPO_ROOT/src/engine/backends/whisper_cpp/" || true)
if [ -n "$WHISPER_VENDOR_OUTSIDE_BACKEND" ]; then
  echo "❌ [LayerGuard ERROR] whisper.h vendor header may only be included inside its concrete Backend:"
  echo "$WHISPER_VENDOR_OUTSIDE_BACKEND"
  exit 1
fi
echo "✅ [LayerGuard PASS] The whisper.h vendor header stays inside its concrete Backend."

# 规则 5：Node 支持代码只消费抽取出的已校验 Node 计划，
# 而不是完整的编排层 Validator 实现契约。
NODE_SUPPORT_HEADER="$REPO_ROOT/include/nodes/node_base.h"
if [ ! -f "$NODE_SUPPORT_HEADER" ] || \
   ! grep -q 'core/validated_node_plan.h' "$NODE_SUPPORT_HEADER" || \
   grep -q 'core/pipeline_validator.h' "$NODE_SUPPORT_HEADER"; then
  echo "❌ [LayerGuard ERROR] node_base.h must depend only on validated_node_plan.h."
  exit 1
fi
echo "✅ [LayerGuard PASS] Node support is decoupled from PipelineValidator."

# 规则 6：CMake 中的源码归属必须保持四个编译期分层和显式的组合根。
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

# 规则 7：用标准 C 编译器检查纯 C11 语法与 ABI 合规性
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

# 规则 8：Demo 层 (demo/) 绝不能直接 include SDK 内部头文件
# (adapter/、core/、biz/、business/、engine/、src/)
VIOLATIONS_DEMO_INTERNAL=$(grep -rnE '#include\s*["<](adapter/|core/|biz/|business/|engine/|src/)' "$REPO_ROOT/demo" || true)

if [ -n "$VIOLATIONS_DEMO_INTERNAL" ]; then
  echo "❌ [LayerGuard ERROR] Found Demo -> SDK internal header violations:"
  echo "$VIOLATIONS_DEMO_INTERNAL"
  echo "Directive: Demo may include public edgeflow/ interfaces and platform_mock/ declarations."
  exit 1
fi
echo "✅ [LayerGuard PASS] Zero Demo -> Internal SDK header violations."

# 规则 9：LLM 厂商运行时与模型语义边界。
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
  '#include\s*["<](llama\.h|onnxruntime_cxx_api\.h|kiteLLM\.h|kitellm_edgeflow_adapter\.h)[">]' \
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

python3 "$(dirname "${BASH_SOURCE[0]}")/check_layer_dependencies.py" --root "${REPO_ROOT}"

if [[ -n "${LLM_EDGEFLOW_LAYER_COMPILE_MANIFEST:-}" ]]; then
  cmake "-DLAYER_COMPILE_MANIFEST=${LLM_EDGEFLOW_LAYER_COMPILE_MANIFEST}" \
    -P "${REPO_ROOT}/tests/contract/architecture/test_layer_header_views.cmake"
fi

echo "======================================================================"
echo " All LayerGuard architectural isolation checks passed successfully!"
echo "======================================================================"
