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

for command_name in java sha256sum; do
  if ! command -v "${command_name}" >/dev/null 2>&1; then
    echo "❌ Required diagram tool is unavailable: ${command_name}"
    exit 1
  fi
done

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
  echo "${PLANTUML_SHA256}  ${DOWNLOAD_FILE}" | sha256sum --check --status
  mv "${DOWNLOAD_FILE}" "${PLANTUML_JAR}"
  trap - EXIT
fi

if ! echo "${PLANTUML_SHA256}  ${PLANTUML_JAR}" | \
    sha256sum --check --status; then
  echo "❌ PlantUML checksum mismatch: ${PLANTUML_JAR}"
  exit 1
fi

TMP_RENDER_DIR="$(mktemp -d "${TMPDIR:-/tmp}/llm-edgeflow-diagrams.XXXXXX")"
cleanup() {
  rm -rf "${TMP_RENDER_DIR}"
}
trap cleanup EXIT INT TERM

# PlantUML's -checkonly still emits PNG files next to its input sources. Run
# syntax validation against isolated copies so a read-only check never dirties
# the repository (or a caller-provided documentation root).
CHECK_SOURCE_DIR="${TMP_RENDER_DIR}/check-sources"
mkdir -p "${CHECK_SOURCE_DIR}"
cp "${CLASS_SOURCE}" "${CHECK_SOURCE_DIR}/architecture.puml"
cp "${FLOW_SOURCE}" "${CHECK_SOURCE_DIR}/architecture_v2.puml"
java -Djava.awt.headless=true -jar "${PLANTUML_JAR}" \
  -charset UTF-8 -failfast2 -checkonly \
  "${CHECK_SOURCE_DIR}/architecture.puml" \
  "${CHECK_SOURCE_DIR}/architecture_v2.puml"
java -Djava.awt.headless=true -jar "${PLANTUML_JAR}" \
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

if [[ "${MODE}" == "--generate" ]]; then
  mkdir -p "${DOC_ROOT}/assets"
  install -m 0644 "${GENERATED_CLASS}" "${CLASS_ASSET}"
  install -m 0644 "${GENERATED_FLOW}" "${FLOW_ASSET}"
  echo "✅ Generated deterministic architecture SVG assets."
else
  for asset_file in "${CLASS_ASSET}" "${FLOW_ASSET}"; do
    if [[ ! -s "${asset_file}" ]]; then
      echo "❌ Missing or empty committed SVG asset: ${asset_file}"
      exit 1
    fi
  done
  if ! cmp --silent "${GENERATED_CLASS}" "${CLASS_ASSET}"; then
    echo "❌ Class diagram asset is stale. Run: $0 --generate"
    exit 1
  fi
  if ! cmp --silent "${GENERATED_FLOW}" "${FLOW_ASSET}"; then
    echo "❌ Flow diagram asset is stale. Run: $0 --generate"
    exit 1
  fi
  echo "✅ Committed SVG assets exactly match their PlantUML sources."
fi

echo "✅ Architecture diagram gate passed with PlantUML ${PLANTUML_VERSION}."
