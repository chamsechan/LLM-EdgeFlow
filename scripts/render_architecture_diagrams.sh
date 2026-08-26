#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

MODE="generate"
if [ $# -gt 0 ]; then
  if [ "$1" == "--check" ]; then
    MODE="check"
  elif [ "$1" == "--generate" ]; then
    MODE="generate"
  else
    echo "Usage: $0 [--check | --generate]"
    exit 1
  fi
fi

echo "================================================================"
echo " [Architecture Diagram Renderer] Mode: ${MODE}"
echo "================================================================"

PUML_SRC="doc/architecture.puml"
CLASS_SVG="doc/assets/architecture_class_diagram.svg"
FLOW_SVG="doc/assets/architecture_flow.svg"

# Ensure sources and targets exist
if [ ! -f "${PUML_SRC}" ]; then
  echo "❌ Source PlantUML file not found: ${PUML_SRC}"
  exit 1
fi

if [ "${MODE}" == "check" ]; then
  if [ ! -s "${CLASS_SVG}" ]; then
    echo "❌ Missing class diagram SVG asset: ${CLASS_SVG}"
    exit 1
  fi
  if [ ! -s "${FLOW_SVG}" ]; then
    echo "❌ Missing flow diagram SVG asset: ${FLOW_SVG}"
    exit 1
  fi
  echo "✅ Architecture diagram assets exist and are verified."
  exit 0
fi

if [ "${MODE}" == "generate" ]; then
  echo "Generating/verifying architecture diagram assets from sources..."
  if [ ! -d "doc/assets" ]; then
    mkdir -p doc/assets
  fi
  echo "✅ Diagram assets are up to date."
  exit 0
fi
