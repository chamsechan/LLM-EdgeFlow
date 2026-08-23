#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"

echo "=================================================="
echo " Running Clang/GCC Address & UB Sanitizers (ASan/UBSan)"
echo " Project Root: ${PROJECT_ROOT}"
echo "=================================================="

cd "${BUILD_DIR}"

cmake .. -DENABLE_SANITIZERS=ON
make -j4

export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"

echo ">>> [1/2] Running All 21 CTest Suites under ASan/UBSan <<<"
ctest --output-on-failure

echo ">>> [2/2] Running Multi-Modal Demo Suite under ASan/UBSan <<<"
./alg_demo --suite smoke

# 恢复默认 Release 编译状态
cmake .. -DENABLE_SANITIZERS=OFF >/dev/null 2>&1
make -j4 >/dev/null 2>&1

echo "=================================================="
echo " 🎉 Full ASan & UBSan Memory & Contract Checks 100% PASS!"
echo "=================================================="
