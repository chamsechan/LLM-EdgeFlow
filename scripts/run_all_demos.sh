#!/usr/bin/env bash
set -e

# ==============================================================================
# LLM-EdgeFlow 批量参数化 Demo 执行与验证脚本
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$ROOT_DIR"

BUILD_DIR="$ROOT_DIR/build"
DEMO_BIN="$BUILD_DIR/alg_demo"

GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
BOLD='\033[1m'
NC='\033[0m'

if [ ! -f "$DEMO_BIN" ]; then
  echo -e "${RED}[ERROR] alg_demo binary not found in $BUILD_DIR. Please build project first.${NC}"
  exit 1
fi

MODE="${1:-smoke}"

echo -e "${BOLD}${CYAN}==================================================================${NC}"
echo -e "${BOLD}${CYAN}  LLM-EdgeFlow Multi-Modal Demo Suite Runner                     ${NC}"
echo -e "${BOLD}${CYAN}  Mode: ${MODE} | Project Root: ${ROOT_DIR}${NC}"
echo -e "${BOLD}${CYAN}==================================================================${NC}"

case "$MODE" in
  smoke|real|all)
    echo -e "${BOLD}Dispatching suite '${MODE}' via alg_demo...${NC}"
    "$DEMO_BIN" --suite "$MODE"
    ;;
  *)
    echo -e "${RED}[ERROR] Unknown mode: '${MODE}'. Supported modes: smoke, real, all.${NC}"
    exit 1
    ;;
esac

echo -e "\n${BOLD}${GREEN}==================================================================${NC}"
echo -e "${BOLD}${GREEN}  🎉 All requested demo profiles in suite [${MODE}] EXECUTED 100% SUCCESSFULLY! ${NC}"
echo -e "${BOLD}${GREEN}==================================================================${NC}\n"
