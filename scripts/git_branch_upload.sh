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

PR_NUMBER="$(gh pr view "${BRANCH_NAME}" --json number --jq '.number')"
gh pr merge "${PR_NUMBER}" --merge --delete-branch
if ! MERGE_SHA="$(gh pr view "${PR_NUMBER}" --json mergeCommit --jq '.mergeCommit.oid // empty')" || \
   [[ -z "${MERGE_SHA}" ]]; then
  echo "Error: cannot confirm the merge SHA of PR #${PR_NUMBER}; main CI was not verified."
  exit 1
fi

echo "PR #${PR_NUMBER} merged as ${MERGE_SHA}; waiting up to 60 seconds for its main push CI..."
RUN_ID=""
for _ in $(seq 1 12); do
  if ! RUN_ID="$(gh run list --workflow ci.yml --branch main --event push \
    --commit "${MERGE_SHA}" --limit 1 --json databaseId --jq '.[0].databaseId // empty')"; then
    echo "Error: PR #${PR_NUMBER} is merged, but its main CI could not be queried."
    exit 1
  fi
  if [[ -n "${RUN_ID}" ]]; then
    break
  fi
  sleep 5
done
if [[ -z "${RUN_ID}" ]]; then
  echo "Error: PR #${PR_NUMBER} is merged, but no main push CI registered for ${MERGE_SHA}."
  exit 1
fi
if ! gh run watch "${RUN_ID}" --exit-status; then
  echo "Error: PR #${PR_NUMBER} is merged, but main CI run ${RUN_ID} did not pass for ${MERGE_SHA}."
  exit 1
fi
if ! RUN_URL="$(gh run view "${RUN_ID}" --json conclusion,url \
  --jq 'select(.conclusion == "success") | .url')" || [[ -z "${RUN_URL}" ]]; then
  echo "Error: PR #${PR_NUMBER} is merged, but main CI run ${RUN_ID} has no confirmed success."
  exit 1
fi
echo "PR #${PR_NUMBER} merged; main push CI passed for ${MERGE_SHA}: ${RUN_URL}"
