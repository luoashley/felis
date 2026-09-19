#!/usr/bin/env bash
set -euo pipefail

build_type="${1:-debug}"
case "$build_type" in
  debug|release) ;;
  *)
    echo "usage: $0 [debug|release]" >&2
    exit 2
    ;;
esac

git submodule update --init --recursive
scons . \
  BUILD_TYPE="$build_type" \
  CCFLAGS="-fno-pie" \
  CXXFLAGS="-fno-pie" \
  LINKFLAGS="-no-pie"

echo "built $build_type/db as a non-PIE executable"
