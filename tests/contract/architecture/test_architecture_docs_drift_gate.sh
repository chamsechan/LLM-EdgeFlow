#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
workspace=$(mktemp -d "${TMPDIR:-/tmp}/architecture-docs-consumer.XXXXXX")
trap 'rm -rf "$workspace"' EXIT
mkdir -p "$workspace/doc/assets"
for document in architecture_classes.puml architecture_flow.puml; do
  cp "$root/doc/$document" "$workspace/doc/$document"
done
cp "$root/doc/assets/architecture_class_diagram.svg" "$workspace/doc/assets/"
cp "$root/doc/assets/architecture_flow.svg" "$workspace/doc/assets/"
cp "$root/doc/assets/framework_overview.svg" "$workspace/doc/assets/"
check() {
  LLM_EDGEFLOW_ARCH_DOC_ROOT="$workspace/doc" bash "$root/scripts/check_architecture_docs.sh"
}
expect_failure() {
  local expected=$1
  if check >"$workspace/output" 2>&1; then
    echo "FAIL: documentation drift passed: $expected" >&2
    exit 1
  fi
  if ! grep -Fq "$expected" "$workspace/output"; then
    cat "$workspace/output" >&2
    exit 1
  fi
}
check >"$workspace/output"
rm "$workspace/doc/assets/framework_overview.svg"
expect_failure 'missing or empty'
cp "$root/doc/assets/framework_overview.svg" "$workspace/doc/assets/"
echo 'PASS documentation gate detects missing assets'
