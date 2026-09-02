#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"

TEST_TMP_DIR=$(mktemp -d /tmp/test_generator_detection_XXXXXX)
trap 'rm -rf "${TEST_TMP_DIR}"' EXIT

echo "[Test] 1. Fresh directory with Ninja available"
mkdir -p "${TEST_TMP_DIR}/fresh_dir"
NINJA_BIN="${TEST_TMP_DIR}/ninja_bin"
mkdir -p "${NINJA_BIN}"
ln -s "$(type -P true)" "${NINJA_BIN}/ninja"
RES1=$(PATH="${NINJA_BIN}:${PATH}" "${PROJECT_ROOT}/scripts/detect_cmake_generator.sh" "${TEST_TMP_DIR}/fresh_dir")
if [[ "${RES1}" != "-G Ninja" ]]; then
  echo "FAILED: Expected '-G Ninja', got '${RES1}'"
  exit 1
fi

echo "[Test] 2. Existing build directory with Unix Makefiles CMakeCache.txt"
mkdir -p "${TEST_TMP_DIR}/make_dir"
echo "CMAKE_GENERATOR:INTERNAL=Unix Makefiles" > "${TEST_TMP_DIR}/make_dir/CMakeCache.txt"
RES2=$("${PROJECT_ROOT}/scripts/detect_cmake_generator.sh" "${TEST_TMP_DIR}/make_dir")
if [[ -n "${RES2}" ]]; then
  echo "FAILED: Expected empty generator arg for existing Makefiles, got '${RES2}'"
  exit 1
fi

echo "[Test] 3. Existing build directory with Ninja CMakeCache.txt"
mkdir -p "${TEST_TMP_DIR}/ninja_dir"
echo "CMAKE_GENERATOR:INTERNAL=Ninja" > "${TEST_TMP_DIR}/ninja_dir/CMakeCache.txt"
RES3=$("${PROJECT_ROOT}/scripts/detect_cmake_generator.sh" "${TEST_TMP_DIR}/ninja_dir")
if [[ -n "${RES3}" ]]; then
  echo "FAILED: Expected empty generator arg for existing Ninja cache, got '${RES3}'"
  exit 1
fi

echo "[Test] 4. PATH without ninja on a fresh directory"
MOCK_BIN="${TEST_TMP_DIR}/mock_bin"
mkdir -p "${MOCK_BIN}"
for cmd in bash sh env dirname; do
  target_cmd=$(command -v "$cmd" || true)
  if [[ -n "${target_cmd}" ]]; then
    ln -sf "${target_cmd}" "${MOCK_BIN}/${cmd}"
  fi
done

RES4=$(PATH="${MOCK_BIN}" "${MOCK_BIN}/bash" "${PROJECT_ROOT}/scripts/detect_cmake_generator.sh" "${TEST_TMP_DIR}/fresh_no_ninja")
if [[ -n "${RES4}" ]]; then
  echo "FAILED: Expected empty output when ninja is not in PATH, got '${RES4}'"
  exit 1
fi

echo "[Test] 5. Idempotent repeated runs"
for i in {1..5}; do
  RES5=$("${PROJECT_ROOT}/scripts/detect_cmake_generator.sh" "${TEST_TMP_DIR}/make_dir")
  if [[ -n "${RES5}" ]]; then
    echo "FAILED: Iteration $i failed"
    exit 1
  fi
done

echo "ALL GENERATOR DETECTION TESTS PASSED 100%!"
