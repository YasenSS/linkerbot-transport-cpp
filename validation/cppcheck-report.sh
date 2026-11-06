#!/usr/bin/env bash
set -euo pipefail
# Cppcheck models the standard library instead of parsing compiler headers.
# Full-mode clang-tidy independently checks every compilation entry for missing
# headers, including angle-bracket project headers. Keep other Cppcheck errors.
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
target_config=()
if [[ -n "${CPPCHECK_TARGET_CONFIG:-}" ]]; then
    test -f "$CPPCHECK_TARGET_CONFIG"
    target_config=("--include=$CPPCHECK_TARGET_CONFIG")
fi
exec cppcheck --library=std --library="$script_dir/catch2-cppcheck.cfg" \
    --suppress=missingIncludeSystem "${target_config[@]}" "$@"
