#!/bin/sh

set -e

if [ $# -ne 1 ]; then
    echo "usage: $0 <output directory>" >&2
    exit 1
fi

out_dir=$1
fusepb_dir=$(cd "$(dirname "$0")" && pwd)
libspectrum_dir=$fusepb_dir/../3rdparty/libspectrum

mkdir -p "$out_dir"

cp "$libspectrum_dir/make-perl.c" "$out_dir/make-perl.c"
xcrun --sdk macosx clang -I "$fusepb_dir" -o "$out_dir/make-perl" "$out_dir/make-perl.c"

"$out_dir/make-perl" > "$out_dir/generate.pl"
cat "$libspectrum_dir/generate.pl.in" >> "$out_dir/generate.pl"

perl -p "$out_dir/generate.pl" "$libspectrum_dir" "$libspectrum_dir/libspectrum.h.in" \
    > "$out_dir/libspectrum.h.tmp"
mv "$out_dir/libspectrum.h.tmp" "$out_dir/libspectrum.h"
