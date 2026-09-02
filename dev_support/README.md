# Development runtime support

This directory owns deterministic runtime implementations used by mock Demo profiles and tests.
They may register test Model or Backend Definitions, but they must never be linked into the
production `alg_sdk` shared library or referenced by real deployment profiles.

Reusable helpers that do not register runtime capabilities remain under `tests/support/`.
