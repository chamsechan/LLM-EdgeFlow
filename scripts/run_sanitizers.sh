#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SANITIZERS="${LLM_EDGEFLOW_SANITIZERS:-address,undefined}"

MODE="fast"
if [[ $# -gt 1 ]]; then
  echo "Usage: $0 [--fast | --full]"
  exit 2
fi
if [[ $# -eq 1 ]]; then
  case "$1" in
    --fast) MODE="fast" ;;
    --full) MODE="full" ;;
    *)
      echo "Usage: $0 [--fast | --full]"
      exit 2
      ;;
  esac
fi
BUILD_DIR_TAG="${SANITIZERS//,/-}"
BUILD_DIR="${PROJECT_ROOT}/build-sanitizers-${BUILD_DIR_TAG}-${MODE}"

DETECT_LEAKS="${DETECT_LEAKS:-0}"

echo "=================================================="
echo " Running Clang/GCC Sanitizer Suite (Mode: ${MODE})"
echo " Project Root: ${PROJECT_ROOT}"
echo " Sanitizers: ${SANITIZERS}"
if [[ "${SANITIZERS}" == *"thread"* ]]; then
  echo " Note: ThreadSanitizer data race detection enabled."
elif [[ "${SANITIZERS}" == "address" ]]; then
  echo " Note: AddressSanitizer memory safety checks enabled."
  if [[ "${DETECT_LEAKS}" == "1" ]]; then
    echo "       LeakSanitizer enabled (detect_leaks=1)."
  else
    echo "       LeakSanitizer disabled (detect_leaks=0)."
  fi
elif [[ "${SANITIZERS}" == "undefined" ]]; then
  echo " Note: UndefinedBehaviorSanitizer checks enabled."
elif [[ "${SANITIZERS}" == *"address"* ]] && [[ "${SANITIZERS}" == *"undefined"* ]]; then
  echo " Note: ASan/UBSan memory safety and UB checks enabled."
  if [[ "${DETECT_LEAKS}" == "1" ]]; then
    echo "       LeakSanitizer enabled (detect_leaks=1)."
  else
    echo "       LeakSanitizer disabled (detect_leaks=0)."
  fi
fi
echo "=================================================="

COMMON_CMAKE_ARGS=(
  -DCMAKE_BUILD_TYPE=Debug
  -DENABLE_SANITIZERS=ON
  -DLLM_EDGEFLOW_SANITIZERS="${SANITIZERS}"
  -DLLM_EDGEFLOW_USE_CCACHE=ON
)
GEN_ARG_STR=$("${SCRIPT_DIR}/detect_cmake_generator.sh" "${BUILD_DIR}")
if [[ -n "${GEN_ARG_STR}" ]]; then
  read -r -a GENERATOR_ARGS <<< "${GEN_ARG_STR}"
  COMMON_CMAKE_ARGS+=("${GENERATOR_ARGS[@]}")
fi
if [[ "${MODE}" == "fast" ]]; then
  COMMON_CMAKE_ARGS+=(
    -DENABLE_LLAMACPP=OFF
    -DENABLE_ONNXRUNTIME=OFF
    -DENABLE_REAL_MODEL_TESTS=OFF
  )
fi

if command -v ccache >/dev/null 2>&1; then
  export CCACHE_DIR="${PROJECT_ROOT}/build/.ccache-sanitizers"
  mkdir -p "${CCACHE_DIR}"
fi

cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" "${COMMON_CMAKE_ARGS[@]}"
if [[ "${MODE}" == "fast" ]]; then
  FAST_TARGETS=(
    alg_demo
    alg_pipeline_tool
    test_adapter_contract_security
    test_adapter_purity
    test_batch_executor
    test_c_abi_safety
    test_common_nodes
    test_concurrency_and_edge_cases
    test_definition_schema_validation
    test_framework_core
    test_node_base_contracts
    test_operator_api
    test_operator_biz_bridge_registry
    test_operator_golden
    test_operator_output_pool
    test_operator_value_registry
    test_pipeline_config
    test_pipeline_studio
    test_typed_blackboard_contracts
    test_validated_pipeline_plan
  )
  cmake --build "${BUILD_DIR}" --target "${FAST_TARGETS[@]}" -j"$(nproc)"
else
  cmake --build "${BUILD_DIR}" -j"$(nproc)"
fi

if [[ "${SANITIZERS}" == *"thread"* ]]; then
  export TSAN_OPTIONS="halt_on_error=1:abort_on_error=1"
else
  export ASAN_OPTIONS="detect_leaks=${DETECT_LEAKS}:abort_on_error=1"
  if [[ "${SANITIZERS}" == *"address"* ]]; then
    LIBASAN_PATH="$(gcc -print-file-name=libasan.so 2>/dev/null || true)"
    if [[ -f "${LIBASAN_PATH}" ]]; then
      export LD_PRELOAD="${LIBASAN_PATH}${LD_PRELOAD:+:$LD_PRELOAD}"
    fi
  fi
  UBSAN_SUPP="${SCRIPT_DIR}/ubsan_suppressions.txt"
  if [[ -f "${UBSAN_SUPP}" ]]; then
    export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:suppressions=${UBSAN_SUPP}"
  else
    export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"
  fi
fi

export LD_LIBRARY_PATH="${BUILD_DIR}:${BUILD_DIR}/_deps/onnxruntime_prebuilt-src/lib:${LD_LIBRARY_PATH:-}"
export DYLD_LIBRARY_PATH="${BUILD_DIR}:${BUILD_DIR}/_deps/onnxruntime_prebuilt-src/lib:${DYLD_LIBRARY_PATH:-}"
export LLM_EDGEFLOW_PIPELINE_TOOL="${BUILD_DIR}/alg_pipeline_tool"
export LLM_EDGEFLOW_DEMO_BINARY="${BUILD_DIR}/alg_demo"

ARCH_PREFIX=()
if [[ "${SANITIZERS}" == *"thread"* ]] && [[ "$(uname -s)" == "Linux" ]] && [[ "$(uname -m)" == "aarch64" ]]; then
  ARCH_PREFIX=(setarch "$(uname -m)" -R)
fi

if [ "${MODE}" == "fast" ]; then
  FAST_TEST_REGEX="^(BatchExecutorTest|FrameworkCoreTest|CAbiSafetyTest|ConcurrencyAndEdgeCasesTest|AdapterContractSecurityTest|PipelineConfigTest|PipelineStudioTest|OperatorApiTest|OperatorOutputPoolTest|OperatorValueRegistryTest|OperatorBizBridgeRegistryTest|VisualizerServerTest|TypedBlackboardContractsTest|ValidatedPipelinePlanTest|NodeBaseContractsTest|DefinitionSchemaValidationTest|CommonNodesTest|OperatorGoldenTest|AdapterPurityTest)$"
  if [[ "${DETECT_LEAKS:-0}" == "1" ]] || [[ "${SANITIZERS}" == *"thread"* ]]; then
    FAST_TEST_REGEX="^(BatchExecutorTest|FrameworkCoreTest|CAbiSafetyTest|ConcurrencyAndEdgeCasesTest|AdapterContractSecurityTest|PipelineConfigTest|PipelineStudioTest|OperatorApiTest|OperatorOutputPoolTest|OperatorValueRegistryTest|OperatorBizBridgeRegistryTest|TypedBlackboardContractsTest|ValidatedPipelinePlanTest|NodeBaseContractsTest|DefinitionSchemaValidationTest|CommonNodesTest|OperatorGoldenTest|AdapterPurityTest)$"
  fi
  echo ">>> [1/2] Running fast sanitized test suites: ${FAST_TEST_REGEX} <<<"
  "${ARCH_PREFIX[@]}" ctest --test-dir "${BUILD_DIR}" -j"$(nproc)" -R "${FAST_TEST_REGEX}" --output-on-failure
else
  echo ">>> [1/2] Running full sanitized CTest suite with [${SANITIZERS}] <<<"
  "${ARCH_PREFIX[@]}" ctest --test-dir "${BUILD_DIR}" -j"$(nproc)" --output-on-failure
fi

if [[ "${MODE}" == "fast" ]]; then
  echo ">>> [2/2] Running emulator-only Demo Smoke Suite with [${SANITIZERS}] <<<"
else
  echo ">>> [2/2] Running full-backend Demo Smoke Suite with [${SANITIZERS}] <<<"
fi
cd "${PROJECT_ROOT}"
"${ARCH_PREFIX[@]}" "${BUILD_DIR}/alg_demo" --suite smoke

echo "=================================================="
echo " 🎉 Sanitizer set [${SANITIZERS}] checks PASSED! (Mode: ${MODE})"
echo "=================================================="
