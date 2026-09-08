---
name: github-branch-merge
description: Deliver verified LLM-EdgeFlow changes through an isolated GitHub branch and PR. Use only when the user explicitly requests upload, PR creation, or merge; PR-only is the default and merge requires explicit authorization.
---

# GitHub Branch and PR Delivery

This skill governs remote delivery, not ordinary local implementation. Read
[`CONTRIBUTING.md`](../../../CONTRIBUTING.md) before acting; it owns branch, RFC, documentation,
and verification policy. Do not repeat those decisions here.

## Preconditions

- The user explicitly authorized the requested remote action.
- Work is already on an approved non-`main` branch.
- The diff contains only intended work and any applicable RFC/index/`doc/CHANGELOG.md` updates.
- Implementation and required focused checks are complete; the intended changes are ready for
  the delivery script's canonical local gate.

If any precondition fails, correct it locally or report the blocker. Do not push a partial or
known-failing change.

## Delivery

Use the repository script; it runs the canonical local gate before committing and pushing.
Do not require a separate full gate before invoking it or reproduce its Git/GitHub sequence manually:

```bash
# Upload, create PR, and verify CI; default stops before merge.
./scripts/git_branch_upload.sh "<type>(<scope>): <summary>" "<branch-type>"

# Only when the user explicitly requested merge.
./scripts/git_branch_upload.sh "<type>(<scope>): <summary>" "<branch-type>" --merge
```

The script is the executable source of delivery behavior. It must never fall back to direct
`main` pushes, local merges, admin merges, or merging without registered successful CI checks.
Before merge, preserve the branch and PR on failure. For `--merge`, completion also requires
successful main push CI for the exact merge SHA; report a post-merge failure as merged but
unverified, without rollback or substituting a later main commit.
