#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "${SCRIPT_DIR}")"
cd "${ROOT_DIR}"

usage() {
  echo "Usage: $0 \"<conventional commit message>\" <branch-type> [--pr-only|--merge]"
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  usage
  exit 0
fi
if [[ $# -lt 2 || $# -gt 3 ]]; then
  usage
  exit 2
fi

MSG="$1"
PREFIX="$2"
DELIVERY_MODE="${3:---pr-only}"

case "${PREFIX}" in
  feat|fix|refactor|docs|test|chore) ;;
  *)
    echo "Error: branch type must be feat, fix, refactor, docs, test, or chore."
    exit 2
    ;;
esac
case "${DELIVERY_MODE}" in
  --pr-only|--merge) ;;
  *)
    usage
    exit 2
    ;;
esac
CONVENTIONAL_RE='^(feat|fix|refactor|docs|test|chore)(\([[:alnum:]_.-]+\))?!?:[[:space:]].+'
if [[ ! "${MSG}" =~ ${CONVENTIONAL_RE} ]]; then
  echo "Error: commit message must follow Conventional Commits."
  exit 2
fi
TYPE_RE="^${PREFIX}(\\(|!|:)"
if [[ ! "${MSG}" =~ ${TYPE_RE} ]]; then
  echo "Error: commit message type must match branch type '${PREFIX}'."
  exit 2
fi
if ! command -v gh >/dev/null 2>&1; then
  echo "Error: GitHub CLI (gh) is required; direct-main fallback is forbidden."
  exit 1
fi

BRANCH_NAME="$(git branch --show-current)"
if [[ -z "${BRANCH_NAME}" || "${BRANCH_NAME}" == "main" ]]; then
  echo "Error: delivery requires an existing isolated non-main branch."
  exit 1
fi
case "${BRANCH_NAME}" in
  "${PREFIX}"/*) ;;
  *)
    echo "Error: branch '${BRANCH_NAME}' does not match requested type '${PREFIX}'."
    exit 1
    ;;
esac

echo "[1/5] Verifying branch ancestry against origin/main..."
git fetch origin main
if ! git merge-base --is-ancestor origin/main HEAD; then
  echo "Error: origin/main is not an ancestor of ${BRANCH_NAME}."
  echo "Resolve the update explicitly, then rerun; this script will not rewrite history."
  exit 1
fi

echo "[2/5] Running the canonical local quality gate..."
"${SCRIPT_DIR}/run_all_tests.sh"

echo "[3/5] Committing the verified working tree..."
git add -A
if git diff --cached --quiet; then
  echo "No uncommitted changes; delivering existing branch commits."
else
  git diff --cached --stat
  git commit -m "${MSG}"
fi
if git diff --quiet && git diff --cached --quiet; then
  :
else
  echo "Error: the working tree changed after staging/commit; rerun verification."
  exit 1
fi
if git diff --quiet origin/main...HEAD; then
  echo "Error: branch has no changes relative to origin/main."
  exit 1
fi

echo "[4/5] Pushing branch and creating or reusing its PR..."
git push -u origin "${BRANCH_NAME}"
if ! gh pr view "${BRANCH_NAME}" --json number >/dev/null 2>&1; then
  gh pr create \
    --title "${MSG}" \
    --body "Verified by the repository's canonical local quality gate." \
    --base main \
    --head "${BRANCH_NAME}"
fi

echo "Waiting up to 60 seconds for CI checks to register..."
CHECK_COUNT=0
for _ in $(seq 1 12); do
  CHECK_COUNT="$(gh pr view "${BRANCH_NAME}" --json statusCheckRollup \
    --jq '.statusCheckRollup | length')"
  if [[ "${CHECK_COUNT}" -gt 0 ]]; then
    break
  fi
  sleep 5
done
if [[ "${CHECK_COUNT}" -eq 0 ]]; then
  echo "Error: GitHub reported no CI checks; branch and PR remain available."
  exit 1
fi
gh pr checks "${BRANCH_NAME}" --watch --fail-fast

echo "[5/5] Applying authorized delivery mode..."
if [[ "${DELIVERY_MODE}" == "--pr-only" ]]; then
  echo "Verified PR is ready; main was not modified."
  gh pr view "${BRANCH_NAME}" --json number,url,state
  exit 0
fi

gh pr merge "${BRANCH_NAME}" --merge --delete-branch
gh pr view "${BRANCH_NAME}" --json number,url,state,mergedAt
