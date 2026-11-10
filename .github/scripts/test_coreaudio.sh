#!/bin/sh
# Run from a configured macOS build; no physical audio device is opened.
set -eu
mode=${1:-optimized}
case "$mode" in
  optimized) flags="-O2 -O3 -O3,-flto" ;;
  tsan) flags="-O2,-g,-fsanitize=thread" ;;
  *) echo "Usage: $0 [optimized|tsan]" >&2; exit 2 ;;
esac
for options in $flags; do
  options=$(printf '%s' "$options" | tr ',' ' ')
  rm -f unittests/coreaudiofilltest unittests/coreaudiofilltest-coreaudiofilltest.o \
    sound/unittests_coreaudiofilltest-sfifo.o
  make unittests/coreaudiofilltest CFLAGS="$options" LDFLAGS="$options"
  ./unittests/coreaudiofilltest
done
rm -f unittests/coreaudiofilltest unittests/coreaudiofilltest-coreaudiofilltest.o \
  sound/unittests_coreaudiofilltest-sfifo.o
