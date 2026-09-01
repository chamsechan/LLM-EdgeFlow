#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# LLM-EdgeFlow canonical quality gate.
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

if [[ $# -ne 0 ]]; then
  echo "Usage: $0"
  exit 2
fi
BUILD_DIR="$ROOT_DIR/build"

SELECTED_LINKER="${LLM_EDGEFLOW_LINKER:-auto}"
JOBS="${LLM_EDGEFLOW_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
SECONDS=0

export LD_LIBRARY_PATH="$ROOT_DIR/3rdparty/onnxruntime/lib:$BUILD_DIR:$BUILD_DIR/_deps/onnxruntime_prebuilt-src/lib:${LD_LIBRARY_PATH:-}"

GREEN='\033[0;32m'
CYAN='\033[0;36m'
RED='\033[0;31m'
BOLD='\033[1m'
NC='\033[0m'

echo -e "${BOLD}${CYAN}==================================================================${NC}"
echo -e "${BOLD}${CYAN}  LLM-EdgeFlow Unified Quality Gate                              ${NC}"
echo -e "${BOLD}${CYAN}  Build: $BUILD_DIR | Linker: $SELECTED_LINKER${NC}"
echo -e "${BOLD}${CYAN}==================================================================${NC}\n"

echo -e "${BOLD}[ Step 1/6: Shell syntax, Google C++ format and Git diff gates ]${NC}"
for sh_file in "$SCRIPT_DIR"/*.sh; do
  bash -n "$sh_file"
done
"$SCRIPT_DIR/format.sh" --check
git -C "$ROOT_DIR" diff --check
echo -e "${GREEN}✓ Static source gates passed.${NC}\n"

echo -e "${BOLD}[ Step 2/6: Configure and build ]${NC}"
GEN_ARG_STR=$("${SCRIPT_DIR}/detect_cmake_generator.sh" "$BUILD_DIR")
CMAKE_GEN_ARGS=()
if [[ -n "$GEN_ARG_STR" ]]; then
  read -r -a CMAKE_GEN_ARGS <<< "$GEN_ARG_STR"
fi

CMAKE_ARGS=(
  -DCMAKE_BUILD_TYPE=Release
  -DLLM_EDGEFLOW_SHARDED_TEST_RUNNERS=ON
  -DLLM_EDGEFLOW_LINKER="$SELECTED_LINKER"
  -DENABLE_REAL_MODEL_TESTS=OFF
  -DENABLE_LLAMACPP=ON
  -DENABLE_ONNXRUNTIME=ON
)

if [[ ${#CMAKE_GEN_ARGS[@]} -gt 0 ]]; then
  cmake -S "$ROOT_DIR" -B "$BUILD_DIR" "${CMAKE_ARGS[@]}" \
    "${CMAKE_GEN_ARGS[@]}"
else
  # macOS 自带 Bash 3.2 在 set -u 下不能展开空数组。
  cmake -S "$ROOT_DIR" -B "$BUILD_DIR" "${CMAKE_ARGS[@]}"
fi
cmake --build "$BUILD_DIR" -j"$JOBS"
echo -e "${GREEN}✓ Build completed.${NC}\n"

# Steps 3-6 share one global scheduler. Labels retain stage ownership while
# allowing slow integration and tooling tests to overlap safely.
echo -e "${BOLD}[ Steps 3-6/6: Unified Tier 1-4 CTest scheduler ]${NC}"
ctest --test-dir "$BUILD_DIR" -j"$JOBS" --output-on-failure

echo -e "\n${BOLD}${GREEN}==================================================================${NC}"
echo -e "${BOLD}${GREEN}  ✓ All required development gates passed in ${SECONDS}s.${NC}"
echo -e "${BOLD}${GREEN}  - Tier 1: Core, DAG, engines and common nodes${NC}"
echo -e "${BOLD}${GREEN}  - Tier 2: C ABI, Operator, concurrency and safety${NC}"
echo -e "${BOLD}${GREEN}  - Tier 3: Business integration and Demo smoke${NC}"
echo -e "${BOLD}${GREEN}  - Tier 4: CLI, Pipeline Studio and documentation tooling${NC}"
echo -e "${BOLD}${GREEN}==================================================================${NC}\n"
