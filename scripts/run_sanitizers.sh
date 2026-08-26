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
BUILD_DIR="${PROJECT_ROOT}/build-sanitizers-${MODE}"

echo "=================================================="
echo " Running Clang/GCC Sanitizer Suite (Mode: ${MODE})"
echo " Project Root: ${PROJECT_ROOT}"
echo " Sanitizers: ${SANITIZERS}"
echo " Note: ASan/UBSan memory safety checks enabled."
echo "       LeakSanitizer disabled (detect_leaks=0)."
echo "=================================================="

COMMON_CMAKE_ARGS=(
  -DCMAKE_BUILD_TYPE=Debug
  -DENABLE_SANITIZERS=ON
  -DLLM_EDGEFLOW_SANITIZERS="${SANITIZERS}"
  -DLLM_EDGEFLOW_USE_CCACHE=ON
)
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
    test_batch_executor
    test_c_abi_safety
    test_concurrency_and_edge_cases
    test_definition_schema_validation
    test_framework_core
    test_node_base_contracts
    test_pipeline_config
    test_pipeline_studio
    test_typed_blackboard_contracts
    test_validated_pipeline_plan
  )
  cmake --build "${BUILD_DIR}" --target "${FAST_TARGETS[@]}" -j4
else
  cmake --build "${BUILD_DIR}" -j4
fi

export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1"
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
export LD_LIBRARY_PATH="${BUILD_DIR}:${BUILD_DIR}/_deps/onnxruntime_prebuilt-src/lib:${LD_LIBRARY_PATH:-}"
export DYLD_LIBRARY_PATH="${BUILD_DIR}:${BUILD_DIR}/_deps/onnxruntime_prebuilt-src/lib:${DYLD_LIBRARY_PATH:-}"
export LLM_EDGEFLOW_PIPELINE_TOOL="${BUILD_DIR}/alg_pipeline_tool"
export LLM_EDGEFLOW_DEMO_BINARY="${BUILD_DIR}/alg_demo"

if [ "${MODE}" == "fast" ]; then
  echo ">>> [1/2] Running emulator-only core CTest suite with [${SANITIZERS}] <<<"
  FAST_TEST_REGEX="^(BatchExecutorTest|FrameworkCoreTest|CAbiSafetyTest|ConcurrencyAndEdgeCasesTest|AdapterContractSecurityTest|PipelineConfigTest|PipelineStudioTest|VisualizerServerTest|TypedBlackboardContractsTest|ValidatedPipelinePlanTest|NodeBaseContractsTest|DefinitionSchemaValidationTest)$"
  ctest --test-dir "${BUILD_DIR}" -R "${FAST_TEST_REGEX}" --output-on-failure
else
  echo ">>> [1/2] Running Full CTest Suite with [${SANITIZERS}] <<<"
  ctest --test-dir "${BUILD_DIR}" --output-on-failure
fi

if [[ "${MODE}" == "fast" ]]; then
  echo ">>> [2/2] Running emulator-only Demo Smoke Suite with [${SANITIZERS}] <<<"
else
  echo ">>> [2/2] Running full-backend Demo Smoke Suite with [${SANITIZERS}] <<<"
fi
cd "${PROJECT_ROOT}"
"${BUILD_DIR}/alg_demo" --suite smoke

echo "=================================================="
echo " 🎉 Sanitizer set [${SANITIZERS}] checks PASSED! (Mode: ${MODE})"
echo "=================================================="
