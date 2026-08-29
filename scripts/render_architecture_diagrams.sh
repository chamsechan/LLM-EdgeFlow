#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DOC_ROOT="${LLM_EDGEFLOW_ARCH_DOC_ROOT:-${ROOT_DIR}/doc}"
TOOL_CACHE_DIR="${LLM_EDGEFLOW_TOOL_CACHE_DIR:-${ROOT_DIR}/build/tool-cache}"

MODE="${1:---check}"
if [[ $# -gt 1 || ("${MODE}" != "--check" && "${MODE}" != "--generate") ]]; then
  echo "Usage: $0 [--check | --generate]"
  exit 2
fi

PLANTUML_VERSION="1.2024.7"
PLANTUML_SHA256="e34c12bbe9944f1f338ca3d88c9b116b86300cc8e90b35c4086b825b5ae96d24"
PLANTUML_JAR="${TOOL_CACHE_DIR}/plantuml-${PLANTUML_VERSION}.jar"
PLANTUML_URL="https://github.com/plantuml/plantuml/releases/download/v${PLANTUML_VERSION}/plantuml-${PLANTUML_VERSION}.jar"

CLASS_SOURCE="${DOC_ROOT}/architecture.puml"
FLOW_SOURCE="${DOC_ROOT}/architecture_v2.puml"
CLASS_ASSET="${DOC_ROOT}/assets/architecture_class_diagram.svg"
FLOW_ASSET="${DOC_ROOT}/assets/architecture_flow.svg"

echo "================================================================"
echo " [Architecture Diagram Renderer & Gate] Mode: ${MODE}"
echo "================================================================"

resolve_java() {
  local candidate
  for candidate in \
    "${LLM_EDGEFLOW_JAVA_BIN:-}" \
    "$(command -v java 2>/dev/null || true)" \
    "/opt/homebrew/opt/openjdk@17/bin/java" \
    "/usr/local/opt/openjdk@17/bin/java"; do
    if [[ -n "${candidate}" && -x "${candidate}" ]] && \
        "${candidate}" -version >/dev/null 2>&1; then
      echo "${candidate}"
      return 0
    fi
  done
  return 1
}

if ! JAVA_BIN="$(resolve_java)"; then
  echo "❌ Required diagram tool is unavailable: Java Runtime 17+"
  exit 1
fi
if ! command -v sha256sum >/dev/null 2>&1 && \
    ! command -v shasum >/dev/null 2>&1; then
  echo "❌ Required diagram hash tool is unavailable: sha256sum or shasum"
  exit 1
fi

sha256_file() {
  local file_path="$1"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "${file_path}" | awk '{print $1}'
  else
    shasum -a 256 "${file_path}" | awk '{print $1}'
  fi
}

verify_sha256() {
  local file_path="$1"
  local expected="$2"
  [[ "$(sha256_file "${file_path}")" == "${expected}" ]]
}

for source_file in "${CLASS_SOURCE}" "${FLOW_SOURCE}"; do
  if [[ ! -s "${source_file}" ]]; then
    echo "❌ Missing or empty PlantUML source: ${source_file}"
    exit 1
  fi
done

mkdir -p "${TOOL_CACHE_DIR}"
if [[ ! -f "${PLANTUML_JAR}" ]]; then
  if ! command -v curl >/dev/null 2>&1; then
    echo "❌ curl is required to acquire PlantUML ${PLANTUML_VERSION}"
    exit 1
  fi
  DOWNLOAD_FILE="$(mktemp "${TOOL_CACHE_DIR}/plantuml-download.XXXXXX")"
  trap 'rm -f "${DOWNLOAD_FILE:-}"' EXIT
  echo "Downloading pinned PlantUML ${PLANTUML_VERSION}..."
  curl --fail --location --silent --show-error "${PLANTUML_URL}" \
    --output "${DOWNLOAD_FILE}"
  if ! verify_sha256 "${DOWNLOAD_FILE}" "${PLANTUML_SHA256}"; then
    echo "❌ Downloaded PlantUML checksum mismatch: ${DOWNLOAD_FILE}"
    exit 1
  fi
  mv "${DOWNLOAD_FILE}" "${PLANTUML_JAR}"
  trap - EXIT
fi

if ! verify_sha256 "${PLANTUML_JAR}" "${PLANTUML_SHA256}"; then
  echo "❌ PlantUML checksum mismatch: ${PLANTUML_JAR}"
  exit 1
fi

TMP_RENDER_DIR="$(mktemp -d "${TMPDIR:-/tmp}/llm-edgeflow-diagrams.XXXXXX")"
cleanup() {
  rm -rf "${TMP_RENDER_DIR}"
}
trap cleanup EXIT INT TERM

# Render SVGs directly to isolated temporary directory in a single pass with
# parallel threads and fast TieredCompilation startup. Syntax errors are caught
# immediately via -failfast2 without polluting workspace.
"${JAVA_BIN}" -Djava.awt.headless=true -XX:+TieredCompilation \
  -XX:TieredStopAtLevel=1 \
  -jar "${PLANTUML_JAR}" -nbthread auto \
  -charset UTF-8 -failfast2 -nometadata -tsvg \
  "${CLASS_SOURCE}" "${FLOW_SOURCE}" -o "${TMP_RENDER_DIR}"

GENERATED_CLASS="${TMP_RENDER_DIR}/LLM_EdgeFlow_Architecture.svg"
GENERATED_FLOW="${TMP_RENDER_DIR}/LLM_EdgeFlow_Target_Architecture_V2.svg"
for generated_file in "${GENERATED_CLASS}" "${GENERATED_FLOW}"; do
  if [[ ! -s "${generated_file}" ]]; then
    echo "❌ PlantUML did not produce expected SVG: ${generated_file}"
    exit 1
  fi
  if grep -q "An error has occured" "${generated_file}"; then
    echo "❌ PlantUML produced an error SVG: ${generated_file}"
    exit 1
  fi
done

for required_class in SharedAlgorithmRuntime Pipeline AlgContext NodeBase FixedBatchExecutor; do
  if ! grep -q "${required_class}" "${GENERATED_CLASS}"; then
    echo "❌ Class diagram is missing required concept: ${required_class}"
    exit 1
  fi
done

for required_flow in PipelineCatalog PipelineValidator NodeBase FixedBatchExecutor SharedAlgorithmRuntime; do
  if ! grep -q "${required_flow}" "${GENERATED_FLOW}"; then
    echo "❌ Flow diagram is missing required concept: ${required_flow}"
    exit 1
  fi
done

CLASS_SOURCE_SHA="$(sha256_file "${CLASS_SOURCE}")"
FLOW_SOURCE_SHA="$(sha256_file "${FLOW_SOURCE}")"
CLASS_PROVENANCE="LLM-EdgeFlow-PlantUML-Source-SHA256:${CLASS_SOURCE_SHA};Generator:${PLANTUML_VERSION}"
FLOW_PROVENANCE="LLM-EdgeFlow-PlantUML-Source-SHA256:${FLOW_SOURCE_SHA};Generator:${PLANTUML_VERSION}"

annotate_generated_asset() {
  local generated_file="$1"
  local provenance="$2"
  if sed --version >/dev/null 2>&1; then
    sed -i "1i<!-- ${provenance} -->" "${generated_file}"
  else
    sed -i '' "1i\\
<!-- ${provenance} -->
" "${generated_file}"
  fi
}

validate_committed_asset() {
  local asset_file="$1"
  local provenance="$2"
  shift 2
  if [[ ! -s "${asset_file}" ]] || ! grep -q '<svg' "${asset_file}" || \
      ! grep -q '</svg>' "${asset_file}"; then
    echo "❌ Missing or invalid committed SVG asset: ${asset_file}"
    exit 1
  fi
  if ! grep -Fq "<!-- ${provenance} -->" "${asset_file}"; then
    echo "❌ SVG source provenance is stale. Run: $0 --generate"
    exit 1
  fi
  for required_concept in "$@"; do
    if ! grep -q "${required_concept}" "${asset_file}"; then
      echo "❌ Committed SVG is missing required concept: ${required_concept}"
      exit 1
    fi
  done
}

annotate_generated_asset "${GENERATED_CLASS}" "${CLASS_PROVENANCE}"
annotate_generated_asset "${GENERATED_FLOW}" "${FLOW_PROVENANCE}"

if [[ "${MODE}" == "--generate" ]]; then
  mkdir -p "${DOC_ROOT}/assets"
  install -m 0644 "${GENERATED_CLASS}" "${CLASS_ASSET}"
  install -m 0644 "${GENERATED_FLOW}" "${FLOW_ASSET}"
  echo "✅ Generated source-provenance-locked architecture SVG assets."
else
  # PlantUML layout coordinates can differ across CPU architectures and font
  # stacks even with the same Jar. The committed asset is therefore locked to
  # the exact source SHA and generator version, while a fresh render above
  # proves syntax/renderability and both outputs are checked semantically.
  validate_committed_asset "${CLASS_ASSET}" "${CLASS_PROVENANCE}" \
    SharedAlgorithmRuntime Pipeline AlgContext NodeBase FixedBatchExecutor
  validate_committed_asset "${FLOW_ASSET}" "${FLOW_PROVENANCE}" \
    PipelineCatalog PipelineValidator NodeBase FixedBatchExecutor \
    SharedAlgorithmRuntime
  echo "✅ Committed SVG source provenance and semantic contracts are current."
fi

echo "✅ Architecture diagram gate passed with PlantUML ${PLANTUML_VERSION}."
