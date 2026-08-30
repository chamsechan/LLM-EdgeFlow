#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"
DOC_ROOT="${LLM_EDGEFLOW_ARCH_DOC_ROOT:-${ROOT_DIR}/doc}"

echo "================================================================"
echo " [Architecture Docs Drift Check] Validating Architecture SSOT"
echo "================================================================"

FAILED=0

ACTIVE_DOCS=(
  "${DOC_ROOT}/architecture.md"
  "${DOC_ROOT}/developer_guide.md"
  "${DOC_ROOT}/README.md"
  "${DOC_ROOT}/architecture.puml"
  "${DOC_ROOT}/architecture_v2.puml"
  "${DOC_ROOT}/assets/architecture_class_diagram.svg"
  "${DOC_ROOT}/assets/architecture_flow.svg"
  "${ROOT_DIR}/README.md"
  "${ROOT_DIR}/CONTRIBUTING.md"
  "${ROOT_DIR}/AGENTS.md"
  "${ROOT_DIR}/.github/copilot-instructions.md"
  "${ROOT_DIR}/.agents/skills"
  "${ROOT_DIR}/src"
  "${ROOT_DIR}/include"
  "${ROOT_DIR}/demo"
  "${ROOT_DIR}/configs"
)

# 1. 检查权威文档与资产中是否存在旧业务名 (包含 SVG 资产)
echo "[Check 1/7] Checking for legacy business names (doc_qa_embedding_v1, doc_qa_rerank_v1)..."
LEGACY_BIZ=$(grep -rnE "(doc_qa_embedding_v1|doc_qa_rerank_v1)" "${ACTIVE_DOCS[@]}" 2>/dev/null || true)
if [ -n "${LEGACY_BIZ}" ]; then
  echo "❌ Found deprecated business names in active docs, assets or codebase:"
  echo "${LEGACY_BIZ}"
  FAILED=1
else
  echo "✅ No legacy business names found."
fi

# 2. 检查旧注册宏 (REGISTER_NODE( / REGISTER_ENGINE( / REGISTER_ENGINE_WITH_DEFINITION("str", ...))
echo "[Check 2/7] Checking for deprecated registration macros..."
LEGACY_NODE_MACROS=$(grep -rnE "\bREGISTER_NODE\([A-Za-z0-9_]+\)" "${ACTIVE_DOCS[@]}" 2>/dev/null | grep -v "#define REGISTER_NODE" || true)
LEGACY_ENGINE_MACROS=$(grep -rnE "\bREGISTER_ENGINE\([A-Za-z0-9_]+,\s*[A-Za-z0-9_]+\)" "${ACTIVE_DOCS[@]}" 2>/dev/null | grep -v "#define REGISTER_ENGINE" || true)
LEGACY_3PARAM_MACROS=$(grep -rnE "\bREGISTER_ENGINE_WITH_DEFINITION\s*\(\s*\"" "${ACTIVE_DOCS[@]}" 2>/dev/null || true)

if [ -n "${LEGACY_NODE_MACROS}" ] || [ -n "${LEGACY_ENGINE_MACROS}" ] || [ -n "${LEGACY_3PARAM_MACROS}" ]; then
  echo "❌ Found deprecated registration macro invocations:"
  [ -n "${LEGACY_NODE_MACROS}" ] && echo "${LEGACY_NODE_MACROS}"
  [ -n "${LEGACY_ENGINE_MACROS}" ] && echo "${LEGACY_ENGINE_MACROS}"
  [ -n "${LEGACY_3PARAM_MACROS}" ] && echo "${LEGACY_3PARAM_MACROS}"
  FAILED=1
else
  echo "✅ No deprecated registration macro invocations found in active docs/source."
fi

# 3. 检查虚构生产节点 (PassthroughNode, ComplianceReportPostNode)
echo "[Check 3/7] Checking for fictitious production nodes..."
FICTITIOUS_NODES=$(grep -rnE "\b(PassthroughNode|ComplianceReportPostNode)\b" "${ACTIVE_DOCS[@]}" 2>/dev/null || true)
if [ -n "${FICTITIOUS_NODES}" ]; then
  echo "❌ Found fictitious production nodes in active docs or codebase:"
  echo "${FICTITIOUS_NODES}"
  FAILED=1
else
  echo "✅ No fictitious production nodes found."
fi

# 4. 检查当前治理入口是否引用已移除的 Engine / Biz Node 架构。
echo "[Check 4/7] Checking active governance for removed architecture identifiers..."
REMOVED_ARCH=$(grep -rnE \
  '(IModelEngine|include/engine/engine_interface\.h|REGISTER_ENGINE_WITH_DEFINITION|src/business/|src/biz/|26 production nodes)' \
  "${ACTIVE_DOCS[@]}" 2>/dev/null || true)
if [ -n "${REMOVED_ARCH}" ]; then
  echo "❌ Found removed architecture identifiers in active governance/docs:"
  echo "${REMOVED_ARCH}"
  FAILED=1
else
  echo "✅ Active governance matches the Model/Backend and Common Node architecture."
fi

# 5. 检查架构文档核心概念完备性 (ValidatedPipelinePlan, BlackboardKey, NodeBase, FixedBatchExecutor)
echo "[Check 5/7] Verifying core architectural concepts in architecture documents..."
for concept in "ValidatedPipelinePlan" "BlackboardKey" "NodeBase" "FixedBatchExecutor"; do
  if ! grep -q "${concept}" "${DOC_ROOT}/architecture.md"; then
    echo "❌ doc/architecture.md is missing reference to '${concept}'"
    FAILED=1
  fi
  if ! grep -q "${concept}" "${DOC_ROOT}/developer_guide.md"; then
    echo "❌ doc/developer_guide.md is missing reference to '${concept}'"
    FAILED=1
  fi
done
for concept in "SharedAlgorithmRuntime" "Pipeline" "AlgContext" "NodeBase" "FixedBatchExecutor"; do
  if ! grep -q "${concept}" "${DOC_ROOT}/architecture.puml"; then
    echo "❌ doc/architecture.puml is missing reference to '${concept}'"
    FAILED=1
  fi
done
if [ ${FAILED} -eq 0 ]; then
  echo "✅ All core architectural concepts verified in architecture docs."
fi

# 6. 检查 architecture_v2.puml 状态图例
echo "[Check 6/7] Checking architecture_v2.puml state legends..."
if ! grep -q "Implemented" "${DOC_ROOT}/architecture_v2.puml" || \
   ! grep -q "Partial" "${DOC_ROOT}/architecture_v2.puml" || \
   ! grep -q "Planned" "${DOC_ROOT}/architecture_v2.puml"; then
  echo "❌ doc/architecture_v2.puml is missing Implemented / Partial / Planned status legends"
  FAILED=1
else
  echo "✅ architecture_v2.puml state legends verified."
fi

# 7. 检查 PlantUML 与 SVG 资产存在性与非空
echo "[Check 7/7] Verifying architecture diagrams exist and are non-empty..."
for puml in "${DOC_ROOT}/architecture.puml" "${DOC_ROOT}/architecture_v2.puml"; do
  if [ ! -s "${puml}" ]; then
    echo "❌ PlantUML file '${puml}' is missing or empty"
    FAILED=1
  fi
done
for svg in "${DOC_ROOT}/assets/architecture_class_diagram.svg" "${DOC_ROOT}/assets/architecture_flow.svg"; do
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
