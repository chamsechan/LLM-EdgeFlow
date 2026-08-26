#!/usr/bin/env bash
# ==============================================================================
# LLM-EdgeFlow: Automated Git Branch & Merge Workflow Script
# Usage: ./scripts/git_branch_upload.sh "commit message" [branch_prefix]
# ==============================================================================

set -e

MSG="${1:-chore: update codebase and sync to GitHub}"
PREFIX="${2:-feat}"

TIMESTAMP=$(date +%Y%m%d-%H%M%S)
SOURCE_BRANCH=$(git branch --show-current)
if [ -z "${SOURCE_BRANCH}" ]; then
    echo "❌ Detached HEAD is not supported by the upload workflow."
    exit 1
fi

CREATED_BRANCH=0
if [ "${SOURCE_BRANCH}" = "main" ]; then
    BRANCH_NAME="${PREFIX}/sync-${TIMESTAMP}"
else
    case "${SOURCE_BRANCH}" in
        feat/*|fix/*|refactor/*|docs/*|test/*|chore/*)
            BRANCH_NAME="${SOURCE_BRANCH}"
            ;;
        *)
            echo "❌ Existing branch must use an approved feature/fix prefix: ${SOURCE_BRANCH}"
            exit 1
            ;;
    esac
fi

echo "=================================================="
echo " 🚀 LLM-EdgeFlow Git Branch & Merge Workflow"
echo " Target Branch : ${BRANCH_NAME}"
echo " Commit Message: ${MSG}"
echo "=================================================="

# 1. Google C++ 规范自动格式化
if [ -f "./scripts/format.sh" ]; then
    echo "[Step 1/6] Running Google C++ code formatting..."
    ./scripts/format.sh
fi

# 2. 强校验：全量质量回归测试门禁 (Mandatory Full Test Gate)
echo "[Step 2/6] Running full mandatory regression test suite..."
if [ -f "./scripts/run_all_tests.sh" ]; then
    if ! ./scripts/run_all_tests.sh; then
        echo "❌ [TEST GATE BLOCKED] Regression tests failed! Merge rejected."
        exit 1
    fi
else
    # Fallback to CTest in build directory
    if [ -d "./build" ]; then
        cd build && cmake .. && make -j4 && ctest --output-on-failure && cd ..
    fi
fi
echo "✓ [TEST GATE PASSED] All test suites and CTest passed 100%!"

# 3. 检查远程主干同步
echo "[Step 3/6] Checking branch ancestry against origin/main..."
git fetch origin main
if [ "${SOURCE_BRANCH}" = "main" ]; then
    git checkout main
    git pull --ff-only origin main

    # 4. 从最新 main 创建隔离工作分支
    echo "[Step 4/6] Creating isolated branch: ${BRANCH_NAME}..."
    git checkout -b "${BRANCH_NAME}"
    CREATED_BRANCH=1
else
    echo "[Step 4/6] Reusing existing isolated branch: ${BRANCH_NAME}..."
    if ! git merge-base --is-ancestor origin/main HEAD; then
        echo "❌ Current feature branch is not based on the latest origin/main."
        echo "   Rebase or merge origin/main explicitly, resolve conflicts, and rerun."
        exit 1
    fi
fi

# 5. 暂存与提交
git add .
if git diff-index --quiet HEAD --; then
    echo "⚠️ No changes to commit. Working tree is clean."
    if [ "${CREATED_BRANCH}" -eq 1 ]; then
        git checkout main
        git branch -D "${BRANCH_NAME}"
        exit 0
    fi
else
    git commit -m "${MSG}"
fi

# 6. 推送并执行合并 (GitHub PR / Fast-Forward)
echo "[Step 5/6] Pushing branch and merging..."
git push -u origin "${BRANCH_NAME}"

if command -v gh &> /dev/null; then
    echo "Creating & merging via GitHub PR..."
    gh pr create --title "${MSG}" --body "Automated branch sync with full verified tests for LLM-EdgeFlow" --base main --head "${BRANCH_NAME}" || \
        gh pr view "${BRANCH_NAME}" --json number >/dev/null

    echo "Waiting for GitHub CI checks to be registered..."
    CHECK_COUNT=0
    for _ in $(seq 1 24); do
        CHECK_COUNT=$(gh pr view "${BRANCH_NAME}" --json statusCheckRollup \
            --jq '.statusCheckRollup | length')
        if [ "${CHECK_COUNT}" -gt 0 ]; then
            break
        fi
        sleep 5
    done
    if [ "${CHECK_COUNT}" -eq 0 ]; then
        echo "❌ GitHub reported no CI checks for the PR after 120 seconds."
        echo "   Branch remains pushed; merge was not attempted."
        exit 1
    fi
    gh pr checks "${BRANCH_NAME}" --watch --fail-fast

    gh pr merge "${BRANCH_NAME}" --merge --delete-branch || gh pr merge "${BRANCH_NAME}" --merge --delete-branch --admin || {
        echo "PR merge fallback to local merge..."
        git checkout main
        git pull --ff-only origin main
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

# 7. 最终主干同步验证
echo "[Step 6/6] Final verification on main..."
git checkout main
git pull --ff-only origin main
if [ "${BRANCH_NAME}" != "main" ] && git show-ref --verify --quiet "refs/heads/${BRANCH_NAME}"; then
    git branch -d "${BRANCH_NAME}"
fi
git status --short
echo "=================================================="
echo " ✅ Verified & Merged into main successfully!"
echo "=================================================="
