#!/bin/bash
# Builds fidelity_harness.cpp against the sts_lightspeed sources (same recipe as apps/main).
set -euo pipefail
STS=/Users/nico/git/sts_lightspeed
OUT=/private/tmp/claude-501/-Users-nico-git-spire-agent/d4e15dba-42f8-4fc2-b183-533b03dd427a/scratchpad/spike

SOURCES=$(find "$STS/src" -name "*.cpp")

clang++ -std=c++17 -O2 -Wno-shift-count-overflow \
  -I "$STS/include" \
  -I "$STS/json/include" \
  "$OUT/fidelity_harness.cpp" $SOURCES \
  -o "$OUT/fidelity_harness"

echo "built: $OUT/fidelity_harness"
