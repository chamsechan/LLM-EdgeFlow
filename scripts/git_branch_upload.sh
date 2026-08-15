#!/usr/bin/env bash
# ==============================================================================
# LLM-EdgeFlow: Automated Git Branch & Merge Workflow Script
# Usage: ./scripts/git_branch_upload.sh "commit message" [branch_prefix]
# ==============================================================================

set -e

MSG="${1:-chore: update codebase and sync to GitHub}"
PREFIX="${2:-feat}"

TIMESTAMP=$(date +%Y%m%d-%H%M%S)
BRANCH_NAME="${PREFIX}/sync-${TIMESTAMP}"

echo "=================================================="
echo " 🚀 LLM-EdgeFlow Git Branch & Merge Workflow"
echo " Target Branch : ${BRANCH_NAME}"
echo " Commit Message: ${MSG}"
echo "=================================================="

# 1. Format code
if [ -f "./scripts/format.sh" ]; then
    ./scripts/format.sh
fi

# 2. Sync main branch
git checkout main
git pull origin main

# 3. Create and switch to new branch
git checkout -b "${BRANCH_NAME}"

# 4. Stage and commit
git add .
if git diff-index --quiet HEAD --; then
    echo "⚠️ No changes to commit. Working tree is clean."
    git checkout main
    git branch -D "${BRANCH_NAME}"
    exit 0
fi

git commit -m "${MSG}"

# 5. Push branch to remote
git push -u origin "${BRANCH_NAME}"

# 6. Merge via GitHub PR or Local Safe Merge
if command -v gh &> /dev/null; then
    echo "Merging via GitHub CLI PR..."
    gh pr create --title "${MSG}" --body "Automated branch sync for LLM-EdgeFlow" --base main --head "${BRANCH_NAME}" || true
    gh pr merge "${BRANCH_NAME}" --merge --delete-branch --admin || gh pr merge "${BRANCH_NAME}" --merge --delete-branch || {
        echo "PR merge fallback to local merge..."
        git checkout main
        git pull origin main
        git merge --no-ff "${BRANCH_NAME}" -m "Merge branch '${BRANCH_NAME}' into main"
        git push origin main
        git branch -d "${BRANCH_NAME}"
        git push origin --delete "${BRANCH_NAME}" 2>/dev/null || true
    }
else
    echo "Merging locally into main..."
    git checkout main
    git pull origin main
    git merge --no-ff "${BRANCH_NAME}" -m "Merge branch '${BRANCH_NAME}' into main"
    git push origin main
    git branch -d "${BRANCH_NAME}"
    git push origin --delete "${BRANCH_NAME}" 2>/dev/null || true
fi

# 7. Final sync verification
git checkout main
git pull origin main
echo "=================================================="
echo " ✅ Branch-and-Merge Workflow Completed Successfully!"
echo "=================================================="
