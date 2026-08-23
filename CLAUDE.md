# CLAUDE.md - Development Guide for LLM-EdgeFlow

Welcome Claude Code. This document outlines project commands, architecture constraints, and workflows for `LLM-EdgeFlow`.

## 🛠️ Common Build & Test Commands

- **Build**: `mkdir -p build && cd build && cmake .. && make -j4`
- **Format Code**: `./scripts/format.sh` (Google C++ Style)
- **Run All CTest Suites**: `cd build && ctest --output-on-failure`
- **Run Full Regression Suite**: `./scripts/run_all_tests.sh`
- **Run Multi-Modal Demo**: `./build/alg_demo`
- **Git Branch & PR Upload**: `./scripts/git_branch_upload.sh "<commit message>" "<type>"`

---

## 🏛️ Architecture & Isolation Directives (Strict)

`LLM-EdgeFlow` is structured into 4 isolated architectural layers. Never violate dependency directions:

1. **Layer 1: Pure C ABI (`include/company_alg_interface.h`, `src/adapter/company_c_adapter.cpp`)**:
   - All 6 exported C functions (`Alg_Init`, `Alg_Create`, `Alg_Process`, `Alg_Control`, `Alg_Destroy`, `Alg_DeInit`) **MUST** be wrapped in `noexcept` and `try-catch`.
   - Never expose C++ STL types, `std::string`, `std::vector`, or third-party headers in `include/company_alg_interface.h`.
2. **Layer 2: Pipeline & Dynamic Blackboard (`include/core/alg_context.h`, `pipeline.h`)**:
   - Nodes communicate strictly via request-scoped `AlgContext` with zero unnecessary memory copies.
3. **Layer 3: Pluggable Business & Common Nodes (`src/business/`, `src/common_nodes/`)**:
   - Inherit from `INode` and register via `REGISTER_NODE(NodeClassName)`.
4. **Layer 4: Heterogeneous Inference Engines (`include/engine/`, `src/engine/`)**:
   - Batch inference implementations **MUST** utilize `FixedBatchExecutor::Execute` for automatic hardware padding, dummy stripping, and sample provenance tracking `(req_id, sub_id)`.

---

## 🔒 Mandatory Workflow Directives

1. **RFC-First Design Governance (`doc/rfcs/`)**:
   - All pending features, new requirements, and architectural changes must first have an RFC created under `doc/rfcs/NNNN-<name>.md` following [`doc/rfcs/RFC_TEMPLATE.md`](doc/rfcs/RFC_TEMPLATE.md).
   - Once implemented in a feature branch and verified with 100% tests, update RFC status to `Completed` before PR merge to `main`.
2. **Mandatory Full Test Gate & Zero-Untested-Code Directive**:
   - Any newly added business pipeline, common node, or inference engine **MUST** have a corresponding Google Test created under `tests/test_<name>.cpp` and registered via `add_test(...)` in `CMakeLists.txt`.
   - All CTest suites (`ctest --output-on-failure`) and `./scripts/run_all_tests.sh` MUST pass 100% before committing or completing a task.
3. **Standard Branch-and-Merge**:
   - Never push directly to `main`. Use `./scripts/git_branch_upload.sh` to create feature branches and merge via PR.
4. **Zero Third-Party Bundling**:
   - All external dependencies (GoogleTest, nlohmann/json, ONNX Runtime, llama.cpp) must be fetched dynamically via CMake `FetchContent`. Never commit `.so` or third-party headers.
5. **Skill References**:
   - RFC Governance & Index: [`doc/rfcs/README.md`](doc/rfcs/README.md)
   - Pipeline Composition & Reusability: [`.agents/skills/pipeline-composer/SKILL.md`](.agents/skills/pipeline-composer/SKILL.md)
   - 4-Tier Extension Guide: [`.agents/skills/llm-edgeflow-developer-guide/SKILL.md`](.agents/skills/llm-edgeflow-developer-guide/SKILL.md)
   - Git Branch Merge: [`.agents/skills/github-branch-merge/SKILL.md`](.agents/skills/github-branch-merge/SKILL.md)
