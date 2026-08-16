---
name: github-branch-merge
description: >-
  Standardized GitHub branch-and-merge workflow. Enforces mandatory full test regression pass,
  major changelog tracking in README, creating a dedicated feature/fix branch,
  pushing to remote, creating and merging a pull request (or merging into main), and verifying clean sync.
  Use whenever uploading changes to GitHub to ensure safe branch isolation.
---

# GitHub Branch-and-Merge Workflow Skill

This skill enforces a robust, enterprise-grade branch-and-merge workflow when uploading code, documentation, or configuration changes to GitHub repositories.

## Workflow Overview

```text
[Working Tree] ➔ [1. Format Code] ➔ [2. Mandatory Full Test Gate (100% Pass)]
    ➔ [3. Update README Changelog if Major Feature] ➔ [4. Create Feature Branch]
    ➔ [5. Commit & Push] ➔ [6. Merge PR into Main] ➔ [7. Clean Up Branch]
```

---

## 🔒 Two Mandatory Rules

### Rule 1: Mandatory Full Test Gate (必须全量测试通过才允许合并)
- Before creating a branch or merging, the agent **MUST** run the full test suite (`./scripts/run_all_tests.sh` or `ctest --output-on-failure`).
- If any test fails, **ABORT the workflow immediately**. Never push broken code to `main`!

### Rule 2: Major Release Changelog Maintenance (重大特性更新日志维护)
- **What constitutes a major change?**
  - New C ABI business modalities or data structures (Layer 1).
  - Architecture core mechanism upgrades / dynamic blackboard additions (Layer 2).
  - New business operator packages (Layer 3).
  - New hardware inference engines (TensorRT, Ascend, ONNX, llama.cpp) (Layer 4).
  - Major test framework or developer toolchain additions.
- **Action**: When major changes occur, prepend the version entry to `## 📝 更新日志 (Changelog)` in `README.md`.
- **Minor changes** (e.g. documentation typos, formatting, tiny bugfixes) do **NOT** require a Changelog entry.

---

## Step-by-Step Execution Guide

### Step 1: Pre-Commit Quality & Test Gate
```bash
# 1. Format code (Google C++ Style)
./scripts/format.sh

# 2. Run full regression test suite (Mandatory Test Gate)
./scripts/run_all_tests.sh
# Or in build directory:
# ctest --output-on-failure
```

### Step 2: Update Changelog (If Major Feature)
Check if changes involve major architectural or capability additions. If so, update `## 📝 更新日志 (Changelog)` in `README.md`.

### Step 3: Create a Dedicated Work Branch
```bash
# Sync local main with origin
git checkout main
git pull origin main

# Create and switch to new branch
BRANCH_NAME="feat/sync-$(date +%Y%m%d-%H%M%S)"
git checkout -b "$BRANCH_NAME"
```

### Step 4: Stage and Commit Changes
Follow Conventional Commits format (`feat:`, `fix:`, `docs:`, `refactor:`, `test:`):
```bash
git add .
git commit -m "<type>(<scope>): <clear descriptive commit message>"
```

### Step 5: Push Branch to Remote GitHub
```bash
git push -u origin "$BRANCH_NAME"
```

### Step 6: Create & Merge Pull Request (or Fast-Forward)
```bash
# Using GitHub CLI
gh pr create --title "<Commit Title>" --body "<Summary>" --base main --head "$BRANCH_NAME"
gh pr merge "$BRANCH_NAME" --merge --delete-branch --admin || gh pr merge "$BRANCH_NAME" --merge --delete-branch
```

### Step 7: Post-Verification
```bash
git checkout main
git pull origin main
git status
```
Ensure working tree is clean and `main` is strictly synchronized with `origin/main`.
