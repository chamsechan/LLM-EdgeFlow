#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build-sanitizers"
SANITIZERS="${LLM_EDGEFLOW_SANITIZERS:-address,undefined}"

MODE="fast"
if [ $# -gt 0 ]; then
  if [ "$1" == "--full" ]; then
    MODE="full"
  elif [ "$1" == "--fast" ]; then
    MODE="fast"
  fi
fi

echo "=================================================="
echo " Running Clang/GCC Sanitizer Suite (Mode: ${MODE})"
echo " Project Root: ${PROJECT_ROOT}"
echo " Sanitizers: ${SANITIZERS}"
echo " Note: ASan/UBSan memory safety checks enabled."
echo "       LeakSanitizer disabled (detect_leaks=0)."
echo "=================================================="

cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DENABLE_SANITIZERS=ON \
    -DLLM_EDGEFLOW_USE_CCACHE=OFF \
    -DLLM_EDGEFLOW_SANITIZERS="${SANITIZERS}"
cmake --build "${BUILD_DIR}" -j4

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

if [ "${MODE}" == "fast" ]; then
  echo ">>> [1/2] Running Fast CTest Suite with [${SANITIZERS}] (excluding long real-model inference) <<<"
  ctest --test-dir "${BUILD_DIR}" -E "QwenEnginesComparisonTest" --output-on-failure
else
  echo ">>> [1/2] Running Full CTest Suite with [${SANITIZERS}] <<<"
  ctest --test-dir "${BUILD_DIR}" --output-on-failure
fi

echo ">>> [2/2] Running Multi-Modal Demo Smoke Suite with [${SANITIZERS}] <<<"
cd "${PROJECT_ROOT}"
"${BUILD_DIR}/alg_demo" --suite smoke

echo "=================================================="
echo " 🎉 Sanitizer set [${SANITIZERS}] checks PASSED! (Mode: ${MODE})"
echo "=================================================="
