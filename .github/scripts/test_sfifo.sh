#!/bin/sh
# Run from a configured build directory. Use make's dependency/compile rules.
set -eu
mode=${1:-optimized}
case "$mode" in
  optimized) flags="-O2 -O3 -O3,-flto" ;;
  tsan) flags="-O2,-g,-fsanitize=thread" ;;
  *) echo "Usage: $0 [optimized|tsan]" >&2; exit 2 ;;
esac
for options in $flags; do
  options=$(printf '%s' "$options" | tr ',' ' ')
  rm -f unittests/sfifotest unittests/sfifotest-sfifotest.o \
    sound/unittests_sfifotest-sfifo.o
  make unittests/sfifotest CFLAGS="$options" LDFLAGS="$options"
  ./unittests/sfifotest
done
# Do not leave instrumented objects for a subsequent ordinary build.
rm -f unittests/sfifotest unittests/sfifotest-sfifotest.o \
  sound/unittests_sfifotest-sfifo.o
