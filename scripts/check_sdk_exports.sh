#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 || ! -f "$1" ]]; then
  echo "Usage: $0 /path/to/libcompany_alg_sdk.so" >&2
  exit 2
fi

SDK_LIBRARY="$1"
if ! command -v nm >/dev/null 2>&1; then
  echo "nm is required to validate the SDK export surface" >&2
  exit 2
fi

EXPECTED_SYMBOLS=$(
  printf '%s\n' \
    AlgBase_getLogLevelByName \
    AlgBase_logPrint \
    AlgBase_setLogLevelByName \
    Alg_Control \
    Alg_Create \
    Alg_DeInit \
    Alg_Destroy \
    Alg_Init \
    Alg_Process \
    _ZN12llm_edgeflow12operator_api20GetOperatorLastErrorEv \
    _ZN12llm_edgeflow12operator_api29ValidateOperatorConfigBindingEPKcS2_iPcm \
    _ZN12llm_edgeflow12operator_api30Get_LLM_EDGEFLOW_OperatorTableEv
)

ACTUAL_SYMBOLS=$(
  nm -D --defined-only "${SDK_LIBRARY}" |
    awk '$2 != "A" {print $3}' |
    sed 's/@.*//' |
    sed '/^$/d' |
    sort -u
)

if [[ "${ACTUAL_SYMBOLS}" != "${EXPECTED_SYMBOLS}" ]]; then
  echo "SDK export surface differs from the 12-symbol allowlist:" >&2
  diff -u <(printf '%s\n' "${EXPECTED_SYMBOLS}") \
          <(printf '%s\n' "${ACTUAL_SYMBOLS}") >&2 || true
  exit 1
fi

echo "SDK export surface is restricted to 12 supported symbols."
