# Contributing to LLM-EdgeFlow

This document is the single source of truth for the development and delivery lifecycle. Current
architecture constraints and task routing live in [AGENTS.md](AGENTS.md); RFC statuses live in
[the RFC index](doc/rfcs/README.md); scripts own the exact mechanics of quality gates and GitHub
delivery.

## 1. Classify before editing

Choose the smallest path that covers the change:

| Change | Path |
| :--- | :--- |
| Read-only review or diagnosis | Inspect and report; no branch or write is required. |
| Pipeline JSON using registered capabilities | Use `pipeline-composer`, Catalog, and Validator; no C++ and normally no RFC. |
| Local bug, test, documentation, or behavior-preserving refactor | Create a branch, implement, and add proportional tests; normally no RFC. |
| Public contract, cross-layer architecture, compatibility/migration policy, new Node/Model/Backend capability, dependency, or high-risk ownership/concurrency/security/performance decision | Create a branch and RFC before implementation. |

If classification changes during investigation, stop implementation at the newly discovered
boundary and add the required RFC or route to the relevant layer guide.

## 2. Work on an isolated branch

Create `feat/*`, `fix/*`, `refactor/*`, `docs/*`, `test/*`, or `chore/*` before tracked edits.
Base it on the intended mainline revision. Do not silently pull, rebase, or merge remote changes
into a dirty worktree.

The branch itself is not evidence of quality; it provides isolation and a reviewable diff.

## 3. Design only when the decision needs a durable record

An RFC is required when a change affects one or more of these boundaries:

- public C ABI, Operator contract, persisted Pipeline schema, or compatibility behavior;
- dependencies between architectural layers or responsibilities shared across layers;
- new externally discoverable Node, Model capability, Backend, modality, or major toolchain;
- data ownership, lifetime, concurrency, security, or performance decisions that are difficult
  to reverse;
- a migration or deprecation that downstream users must coordinate.

An RFC is not required for a contained bug fix, test improvement, documentation correction,
mechanical refactor, or Pipeline composition that reuses existing registered contracts. A small
change may still use an RFC when the decision is contentious or has lasting operational cost.

Create RFCs from [RFC_TEMPLATE.md](doc/rfcs/RFC_TEMPLATE.md), add them to the index, and keep
scope, invariants, decisions, and verification current while implementing.

## 4. Implement at the narrowest layer

- Query the runtime Catalog before adding a capability.
- Keep each rule in its owning layer; do not reproduce Validator or Definition logic in UI,
  scripts, prompts, or documentation tables.
- Add focused coverage next to the behavior being changed. Extend an existing test runner when
  it already owns the contract; create a new test target only for a genuinely independent suite.
- During development, run the smallest relevant build/test command for fast feedback. This is
  not a delivery gate.

## 5. Update durable documentation proportionally

- Keep active architecture and developer documentation aligned with the implementation.
- Update `CHANGELOG.md` for user-visible capabilities, public behavior, architecture, or major
  developer-tool changes; do not add entries for typo-only or internal mechanical changes.
- Update README only when the current overview, capability maturity, quick start, or navigation
  changes.
- Historical RFCs and acceptance reports record their original context; do not rewrite them to
  mimic current architecture. Supersede them with a new RFC when a decision changes.

## 6. Run one canonical delivery gate

Before declaring a tracked change ready for review, run:

```bash
./scripts/run_all_tests.sh
```

This single command is authoritative for the default deliverable: shell syntax, C/C++ format
check, Git whitespace, complete default configuration/build, and all registered CTest tests.
Do not also require separate full `ctest` or formatting passes unless diagnosing a failure or
verifying a non-default build such as sanitizer or real-model execution.

Documentation-only changes still run the canonical gate before PR because documentation and
governance checks are registered in CTest. If the environment cannot run the gate, report the
exact missing prerequisite and do not claim full verification.

After the gate succeeds, mark an applicable RFC `Completed` and update its index row. Here,
`Completed` means the scoped implementation and required verification are complete and the same
commit set is ready to land; GitHub merge state remains observable in Git rather than duplicated
in RFC metadata.

## 7. Deliver only with explicit authorization

Local implementation does not authorize remote writes. When the user asks to upload or open a
PR, use:

```bash
./scripts/git_branch_upload.sh "<conventional commit message>" "<branch type>"
```

The default stops after pushing the current isolated branch, opening the PR, and verifying CI.
Only when the user explicitly asks to merge, pass `--merge`:

```bash
./scripts/git_branch_upload.sh "<conventional commit message>" "<branch type>" --merge
```

There is no direct-push or admin-merge fallback. A failed local gate, stale branch, missing
GitHub CLI, absent CI checks, or failed CI leaves the branch/PR intact for correction.
