---
name: llm-edgeflow-developer-guide
description: Route implementation work across LLM-EdgeFlow's four architectural layers. Use for new C ABI modalities or adapters, Pipeline/Validator/Blackboard changes, business or common nodes, inference engines, and their verification; configuration-only composition belongs to pipeline-composer.
---

# LLM-EdgeFlow Developer Guide

First classify the requested change. Read only the references needed for the affected layer; do not load every reference by default.

- New modality, C ABI structure/function behavior, Adapter, or allowed runtime Pipeline name: read [Layer 1](references/layer1-adapter.md).
- Pipeline lifecycle, Validator, DAG planning, `AlgContext`, `BlackboardKey`, or session behavior: read [Layer 2](references/layer2-pipeline.md).
- New or modified business/common node and its Definition: read [Layer 3](references/layer3-node.md).
- New inference backend, engine capability, or batch execution behavior: read [Layer 4](references/layer4-engine.md).
- Before completing any implementation, read [Verification](references/verification.md).

Multi-layer features must preserve the dependency direction Layer 1 → Layer 2 → Layer 3 → Layer 4. Never introduce an upward dependency.

If the request only creates or changes Pipeline JSON using existing nodes, stop here and use `pipeline-composer`; ordinary composition must not modify Core or node implementations.

Use `github-branch-merge` only when the user explicitly asks to upload, open a PR, or merge.
