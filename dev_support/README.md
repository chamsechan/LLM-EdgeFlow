# Development runtime support

This directory owns deterministic runtime implementations used by mock Demo profiles and tests.
They may register test Model or Backend Definitions, but they must never be linked into the
production `alg_sdk` shared library or referenced by real deployment profiles.

Reusable helpers that do not register runtime capabilities remain under `tests/support/`.

`node_authoring/starter_llm_node.cpp` is readable input to the custom Node scaffolder. It is not
linked into any production target here. Generated copies are compiled in the existing Node test
runner; authors generate their own named copy under `src/custom_nodes/` to include it in a solution.
The introductory walkthrough compiles its two documented business function bodies in those tests.
