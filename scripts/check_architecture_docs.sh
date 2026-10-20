#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"
DOC_ROOT="${LLM_EDGEFLOW_ARCH_DOC_ROOT:-${ROOT_DIR}/doc}"

echo "================================================================"
echo " [Architecture Docs Drift Check] Validating Architecture SSOT"
echo "================================================================"

FAILED=0

# 检查 PlantUML 与 SVG 资产存在性与非空
echo "Verifying architecture diagrams exist and are non-empty..."
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

if [ ${FAILED} -ne 0 ]; then
  echo "❌ Architecture docs drift check FAILED."
  exit 1
fi

echo "✅ All architecture documentation drift checks PASSED."
exit 0
