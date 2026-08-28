#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# LLM-EdgeFlow unified development/full quality gate.
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

MODE="full"
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

if [[ "$MODE" == "fast" ]]; then
  BUILD_DIR="$ROOT_DIR/build-fast"
else
  BUILD_DIR="$ROOT_DIR/build"
fi

SELECTED_LINKER="${LLM_EDGEFLOW_LINKER:-auto}"
JOBS="${LLM_EDGEFLOW_JOBS:-$(nproc)}"
SECONDS=0

export LD_LIBRARY_PATH="$BUILD_DIR:$BUILD_DIR/_deps/onnxruntime_prebuilt-src/lib:${LD_LIBRARY_PATH:-}"

GREEN='\033[0;32m'
CYAN='\033[0;36m'
RED='\033[0;31m'
BOLD='\033[1m'
NC='\033[0m'

echo -e "${BOLD}${CYAN}==================================================================${NC}"
echo -e "${BOLD}${CYAN}  LLM-EdgeFlow Unified Quality Gate                              ${NC}"
echo -e "${BOLD}${CYAN}  Mode: $MODE | Build: $BUILD_DIR | Linker: $SELECTED_LINKER${NC}"
echo -e "${BOLD}${CYAN}==================================================================${NC}\n"

echo -e "${BOLD}[ Step 1/6: Shell syntax, Google C++ format and Git diff gates ]${NC}"
for sh_file in "$SCRIPT_DIR"/*.sh; do
  bash -n "$sh_file"
done
"$SCRIPT_DIR/format.sh" --check
git -C "$ROOT_DIR" diff --check
echo -e "${GREEN}✓ Static source gates passed.${NC}\n"

echo -e "${BOLD}[ Step 2/6: Configure and build with Ninja + ccache ]${NC}"
GEN_ARG_STR=$("${SCRIPT_DIR}/detect_cmake_generator.sh" "$BUILD_DIR")
CMAKE_GEN_ARGS=()
if [[ -n "$GEN_ARG_STR" ]]; then
  read -r -a CMAKE_GEN_ARGS <<< "$GEN_ARG_STR"
fi

CMAKE_ARGS=(
  -DLLM_EDGEFLOW_USE_CCACHE=ON
  -DLLM_EDGEFLOW_SHARDED_TEST_RUNNERS=ON
  -DLLM_EDGEFLOW_LINKER="$SELECTED_LINKER"
  -DENABLE_REAL_MODEL_TESTS=OFF
)
if [[ "$MODE" == "fast" ]]; then
  CMAKE_ARGS+=(
    -DCMAKE_BUILD_TYPE=Debug
    -DLLM_EDGEFLOW_FAST_BUILD=ON
    -DENABLE_LLAMACPP=OFF
    -DENABLE_ONNXRUNTIME=OFF
  )
else
  CMAKE_ARGS+=(
    -DCMAKE_BUILD_TYPE=Release
    -DLLM_EDGEFLOW_FAST_BUILD=OFF
    -DENABLE_LLAMACPP=ON
    -DENABLE_ONNXRUNTIME=ON
  )
fi

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" "${CMAKE_ARGS[@]}" \
  "${CMAKE_GEN_ARGS[@]}"
if [[ "$MODE" == "fast" ]]; then
  cmake --build "$BUILD_DIR" --target edgeflow_dev_tests -j"$JOBS"
else
  cmake --build "$BUILD_DIR" -j"$JOBS"
fi
echo -e "${GREEN}✓ Build completed.${NC}\n"

# Steps 3-6 share one global scheduler. Labels retain stage ownership while
# allowing slow integration and tooling tests to overlap safely.
echo -e "${BOLD}[ Steps 3-6/6: Unified Tier 1-4 CTest scheduler ]${NC}"
CTEST_ARGS=(
  --test-dir "$BUILD_DIR"
  -j"$JOBS"
  --output-on-failure
)
if [[ "$MODE" == "fast" ]]; then
  CTEST_ARGS+=( -L dev-fast )
fi
ctest "${CTEST_ARGS[@]}"

echo -e "\n${BOLD}${GREEN}==================================================================${NC}"
echo -e "${BOLD}${GREEN}  ✓ All required $MODE development gates passed in ${SECONDS}s.${NC}"
echo -e "${BOLD}${GREEN}  - Tier 1: Core, DAG, engines and common nodes${NC}"
echo -e "${BOLD}${GREEN}  - Tier 2: C ABI, Operator, concurrency and safety${NC}"
echo -e "${BOLD}${GREEN}  - Tier 3: Business integration and Demo smoke${NC}"
echo -e "${BOLD}${GREEN}  - Tier 4: CLI, Pipeline Studio and documentation tooling${NC}"
echo -e "${BOLD}${GREEN}==================================================================${NC}\n"
