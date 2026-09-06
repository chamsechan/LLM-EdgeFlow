#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
MODEL_DIR="${PROJECT_ROOT}/models"
MODE="all"

if [[ $# -gt 1 ]]; then
  echo "Usage: $0 [--all | --gguf-only | --kite | --whisper]"
  exit 2
fi
if [[ $# -eq 1 ]]; then
  case "$1" in
    --all) MODE="all" ;;
    --gguf-only) MODE="gguf-only" ;;
    --kite) MODE="kite" ;;
    --whisper) MODE="whisper" ;;
    *)
      echo "Usage: $0 [--all | --gguf-only | --kite | --whisper]"
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

# File names, hashes and upstream URLs share the selection manifest.
MANIFEST_ROWS="$(mktemp)"
trap 'rm -f "$MANIFEST_ROWS"' EXIT
python3 - "$PROJECT_ROOT/models/asset_manifest.json" "$MODE" > "$MANIFEST_ROWS" <<'MANIFEST'
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
echo "Pinned real-model artifact verification completed (${MODE})."
