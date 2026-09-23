# Development runtime support

This directory owns development fixtures, Node authoring templates and focused benchmarks.
`inference/` provides deterministic runtime implementations used by mock Demo profiles and tests.
They may register test Model or Backend Definitions, but they must never be linked into the
production `alg_sdk` shared library or referenced by real deployment profiles.

Reusable helpers that do not register runtime capabilities remain under `tests/support/`.

`node_authoring/starter_llm_node.cpp` and `starter_control_node.cpp` are inputs to the custom Node
scaffolder. Generated copies are compiled in the existing test runners; authors generate their
own named copy under `src/custom_nodes/` to include it in a solution. The introductory LLM
walkthrough compiles its two documented business function bodies in those tests.

The other `starter_*.cpp` files demonstrate batch operations and multiple-model use. The Node
runner compiles these examples directly. No starter file is linked into a production target here.

`benchmarks/` and `node_authoring/benchmark/` contain opt-in development measurements. They are
separate from the default runtime and correctness suites.
