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
| Solution configuration using registered capabilities and biz contracts (Pipeline JSON, necessary `.conf`, optional Demo Profiles) | Use `pipeline-composer`, Catalog, and Validator; no C++ and normally no RFC. |
| Local bug, test, documentation, behavior-preserving refactor, or routine custom Node using existing contracts | Create a branch, implement, and add proportional tests; normally no RFC. |
| Public contract, cross-layer architecture, compatibility/migration policy, new shared Node/Model/Backend capability, dependency, or high-risk ownership/concurrency/security/performance decision | Create a branch and RFC before implementation. |

If classification changes during investigation, pause only work crossing the newly discovered
boundary, add the required RFC or route to the affected-layer guide, and resolve the decision
before that work resumes. A route change alone is not completion of an implementation request.

## 2. Work on an isolated branch

Create `feat/*`, `fix/*`, `refactor/*`, `docs/*`, `test/*`, or `chore/*` before tracked edits.
Base it on the intended mainline revision. Do not silently pull, rebase, or merge remote changes
into a dirty worktree.

The branch itself is not evidence of quality; it provides isolation and a reviewable diff.

## 3. Design only when the decision needs a durable record

An RFC is required when a change affects one or more of these boundaries:

- public C ABI, Operator contract, persisted Pipeline schema, or compatibility behavior;
- dependencies between architectural layers or responsibilities shared across layers;
- new framework-maintained common Node, Model capability, Backend, modality, shared port type, or major toolchain;
- data ownership, lifetime, concurrency, security, or performance decisions that are difficult
  to reverse;
- a migration or deprecation that downstream users must coordinate.

An RFC is not required for a contained bug fix, test improvement, documentation correction,
mechanical refactor, or Pipeline composition that reuses existing registered contracts. A routine
custom Node also needs no RFC when it uses existing port types/model capabilities and preserves
public contracts, layer boundaries and established ownership/concurrency rules. Its Definition,
change description and focused behavior tests record the extension. Registration alone does not
trigger an RFC; a new shared contract or one of the boundaries above still does. A small
change may still use an RFC when the decision is contentious or has lasting operational cost.

Create RFCs from [RFC_TEMPLATE.md](doc/rfcs/RFC_TEMPLATE.md), add them to the index, and keep
scope, invariants, decisions, and verification current while implementing.

### RFC lookup

RFCs are durable decision records, not default development context. Start routine work from
current guides and affected code/tests. Do not pre-read the index or bulk-read RFC, review, or
archive trees; exclude those trees from ordinary code searches unless the task needs them.
A citation in a guide alone does not require opening an RFC.

Read relevant RFC material for an explicit RFC task, implementation/review against its
requirements, an architecture/contract/migration decision that needs its governing rationale
or acceptance criteria, or unclear/conflicting intent in current sources. The creation
thresholds above still apply; reading less never exempts a required new RFC.

For a known number/path, locate that RFC directly. Search only matching index rows when its
path, status, or supersession is unclear; otherwise skip the index. For an unknown RFC, search
index titles/topics before RFC bodies. Read relevant headings/sections and expand only for
necessary dependencies. Open the template to author an RFC, not as routine startup reading;
open linked reviews only when their evidence is needed.

For RFC implementation/review, cover every applicable requirement, invariant, migration step,
stage checkpoint, and acceptance criterion before claiming completion. Selected snippets do
not prove full coverage; read the whole relevant RFC when necessary. Pass delegated work
specific paths/sections and decisions, not an entire history; expand for its actual scope.

Check status, target baseline, and explicit supersession. `Completed` does not mean obsolete;
`Proposed` does not mean implemented. Current docs/code/tests guide discovery, not silent
overrides of an applicable approved contract. Surface conflicts and resolve whether they are
bugs, stale documentation, or authorized design changes before crossing that boundary.

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
configuration and Profiles. Preserve historical RFCs and acceptance records when renaming code.

## 5. Update durable documentation proportionally

- Keep active architecture and developer documentation aligned with the implementation.
- Update `doc/CHANGELOG.md` for user-visible capabilities, public behavior, architecture, or major
  developer-tool changes; do not add entries for typo-only or internal mechanical changes.
- Update README only when the current overview, capability maturity, quick start, or navigation
  changes.
- Historical RFCs and acceptance reports record their original context; do not rewrite them to
  mimic current architecture. Supersede them with a new RFC when a decision changes.

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

When an RFC's implementation and required focused/non-default checks are complete, prepare its
`Completed` status and matching index row in the final diff submitted to the gate. Completion is
confirmed only after the gate succeeds; on failure, restore `In Implementation` while correcting
the remaining work. This keeps the RFC closeout in the verified changes. `Completed` means the
scoped implementation and required verification are complete; GitHub merge state remains
observable in Git rather than duplicated in RFC metadata.

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
