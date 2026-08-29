#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="${LLM_EDGEFLOW_GOVERNANCE_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

required_files=(
  "AGENTS.md"
  "CONTRIBUTING.md"
  "CLAUDE.md"
  ".github/copilot-instructions.md"
  ".agents/skills/pipeline-composer/SKILL.md"
  ".agents/skills/llm-edgeflow-developer-guide/SKILL.md"
  ".agents/skills/github-branch-merge/SKILL.md"
  "doc/rfcs/README.md"
  "scripts/run_all_tests.sh"
  "scripts/git_branch_upload.sh"
)

for relative_path in "${required_files[@]}"; do
  if [[ ! -s "${ROOT_DIR}/${relative_path}" ]]; then
    echo "Error: missing governance source ${relative_path}"
    exit 1
  fi
done

for shim in "CLAUDE.md" ".github/copilot-instructions.md"; do
  if ! grep -q 'AGENTS.md' "${ROOT_DIR}/${shim}" || \
     ! grep -q 'CONTRIBUTING.md' "${ROOT_DIR}/${shim}"; then
    echo "Error: ${shim} must route to AGENTS.md and CONTRIBUTING.md"
    exit 1
  fi
done

for skill in \
  ".agents/skills/pipeline-composer/SKILL.md" \
  ".agents/skills/llm-edgeflow-developer-guide/SKILL.md" \
  ".agents/skills/github-branch-merge/SKILL.md"; do
  if ! grep -q '^name:' "${ROOT_DIR}/${skill}" || \
     ! grep -q '^description:' "${ROOT_DIR}/${skill}"; then
    echo "Error: ${skill} is missing required skill frontmatter"
    exit 1
  fi
done

DELIVERY_SCRIPT="${ROOT_DIR}/scripts/git_branch_upload.sh"
bash -n "${DELIVERY_SCRIPT}"
if grep -nE -- \
  '(git (checkout|switch) main|git push.*[[:space:]]main([[:space:]]|$)|git merge --no-ff|--admin)' \
  "${DELIVERY_SCRIPT}"; then
  echo "Error: GitHub delivery script contains a forbidden main/admin fallback"
  exit 1
fi
if ! grep -q -- '--pr-only' "${DELIVERY_SCRIPT}" || \
   ! grep -q -- '--merge' "${DELIVERY_SCRIPT}"; then
  echo "Error: GitHub delivery script must expose PR-only and explicit merge modes"
  exit 1
fi
if ! grep -Fq 'DELIVERY_MODE="${3:---pr-only}"' "${DELIVERY_SCRIPT}"; then
  echo "Error: GitHub delivery must default to PR-only mode"
  exit 1
fi
QUALITY_GATE_CALLS="$(grep -c 'run_all_tests.sh' "${DELIVERY_SCRIPT}")"
if [[ "${QUALITY_GATE_CALLS}" -ne 1 ]]; then
  echo "Error: GitHub delivery must invoke the canonical quality gate exactly once"
  exit 1
fi

if grep -rnE '(six-stage|6-stage|六阶段|7 CTest|src/business/|src/biz/|IModelEngine|REGISTER_ENGINE_WITH_DEFINITION)' \
  "${ROOT_DIR}/AGENTS.md" \
  "${ROOT_DIR}/CONTRIBUTING.md" \
  "${ROOT_DIR}/CLAUDE.md" \
  "${ROOT_DIR}/.github/copilot-instructions.md" \
  "${ROOT_DIR}/.agents/skills" \
  "${ROOT_DIR}/doc/rfcs/README.md"; then
  echo "Error: active governance contains obsolete architecture or test-count guidance"
  exit 1
fi

echo "Governance sources, routing, and delivery safety invariants are consistent."
