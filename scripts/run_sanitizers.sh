#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SANITIZERS="${LLM_EDGEFLOW_SANITIZERS:-address,undefined}"

MODE="fast"
if [[ $# -gt 1 ]]; then
  echo "Usage: $0 [--fast | --full]"
  exit 2
fi
if [[ $# -eq 1 ]]; then
  case "$1" in
    --fast) MODE="fast" ;;
    --full) MODE="full" ;;
    *)
      echo "Usage: $0 [--fast | --full]"
      exit 2
      ;;
  esac
fi
BUILD_DIR_TAG="${SANITIZERS//,/-}"
BUILD_DIR="${PROJECT_ROOT}/build-sanitizers-${BUILD_DIR_TAG}-${MODE}"

DETECT_LEAKS="${DETECT_LEAKS:-0}"

echo "=================================================="
echo " Running Clang/GCC Sanitizer Suite (Mode: ${MODE})"
echo " Project Root: ${PROJECT_ROOT}"
echo " Sanitizers: ${SANITIZERS}"
if [[ "${SANITIZERS}" == *"thread"* ]]; then
  echo " Note: ThreadSanitizer data race detection enabled."
elif [[ "${SANITIZERS}" == "address" ]]; then
  echo " Note: AddressSanitizer memory safety checks enabled."
  if [[ "${DETECT_LEAKS}" == "1" ]]; then
    echo "       LeakSanitizer enabled (detect_leaks=1)."
  else
    echo "       LeakSanitizer disabled (detect_leaks=0)."
  fi
elif [[ "${SANITIZERS}" == "undefined" ]]; then
  echo " Note: UndefinedBehaviorSanitizer checks enabled."
elif [[ "${SANITIZERS}" == *"address"* ]] && [[ "${SANITIZERS}" == *"undefined"* ]]; then
  echo " Note: ASan/UBSan memory safety and UB checks enabled."
  if [[ "${DETECT_LEAKS}" == "1" ]]; then
    echo "       LeakSanitizer enabled (detect_leaks=1)."
  else
    echo "       LeakSanitizer disabled (detect_leaks=0)."
  fi
fi
echo "=================================================="

COMMON_CMAKE_ARGS=(
  -DCMAKE_BUILD_TYPE=Debug
  -DENABLE_SANITIZERS=ON
  -DLLM_EDGEFLOW_SANITIZERS="${SANITIZERS}"
  -DLLM_EDGEFLOW_SHARDED_TEST_RUNNERS=ON
  -DLLM_EDGEFLOW_LINKER="${LLM_EDGEFLOW_LINKER:-auto}"
)
# Reuse already-fetched source trees when available. This keeps sanitizer
# builds deterministic in restricted/offline development environments while
# preserving FetchContent's normal download behavior on a clean checkout.
if [[ -d "${PROJECT_ROOT}/build/_deps/nlohmann_json-src" ]]; then
  COMMON_CMAKE_ARGS+=(
    -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON="${PROJECT_ROOT}/build/_deps/nlohmann_json-src"
  )
fi
if [[ -d "${PROJECT_ROOT}/build/_deps/googletest-src" ]]; then
  COMMON_CMAKE_ARGS+=(
    -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="${PROJECT_ROOT}/build/_deps/googletest-src"
  )
fi
GEN_ARG_STR=$("${SCRIPT_DIR}/detect_cmake_generator.sh" "${BUILD_DIR}")
if [[ -n "${GEN_ARG_STR}" ]]; then
  read -r -a GENERATOR_ARGS <<< "${GEN_ARG_STR}"
  COMMON_CMAKE_ARGS+=("${GENERATOR_ARGS[@]}")
fi
if [[ "${MODE}" == "fast" ]]; then
  COMMON_CMAKE_ARGS+=(
    -DENABLE_LLAMACPP=OFF
    -DENABLE_ONNXRUNTIME=OFF
    -DENABLE_REAL_MODEL_TESTS=OFF
  )
else
  COMMON_CMAKE_ARGS+=(
    -DENABLE_LLAMACPP=ON
    -DENABLE_ONNXRUNTIME=ON
    -DENABLE_REAL_MODEL_TESTS=OFF
  )
fi

if command -v ccache >/dev/null 2>&1; then
  export CCACHE_DIR="${PROJECT_ROOT}/build/.ccache-sanitizers"
  mkdir -p "${CCACHE_DIR}"
fi

NCPU="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" "${COMMON_CMAKE_ARGS[@]}"
if [[ "${MODE}" == "fast" ]]; then
  cmake --build "${BUILD_DIR}" --target edgeflow_dev_tests -j"${NCPU}"
else
  cmake --build "${BUILD_DIR}" -j"${NCPU}"
fi

if [[ "${SANITIZERS}" == *"thread"* ]]; then
  export TSAN_OPTIONS="halt_on_error=1:abort_on_error=1"
else
  export ASAN_OPTIONS="detect_leaks=${DETECT_LEAKS}:abort_on_error=1"
  if [[ "${SANITIZERS}" == *"address"* ]]; then
    LIBASAN_PATH="$(gcc -print-file-name=libasan.so 2>/dev/null || true)"
    if [[ -f "${LIBASAN_PATH}" ]]; then
      export LD_PRELOAD="${LIBASAN_PATH}${LD_PRELOAD:+:$LD_PRELOAD}"
    fi
  fi
  UBSAN_SUPP="${SCRIPT_DIR}/ubsan_suppressions.txt"
  if [[ -f "${UBSAN_SUPP}" ]]; then
    export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:suppressions=${UBSAN_SUPP}"
  else
    export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"
  fi
fi

export LD_LIBRARY_PATH="${PROJECT_ROOT}/3rdparty/onnxruntime/lib:${BUILD_DIR}:${BUILD_DIR}/_deps/onnxruntime_prebuilt-src/lib:${LD_LIBRARY_PATH:-}"
export DYLD_LIBRARY_PATH="${PROJECT_ROOT}/3rdparty/onnxruntime/lib:${BUILD_DIR}:${BUILD_DIR}/_deps/onnxruntime_prebuilt-src/lib:${DYLD_LIBRARY_PATH:-}"
export LLM_EDGEFLOW_PIPELINE_TOOL="${BUILD_DIR}/alg_pipeline_tool"
export LLM_EDGEFLOW_DEMO_BINARY="${BUILD_DIR}/alg_demo"

ARCH_PREFIX=()
if [[ "${SANITIZERS}" == *"thread"* ]] && [[ "$(uname -s)" == "Linux" ]] && [[ "$(uname -m)" == "aarch64" ]]; then
  ARCH_PREFIX=(setarch "$(uname -m)" -R)
fi

CTEST_ARGS=(
  --test-dir "${BUILD_DIR}"
  -j"${NCPU}"
  --output-on-failure
)
if [[ "${MODE}" == "fast" ]]; then
  CTEST_ARGS+=( -L sanitizer-compatible )
  echo ">>> Running label-driven fast sanitized test suite <<<"
else
  echo ">>> Running full sanitized CTest suite with [${SANITIZERS}] <<<"
fi
if [[ "${DETECT_LEAKS:-0}" == "1" ]] || [[ "${SANITIZERS}" == *"thread"* ]]; then
  CTEST_ARGS+=( -E '^VisualizerServerTest$' )
fi
if [[ ${#ARCH_PREFIX[@]} -gt 0 ]]; then
  "${ARCH_PREFIX[@]}" ctest "${CTEST_ARGS[@]}"
else
  ctest "${CTEST_ARGS[@]}"
fi

echo "=================================================="
echo " 🎉 Sanitizer set [${SANITIZERS}] checks PASSED! (Mode: ${MODE})"
echo "=================================================="
