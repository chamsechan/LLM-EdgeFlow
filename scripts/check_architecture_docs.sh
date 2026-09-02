#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"
DOC_ROOT="${LLM_EDGEFLOW_ARCH_DOC_ROOT:-${ROOT_DIR}/doc}"

echo "================================================================"
echo " [Architecture Docs Drift Check] Validating Architecture SSOT"
echo "================================================================"

FAILED=0

report_matches() {
  local matches="$1"
  local failure_message="$2"
  local success_message="$3"
  if [[ -z "${matches}" ]]; then
    echo "✅ ${success_message}"
    return
  fi
  echo "❌ ${failure_message}"
  echo "${matches}"
  FAILED=1
}

require_concepts() {
  local document="$1"
  local concept
  shift
  for concept in "$@"; do
    if ! grep -q "${concept}" "${document}"; then
      echo "❌ ${document#"${ROOT_DIR}/"} is missing reference to '${concept}'"
      FAILED=1
    fi
  done
}

find_deprecated_registration_macros() {
  grep -rnE "\bREGISTER_NODE\([A-Za-z0-9_]+\)" \
    "${ACTIVE_DOCS[@]}" 2>/dev/null | grep -v "#define REGISTER_NODE" || true
  grep -rnE "\bREGISTER_ENGINE\([A-Za-z0-9_]+,[[:space:]]*[A-Za-z0-9_]+\)" \
    "${ACTIVE_DOCS[@]}" 2>/dev/null | grep -v "#define REGISTER_ENGINE" || true
  grep -rnE '\bREGISTER_ENGINE_WITH_DEFINITION[[:space:]]*\([[:space:]]*"' \
    "${ACTIVE_DOCS[@]}" 2>/dev/null || true
}

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
echo "[Check 1/8] Checking for legacy business names (doc_qa_embedding_v1, doc_qa_rerank_v1)..."
LEGACY_BIZ=$(grep -rnE "(doc_qa_embedding_v1|doc_qa_rerank_v1)" "${ACTIVE_DOCS[@]}" 2>/dev/null || true)
report_matches "${LEGACY_BIZ}" \
  "Found deprecated business names in active docs, assets or codebase:" \
  "No legacy business names found."

# 2. 检查旧注册宏 (REGISTER_NODE( / REGISTER_ENGINE( / REGISTER_ENGINE_WITH_DEFINITION("str", ...))
echo "[Check 2/8] Checking for deprecated registration macros..."
LEGACY_MACROS="$(find_deprecated_registration_macros)"
report_matches "${LEGACY_MACROS}" \
  "Found deprecated registration macro invocations:" \
  "No deprecated registration macro invocations found in active docs/source."

# 3. 检查虚构生产节点 (PassthroughNode, ComplianceReportPostNode)
echo "[Check 3/8] Checking for fictitious production nodes..."
FICTITIOUS_NODES=$(grep -rnE "\b(PassthroughNode|ComplianceReportPostNode)\b" "${ACTIVE_DOCS[@]}" 2>/dev/null || true)
report_matches "${FICTITIOUS_NODES}" \
  "Found fictitious production nodes in active docs or codebase:" \
  "No fictitious production nodes found."

# 4. 检查当前治理入口是否引用已移除的 Engine / Biz Node 架构。
echo "[Check 4/8] Checking active governance for removed architecture identifiers..."
REMOVED_ARCH=$(grep -rnE \
  '(IModelEngine|include/engine/engine_interface\.h|REGISTER_ENGINE_WITH_DEFINITION|src/business/|src/biz/|26 production nodes)' \
  "${ACTIVE_DOCS[@]}" 2>/dev/null || true)
report_matches "${REMOVED_ARCH}" \
  "Found removed architecture identifiers in active governance/docs:" \
  "Active governance matches the Model/Backend and Common Node architecture."

# 5. 检查架构文档核心概念完备性 (ValidatedPipelinePlan, BlackboardKey, NodeBase, FixedBatchExecutor)
echo "[Check 5/8] Verifying core architectural concepts in architecture documents..."
require_concepts "${DOC_ROOT}/architecture.md" \
  "ValidatedPipelinePlan" "BlackboardKey" "NodeBase" "FixedBatchExecutor"
require_concepts "${DOC_ROOT}/developer_guide.md" \
  "ValidatedPipelinePlan" "BlackboardKey" "NodeBase" "FixedBatchExecutor"
require_concepts "${DOC_ROOT}/architecture.puml" \
  "SharedAlgorithmRuntime" "Pipeline" "AlgContext" "NodeBase" "FixedBatchExecutor"
if [ ${FAILED} -eq 0 ]; then
  echo "✅ All core architectural concepts verified in architecture docs."
fi

# 6. 检查 architecture_v2.puml 状态图例
echo "[Check 6/8] Checking architecture_v2.puml state legends..."
if ! grep -q "Implemented" "${DOC_ROOT}/architecture_v2.puml" || \
   ! grep -q "Partial" "${DOC_ROOT}/architecture_v2.puml" || \
   ! grep -q "Planned" "${DOC_ROOT}/architecture_v2.puml"; then
  echo "❌ doc/architecture_v2.puml is missing Implemented / Partial / Planned status legends"
  FAILED=1
else
  echo "✅ architecture_v2.puml state legends verified."
fi

# 7. 检查 PlantUML 与 SVG 资产存在性与非空
echo "[Check 7/8] Verifying architecture diagrams exist and are non-empty..."
for diagram in \
  "${DOC_ROOT}/architecture.puml" \
  "${DOC_ROOT}/architecture_v2.puml" \
  "${DOC_ROOT}/assets/architecture_class_diagram.svg" \
  "${DOC_ROOT}/assets/architecture_flow.svg"; do
  if [[ ! -s "${diagram}" ]]; then
    echo "❌ Architecture diagram '${diagram}' is missing or empty"
    FAILED=1
  fi
done

# 8. 检查 CMake、生成版本头和活跃文档是否共享同一产品/ABI 版本。
echo "[Check 8/8] Verifying product and ABI version single source of truth..."
PRODUCT_VERSION="$({
  sed -nE 's/^project\(LLMEdgeFlow VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES C CXX\)$/\1/p' \
    "${ROOT_DIR}/CMakeLists.txt"
} | head -n 1)"
ABI_VERSION="$({
  sed -nE 's/^set\(LLM_EDGEFLOW_ABI_VERSION "([0-9]+\.[0-9]+\.[0-9]+)"\)$/\1/p' \
    "${ROOT_DIR}/CMakeLists.txt"
} | head -n 1)"
ABI_MAJOR="$({
  sed -nE 's/^set\(LLM_EDGEFLOW_ABI_VERSION_MAJOR ([0-9]+)\)$/\1/p' \
    "${ROOT_DIR}/CMakeLists.txt"
} | head -n 1)"

if [[ -z "${PRODUCT_VERSION}" || -z "${ABI_VERSION}" || -z "${ABI_MAJOR}" ]]; then
  echo "❌ Unable to derive product and ABI versions from CMakeLists.txt"
  FAILED=1
elif [[ "${ABI_VERSION%%.*}" != "${ABI_MAJOR}" ]]; then
  echo "❌ ABI version '${ABI_VERSION}' and ABI major '${ABI_MAJOR}' disagree"
  FAILED=1
fi

VERSION_TEMPLATE="${ROOT_DIR}/cmake/company_alg_version.h.in"
VERSION_SCRIPT_TEMPLATE="${ROOT_DIR}/cmake/company_alg_sdk.map.in"
PUBLIC_INTERFACE="${ROOT_DIR}/include/company_alg_interface.h"
if ! grep -Fq '#include "company_alg_version.h"' "${PUBLIC_INTERFACE}"; then
  echo "❌ Public C interface does not include the generated version header"
  FAILED=1
fi
if grep -Eq '^#define COMPANY_ALG_(PRODUCT_VERSION|ABI_VERSION|ABI_VERSION_MAJOR)' \
    "${PUBLIC_INTERFACE}"; then
  echo "❌ Public C interface contains a duplicate hard-coded version definition"
  FAILED=1
fi
for placeholder in \
  '@PROJECT_VERSION@' \
  '@LLM_EDGEFLOW_ABI_VERSION@' \
  '@LLM_EDGEFLOW_ABI_VERSION_MAJOR@'; do
  if ! grep -Fq "${placeholder}" "${VERSION_TEMPLATE}"; then
    echo "❌ Generated version header template is missing '${placeholder}'"
    FAILED=1
  fi
done
if ! grep -Fq 'LLM_EDGEFLOW_@LLM_EDGEFLOW_ABI_VERSION_MAJOR@ {' \
    "${VERSION_SCRIPT_TEMPLATE}"; then
  echo "❌ SDK version script does not derive its version node from the ABI major"
  FAILED=1
fi
if ! grep -Fq 'SOVERSION ${LLM_EDGEFLOW_ABI_VERSION_MAJOR}' \
    "${ROOT_DIR}/CMakeLists.txt"; then
  echo "❌ alg_sdk SOVERSION is not derived from LLM_EDGEFLOW_ABI_VERSION_MAJOR"
  FAILED=1
fi

VERSION_DOCS=(
  "${ROOT_DIR}/README.md"
  "${DOC_ROOT}/architecture.md"
  "${DOC_ROOT}/developer_guide.md"
  "${DOC_ROOT}/CHANGELOG.md"
)
if [[ -n "${PRODUCT_VERSION}" && -n "${ABI_MAJOR}" ]]; then
  for document in "${VERSION_DOCS[@]}"; do
    if ! grep -Fq "${PRODUCT_VERSION}" "${document}"; then
      echo "❌ ${document} does not name current product version ${PRODUCT_VERSION}"
      FAILED=1
    fi
    if ! grep -Eq "ABI( major)?[^0-9]{0,16}${ABI_MAJOR}([^0-9]|$)" \
        "${document}"; then
      echo "❌ ${document} does not name current ABI major ${ABI_MAJOR}"
      FAILED=1
    fi
  done
fi

if [ ${FAILED} -eq 0 ]; then
  echo "✅ Product ${PRODUCT_VERSION} and ABI ${ABI_VERSION} version facts verified."
fi

if [ ${FAILED} -ne 0 ]; then
  echo "❌ Architecture docs drift check FAILED."
  exit 1
fi

echo "✅ All architecture documentation drift checks PASSED."
exit 0
