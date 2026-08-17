# 🤖 LLM-EdgeFlow AI Agent Development Directives & Governance

Welcome, AI Coding Assistant / Agent. This document defines the **supreme architectural rules, coding standards, and operational workflows** for the `LLM-EdgeFlow` repository.

Whenever you work on this codebase, you **MUST** strictly adhere to the guidelines below.

---

## 🏛️ 1. 4-Tier Architectural Isolation Directives

`LLM-EdgeFlow` is organized into 4 strict, non-negotiable architectural layers. You must never violate dependency directions:

```text
Layer 1: Pure C ABI Adapter (company_c_adapter.cpp)
    │  ▲  (Unpacks C structs to AlgContext, invokes Pipeline, packs results)
    ▼  │
Layer 2: Pipeline & Dynamic Blackboard (Pipeline, AlgContext, SessionContext)
    │
    ▼
Layer 3: Pluggable Business & Common Nodes (INode, REGISTER_NODE)
    │
    ▼
Layer 4: Heterogeneous Inference Engines & Batch Schedulers (FixedBatchExecutor, IModelEngine)
```

1. **Layer 1 (C ABI Safety Barrier)**:
   - Target files: `include/company_alg_interface.h`, `src/adapter/company_c_adapter.cpp`.
   - **Rule**: All 6 exported C functions (`Alg_Init`, `Alg_Create`, `Alg_Process`, `Alg_Control`, `Alg_Destroy`, `Alg_DeInit`) **MUST** be wrapped in `noexcept` and standard `try-catch` blocks to prevent downstream host crashes.
   - **Rule**: Never expose C++ STL types, `std::string`, `std::vector`, or third-party headers in `include/company_alg_interface.h`. Only pure C structs and primitive types are allowed.
2. **Layer 2 (Pipeline & Blackboard)**:
   - Target files: `include/core/alg_context.h`, `include/core/session_context.h`, `include/core/pipeline.h`.
   - **Rule**: Nodes communicate strictly via the request-scoped blackboard `AlgContext`. Zero unnecessary memory copying.
3. **Layer 3 (Business & Common Nodes)**:
   - Target files: `src/business/<biz_name>/`, `src/common_nodes/`.
   - **Rule**: All nodes must inherit from `INode` and register via `REGISTER_NODE(NodeClassName)`. Nodes must be stateless regarding individual requests; request data is stored in `AlgContext`.
4. **Layer 4 (Engines & Hardware Acceleration)**:
   - Target files: `include/engine/engine_interface.h`, `include/engine/fixed_batch_executor.h`, `src/engine/`.
   - **Rule**: All batch inference implementations **MUST** utilize `FixedBatchExecutor::Execute` for automatic hardware padding, dummy stripping, and sample provenance tracking `(req_id, sub_id)`.

---

## 🛠️ 2. Mandatory Skill Invocations

When performing specific tasks, you **MUST** reference and execute the corresponding project skills:

1. **When Extending the Framework (New Modality, Node, or Engine)**:
   - Read and follow: [`.agents/skills/llm-edgeflow-developer-guide/SKILL.md`](.agents/skills/llm-edgeflow-developer-guide/SKILL.md)
   - Follow the step-by-step checklists for Layer 1, Layer 2, Layer 3, and Layer 4.
2. **When Uploading Changes to GitHub / Merging Code**:
   - Read and follow: [`.agents/skills/github-branch-merge/SKILL.md`](.agents/skills/github-branch-merge/SKILL.md)
   - **Mandatory**: Use `./scripts/git_branch_upload.sh "<commit message>" "<type>"` to enforce branch creation, local test gating, PR creation, and automated merge.
   - **Never push directly to `main` without branch isolation!**

---

## 🔒 3. Testing, Quality & Git Hygiene Directives

1. **Mandatory Full Test Gate (100% Pass Requirement)**:
   - Before completing any feature or bugfix, you **MUST** run and pass all 7 CTest suites:
     ```bash
     cd build && ctest --output-on-failure
     ```
   - Run the full 6-stage regression test suite:
     ```bash
     ./scripts/run_all_tests.sh
     ```
2. **Google C++ Code Formatting**:
   - Always run `./scripts/format.sh` before committing to adhere to Google C++ Style (`.clang-format`).
3. **Zero Third-Party Bundling**:
   - **DO NOT** commit precompiled binaries, `.so` libraries, or third-party source files into the repository.
   - All third-party dependencies (GoogleTest, nlohmann/json, ONNX Runtime, llama.cpp) **MUST** be fetched dynamically via CMake `FetchContent` in `cmake/*.cmake`.
4. **Changelog Maintenance**:
   - When introducing major architectural changes, new C ABI modalities, or new hardware backends, update `## 📝 更新日志 (Changelog)` in `README.md`.

---

## 🚀 4. Quick Verification Commands

```bash
# 1. Format code
./scripts/format.sh

# 2. Build and run all CTest suites
mkdir -p build && cd build && cmake .. && make -j4 && ctest --output-on-failure

# 3. Run full end-to-end multi-modal demo
./build/alg_demo

# 4. Standardized Git Branch & Merge Upload
./scripts/git_branch_upload.sh "feat(scope): descriptive message" "feat"
```
