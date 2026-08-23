#!/usr/bin/env bash
set -e

# ==============================================================================
# LLM-EdgeFlow 批量参数化 Demo 执行与验证脚本
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
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

SMOKE_PROFILES=(
  "entity_extract_mock"
  "keyword_match_mock"
  "doc_qa_mock"
  "doc_qa_onnx"
  "doc_qa_rerank"
  "dialogue_audit_mock"
  "ocr_doc_qa_mock"
  "audio_asr_mock"
  "cross_rerank_mock"
)

REAL_PROFILES=(
  "entity_extract_llamacpp"
  "doc_qa_rerank_real"
)

run_profile() {
  local profile="$1"
  echo -e "\n${BOLD}${CYAN}>>> [Running Demo Profile: ${profile}] <<<${NC}"
  "$DEMO_BIN" --profile "$profile"
  local status=$?
  if [ $status -ne 0 ]; then
    echo -e "${RED}❌ [FAILED] Profile '${profile}' exited with code ${status}.${NC}"
    exit $status
  fi
  echo -e "${GREEN}✓ [PASS] Profile '${profile}' completed successfully.${NC}"
}

echo -e "${BOLD}${CYAN}==================================================================${NC}"
echo -e "${BOLD}${CYAN}  LLM-EdgeFlow Multi-Modal Demo Batch Runner                     ${NC}"
echo -e "${BOLD}${CYAN}  Mode: ${MODE} | Project Root: ${ROOT_DIR}${NC}"
echo -e "${BOLD}${CYAN}==================================================================${NC}"

case "$MODE" in
  smoke)
    echo -e "${BOLD}Running SMOKE demo suite (${#SMOKE_PROFILES[@]} profiles)...${NC}"
    for prof in "${SMOKE_PROFILES[@]}"; do
      run_profile "$prof"
    done
    ;;
  real)
    echo -e "${BOLD}Running REAL model demo suite (${#REAL_PROFILES[@]} profiles)...${NC}"
    for prof in "${REAL_PROFILES[@]}"; do
      run_profile "$prof"
    done
    ;;
  all)
    echo -e "${BOLD}Running ALL demo suites (Smoke + Real)...${NC}"
    for prof in "${SMOKE_PROFILES[@]}"; do
      run_profile "$prof"
    done
    for prof in "${REAL_PROFILES[@]}"; do
      run_profile "$prof"
    done
    ;;
  *)
    echo -e "${RED}[ERROR] Unknown mode: '${MODE}'. Supported modes: smoke, real, all.${NC}"
    exit 1
    ;;
esac

echo -e "\n${BOLD}${GREEN}==================================================================${NC}"
echo -e "${BOLD}${GREEN}  🎉 All requested demo profiles (${MODE}) EXECUTED 100% SUCCESSFULLY! ${NC}"
echo -e "${BOLD}${GREEN}==================================================================${NC}\n"
