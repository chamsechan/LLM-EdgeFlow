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
cp "$root/doc/assets/framework_overview.svg" "$workspace/doc/assets/"
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
rm "$workspace/doc/assets/framework_overview.svg"
expect_failure 'Missing or empty framework overview' run --check
touch "$workspace/doc/assets/framework_overview.svg"
expect_failure 'Missing or empty framework overview' run --check
cp "$root/doc/assets/framework_overview.svg" "$workspace/doc/assets/"
# 用最小的 Java 传输桩证明被拒绝的缓存字节不会进入渲染。
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
# 正向检查和过期来源检查都使用实际固定版本的渲染器，而不是伪造的 SVG。
run --check >"$workspace/output"
cp "$workspace/doc/assets/architecture_class_diagram.svg" "$workspace/committed.svg"
printf '\n\x27 source comment changed after asset generation\n' >> "$workspace/doc/architecture_classes.puml"
expect_failure 'SVG source provenance is stale' run --check
cmp "$workspace/committed.svg" "$workspace/doc/assets/architecture_class_diagram.svg"
run --generate >"$workspace/output"
run --check >"$workspace/output"
# 渲染进程的错误会向上传递，已提交的资源保持不变。
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
