#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "${ROOT_DIR}"

echo "================================================================"
echo " [Self-Test] Testing Diagram Render Gate Failure Interceptions"
echo "================================================================"

TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/edgeflow-diagram-gate.XXXXXX")"
cleanup() {
  rm -rf "${TMP_ROOT}"
}
trap cleanup EXIT INT TERM
cp -R doc "${TMP_ROOT}/doc"
FIXTURE_DOC_ROOT="${TMP_ROOT}/doc"

run_fixture_renderer() {
  LLM_EDGEFLOW_ARCH_DOC_ROOT="${FIXTURE_DOC_ROOT}" \
    ./scripts/render_architecture_diagrams.sh "$@" >/dev/null 2>&1
}

./scripts/render_architecture_diagrams.sh --check >/dev/null 2>&1

# 语法合法的源文件改动必须使已提交的资源过期。
if sed --version >/dev/null 2>&1; then
  sed -i '/@enduml/i class SourceDriftProbe' \
    "${FIXTURE_DOC_ROOT}/architecture_classes.puml"
else
  sed -i '' '/@enduml/i\
class SourceDriftProbe
' "${FIXTURE_DOC_ROOT}/architecture_classes.puml"
fi
if run_fixture_renderer --check; then
  echo "❌ Render gate missed source/asset drift"
  exit 1
fi
cp doc/architecture_classes.puml "${FIXTURE_DOC_ROOT}/architecture_classes.puml"

# 已提交的资源与生成结果不一致时必须失败。
echo "corrupted svg" > \
  "${FIXTURE_DOC_ROOT}/assets/architecture_class_diagram.svg"
if run_fixture_renderer --check; then
  echo "❌ Render gate missed a corrupted SVG asset"
  exit 1
fi

# generate 只能修复临时夹具，之后 check 必须通过。
run_fixture_renderer --generate
run_fixture_renderer --check

echo "✅ Diagram generate/check and negative self-tests passed."
