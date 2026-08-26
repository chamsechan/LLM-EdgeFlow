#!/bin/bash
set -euo pipefail

echo "======================================================================"
echo " [LayerGuard] Checking 4-Tier Architectural Isolation Directives..."
echo "======================================================================"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Rule 1: Layer 3 (src/business/ and src/common_nodes/) MUST NEVER include Layer 1 header (company_alg_interface.h)
VIOLATIONS_L3_L1=$(grep -rnE '#include\s*["<]company_alg_interface\.h[">]' "$REPO_ROOT/src/business" "$REPO_ROOT/src/common_nodes" || true)

if [ -n "$VIOLATIONS_L3_L1" ]; then
  echo "❌ [LayerGuard ERROR] Found Layer 3 -> Layer 1 reverse dependency violations:"
  echo "$VIOLATIONS_L3_L1"
  echo "Directive: Layer 3 business and common nodes must strictly communicate via DTOs and AlgContext (ARCH-002)."
  exit 1
fi
echo "✅ [LayerGuard PASS] Zero Layer 3 -> Layer 1 reverse include violations."

# Rule 2: Layer 1 Adapters (src/adapter/adapters/) MUST NEVER directly include Layer 4 Engine headers
VIOLATIONS_L1_L4=$(grep -rnE '#include\s*["<](engine/|src/engine/)' "$REPO_ROOT/src/adapter/adapters" || true)

if [ -n "$VIOLATIONS_L1_L4" ]; then
  echo "❌ [LayerGuard ERROR] Found Layer 1 -> Layer 4 illegal bypass dependency violations:"
  echo "$VIOLATIONS_L1_L4"
  echo "Directive: Layer 1 adapters must only convert C structs to/from business DTOs, not bypass Layer 3."
  exit 1
fi
echo "✅ [LayerGuard PASS] Zero Layer 1 -> Layer 4 illegal engine include violations."

# Rule 3: Common Nodes (src/common_nodes/) MUST NEVER depend on business-specific nodes (src/business/)
VIOLATIONS_COMMON_BIZ=$(grep -rnE '#include\s*["<](business/|src/business/)' "$REPO_ROOT/src/common_nodes" || true)

if [ -n "$VIOLATIONS_COMMON_BIZ" ]; then
  echo "❌ [LayerGuard ERROR] Found Common Node -> Business Node dependency violations:"
  echo "$VIOLATIONS_COMMON_BIZ"
  exit 1
fi
echo "✅ [LayerGuard PASS] Zero Common Node -> Business Node reverse include violations."

# Rule 4: Pure C11 Syntax & ABI Compliance Check via standard C compiler
if command -v gcc >/dev/null 2>&1; then
  gcc -std=c11 -pedantic-errors -fsyntax-only -x c -I"$REPO_ROOT/include" "$REPO_ROOT/include/company_alg_interface.h"
  gcc -std=c11 -pedantic-errors -fsyntax-only -x c -I"$REPO_ROOT/include" "$REPO_ROOT/include/platform/company_platform_types.h"
  echo "✅ [LayerGuard PASS] GCC pure C11 strict syntax and ABI verification passed."
elif command -v clang >/dev/null 2>&1; then
  clang -std=c11 -pedantic-errors -fsyntax-only -x c -I"$REPO_ROOT/include" "$REPO_ROOT/include/company_alg_interface.h"
  clang -std=c11 -pedantic-errors -fsyntax-only -x c -I"$REPO_ROOT/include" "$REPO_ROOT/include/platform/company_platform_types.h"
  echo "✅ [LayerGuard PASS] Clang pure C11 strict syntax and ABI verification passed."
else
  echo "⚠️ [LayerGuard WARN] Neither gcc nor clang found for C11 syntax-only check."
fi

# Rule 5: Demo Layer (demo/) MUST NEVER directly include internal SDK headers (adapter/, core/, business/, engine/, src/)
VIOLATIONS_DEMO_INTERNAL=$(grep -rnE '#include\s*["<](adapter/|core/|business/|engine/|src/)' "$REPO_ROOT/demo" || true)

if [ -n "$VIOLATIONS_DEMO_INTERNAL" ]; then
  echo "❌ [LayerGuard ERROR] Found Demo -> SDK internal header violations:"
  echo "$VIOLATIONS_DEMO_INTERNAL"
  echo "Directive: Demo must strictly behave as external user and only include platform/ and public company_alg_interface.h."
  exit 1
fi
echo "✅ [LayerGuard PASS] Zero Demo -> Internal SDK header violations."

echo "======================================================================"
echo " All LayerGuard architectural isolation checks passed successfully!"
echo "======================================================================"
