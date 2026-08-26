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
echo " [Architecture Diagram Renderer & Gate] Mode: ${MODE}"
echo "================================================================"

PLANTUML_VERSION="1.2024.7"
PLANTUML_CACHE_DIR="${HOME}/.cache/plantuml"
PLANTUML_JAR="${PLANTUML_CACHE_DIR}/plantuml-${PLANTUML_VERSION}.jar"

# 1. 确保固定版本 PlantUML Jar 存在
if [ ! -f "${PLANTUML_JAR}" ]; then
  echo "Ensuring PlantUML v${PLANTUML_VERSION} in ${PLANTUML_CACHE_DIR}..."
  mkdir -p "${PLANTUML_CACHE_DIR}"
  if command -v curl >/dev/null 2>&1; then
    curl -sSL "https://github.com/plantuml/plantuml/releases/download/v${PLANTUML_VERSION}/plantuml-${PLANTUML_VERSION}.jar" -o "${PLANTUML_JAR}" || {
      echo "⚠️ PlantUML download failed. Falling back to local syntax validation."
    }
  fi
fi

# 2. 检查源文件
for puml in doc/architecture.puml doc/architecture_v2.puml; do
  if [ ! -f "${puml}" ]; then
    echo "❌ Source PlantUML file not found: ${puml}"
    exit 1
  fi
done

# 3. 语法校验与渲染性预检
if [ -f "${PLANTUML_JAR}" ] && command -v java >/dev/null 2>&1; then
  echo "Validating PlantUML syntax for doc/architecture.puml and doc/architecture_v2.puml..."
  java -jar "${PLANTUML_JAR}" -checkonly doc/architecture.puml doc/architecture_v2.puml
  echo "✅ PlantUML syntax verification passed (v${PLANTUML_VERSION})."

  # 渲染到临时目录以验证生成确定性
  TMP_RENDER_DIR=$(mktemp -d)
  trap 'rm -rf "${TMP_RENDER_DIR}"' EXIT
  java -jar "${PLANTUML_JAR}" -tsvg doc/architecture.puml -o "${TMP_RENDER_DIR}"
  if [ ! -s "${TMP_RENDER_DIR}/LLM_EdgeFlow_Architecture.svg" ]; then
    echo "❌ PlantUML failed to generate SVG in temporary workspace."
    exit 1
  fi
  echo "✅ PlantUML renderability test passed."
else
  # 基础纯文本语法自检兜底
  for puml in doc/architecture.puml doc/architecture_v2.puml; do
    if ! grep -q "@startuml" "${puml}" || ! grep -q "@enduml" "${puml}"; then
      echo "❌ Invalid PlantUML envelope in ${puml}"
      exit 1
    fi
  done
  echo "✅ PlantUML basic envelope syntax verified."
fi

# 4. 校验目标 SVG 资产
CLASS_SVG="doc/assets/architecture_class_diagram.svg"
FLOW_SVG="doc/assets/architecture_flow.svg"

for svg in "${CLASS_SVG}" "${FLOW_SVG}"; do
  if [ ! -s "${svg}" ]; then
    echo "❌ Missing or empty SVG asset: ${svg}"
    exit 1
  fi
  if ! grep -q "<svg" "${svg}" || ! grep -q "</svg>" "${svg}"; then
    echo "❌ Corrupted SVG XML structure in ${svg}"
    exit 1
  fi
done

# 5. 校验核心架构元素在类图中的映射
REQUIRED_CLASSES=("SharedAlgorithmRuntime" "Pipeline" "AlgContext" "NodeBase" "FixedBatchExecutor")
for cls in "${REQUIRED_CLASSES[@]}"; do
  if ! grep -q "${cls}" "${CLASS_SVG}"; then
    echo "❌ Class diagram SVG is missing required core architecture class: ${cls}"
    exit 1
  fi
done

# 6. 校验核心架构元素在流程图中的映射
REQUIRED_FLOW_ELEMENTS=("Alg_Process" "Pipeline" "AlgContext" "FixedBatchExecutor" "ModelManager")
for elem in "${REQUIRED_FLOW_ELEMENTS[@]}"; do
  if ! grep -q "${elem}" "${FLOW_SVG}"; then
    echo "❌ Flow diagram SVG is missing required architecture element: ${elem}"
    exit 1
  fi
done

echo "✅ All architecture diagrams verified successfully in mode: ${MODE}"
exit 0
