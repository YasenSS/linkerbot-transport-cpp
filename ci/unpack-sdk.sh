#!/usr/bin/env bash
set -euo pipefail
archive_dir=${1:?archive directory required}
destination=${2:?destination required}
shopt -s nullglob
archives=("$archive_dir"/*.tar.gz)
checksums=("$archive_dir"/*.sha256)
test "${#archives[@]}" -eq 1
test "${#checksums[@]}" -eq 1
(cd "$archive_dir" && sha256sum --check -- *.sha256) >&2
mkdir -p "$destination"
tar -xzf "${archives[0]}" -C "$destination"
roots=("$destination"/*)
test "${#roots[@]}" -eq 1
test -d "${roots[0]}"
test -s "${roots[0]}/share/linkerbot-comm/build-info.json"
printf '%s\n' "${roots[0]}"
