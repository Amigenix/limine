#!/bin/sh

set -ex

TARGET=x86_64-elf
BINUTILSVERSION=2.37
GCCVERSION=11.2.0

CFLAGS="-O2 -pipe"

if [ "$(uname)" = "OpenBSD" ]; then
    # OpenBSD has an awfully ancient GCC which fails to build our toolchain.
    # Force clang/clang++.
    export CC="clang"
    export CXX="clang++"
fi

mkdir -p "$1" && cd "$1"
PREFIX="$(pwd)"

export MAKEFLAGS="$2"

export PATH="$PREFIX/bin:$PATH"

if [ ! -f binutils-$BINUTILSVERSION.tar.gz ]; then
    curl -o binutils-$BINUTILSVERSION.tar.gz https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILSVERSION.tar.gz
fi
if [ ! -f gcc-$GCCVERSION.tar.gz ]; then
    curl -o gcc-$GCCVERSION.tar.gz https://ftp.gnu.org/gnu/gcc/gcc-$GCCVERSION/gcc-$GCCVERSION.tar.gz
fi

rm -rf build
mkdir build
cd build

tar -zxf ../binutils-$BINUTILSVERSION.tar.gz
tar -zxf ../gcc-$GCCVERSION.tar.gz

mkdir build-binutils
cd build-binutils
../binutils-$BINUTILSVERSION/configure CFLAGS="$CFLAGS" CXXFLAGS="$CFLAGS"  --target=$TARGET --prefix="$PREFIX" --program-prefix=limine- --with-sysroot --disable-nls --disable-werror --enable-targets=x86_64-elf,x86_64-pe
$MAKE
$MAKE install
cd ..

cd gcc-$GCCVERSION
contrib/download_prerequisites
cd ..
mkdir build-gcc
cd build-gcc
../gcc-$GCCVERSION/configure CFLAGS="$CFLAGS" CXXFLAGS="$CFLAGS" --target=$TARGET --prefix="$PREFIX" --program-prefix=limine- --disable-nls --enable-languages=c --without-headers
$MAKE all-gcc
$MAKE all-target-libgcc
$MAKE install-gcc
$MAKE install-target-libgcc
cd ..
