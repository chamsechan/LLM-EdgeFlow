#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
MODEL_DIR="${PROJECT_ROOT}/configs"
MODE="all"

if [[ $# -gt 1 ]]; then
  echo "Usage: $0 [--all | --gguf-only | --kite | --whisper | --whisper-e2e | --onnx-llm]"
  exit 2
fi
if [[ $# -eq 1 ]]; then
  case "$1" in
    --all) MODE="all" ;;
    --gguf-only) MODE="gguf-only" ;;
    --kite) MODE="kite" ;;
    --whisper) MODE="whisper" ;;
    --whisper-e2e) MODE="whisper-e2e" ;;
    --onnx-llm) MODE="onnx-llm" ;;
    *)
      echo "Usage: $0 [--all | --gguf-only | --kite | --whisper | --whisper-e2e | --onnx-llm]"
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

# 文件名、哈希和上游 URL 共用同一份选型清单。
MANIFEST_ROWS="$(mktemp)"
trap 'rm -f "$MANIFEST_ROWS"' EXIT
python3 - "$PROJECT_ROOT/configs/asset_manifest.json" "$MODE" > "$MANIFEST_ROWS" <<'MANIFEST'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as stream:
    manifest = json.load(stream)
for filename, artifact in manifest["artifacts"].items():
    if sys.argv[2] in artifact.get("download_groups", []):
        print(filename, artifact["sha256"], artifact["url"], sep="\t")
MANIFEST
while IFS=$'\t' read -r filename sha url; do
  download_verified "$filename" "$sha" "$url"
done < "$MANIFEST_ROWS"

# 由下载的分词器源文件生成 <模型>.eftok，并核对清单中记录的 SHA-256。
# 生成工具读取 <模型>-tokenizer.json、<模型>-generation_config.json 和 Unicode 数据；
# 目录里有 <模型>-tokenizer.model 时一并传入，由工具按它的规则选择合并优先级。
# 用 EDGEFLOW_TOKENIZER_PYTHON 指定已安装 tokenizers==0.23.3 的 Python 解释器（默认 python3）。
generate_tokenizer() {
  local model="$1"
  local filename="${model}.eftok"
  local expected_sha
  expected_sha="$(python3 - "${PROJECT_ROOT}/configs/asset_manifest.json" "${filename}" <<'MANIFEST'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as stream:
    print(json.load(stream)["artifacts"][sys.argv[2]]["sha256"])
MANIFEST
)"
  local target="${MODEL_DIR}/${filename}"
  if [[ -f "${target}" && "$(sha256_file "${target}")" == "${expected_sha}" ]]; then
    echo "✓ ${filename} (${expected_sha})"
    return 0
  fi

  local arguments=(
    compile
    --tokenizer-json "${MODEL_DIR}/${model}-tokenizer.json"
    --generation-config "${MODEL_DIR}/${model}-generation_config.json"
    --unicode-data "${MODEL_DIR}/unicode-9.0.0-UnicodeData.txt"
  )
  if [[ -f "${MODEL_DIR}/${model}-tokenizer.model" ]]; then
    arguments+=(--sentencepiece-model "${MODEL_DIR}/${model}-tokenizer.model")
  fi
  local temporary
  temporary="$(mktemp "${MODEL_DIR}/.${filename}.partial.XXXXXX")"
  if ! "${EDGEFLOW_TOKENIZER_PYTHON:-python3}" -I \
      "${PROJECT_ROOT}/tools/prepare_bpe_tokenizer.py" \
      "${arguments[@]}" --output "${temporary}" >/dev/null; then
    rm -f "${temporary}" "${temporary}.tmp"
    echo "Failed to generate ${filename}" >&2
    return 1
  fi
  local actual_sha
  actual_sha="$(sha256_file "${temporary}")"
  if [[ "${actual_sha}" != "${expected_sha}" ]]; then
    rm -f "${temporary}"
    echo "SHA-256 mismatch for ${filename}: expected ${expected_sha}, got ${actual_sha}" >&2
    return 1
  fi
  mv -f "${temporary}" "${target}"
  echo "✓ ${filename} (${expected_sha})"
}

if [[ "${MODE}" == "onnx-llm" ]]; then
  # 清单中由生成工具产出的分词器文件，文件名为 <模型>.eftok。
  GENERATED_MODELS="$(python3 - "${PROJECT_ROOT}/configs/asset_manifest.json" <<'MANIFEST'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as stream:
    manifest = json.load(stream)
for filename, artifact in manifest["artifacts"].items():
    if filename.endswith(".eftok") and artifact.get("source", "").startswith("generated by tools/prepare_bpe_tokenizer.py"):
        print(filename[: -len(".eftok")])
MANIFEST
)"
  while IFS= read -r model; do
    [[ -n "${model}" ]] && generate_tokenizer "${model}"
  done <<< "${GENERATED_MODELS}"
fi
echo "Pinned real-model artifact verification completed (${MODE})."
