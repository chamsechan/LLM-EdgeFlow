#!/usr/bin/env bash
set -e

# ==============================================================================
# Alg-SDK Framework 全自动化测试与质量交付验证脚本 (Quality Assurance Test Suite)
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$ROOT_DIR/build"

GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
BOLD='\033[1m'
NC='\033[0m'

echo -e "${BOLD}${CYAN}==================================================================${NC}"
echo -e "${BOLD}${CYAN}  Alg-SDK 框架全层级自动化测试与交付质量验证套件                  ${NC}"
echo -e "${BOLD}${CYAN}  Project Root: $ROOT_DIR${NC}"
echo -e "${BOLD}${CYAN}==================================================================${NC}\n"

# 1. 代码规范与格式化检验
echo -e "${BOLD}[ Step 1/6: Google C++ 代码规范扫描与格式化检验 ]${NC}"
"$SCRIPT_DIR/format.sh"
echo -e "${GREEN}✓ 代码规范校验通过！${NC}\n"

# 2. 全量工程编译
echo -e "${BOLD}[ Step 2/6: CMake 构建与二进制链接 ]${NC}"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake "$ROOT_DIR" > /dev/null
make -j4
echo -e "${GREEN}✓ 核心动态库与测试目标编译通过！${NC}\n"

# 3. 核心机制与多模态单元测试 (Tier 1)
echo -e "${BOLD}[ Step 3/6: 核心架构、DAG拓扑排序与全业务细粒度 GTest 单元测试 ]${NC}"
"$BUILD_DIR/test_batch_executor"
"$BUILD_DIR/test_framework_core"
"$BUILD_DIR/test_dag_pipeline"
"$BUILD_DIR/test_qwen_engines_comparison"
"$BUILD_DIR/test_different_io_modalities"
"$BUILD_DIR/test_all_business_pipelines"
echo -e "${GREEN}✓ Tier 1 核心架构、DAG拓扑调度与 7 大业务细粒度 GTest 断言测试全部通过！${NC}\n"

# 4. C ABI 安全防御、多线程并发与边界压测 (Tier 2)
echo -e "${BOLD}[ Step 4/6: C ABI 安全、8 线程高并发与极端边界鲁棒性压测 ]${NC}"
"$BUILD_DIR/test_c_abi_safety"
"$BUILD_DIR/test_concurrency_and_edge_cases"
echo -e "${GREEN}✓ Tier 2 C ABI 安全、多线程并发与边界容错测试全部通过！${NC}\n"

# 5. 7 大业务端到端全流程集成测试 (Tier 3)
echo -e "${BOLD}[ Step 5/6: 7 大业务端到端全链路集成测试 (规则/NLP/问答/质检/OCR/语音ASR/精排) ]${NC}"
"$BUILD_DIR/alg_demo"
echo -e "${GREEN}✓ Tier 3 业务 1 ~ 业务 7 全链路端到端集成测试全部通过！${NC}\n"

# 6. 可视化与命令行工具链测试 (Tier 4)
echo -e "${BOLD}[ Step 6/6: CLI 可视化工具链双模测试 (Python & 纯 C++) ]${NC}"
cd "$ROOT_DIR"

echo -n "  Testing Python CLI (./show) on all 9 configs ... "
./show configs/pipeline_keyword_match.json > /dev/null
./show configs/pipeline_entity_extract.json > /dev/null
./show configs/pipeline_doc_qa.json > /dev/null
./show configs/pipeline_dialogue_audit.json > /dev/null
./show configs/pipeline_doc_qa_onnx.json > /dev/null
./show configs/pipeline_entity_extract_llamacpp.json > /dev/null
./show configs/pipeline_ocr_doc_qa.json > /dev/null
./show configs/pipeline_audio_asr_intent.json > /dev/null
./show configs/pipeline_cross_rerank.json > /dev/null
echo -e "${GREEN}✓ PASS${NC}"

echo -n "  Testing Native C++ CLI (./build/alg_show) on all 9 configs ... "
./build/alg_show configs/pipeline_keyword_match.json > /dev/null
./build/alg_show configs/pipeline_entity_extract.json > /dev/null
./build/alg_show configs/pipeline_doc_qa.json > /dev/null
./build/alg_show configs/pipeline_dialogue_audit.json > /dev/null
./build/alg_show configs/pipeline_doc_qa_onnx.json > /dev/null
./build/alg_show configs/pipeline_entity_extract_llamacpp.json > /dev/null
./build/alg_show configs/pipeline_ocr_doc_qa.json > /dev/null
./build/alg_show configs/pipeline_audio_asr_intent.json > /dev/null
./build/alg_show configs/pipeline_cross_rerank.json > /dev/null
echo -e "${GREEN}✓ PASS${NC}"

echo -e "\n${BOLD}${GREEN}==================================================================${NC}"
echo -e "${BOLD}${GREEN}  🎉 交付验证通过：全部 6 大测试阶段 100% PASS！                 ${NC}"
echo -e "${BOLD}${GREEN}  - 核心架构单元测试   : PASSED (6/6 模块)${NC}"
echo -e "${BOLD}${GREEN}  - C ABI 安全边界测试 : PASSED (空指针拦截 / 50轮无泄露)${NC}"
echo -e "${BOLD}${GREEN}  - 4 大业务端到端测试 : PASSED (规则 / 小模型 / 复杂问答 / 风控质检)${NC}"
echo -e "${BOLD}${GREEN}  - CLI 工具双模适配   : PASSED (Python CLI & C++ Native CLI)${NC}"
echo -e "${BOLD}${GREEN}  - Google C++ 规范    : PASSED (100% 格式对齐)${NC}"
echo -e "${BOLD}${GREEN}==================================================================${NC}\n"
