---
name: github-branch-merge
description: >-
  Standardized GitHub branch-and-merge workflow. Enforces creating a dedicated feature/fix branch,
  pushing to remote, creating and merging a pull request (or merging into main), and verifying clean sync.
  Use whenever uploading changes to GitHub to ensure safe branch isolation.
---

# GitHub Branch-and-Merge Workflow Skill

This skill enforces a robust, professional branch-and-merge workflow when uploading code, documentation, or configuration changes to GitHub repositories.

## Workflow Overview

```text
Working Tree (Dirty/New) ➔ Run Format/Tests ➔ Create New Branch (feat/fix/...) 
    ➔ Commit ➔ Push Feature Branch ➔ Merge into Main (via PR / Fast-Forward) 
    ➔ Sync Main to Remote ➔ Clean Up Temporary Branch
```

---

## Step-by-Step Execution Guide

### Step 1: Pre-Commit Quality Checks
Ensure all files are formatted and tests pass before committing:
```bash
# 1. Format code (if applicable)
./scripts/format.sh 2>/dev/null || true

# 2. Check git status
git status
```

### Step 2: Create a Feature/Update Branch
Generate a semantic branch name based on the task:
- `feat/<feature-name>` for new features / algorithms
- `fix/<bug-name>` for bugfixes
- `docs/<doc-topic>` for documentation updates
- `refactor/<target>` for refactoring
- `chore/sync-<timestamp>` for general synchronization

```bash
# Ensure local main is up to date
git checkout main
git pull origin main

# Create and switch to new branch
BRANCH_NAME="feat/update-$(date +%Y%m%d-%H%M%S)"
git checkout -b "$BRANCH_NAME"
```

### Step 3: Stage and Commit Changes
Follow Conventional Commits format (`feat:`, `fix:`, `docs:`, `refactor:`, `test:`, `chore:`):
```bash
git add .
git commit -m "<type>(<scope>): <clear descriptive commit message>"
```

### Step 4: Push Branch to Remote GitHub
```bash
git push -u origin "$BRANCH_NAME"
```

### Step 5: Merge Branch into Main
Execute either **GitHub PR Merge** (Preferred if `gh` CLI is available) or **Local Safe Merge**:

#### Method A: GitHub PR & Auto-Merge (Recommended)
```bash
# Create Pull Request
PR_URL=$(gh pr create --title "<Commit Title>" --body "<Brief Summary of Changes>" --base main --head "$BRANCH_NAME")

# Merge Pull Request and delete remote branch
gh pr merge "$BRANCH_NAME" --merge --delete-branch --admin || gh pr merge "$BRANCH_NAME" --merge --delete-branch
```

#### Method B: Local Merge & Fast-Forward Push (Fallback)
```bash
git checkout main
git pull origin main
git merge --no-ff "$BRANCH_NAME" -m "Merge branch '$BRANCH_NAME' into main"
git push origin main

# Clean up local and remote branch
git branch -d "$BRANCH_NAME"
git push origin --delete "$BRANCH_NAME" 2>/dev/null || true
```

### Step 6: Post-Verification
```bash
git checkout main
git pull origin main
git status
```
Ensure working tree is clean and `main` is strictly in sync with `origin/main`.
