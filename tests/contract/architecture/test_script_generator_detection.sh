#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
workspace=$(mktemp -d "${TMPDIR:-/tmp}/generator-consumer.XXXXXX")
trap 'rm -rf "$workspace"' EXIT
mkdir -p "$workspace/bin" "$workspace/build"
cat > "$workspace/bin/cmake" <<'CMAKE'
#!/usr/bin/env bash
printf '%s\n' "$@" > "$CALL_TRACE"
exit "${CMAKE_STATUS:-0}"
CMAKE
chmod +x "$workspace/bin/cmake"
invoke() {
  PATH="$workspace/bin:$PATH" CALL_TRACE="$workspace/arguments" \
    bash "$root/scripts/configure_build.sh" "$root" "$workspace/build" dev-gate -DUSER_OPTION=literal
}
printf 'CMAKE_GENERATOR:INTERNAL=Unix Makefiles\n' > "$workspace/build/CMakeCache.txt"
invoke
python3 - "$workspace/arguments" "$root" "$workspace/build" <<'PY'
from pathlib import Path
import sys
args = Path(sys.argv[1]).read_text().splitlines()
assert args == ['--preset=dev-gate', '-S', sys.argv[2], '-B', sys.argv[3],
                '-G', 'Unix Makefiles', '-DUSER_OPTION=literal'], args
PY
printf 'CMAKE_GENERATOR:INTERNAL=Ninja Multi-Config\n' > "$workspace/build/CMakeCache.txt"
invoke
grep -Fxq 'Ninja Multi-Config' "$workspace/arguments"
printf 'unrelated cache metadata\n' > "$workspace/build/CMakeCache.txt"
rm "$workspace/arguments"
if invoke >"$workspace/output" 2>&1; then
  echo 'FAIL: missing cached generator was accepted' >&2
  exit 1
fi
[[ ! -e "$workspace/arguments" ]]
grep -Fq 'Missing CMake generator' "$workspace/output"
rm "$workspace/build/CMakeCache.txt"
invoke
if command -v ninja >/dev/null 2>&1; then expected=Ninja; else expected='Unix Makefiles'; fi
grep -Fxq "$expected" "$workspace/arguments"
if CMAKE_STATUS=29 invoke; then
  echo 'FAIL: configure process failure was swallowed' >&2
  exit 1
fi
echo 'PASS current generator detection, option forwarding and configure failures'
