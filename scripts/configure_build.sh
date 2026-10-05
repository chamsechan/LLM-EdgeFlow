#!/usr/bin/env bash
set -euo pipefail

# 应用原生 CMake preset，同时保留已有构建目录的生成器。
if [[ $# -lt 3 ]]; then
  echo "Usage: $0 <source-dir> <build-dir> <preset> [cmake options...]" >&2
  exit 2
fi
SOURCE_DIR="$1"
BUILD_DIR="$2"
PRESET="$3"
shift 3

if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  GENERATOR=$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$BUILD_DIR/CMakeCache.txt")
  if [[ -z "$GENERATOR" ]]; then
    echo "Missing CMake generator in $BUILD_DIR/CMakeCache.txt" >&2
    exit 1
  fi
elif command -v ninja >/dev/null 2>&1; then
  GENERATOR=Ninja
else
  GENERATOR="Unix Makefiles"
fi

exec cmake --preset="$PRESET" -S "$SOURCE_DIR" -B "$BUILD_DIR" \
  -G "$GENERATOR" "$@"
