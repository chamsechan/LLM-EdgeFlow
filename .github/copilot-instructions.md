# GitHub Copilot Custom Instructions for LLM-EdgeFlow

Please refer to `AGENTS.md` and `CLAUDE.md` for project architecture, 4-tier layer constraints, and test gate requirements.

- Always format C++ code with `./scripts/format.sh` (Google C++ Style).
- Ensure all 7 CTest suites pass via `ctest --output-on-failure`.
- Pure C ABI functions in Layer 1 must never leak C++ STL exceptions.
