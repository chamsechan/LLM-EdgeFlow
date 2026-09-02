#!/usr/bin/env bash

set -euo pipefail

if [[ $# -lt 4 || $# -gt 5 ]]; then
  echo "Usage: $0 OUTPUT_JSON CANONICAL_GATE SANITIZER_GATE REAL_GATE [SCOPE]"
  exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUTPUT_JSON="$1"
CANONICAL_GATE="$2"
SANITIZER_GATE="$3"
REAL_GATE="$4"
EVIDENCE_SCOPE="${5:-project-quality-gate}"

sha256_stream() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum | awk '{print $1}'
  else
    shasum -a 256 | awk '{print $1}'
  fi
}

cd "${PROJECT_ROOT}"
BASE_MAIN_SHA="$(git rev-parse origin/main 2>/dev/null || git rev-parse main)"
HEAD_SHA="$(git rev-parse HEAD)"
BRANCH="$(git branch --show-current)"
SOURCE_TREE_SHA256="$({
  while IFS= read -r -d '' source_file; do
    if command -v sha256sum >/dev/null 2>&1; then
      sha256sum -- "${source_file}"
    else
      shasum -a 256 -- "${source_file}"
    fi
  done < <(git ls-files --cached --others --exclude-standard -z | sort -z)
} | sha256_stream)"
if [[ -z "$(git status --porcelain)" ]]; then
  WORKTREE_CLEAN="true"
else
  WORKTREE_CLEAN="false"
fi

mkdir -p "$(dirname "${OUTPUT_JSON}")"
export BASE_MAIN_SHA HEAD_SHA BRANCH SOURCE_TREE_SHA256 WORKTREE_CLEAN
export CANONICAL_GATE SANITIZER_GATE REAL_GATE OUTPUT_JSON
export EVIDENCE_SCOPE
export EVIDENCE_GITHUB_SHA="${GITHUB_SHA:-}"
python3 - <<'PY'
import datetime
import json
import os

evidence = {
    "schema_version": 2,
    "scope": os.environ["EVIDENCE_SCOPE"],
    "generated_at_utc": datetime.datetime.now(
        datetime.timezone.utc).isoformat(),
    "base_main_sha": os.environ["BASE_MAIN_SHA"],
    "head_sha": os.environ["HEAD_SHA"],
    "github_sha": os.environ["EVIDENCE_GITHUB_SHA"] or None,
    "branch": os.environ["BRANCH"],
    "source_tree_sha256": os.environ["SOURCE_TREE_SHA256"],
    "worktree_clean": os.environ["WORKTREE_CLEAN"] == "true",
    "gates": {
        "canonical_run_all_tests": os.environ["CANONICAL_GATE"],
        "full_address_undefined_sanitizer": os.environ["SANITIZER_GATE"],
        "real_c_abi_and_public_profile": os.environ["REAL_GATE"],
    },
}
with open(os.environ["OUTPUT_JSON"], "w", encoding="utf-8") as stream:
    json.dump(evidence, stream, ensure_ascii=False, indent=2, sort_keys=True)
    stream.write("\n")
PY

echo "Acceptance evidence written to ${OUTPUT_JSON}"
