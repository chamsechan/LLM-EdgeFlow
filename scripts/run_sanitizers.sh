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
make test_adapter_contract_security test_c11_abi_compliance -j4

export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"

echo ">>> [1/2] Running AdapterContractSecurityTest under ASan/UBSan <<<"
./test_adapter_contract_security

echo ">>> [2/2] Running C11AbiComplianceTest under ASan/UBSan <<<"
./test_c11_abi_compliance

# 恢复默认 Release 编译状态
cmake .. -DENABLE_SANITIZERS=OFF >/dev/null 2>&1

echo "=================================================="
echo " 🎉 ASan & UBSan Memory & Contract Checks 100% PASS!"
echo "=================================================="
