#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
MODEL_DIR="${PROJECT_ROOT}/models"
MODE="all"

if [[ $# -gt 1 ]]; then
  echo "Usage: $0 [--all | --gguf-only]"
  exit 2
fi
if [[ $# -eq 1 ]]; then
  case "$1" in
    --all) MODE="all" ;;
    --gguf-only) MODE="gguf-only" ;;
    *)
      echo "Usage: $0 [--all | --gguf-only]"
      exit 2
      ;;
  esac
fi

mkdir -p "${MODEL_DIR}"

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    echo "No SHA-256 utility found (need sha256sum or shasum)" >&2
    return 1
  fi
}

download_verified() {
  local filename="$1"
  local expected_sha="$2"
  local url="$3"
  local target="${MODEL_DIR}/${filename}"
  local actual_sha=""

  if [[ -f "${target}" ]]; then
    actual_sha="$(sha256_file "${target}")"
    if [[ "${actual_sha}" == "${expected_sha}" ]]; then
      echo "✓ ${filename} (${expected_sha})"
      return 0
    fi
    echo "Existing ${filename} has SHA-256 ${actual_sha}; replacing it."
  fi

  local temporary
  temporary="$(mktemp "${MODEL_DIR}/.${filename}.partial.XXXXXX")"
  if command -v curl >/dev/null 2>&1; then
    curl -fL --retry 3 --retry-delay 2 "${url}" -o "${temporary}"
  elif command -v wget >/dev/null 2>&1; then
    wget -O "${temporary}" "${url}"
  else
    rm -f "${temporary}"
    echo "Neither curl nor wget is available" >&2
    return 1
  fi

  actual_sha="$(sha256_file "${temporary}")"
  if [[ "${actual_sha}" != "${expected_sha}" ]]; then
    rm -f "${temporary}"
    echo "SHA-256 mismatch for ${filename}: expected ${expected_sha}, got ${actual_sha}" >&2
    return 1
  fi
  mv -f "${temporary}" "${target}"
  echo "✓ ${filename} (${expected_sha})"
}

echo "Preparing pinned real-model artifacts in ${MODEL_DIR}"

download_verified \
  "qwen2.5-0.5b-instruct-q4_k_m.gguf" \
  "74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db" \
  "https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/9217f5db79a29953eb74d5343926648285ec7e67/qwen2.5-0.5b-instruct-q4_k_m.gguf"

if [[ "${MODE}" == "all" ]]; then
  download_verified \
    "bge_base_zh_v1.5.onnx" \
    "5e5619f7cca7380b824d329c157dba10bee7cc00d0c139e82fdb7906051b8e4f" \
    "https://huggingface.co/Xenova/bge-base-zh-v1.5/resolve/71e50dc531959f9e04ebf190ea25b00261a0a186/onnx/model.onnx"
  download_verified \
    "bge_base_zh_v1.5_vocab.txt" \
    "45bbac6b341c319adc98a532532882e91a9cefc0329aa57bac9ae761c27b291c" \
    "https://huggingface.co/Xenova/bge-base-zh-v1.5/resolve/71e50dc531959f9e04ebf190ea25b00261a0a186/vocab.txt"
  download_verified \
    "ms_marco_tinybert_l2_v2_quantized.onnx" \
    "026c2ec3257cd351696e45bbd6040bb83cf818ba89059b4344bd6350138b62ce" \
    "https://huggingface.co/Xenova/ms-marco-TinyBERT-L-2-v2/resolve/b76bb5e1fefd66aa36cd108622d768e86c015ff1/onnx/model_quantized.onnx"
  download_verified \
    "ms_marco_bert_vocab.txt" \
    "07eced375cec144d27c900241f3e339478dec958f92fddbc551f295c992038a3" \
    "https://huggingface.co/Xenova/ms-marco-TinyBERT-L-2-v2/resolve/b76bb5e1fefd66aa36cd108622d768e86c015ff1/vocab.txt"
fi

echo "Pinned real-model artifact verification completed (${MODE})."
