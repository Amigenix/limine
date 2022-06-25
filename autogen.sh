#! /bin/sh

set -ex

origdir="$(pwd -P)"

srcdir="$(dirname "$0")"
test -z "$srcdir" && srcdir=.

cd "$srcdir"

[ -d freestanding_headers ] || git clone https://github.com/mintsuki/freestanding_headers.git
[ -d stivale ] || git clone https://github.com/stivale/stivale.git
[ -d libgcc-binaries ] || git clone https://github.com/mintsuki/libgcc-binaries.git
[ -d limine-efi ] || git clone https://github.com/limine-bootloader/limine-efi.git

automake_libdir="$(automake --print-libdir)"

mkdir -p build-aux
cp "${automake_libdir}/install-sh" build-aux

autoconf

cd "$origdir"

if test -z "$NOCONFIGURE"; then
    exec "$srcdir"/configure "$@"
fi
