#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

echo "================================================================"
echo " [Self-Test] Testing Architecture Docs Drift Gate Failures"
echo "================================================================"

TMP_DOC_DIR=$(mktemp -d)
trap 'rm -rf "${TMP_DOC_DIR}"' EXIT

# 1. 正常 baseline
./scripts/check_architecture_docs.sh >/dev/null 2>&1 || {
  echo "❌ Baseline architecture docs check failed unexpectedly"
  exit 1
}

# 2. 负向测试：向 SVG 注入旧业务名
cp doc/assets/architecture_flow.svg "${TMP_DOC_DIR}/flow.svg.bak"
echo "<!-- doc_qa_embedding_v1 -->" >> doc/assets/architecture_flow.svg

if ./scripts/check_architecture_docs.sh >/dev/null 2>&1; then
  cp "${TMP_DOC_DIR}/flow.svg.bak" doc/assets/architecture_flow.svg
  echo "❌ Docs drift gate failed to catch legacy business name in SVG!"
  exit 1
fi
cp "${TMP_DOC_DIR}/flow.svg.bak" doc/assets/architecture_flow.svg
echo "✅ Negative test passed: Legacy business name in SVG caught."

# 3. 负向测试：注入旧宏
cp doc/developer_guide.md "${TMP_DOC_DIR}/dev_guide.md.bak"
echo "REGISTER_NODE(OldNode);" >> doc/developer_guide.md

if ./scripts/check_architecture_docs.sh >/dev/null 2>&1; then
  cp "${TMP_DOC_DIR}/dev_guide.md.bak" doc/developer_guide.md
  echo "❌ Docs drift gate failed to catch deprecated REGISTER_NODE!"
  exit 1
fi
cp "${TMP_DOC_DIR}/dev_guide.md.bak" doc/developer_guide.md
echo "✅ Negative test passed: Deprecated REGISTER_NODE caught."

# 4. 负向测试：注入虚构节点
echo "PassthroughNode" >> doc/developer_guide.md
if ./scripts/check_architecture_docs.sh >/dev/null 2>&1; then
  cp "${TMP_DOC_DIR}/dev_guide.md.bak" doc/developer_guide.md
  echo "❌ Docs drift gate failed to catch fictitious node!"
  exit 1
fi
cp "${TMP_DOC_DIR}/dev_guide.md.bak" doc/developer_guide.md
echo "✅ Negative test passed: Fictitious node caught."

echo "✅ All architecture docs drift gate negative self-tests PASSED."
exit 0
