#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build-sanitizers"
SANITIZERS="${LLM_EDGEFLOW_SANITIZERS:-address,undefined}"

echo "=================================================="
echo " Running Clang/GCC Sanitizer Suite"
echo " Project Root: ${PROJECT_ROOT}"
echo " Sanitizers: ${SANITIZERS}"
echo "=================================================="

cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DENABLE_SANITIZERS=ON \
    -DLLM_EDGEFLOW_USE_CCACHE=OFF \
    -DLLM_EDGEFLOW_SANITIZERS="${SANITIZERS}"
cmake --build "${BUILD_DIR}" -j4

export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"
export LD_LIBRARY_PATH="${BUILD_DIR}:${BUILD_DIR}/_deps/onnxruntime_prebuilt-src/lib:${LD_LIBRARY_PATH:-}"
export DYLD_LIBRARY_PATH="${BUILD_DIR}:${BUILD_DIR}/_deps/onnxruntime_prebuilt-src/lib:${DYLD_LIBRARY_PATH:-}"

echo ">>> [1/2] Running all registered CTest suites with [${SANITIZERS}] <<<"
ctest --test-dir "${BUILD_DIR}" --output-on-failure

echo ">>> [2/2] Running Multi-Modal Demo Suite with [${SANITIZERS}] <<<"
cd "${PROJECT_ROOT}"
"${BUILD_DIR}/alg_demo" --suite smoke

echo "=================================================="
echo " 🎉 Sanitizer set [${SANITIZERS}] checks 100% PASS!"
echo "=================================================="
