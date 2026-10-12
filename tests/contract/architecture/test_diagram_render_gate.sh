#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
workspace=$(mktemp -d "${TMPDIR:-/tmp}/diagram-consumer.XXXXXX")
trap 'rm -rf "$workspace"' EXIT
mkdir -p "$workspace/doc/assets" "$workspace/bad-cache"
cp "$root/doc/architecture_classes.puml" "$workspace/doc/"
cp "$root/doc/architecture_flow.puml" "$workspace/doc/"
cp "$root/doc/assets/architecture_class_diagram.svg" "$workspace/doc/assets/"
cp "$root/doc/assets/architecture_flow.svg" "$workspace/doc/assets/"
renderer="$root/scripts/render_architecture_diagrams.sh"
cache="${LLM_EDGEFLOW_TOOL_CACHE_DIR:-$root/build/tool-cache}"
run() {
  LLM_EDGEFLOW_ARCH_DOC_ROOT="$workspace/doc" LLM_EDGEFLOW_TOOL_CACHE_DIR="$cache" bash "$renderer" "$@"
}
expect_failure() {
  local message=$1
  shift
  if "$@" >"$workspace/output" 2>&1; then
    echo "FAIL: diagram failure reported success: $message" >&2
    exit 1
  fi
  grep -Fq "$message" "$workspace/output" || { cat "$workspace/output" >&2; exit 1; }
}
expect_failure 'Usage:' run --unsupported
# A narrow Java transport stub proves rejected cache bytes never reach rendering.
cat > "$workspace/java-version-only" <<'JAVA'
#!/usr/bin/env bash
if [[ "${1:-}" == '-version' ]]; then exit 0; fi
printf 'unexpected render invocation\n' > "$JAVA_TRACE"
exit 19
JAVA
chmod +x "$workspace/java-version-only"
printf 'unverified bytes' > "$workspace/bad-cache/plantuml-1.2024.7.jar"
expect_failure 'checksum mismatch' env LLM_EDGEFLOW_JAVA_BIN="$workspace/java-version-only" \
  JAVA_TRACE="$workspace/java-trace" LLM_EDGEFLOW_TOOL_CACHE_DIR="$workspace/bad-cache" \
  LLM_EDGEFLOW_ARCH_DOC_ROOT="$workspace/doc" bash "$renderer" --check
[[ ! -e "$workspace/java-trace" ]]
# Positive and stale-provenance checks use the actual pinned renderer, not fake SVGs.
run --check >"$workspace/output"
cp "$workspace/doc/assets/architecture_class_diagram.svg" "$workspace/committed.svg"
printf '\n\x27 source comment changed after asset generation\n' >> "$workspace/doc/architecture_classes.puml"
expect_failure 'SVG source provenance is stale' run --check
cmp "$workspace/committed.svg" "$workspace/doc/assets/architecture_class_diagram.svg"
run --generate >"$workspace/output"
run --check >"$workspace/output"
# Renderer process errors propagate and leave committed assets unchanged.
cp "$workspace/doc/assets/architecture_class_diagram.svg" "$workspace/committed.svg"
if LLM_EDGEFLOW_JAVA_BIN="$workspace/java-version-only" JAVA_TRACE="$workspace/java-trace" \
    LLM_EDGEFLOW_TOOL_CACHE_DIR="$cache" LLM_EDGEFLOW_ARCH_DOC_ROOT="$workspace/doc" \
    bash "$renderer" --generate >"$workspace/output" 2>&1; then
  echo 'FAIL: renderer process error reported success' >&2
  exit 1
fi
[[ -s "$workspace/java-trace" ]]
cmp "$workspace/committed.svg" "$workspace/doc/assets/architecture_class_diagram.svg"
echo 'PASS actual diagram rendering, provenance and dependency failure propagation'
