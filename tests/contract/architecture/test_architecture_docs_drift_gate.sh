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

echo "REGISTER_NODE(OldTutorialNode);" >> \
  "${FIXTURE_DOC_ROOT}/dev_guide/first_custom_node.md"
if run_fixture_gate; then
  echo "❌ Docs drift gate missed a deprecated registration macro in a tutorial"
  exit 1
fi
cp doc/dev_guide/first_custom_node.md \
  "${FIXTURE_DOC_ROOT}/dev_guide/first_custom_node.md"

rm "${FIXTURE_DOC_ROOT}/assets/framework_overview.svg"
if run_fixture_gate; then
  echo "❌ Docs drift gate missed the missing README overview SVG"
  exit 1
fi
cp doc/assets/framework_overview.svg \
  "${FIXTURE_DOC_ROOT}/assets/framework_overview.svg"

for removed_api in \
  Alg_Init Alg_Create Alg_Process Alg_Control Alg_Destroy Alg_DeInit \
  "C ABI / Operator" "C ABI 或 Operator"; do
  echo "<!-- ${removed_api} -->" >> \
    "${FIXTURE_DOC_ROOT}/assets/framework_overview.svg"
  if run_fixture_gate; then
    echo "❌ Docs drift gate missed removed C ABI access '${removed_api}' in the README SVG"
    exit 1
  fi
  cp doc/assets/framework_overview.svg \
    "${FIXTURE_DOC_ROOT}/assets/framework_overview.svg"
done

sed -i.bak 's/ValidatedIoPlan/ObsoleteIoPlan/g' \
  "${FIXTURE_DOC_ROOT}/architecture_v2.puml"
rm -f "${FIXTURE_DOC_ROOT}/architecture_v2.puml.bak"
if run_fixture_gate; then
  echo "❌ Docs drift gate missed a missing current deployment plan concept"
  exit 1
fi
cp doc/architecture_v2.puml "${FIXTURE_DOC_ROOT}/architecture_v2.puml"

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

CURRENT_PRODUCT_VERSION="$(sed -nE 's/^project\(LLMEdgeFlow VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES C CXX\)$/\1/p' CMakeLists.txt)"
if [[ -z "${CURRENT_PRODUCT_VERSION}" ]]; then
  echo "❌ Failed to parse project VERSION from CMakeLists.txt"
  exit 1
fi
if ! grep -Fq "${CURRENT_PRODUCT_VERSION}" "${FIXTURE_DOC_ROOT}/architecture.md"; then
  echo "❌ Expected architecture.md fixture to contain current product version ${CURRENT_PRODUCT_VERSION}"
  exit 1
fi
sed -i.bak "s/${CURRENT_PRODUCT_VERSION//./\\.}/99.0.0/g" \
  "${FIXTURE_DOC_ROOT}/architecture.md"
rm -f "${FIXTURE_DOC_ROOT}/architecture.md.bak"
if grep -Fq "${CURRENT_PRODUCT_VERSION}" "${FIXTURE_DOC_ROOT}/architecture.md"; then
  echo "❌ Failed to mutate product version in architecture.md fixture"
  exit 1
fi
if run_fixture_gate; then
  echo "❌ Docs drift gate missed a stale product version"
  exit 1
fi

echo "✅ Architecture docs drift gate negative self-tests passed."
