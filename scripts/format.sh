#!/usr/bin/env bash
set -euo pipefail

# 获取脚本所在根目录
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:-format}"

if [[ "$MODE" != "format" && "$MODE" != "--check" ]]; then
    echo "Usage: $0 [--check]"
    exit 2
fi

echo "=================================================="
if [[ "$MODE" == "--check" ]]; then
    echo " Checking C++ formatting using Google Style (.clang-format) "
else
    echo " Formatting C++ codebase using Google Style (.clang-format) "
fi
echo " Project Root: ${PROJECT_ROOT}"
echo "=================================================="

if ! command -v clang-format &> /dev/null; then
    echo "[Error] clang-format is not installed."
    echo "Please install it via: sudo apt-get install clang-format"
    exit 1
fi

FORMAT_DIRS=("include" "src" "demo" "tests")

for dir in "${FORMAT_DIRS[@]}"; do
    if [ -d "${PROJECT_ROOT}/${dir}" ]; then
        if [[ "$MODE" == "--check" ]]; then
            echo "Checking directory: ${dir}/..."
            find "${PROJECT_ROOT}/${dir}" \
                -type f \( -name "*.h" -o -name "*.hpp" -o -name "*.cpp" -o -name "*.cc" -o -name "*.c" \) \
                -not -path "*/third_party/*" \
                -exec clang-format --dry-run --Werror --style=file {} +
        else
            echo "Formatting directory: ${dir}/..."
            find "${PROJECT_ROOT}/${dir}" \
                -type f \( -name "*.h" -o -name "*.hpp" -o -name "*.cpp" -o -name "*.cc" -o -name "*.c" \) \
                -not -path "*/third_party/*" \
                -exec clang-format -i --style=file {} +
        fi
    fi
done

echo "=================================================="
if [[ "$MODE" == "--check" ]]; then
    echo " All C/C++ files satisfy the formatting policy!"
else
    echo " All C/C++ files formatted successfully!"
fi
echo "=================================================="
