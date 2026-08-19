#!/bin/bash
set -euo pipefail

echo "======================================================================"
echo " [LayerGuard] Checking 4-Tier Architectural Isolation Directives..."
echo "======================================================================"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Rule 1: Layer 3 (src/business/ and src/common_nodes/) MUST NEVER include Layer 1 header (company_alg_interface.h)
VIOLATIONS=$(grep -rnE '#include\s*["<]company_alg_interface\.h[">]' "$REPO_ROOT/src/business" "$REPO_ROOT/src/common_nodes" || true)

if [ -n "$VIOLATIONS" ]; then
  echo "❌ [LayerGuard ERROR] Found Layer 3 -> Layer 1 reverse dependency violations:"
  echo "$VIOLATIONS"
  echo ""
  echo "Directive: Layer 3 business and common nodes must strictly communicate via DTOs and AlgContext."
  echo "Directives reference: doc/architecture_review.md (ARCH-002)"
  exit 1
fi

echo "✅ [LayerGuard PASS] Zero Layer 1 reverse include violations found in src/business/ and src/common_nodes/."

# Rule 2: Pure C ABI Header (include/company_alg_interface.h) MUST NEVER include C++ STL headers
STL_INCLUDES=$(grep -nE '#include\s*<[a-z_]+>' "$REPO_ROOT/include/company_alg_interface.h" | grep -vE '<stdint\.h>|<stddef\.h>|<vector>' || true)

if [ -n "$STL_INCLUDES" ]; then
  echo "❌ [LayerGuard ERROR] Found illegal C++ STL headers in include/company_alg_interface.h:"
  echo "$STL_INCLUDES"
  exit 1
fi

echo "✅ [LayerGuard PASS] Pure C ABI header is clean of non-standard C headers."
echo "======================================================================"
echo " All Layer Isolation checks passed successfully!"
echo "======================================================================"
