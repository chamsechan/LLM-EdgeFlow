#!/usr/bin/env bash
set -euo pipefail

# 获取脚本所在根目录
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:-format}"
REQUIRED_CLANG_FORMAT_MAJOR=18

resolve_clang_format() {
    local candidate
    if [[ -n "${CLANG_FORMAT:-}" ]]; then
        candidate="${CLANG_FORMAT}"
        command -v "${candidate}" 2>/dev/null || return 1
        return
    fi

    for candidate in \
        clang-format-18 \
        /opt/homebrew/opt/llvm@18/bin/clang-format \
        clang-format; do
        if command -v "${candidate}" >/dev/null 2>&1; then
            command -v "${candidate}"
            return
        fi
    done
    return 1
}

if ! CLANG_FORMAT_BIN="$(resolve_clang_format)"; then
    echo "Error: clang-format ${REQUIRED_CLANG_FORMAT_MAJOR} is required."
    echo "Set CLANG_FORMAT to its executable path if it is not on PATH."
    exit 1
fi

CLANG_FORMAT_VERSION="$("${CLANG_FORMAT_BIN}" --version)"
if [[ ! "${CLANG_FORMAT_VERSION}" =~ clang-format[[:space:]]+version[[:space:]]+${REQUIRED_CLANG_FORMAT_MAJOR}\. ]]; then
    echo "Error: ${CLANG_FORMAT_BIN} reports '${CLANG_FORMAT_VERSION}'."
    echo "clang-format ${REQUIRED_CLANG_FORMAT_MAJOR}.x is required for reproducible formatting."
    exit 1
fi

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
echo " Formatter: ${CLANG_FORMAT_BIN} (${CLANG_FORMAT_VERSION})"
echo "=================================================="

FORMAT_DIRS=("include" "src" "demo" "tests")

for dir in "${FORMAT_DIRS[@]}"; do
    if [ -d "${PROJECT_ROOT}/${dir}" ]; then
        if [[ "$MODE" == "--check" ]]; then
            echo "Checking directory: ${dir}/..."
            find "${PROJECT_ROOT}/${dir}" \
                -type f \( -name "*.h" -o -name "*.hpp" -o -name "*.cpp" -o -name "*.cc" -o -name "*.c" \) \
                -not -path "*/third_party/*" \
                -exec "${CLANG_FORMAT_BIN}" --dry-run --Werror --style=file {} +
        else
            echo "Formatting directory: ${dir}/..."
            find "${PROJECT_ROOT}/${dir}" \
                -type f \( -name "*.h" -o -name "*.hpp" -o -name "*.cpp" -o -name "*.cc" -o -name "*.c" \) \
                -not -path "*/third_party/*" \
                -exec "${CLANG_FORMAT_BIN}" -i --style=file {} +
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
