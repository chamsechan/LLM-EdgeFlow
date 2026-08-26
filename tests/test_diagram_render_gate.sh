#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

echo "================================================================"
echo " [Self-Test] Testing Diagram Render Gate Failure Interceptions"
echo "================================================================"

TMP_DOC_DIR=$(mktemp -d)
trap 'rm -rf "${TMP_DOC_DIR}"' EXIT

# 1. 验证正常状态下返回 0
./scripts/render_architecture_diagrams.sh --check >/dev/null 2>&1 || {
  echo "❌ Baseline diagram check failed unexpectedly"
  exit 1
}

# 2. 负向测试：备份并在临时副本上测试 syntax error
cp doc/architecture.puml "${TMP_DOC_DIR}/architecture.puml.bak"
cat << 'EOF' > doc/architecture.puml
@startuml
class InvalidSyntax {
  unclosed brace
EOF

if ./scripts/render_architecture_diagrams.sh --check >/dev/null 2>&1; then
  cp "${TMP_DOC_DIR}/architecture.puml.bak" doc/architecture.puml
  echo "❌ Render gate failed to catch PlantUML syntax error!"
  exit 1
fi
cp "${TMP_DOC_DIR}/architecture.puml.bak" doc/architecture.puml
echo "✅ Negative test passed: PlantUML syntax error caught."

# 3. 负向测试：损坏 SVG 资产
cp doc/assets/architecture_class_diagram.svg "${TMP_DOC_DIR}/class_svg.bak"
echo "corrupted svg" > doc/assets/architecture_class_diagram.svg

if ./scripts/render_architecture_diagrams.sh --check >/dev/null 2>&1; then
  cp "${TMP_DOC_DIR}/class_svg.bak" doc/assets/architecture_class_diagram.svg
  echo "❌ Render gate failed to catch corrupted SVG!"
  exit 1
fi
cp "${TMP_DOC_DIR}/class_svg.bak" doc/assets/architecture_class_diagram.svg
echo "✅ Negative test passed: Corrupted SVG asset caught."

echo "✅ All diagram render gate negative self-tests PASSED."
exit 0
