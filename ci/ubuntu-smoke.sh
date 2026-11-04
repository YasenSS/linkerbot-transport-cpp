#!/usr/bin/env bash
# Run inside the target Ubuntu userspace with its default GCC and CMake.
set -euo pipefail
prefix=${1:?SDK prefix required}
cmake --version
g++ --version
cat /etc/os-release
case ${2:-all} in
  all)
    cmake -S . -B build/source-smoke -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DLINKERBOT_COMM_BUILD_TESTS=OFF -DLINKERBOT_COMM_BUILD_EXAMPLES=ON \
      -DLINKERBOT_ALLOW_DEPENDENCY_DOWNLOADS=OFF
    cmake --build build/source-smoke --parallel 4
    ./build/source-smoke/comm-example
    ;;
  --consume-only) ;;
  *) echo "Unknown smoke mode: $2" >&2; exit 2 ;;
esac
for consumer in "$prefix/share/linkerbot-comm/validation/installed-consumer" "$prefix/share/linkerbot-comm/examples/quickstart" validation/shared-consumer; do
  name=${consumer##*/}
  cmake -S "$consumer" -B "build/smoke-$name" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$prefix"
  cmake --build "build/smoke-$name" --parallel 4
  # CMake 3.16 has no --test-dir or --no-tests=error.
  grep -q "^add_test(" "build/smoke-$name/CTestTestfile.cmake"
  (cd "build/smoke-$name" && ctest --output-on-failure --timeout 60)
done
mkdir -p build/evidence
find build/smoke-* -type f \( -name comm-installed-consumer -o -name linkerbot-quickstart -o -name shared-consumer-smoke -o -name libshared-consumer.so \) \
  -exec readelf --version-info '{}' \; > build/evidence/consumer-symbol-versions.txt
