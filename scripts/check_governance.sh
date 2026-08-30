#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="${LLM_EDGEFLOW_GOVERNANCE_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

fail() {
  echo "Error: $*" >&2
  exit 1
}

required_files=(
  "AGENTS.md"
  "CONTRIBUTING.md"
  ".github/copilot-instructions.md"
  ".agents/skills/pipeline-composer/SKILL.md"
  ".agents/skills/llm-edgeflow-developer-guide/SKILL.md"
  ".agents/skills/github-branch-merge/SKILL.md"
  "doc/rfcs/README.md"
  "scripts/run_all_tests.sh"
  "scripts/git_branch_upload.sh"
)

for relative_path in "${required_files[@]}"; do
  [[ -s "${ROOT_DIR}/${relative_path}" ]] || fail "missing governance source ${relative_path}"
done

shim=".github/copilot-instructions.md"
grep -q 'AGENTS.md' "${ROOT_DIR}/${shim}" && \
  grep -q 'CONTRIBUTING.md' "${ROOT_DIR}/${shim}" || \
  fail "${shim} must route to AGENTS.md and CONTRIBUTING.md"

for skill in \
  ".agents/skills/pipeline-composer/SKILL.md" \
  ".agents/skills/llm-edgeflow-developer-guide/SKILL.md" \
  ".agents/skills/github-branch-merge/SKILL.md"; do
  grep -q '^name:' "${ROOT_DIR}/${skill}" && \
    grep -q '^description:' "${ROOT_DIR}/${skill}" || \
    fail "${skill} is missing required skill frontmatter"
done

DELIVERY_SCRIPT="${ROOT_DIR}/scripts/git_branch_upload.sh"
bash -n "${DELIVERY_SCRIPT}"
grep -nE -- \
  '(git (checkout|switch) main|git push.*[[:space:]]main([[:space:]]|$)|git merge --no-ff|--admin)' \
  "${DELIVERY_SCRIPT}" && \
  fail "GitHub delivery script contains a forbidden main/admin fallback"
grep -q -- '--pr-only' "${DELIVERY_SCRIPT}" && \
  grep -q -- '--merge' "${DELIVERY_SCRIPT}" || \
  fail "GitHub delivery script must expose PR-only and explicit merge modes"
grep -Fq 'DELIVERY_MODE="${3:---pr-only}"' "${DELIVERY_SCRIPT}" || \
  fail "GitHub delivery must default to PR-only mode"
QUALITY_GATE_CALLS="$(grep -c 'run_all_tests.sh' "${DELIVERY_SCRIPT}")"
[[ "${QUALITY_GATE_CALLS}" -eq 1 ]] || \
  fail "GitHub delivery must invoke the canonical quality gate exactly once"

grep -rnE '(six-stage|6-stage|六阶段|7 CTest|src/business/|src/biz/|IModelEngine|REGISTER_ENGINE_WITH_DEFINITION)' \
  "${ROOT_DIR}/AGENTS.md" \
  "${ROOT_DIR}/CONTRIBUTING.md" \
  "${ROOT_DIR}/.github/copilot-instructions.md" \
  "${ROOT_DIR}/.agents/skills" \
  "${ROOT_DIR}/doc/rfcs/README.md" && \
  fail "active governance contains obsolete architecture or test-count guidance"

echo "Governance sources, routing, and delivery safety invariants are consistent."
