#!/usr/bin/env bash
set -euo pipefail

# Helper function to safely determine CMake generator arguments
detect_cmake_generator_args() {
  local target_build_dir="${1:-.}"
  if [[ ! -f "${target_build_dir}/CMakeCache.txt" ]]; then
    if command -v ninja >/dev/null 2>&1; then
      echo "-G Ninja"
    fi
  fi
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
  detect_cmake_generator_args "${1:-.}"
fi
