# Contributing to LLM-EdgeFlow

This document is the single source of truth for the development and delivery lifecycle. Current
architecture constraints and task routing live in [AGENTS.md](AGENTS.md); current contracts live
in [the architecture and developer guides](doc/README.md). Scripts own the exact mechanics of
quality gates and GitHub delivery.

## 1. Classify before editing

Choose the smallest path that covers the change:

| Change | Path |
| :--- | :--- |
| Read-only review or diagnosis | Inspect and report; no branch or write is required. |
| Solution configuration using registered capabilities and biz contracts (Pipeline JSON, necessary `.conf`, optional Demo Profiles) | Use `pipeline-composer`, Catalog, and Validator; no C++ or separate design review normally needed. |
| Local bug, test, documentation, behavior-preserving refactor, or routine custom Node using existing contracts | Create a branch, implement, and add proportional tests. |
| Public contract, cross-layer architecture, compatibility/migration policy, new shared Node/Model/Backend capability, dependency, or high-risk ownership/concurrency/security/performance decision | Create a branch and settle the design before implementation. |

If classification changes during investigation, pause only work crossing the newly discovered
boundary, record the design or route to the affected-layer guide, and resolve the decision
before that work resumes. A route change alone is not completion of an implementation request.

## 2. Work on an isolated branch

Create `feat/*`, `fix/*`, `refactor/*`, `docs/*`, `test/*`, or `chore/*` before tracked edits.
Base it on the intended mainline revision. Do not silently pull, rebase, or merge remote changes
into a dirty worktree.

The branch itself is not evidence of quality; it provides isolation and a reviewable diff.

## 3. Design and current contracts

Design review is required when a change affects one or more of these boundaries:

- public SDK ABI, Operator contract, persisted Pipeline schema, or compatibility behavior;
- dependencies between architectural layers or responsibilities shared across layers;
- new framework-maintained common Node, Model capability, Backend, modality, shared port type, or major toolchain;
- data ownership, lifetime, concurrency, security, or performance decisions that are difficult
  to reverse;
- a migration or deprecation that downstream users must coordinate.

A contained bug fix, test improvement, documentation correction, mechanical refactor, or
Pipeline composition using existing contracts needs no separate design document. A routine
custom Node uses its Definition, change description and focused behavior tests when it
preserves existing port types, model capabilities, public contracts, layer boundaries and
ownership/concurrency rules. Registration alone does not trigger design review.

Record the problem, chosen design, affected contracts, important trade-offs, and acceptance
criteria in the work description or PR before implementing a boundary change. For complex
work, include independently verifiable stages and a rollback path. Keep this proportionate to
the decision; a local task does not require opening a remote PR or obtaining new permission.

Maintain the resulting rules and necessary rationale in their current owning guide, alongside
the implementation and contract tests. Do not create numbered proposal archives or duplicate
the same rule across documents. Git and PR history retain the development discussion.

Start from the current guides and affected code/tests. Consult Git history only when the task
needs a past decision or regression baseline. If current documentation, implementation and
tests disagree, resolve whether this is a defect, stale documentation, or an authorized design
change before altering the contract. An unfinished proposal is not an implemented feature.

Review every applicable requirement, invariant, migration step and acceptance criterion in
the agreed design before claiming completion. Pass delegated work the relevant decisions and
evidence, expanding context only as its scope requires.

## 4. Implement at the narrowest layer

- Query the runtime Catalog before adding a capability.
- Keep each rule in its owning layer; do not reproduce Validator or Definition logic in UI,
  scripts, prompts, or documentation tables.
- Add focused coverage next to the behavior being changed. Extend an existing test runner when
  it already owns the contract; create a new test target only for a genuinely independent suite.
- During development, run the smallest relevant build/test command for fast feedback. This is
  not a delivery gate.

### Work through the requested outcome

Within the authorized scope, continue from implementation to focused checks, inspect results,
fix change-caused failures, and revalidate. Ordinary local edits and check/fix retries do not
need a new user approval for each command, subject to tool permissions and sandbox policy.
A failed check is a diagnostic signal, not automatic permission to weaken assertions, skip a
gate, or expand into unrelated fixes. Report unrelated baseline failures separately.

Before running unfamiliar commands, check their side effects. This policy does not authorize
remote writes, production access, paid services, destructive cleanup, internal SDK access, or
changes beyond the agreed scope. Resolve genuinely new public-contract/architecture decisions
and required approvals before crossing those boundaries; do not bypass an explicit checkpoint.

For complex architecture changes, define independently testable stages with acceptance criteria
and a rollback path (including staged stubs when useful). Finish and verify each stage before
advancing; honor user-required acceptance checkpoints. A failed or unaccepted stage must not
be reported as complete or used to justify progressing to the next stage.

Done means the requested behavior/configuration is implemented, any needed tests and
documentation are updated, required checks have passed, and observable results have been
inspected. Unchanged behavior does not require new tests merely to create a test diff. A requested runnable solution also needs execution of its actual edited
configuration, not just a compiled Node or an unchanged Demo Profile. Read-only review requests
end with findings, not unsolicited implementation. Missing assets/tools/permissions require a
precise blocked or unverified report, not a success claim or a fictitious test result.

### Source and identifier names

Follow [source layout and naming](doc/dev_guide/source_layout.md) for SDK headers,
source-extension contracts, private headers, and the distinct Adapter/biz/port names.
Keep private declarations beside their implementation; templates and inline extension
helpers may remain in authoring headers. SDK targets expose only the public header view.

Use `snake_case` C/C++ filenames that describe the primary type or operation. C++ types and
ordinary functions use `PascalCase`, variables use `snake_case`, private data members end in
`_`, and constants/enumerators use `kPascalCase`; conventional accessors may use `snake_case`.
Keep public ABI and vendor-defined names unchanged. Use `biz` for new internal business
identifiers. Share model helpers under a common owner, not inside a consuming model's directory.

A `biz_name` identifies an I/O contract; model size and Backend selection belong in deployment
configuration and Profiles. Update current documentation and examples when renaming code.

## 5. Update durable documentation proportionally

- Keep active architecture and developer documentation aligned with the implementation.
- Update `doc/CHANGELOG.md` for user-visible capabilities, public behavior, architecture, or major
  developer-tool changes; do not add entries for typo-only or internal mechanical changes.
- Update README only when the current overview, capability maturity, quick start, or navigation
  changes.
- Keep current usage, contracts and verification limits in the working tree. Development
  history belongs in Git and PR discussions; do not accumulate dated implementation reports.
- Distinguish supported behavior from proposed work and unverified effects or deployment
  environments. A passing default gate does not establish production readiness.

## 6. Run one canonical delivery gate

Finish the intended changes and durable documentation, then choose the gate entrypoint:

- For a local handoff without remote delivery, run:

```bash
./scripts/run_all_tests.sh
```

- When the user has authorized PR delivery or merge, use the delivery script in section 7.
  It runs this same gate before committing and pushing; do not run a separate full gate first.

This single command is authoritative for the default deliverable: shell syntax, C/C++ format
check, Git whitespace, complete default configuration/build, and all registered CTest tests.
Do not also require separate full `ctest` or formatting passes unless diagnosing a failure or
verifying a non-default build such as sanitizer or real-model execution.
Changes after verification or a failed gate require revalidation. A prior local handoff does not
bypass the delivery script's gate when remote delivery is requested later.

Documentation-only changes still run the canonical gate before PR because documentation and
governance checks are registered in CTest. If the environment cannot run the gate, report the
exact missing prerequisite and do not claim full verification.

Complete the agreed scope, focused/non-default checks and current documentation before the
final gate. Report exact results, skips and unverified requirements in the handoff or PR;
completion is confirmed only after the required checks succeed. GitHub merge state remains
observable in Git rather than duplicated in documentation metadata.

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

There is no direct-push or admin-merge fallback. Failures before merge leave the branch/PR
intact for correction. After an authorized merge, delivery completes only when the `ci.yml`
push run for that exact merge SHA succeeds; a failed, cancelled, or missing run must be
reported as merged but unverified, without rollback or substitution of a later main commit.
