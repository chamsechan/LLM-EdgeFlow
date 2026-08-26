#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

echo "================================================================"
echo " [Architecture Docs Drift Check] Validating Architecture SSOT"
echo "================================================================"

FAILED=0

# 1. 检查权威文档与资产中是否存在旧业务名
echo "[Check 1/6] Checking for legacy business names (doc_qa_embedding_v1, doc_qa_rerank_v1)..."
LEGACY_BIZ=$(grep -rnE "(doc_qa_embedding_v1|doc_qa_rerank_v1)" doc/ README.md src/ include/ 2>/dev/null | grep -v "0008-architecture-contract-consolidation-remediation-plan.md" | grep -v "check_architecture_docs.sh" || true)
if [ -n "${LEGACY_BIZ}" ]; then
  echo "❌ Found deprecated business names in active docs or codebase:"
  echo "${LEGACY_BIZ}"
  FAILED=1
else
  echo "✅ No legacy business names found."
fi

# 2. 检查旧注册宏 (REGISTER_NODE( / REGISTER_ENGINE()
echo "[Check 2/6] Checking for deprecated registration macros without definitions..."
LEGACY_MACROS=$(grep -rnE "\bREGISTER_NODE\([A-Za-z0-9_]+\)" doc/architecture.md doc/developer_guide.md doc/README.md README.md CLAUDE.md AGENTS.md src/ 2>/dev/null || true)
if [ -n "${LEGACY_MACROS}" ]; then
  echo "❌ Found deprecated REGISTER_NODE(Name) in active docs or source:"
  echo "${LEGACY_MACROS}"
  FAILED=1
else
  echo "✅ No deprecated REGISTER_NODE(...) found in active docs/source."
fi

# 3. 检查虚构生产节点 (PassthroughNode, ComplianceReportPostNode)
echo "[Check 3/6] Checking for fictitious production nodes..."
FICTITIOUS_NODES=$(grep -rnE "\b(PassthroughNode|ComplianceReportPostNode)\b" doc/ README.md src/ include/ 2>/dev/null | grep -v "0008-architecture-contract-consolidation-remediation-plan.md" | grep -v "check_architecture_docs.sh" || true)
if [ -n "${FICTITIOUS_NODES}" ]; then
  echo "❌ Found fictitious production nodes in active docs or codebase:"
  echo "${FICTITIOUS_NODES}"
  FAILED=1
else
  echo "✅ No fictitious production nodes found."
fi

# 4. 检查架构文档核心概念完备性 (ValidatedPipelinePlan, BlackboardKey, NodeBase, FixedBatchExecutor)
echo "[Check 4/6] Verifying core architectural concepts in architecture documents..."
for concept in "ValidatedPipelinePlan" "BlackboardKey" "NodeBase" "FixedBatchExecutor"; do
  if ! grep -q "${concept}" doc/architecture.md; then
    echo "❌ doc/architecture.md is missing reference to '${concept}'"
    FAILED=1
  fi
  if ! grep -q "${concept}" doc/developer_guide.md; then
    echo "❌ doc/developer_guide.md is missing reference to '${concept}'"
    FAILED=1
  fi
done
if [ ${FAILED} -eq 0 ]; then
  echo "✅ All core architectural concepts verified in architecture docs."
fi

# 5. 检查 architecture_v2.puml 状态图例
echo "[Check 5/6] Checking architecture_v2.puml state legends..."
if ! grep -q "Implemented" doc/architecture_v2.puml || \
   ! grep -q "Partial" doc/architecture_v2.puml || \
   ! grep -q "Planned" doc/architecture_v2.puml; then
  echo "❌ doc/architecture_v2.puml is missing Implemented / Partial / Planned status legends"
  FAILED=1
else
  echo "✅ architecture_v2.puml state legends verified."
fi

# 6. 检查 PlantUML 语法有效性 (如果可用)
echo "[Check 6/6] Verifying architecture diagrams exist and are non-empty..."
for puml in doc/architecture.puml doc/architecture_v2.puml; do
  if [ ! -s "${puml}" ]; then
    echo "❌ PlantUML file '${puml}' is missing or empty"
    FAILED=1
  fi
done
for svg in doc/assets/architecture_class_diagram.svg doc/assets/architecture_flow.svg; do
  if [ ! -s "${svg}" ]; then
    echo "❌ SVG asset '${svg}' is missing or empty"
    FAILED=1
  fi
done

if [ ${FAILED} -ne 0 ]; then
  echo "❌ Architecture docs drift check FAILED."
  exit 1
fi

echo "✅ All architecture documentation drift checks PASSED."
exit 0
