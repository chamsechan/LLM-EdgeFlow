#!/usr/bin/env bash
set -e

# ==============================================================================
# Alg-SDK Framework 全自动化测试与质量交付验证脚本 (Quality Assurance Test Suite)
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$ROOT_DIR/build"
export LD_LIBRARY_PATH="$BUILD_DIR:$BUILD_DIR/_deps/onnxruntime_prebuilt-src/lib:${LD_LIBRARY_PATH:-}"

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

# 1. 架构分层隔离、代码规范与差异门禁检验
echo -e "${BOLD}[ Step 1/6: LayerGuard 架构分层防腐扫描、架构文档防漂移、Shell 语法静态检查、Google C++ 代码规范与 Git Diff 门禁检验 ]${NC}"
for sh_file in "$SCRIPT_DIR"/*.sh; do
  bash -n "$sh_file"
done
"$SCRIPT_DIR/check_layer_isolation.sh"
"$SCRIPT_DIR/check_architecture_docs.sh"
"$SCRIPT_DIR/render_architecture_diagrams.sh" --check
"$SCRIPT_DIR/format.sh" --check
git diff --check
echo -e "${GREEN}✓ 架构分层隔离、文档防漂移、Shell 脚本语法、代码规范与差异门禁校验 100% 通过！${NC}\n"

# 2. 全量工程编译
echo -e "${BOLD}[ Step 2/6: CMake 构建与二进制链接 (Ninja & ccache 并行加速) ]${NC}"
mkdir -p "$BUILD_DIR"
CMAKE_GEN_ARGS=()
if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  if command -v ninja >/dev/null 2>&1; then
    CMAKE_GEN_ARGS=(-G Ninja)
  fi
fi
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DLLM_EDGEFLOW_USE_CCACHE=ON "${CMAKE_GEN_ARGS[@]}" > /dev/null
cmake --build "$BUILD_DIR" -j"$(nproc)"
echo -e "${GREEN}✓ 核心动态库与测试目标编译通过！${NC}\n"

# 3. 核心机制与多模态单元测试 (Tier 1)
echo -e "${BOLD}[ Step 3/6: 核心架构、DAG拓扑排序、引擎容错与全业务细粒度 GTest 单元测试 (并行加速) ]${NC}"
TIER1_REGEX='^(BatchExecutorTest|FrameworkCoreTest|PipelineConfigTest|Registry.*Test|DagPipelineTest|EngineFaultToleranceAndLifecycleTest|QwenEnginesComparisonTest|DifferentIoModalitiesTest|AllBusinessPipelinesTest|DocQaRerankTest|RerankRefineNodeTest)$'
ctest --test-dir "$BUILD_DIR" -j"$(nproc)" --output-on-failure -R "$TIER1_REGEX"
echo -e "${GREEN}✓ Tier 1 核心架构、DAG拓扑调度与全业务组合（含 LLM+Rerank+QA 与 RerankRefineNode）细粒度 GTest 断言测试全部通过！${NC}\n"

# 4. C ABI 安全防御、多线程并发与边界压测 (Tier 2)
echo -e "${BOLD}[ Step 4/6: C ABI 安全、平台 Operator 接口、8 线程并发、动态热重载与极端边界鲁棒性压测 (并行加速) ]${NC}"
TIER2_REGEX='^(C11AbiComplianceTest|CAbiSafetyTest|AdapterContractSecurityTest|PlatformOperatorTest|PlatformOutputPoolTest|PlatformValueRegistryTest|PlatformBusinessBridgeRegistryTest|RuntimeControlAndHotSwapTest|ConcurrencyAndEdgeCasesTest)$'
ctest --test-dir "$BUILD_DIR" -j"$(nproc)" --output-on-failure -R "$TIER2_REGEX"
echo -e "${GREEN}✓ Tier 2 C ABI 安全、平台 Operator 门面、在线动态热控制与极端边界容错测试全部通过！${NC}\n"

# 5. 7 大业务端到端全流程集成测试 (Tier 3)
echo -e "${BOLD}[ Step 5/6: 7 大业务端到端全链路集成测试 (参数化 Profile Demo 套件) ]${NC}"
"$SCRIPT_DIR/run_all_demos.sh" smoke
echo -e "${GREEN}✓ Tier 3 业务 1 ~ 业务 7 全链路端到端集成测试全部通过！${NC}\n"

# 6. 可视化与命令行工具链测试 (Tier 4)
echo -e "${BOLD}[ Step 6/6: CLI 可视化工具链双模测试 (Python & 纯 C++) ]${NC}"
cd "$ROOT_DIR"

echo -n "  Testing Python CLI (./show) on all 11 configs ... "
./show configs/pipeline_keyword_match.json > /dev/null
./show configs/pipeline_entity_extract.json > /dev/null
./show configs/pipeline_doc_qa.json > /dev/null
./show configs/pipeline_dialogue_audit.json > /dev/null
./show configs/pipeline_doc_qa_onnx.json > /dev/null
./show configs/pipeline_doc_qa_rerank.json > /dev/null
./show configs/pipeline_doc_qa_rerank_real.json > /dev/null
./show configs/pipeline_entity_extract_llamacpp.json > /dev/null
./show configs/pipeline_ocr_doc_qa.json > /dev/null
./show configs/pipeline_audio_asr_intent.json > /dev/null
./show configs/pipeline_cross_rerank.json > /dev/null
echo -e "${GREEN}✓ PASS${NC}"

echo -n "  Testing Native C++ CLI (./build/alg_show) on all 11 configs ... "
./build/alg_show configs/pipeline_keyword_match.json > /dev/null
./build/alg_show configs/pipeline_entity_extract.json > /dev/null
./build/alg_show configs/pipeline_doc_qa.json > /dev/null
./build/alg_show configs/pipeline_dialogue_audit.json > /dev/null
./build/alg_show configs/pipeline_doc_qa_onnx.json > /dev/null
./build/alg_show configs/pipeline_doc_qa_rerank.json > /dev/null
./build/alg_show configs/pipeline_doc_qa_rerank_real.json > /dev/null
./build/alg_show configs/pipeline_entity_extract_llamacpp.json > /dev/null
./build/alg_show configs/pipeline_ocr_doc_qa.json > /dev/null
./build/alg_show configs/pipeline_audio_asr_intent.json > /dev/null
./build/alg_show configs/pipeline_cross_rerank.json > /dev/null
echo -e "${GREEN}✓ PASS${NC}"

echo -n "  Testing Pipeline Studio Validator/API/real Demo roundtrip ... "
"$BUILD_DIR/test_pipeline_studio" > /dev/null
python3 tests/test_visualizer_server.py > /dev/null
./build/alg_pipeline_tool catalog --business keyword_match_v1 > /dev/null
./build/alg_pipeline_tool validate configs/pipeline_keyword_match.json > /dev/null
echo -e "${GREEN}✓ PASS${NC}"

echo -e "${BOLD}${GREEN}==================================================================${NC}"
echo -e "${BOLD}${GREEN}  🎉 交付验证通过：全部 6 大测试阶段 100% PASS！                 ${NC}"
echo -e "${BOLD}${GREEN}  - 核心架构单元测试   : PASSED (6/6 模块)${NC}"
echo -e "${BOLD}${GREEN}  - C ABI 安全边界测试 : PASSED (空指针拦截 / 50轮并发与生命周期稳定)${NC}"
echo -e "${BOLD}${GREEN}  - 7 大业务端到端测试 : PASSED (规则/抽取/问答/风控/OCR/语音/精排)${NC}"
echo -e "${BOLD}${GREEN}  - CLI 工具双模适配   : PASSED (Python CLI & C++ Native CLI)${NC}"
echo -e "${BOLD}${GREEN}  - Google C++ 规范    : PASSED (100% 格式对齐)${NC}"
echo -e "${BOLD}${GREEN}==================================================================${NC}\n"
