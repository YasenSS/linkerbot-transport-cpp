#!/usr/bin/env bash
set -euo pipefail
# Cppcheck models the standard library instead of parsing compiler headers.
# Full-mode clang-tidy independently checks every compilation entry for missing
# headers, including angle-bracket project headers. Keep other Cppcheck errors.
exec cppcheck --library=std --suppress=missingIncludeSystem "$@"
