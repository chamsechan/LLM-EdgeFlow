#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
TEST_TMP_DIR=$(mktemp -d /tmp/test_sanitizer_ccache_XXXXXX)
trap 'rm -rf "${TEST_TMP_DIR}"' EXIT

fail() {
  echo "FAILED: $*" >&2
  exit 1
}

MOCK_BIN="${TEST_TMP_DIR}/bin"
COMMAND_LOG="${TEST_TMP_DIR}/commands.log"
CACHE_DIR="${TEST_TMP_DIR}/cache"
BUILD_DIR="${TEST_TMP_DIR}/build"
mkdir -p "${MOCK_BIN}"

for command_name in cmake ctest ccache; do
  command_path="${MOCK_BIN}/${command_name}"
  if [[ "${command_name}" == "cmake" ]]; then
    cat >"${command_path}" <<'EOF'
#!/usr/bin/env bash
printf 'cmake %s\n' "$*" >>"${EDGEFLOW_TEST_COMMAND_LOG}"
printf 'ccache_dir=%s\n' "${CCACHE_DIR:-}" >>"${EDGEFLOW_TEST_COMMAND_LOG}"
printf 'ccache_sloppiness=%s\n' "${CCACHE_SLOPPINESS:-}" >>"${EDGEFLOW_TEST_COMMAND_LOG}"
EOF
  elif [[ "${command_name}" == "ctest" ]]; then
    cat >"${command_path}" <<'EOF'
#!/usr/bin/env bash
printf 'ctest %s\n' "$*" >>"${EDGEFLOW_TEST_COMMAND_LOG}"
EOF
  else
    cat >"${command_path}" <<'EOF'
#!/usr/bin/env bash
printf 'ccache %s\n' "$*" >>"${EDGEFLOW_TEST_COMMAND_LOG}"
EOF
  fi
  chmod +x "${command_path}"
done

EDGEFLOW_TEST_COMMAND_LOG="${COMMAND_LOG}" \
CCACHE_DIR="${CACHE_DIR}" \
LLM_EDGEFLOW_SANITIZER_BUILD_DIR="${BUILD_DIR}" \
LLM_EDGEFLOW_SANITIZERS="undefined" \
PATH="${MOCK_BIN}:${PATH}" \
  "${PROJECT_ROOT}/scripts/run_sanitizers.sh" --fast

grep -Fq -- '-DCMAKE_C_COMPILER_LAUNCHER=ccache' "${COMMAND_LOG}" ||
  fail "Sanitizer configure did not enable the C ccache launcher"
grep -Fq -- '-DCMAKE_CXX_COMPILER_LAUNCHER=ccache' "${COMMAND_LOG}" ||
  fail "Sanitizer configure did not enable the C++ ccache launcher"
grep -Fq -- '-DLLM_EDGEFLOW_TEST_PCH=OFF' "${COMMAND_LOG}" ||
  fail "Sanitizer configure did not disable cache-hostile test PCH"
grep -Fq "ccache_dir=${CACHE_DIR}" "${COMMAND_LOG}" ||
  fail "Sanitizer script ignored the caller-provided ccache directory"
grep -Fq 'ccache --zero-stats' "${COMMAND_LOG}" ||
  fail "Sanitizer script did not reset ccache statistics"
grep -Fq 'ccache --show-stats' "${COMMAND_LOG}" ||
  fail "Sanitizer script did not report ccache statistics"

WORKFLOW="${PROJECT_ROOT}/.github/workflows/ci.yml"
grep -Fq 'CCACHE_DIR: ${{ github.workspace }}/build/.ccache-sanitizers' \
  "${WORKFLOW}" || fail "Workflow cache path does not match the sanitizer script"
grep -Fq 'key: sanitizer-ccache-v1-' "${WORKFLOW}" ||
  fail "Workflow does not define a versioned sanitizer ccache key"

echo "Sanitizer ccache contract passed."
