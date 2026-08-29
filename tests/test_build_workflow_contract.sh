#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_MATRIX="${PROJECT_ROOT}/cmake/Tests.cmake"
TEST_SCRIPT="${PROJECT_ROOT}/scripts/run_all_tests.sh"
ENGINE_DEPS="${PROJECT_ROOT}/cmake/ThirdPartyEngines.cmake"
LLAMA_PATCH="${PROJECT_ROOT}/cmake/PatchLlamaCppBuildInfo.cmake"
JSON_DEPS="${PROJECT_ROOT}/cmake/NlohmannJson.cmake"
GTEST_DEPS="${PROJECT_ROOT}/cmake/GoogleTest.cmake"

DEV_TARGET_BLOCK=$(sed -n \
  '/add_custom_target(edgeflow_dev_tests DEPENDS/,/edgeflow_test_tooling_runner)/p' \
  "${TEST_MATRIX}")
if ! grep -q 'alg_pipeline_tool_test' <<< "${DEV_TARGET_BLOCK}"; then
  echo "FAILED: edgeflow_dev_tests must build alg_pipeline_tool_test"
  exit 1
fi

if ! grep -q -- '--fast is a compatibility alias for --quick' "${TEST_SCRIPT}"; then
  echo "FAILED: legacy --fast alias contract is missing"
  exit 1
fi
if ! grep -q 'BUILD_DIR="\$ROOT_DIR/build-minimal"' "${TEST_SCRIPT}"; then
  echo "FAILED: minimal mode must use an isolated build directory"
  exit 1
fi
if [[ $(grep -c 'BUILD_DIR="\$ROOT_DIR/build"' "${TEST_SCRIPT}") -ne 1 ]]; then
  echo "FAILED: quick and full must share the canonical build directory"
  exit 1
fi

if grep -q 'refs/heads/master' "${ENGINE_DEPS}"; then
  echo "FAILED: third-party dependencies must not track a floating branch"
  exit 1
fi
if [[ $(grep -c 'URL_HASH SHA256=' "${ENGINE_DEPS}") -lt 2 ]]; then
  echo "FAILED: engine archives must be protected by SHA256"
  exit 1
fi
if ! grep -q 'deterministic FetchContent build metadata' "${LLAMA_PATCH}"; then
  echo "FAILED: pinned llama.cpp must not embed the host repository revision"
  exit 1
fi
for dependency_file in "${JSON_DEPS}" "${GTEST_DEPS}"; do
  if ! grep -q 'URL_HASH SHA256=' "${dependency_file}"; then
    echo "FAILED: every FetchContent archive must be protected by SHA256"
    exit 1
  fi
done

echo "BUILD WORKFLOW CONTRACT TESTS PASSED 100%!"
