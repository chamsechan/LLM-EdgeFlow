#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 || ! -f "$1" ]]; then
  echo "Usage: $0 <SDK shared-library path> <ELF|MachO> <target-nm>" >&2
  exit 2
fi

SDK_LIBRARY="$1"
FORMAT="$2"
NM="$3"
if ! command -v "$NM" >/dev/null 2>&1; then
  echo "nm is required to validate the SDK export surface" >&2
  exit 2
fi

# 独立的公开 ABI 基线，不能从生成链接限制的清单读取预期。
EXPECTED_SYMBOLS=$(
  printf '%s\n' \
    AlgBase_getLogLevelByName \
    AlgBase_setLogLevelByName \
    _ZN12llm_edgeflow12operator_api20GetOperatorLastErrorEv \
    _ZN12llm_edgeflow12operator_api23ResolveOperatorConfigIoEPKcS2_PNS0_18OperatorIoContractEPcm \
    _ZN12llm_edgeflow12operator_api30Get_LLM_EDGEFLOW_OperatorTableEv |
    sort -u
)

case "$FORMAT" in
  ELF)
    ACTUAL_SYMBOLS=$("$NM" -D --defined-only "${SDK_LIBRARY}" |
      awk '$2 != "A" {print $3}' | sed 's/@.*//' | sed '/^$/d' | sort -u)
    ;;
  MachO)
    ACTUAL_SYMBOLS=$("$NM" -gjU "${SDK_LIBRARY}" | sed 's/^_//' | sed '/^$/d' | sort -u)
    ;;
  *) echo "Unsupported binary format: $FORMAT" >&2; exit 2 ;;
esac

if [[ "${ACTUAL_SYMBOLS}" != "${EXPECTED_SYMBOLS}" ]]; then
  echo "SDK export surface differs from the supported Operator ABI:" >&2
  diff -u <(printf '%s\n' "${EXPECTED_SYMBOLS}") \
          <(printf '%s\n' "${ACTUAL_SYMBOLS}") >&2 || true
  exit 1
fi

echo "SDK export surface matches the supported Operator ABI."
