#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "${ROOT_DIR}"

echo "================================================================"
echo " [Self-Test] Testing Architecture Docs Drift Gate Failures"
echo "================================================================"

TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/edgeflow-doc-drift.XXXXXX")"
cleanup() {
  rm -rf "${TMP_ROOT}"
}
trap cleanup EXIT INT TERM
cp -R doc "${TMP_ROOT}/doc"
FIXTURE_DOC_ROOT="${TMP_ROOT}/doc"

run_fixture_gate() {
  LLM_EDGEFLOW_ARCH_DOC_ROOT="${FIXTURE_DOC_ROOT}" \
    ./scripts/check_architecture_docs.sh >/dev/null 2>&1
}

./scripts/check_architecture_docs.sh >/dev/null 2>&1

echo "<!-- doc_qa_embedding_v1 -->" >> \
  "${FIXTURE_DOC_ROOT}/assets/architecture_flow.svg"
if run_fixture_gate; then
  echo "❌ Docs drift gate missed a legacy business name in SVG"
  exit 1
fi
cp doc/assets/architecture_flow.svg \
  "${FIXTURE_DOC_ROOT}/assets/architecture_flow.svg"

echo "REGISTER_NODE(OldNode);" >> "${FIXTURE_DOC_ROOT}/developer_guide.md"
if run_fixture_gate; then
  echo "❌ Docs drift gate missed a deprecated registration macro"
  exit 1
fi
cp doc/developer_guide.md "${FIXTURE_DOC_ROOT}/developer_guide.md"

echo "PassthroughNode" >> "${FIXTURE_DOC_ROOT}/developer_guide.md"
if run_fixture_gate; then
  echo "❌ Docs drift gate missed a fictitious production node"
  exit 1
fi
cp doc/developer_guide.md "${FIXTURE_DOC_ROOT}/developer_guide.md"

echo "IModelEngine" >> "${FIXTURE_DOC_ROOT}/developer_guide.md"
if run_fixture_gate; then
  echo "❌ Docs drift gate missed a removed architecture identifier"
  exit 1
fi
cp doc/developer_guide.md "${FIXTURE_DOC_ROOT}/developer_guide.md"

sed -i 's/10\.0\.0/99.0.0/g' \
  "${FIXTURE_DOC_ROOT}/architecture.md"
if run_fixture_gate; then
  echo "❌ Docs drift gate missed a stale product version"
  exit 1
fi

echo "✅ Architecture docs drift gate negative self-tests passed."
