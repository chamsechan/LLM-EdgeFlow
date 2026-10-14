#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"
DOC_ROOT="${LLM_EDGEFLOW_ARCH_DOC_ROOT:-${ROOT_DIR}/doc}"

echo "================================================================"
echo " [Architecture Docs Drift Check] Validating Architecture SSOT"
echo "================================================================"

FAILED=0

# 1. 检查 PlantUML 与 SVG 资产存在性与非空
echo "[Check 1/2] Verifying architecture diagrams exist and are non-empty..."
for diagram in \
  "${DOC_ROOT}/architecture_classes.puml" \
  "${DOC_ROOT}/architecture_flow.puml" \
  "${DOC_ROOT}/assets/architecture_class_diagram.svg" \
  "${DOC_ROOT}/assets/architecture_flow.svg" \
  "${DOC_ROOT}/assets/framework_overview.svg"; do
  if [[ ! -s "${diagram}" ]]; then
    echo "❌ Architecture diagram '${diagram}' is missing or empty"
    FAILED=1
  fi
done

# 2. 检查 CMake、生成版本头和活跃文档是否共享同一产品版本。
echo "[Check 2/2] Verifying product version single source of truth..."
PRODUCT_VERSION="$({
  sed -nE 's/^project\(LLMEdgeFlow VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES C CXX\)$/\1/p' \
    "${ROOT_DIR}/CMakeLists.txt"
} | head -n 1)"

if [[ -z "${PRODUCT_VERSION}" ]]; then
  echo "❌ Unable to derive the product version from CMakeLists.txt"
  FAILED=1
fi

VERSION_TEMPLATE="${ROOT_DIR}/cmake_ext/edgeflow_version.h.in"
PUBLIC_INTERFACE="${ROOT_DIR}/include/edgeflow/operator/interface.h"
if ! grep -Fq '#include "edgeflow/version.h"' "${PUBLIC_INTERFACE}"; then
  echo "❌ Public Operator interface does not include the generated version header"
  FAILED=1
fi
if grep -Eq '^#define COMPANY_ALG_PRODUCT_VERSION' \
    "${PUBLIC_INTERFACE}"; then
  echo "❌ Public Operator interface contains a duplicate hard-coded version definition"
  FAILED=1
fi
if ! grep -Fq '@PROJECT_VERSION@' "${VERSION_TEMPLATE}"; then
  echo "❌ Generated version header template is missing '@PROJECT_VERSION@'"
  FAILED=1
fi

VERSION_DOCS=(
  "${ROOT_DIR}/README.md"
  "${DOC_ROOT}/architecture.md"
  "${DOC_ROOT}/developer_guide.md"
  "${DOC_ROOT}/CHANGELOG.md"
)
if [[ -n "${PRODUCT_VERSION}" ]]; then
  for document in "${VERSION_DOCS[@]}"; do
    if ! grep -Fq "${PRODUCT_VERSION}" "${document}"; then
      echo "❌ ${document} does not name current product version ${PRODUCT_VERSION}"
      FAILED=1
    fi
  done
fi

if [ ${FAILED} -eq 0 ]; then
  echo "✅ Product version ${PRODUCT_VERSION} facts verified."
fi

if [ ${FAILED} -ne 0 ]; then
  echo "❌ Architecture docs drift check FAILED."
  exit 1
fi

echo "✅ All architecture documentation drift checks PASSED."
exit 0
