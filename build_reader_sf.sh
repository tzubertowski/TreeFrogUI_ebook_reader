#!/bin/sh
# Build the standalone ebook reader (reader.c + hwdisp) against libmupdf.
set -e
E=/home/tomaszz/sf3000-work/ebook
MUPDF=/home/tomaszz/sf3000-work/mupdf
TCBIN=/home/tomaszz/sf3000-work/sf3000toolchain/mipsel-buildroot-linux-gnu_sdk-buildroot/opt/ext-toolchain/bin
SYSROOT=/home/tomaszz/sf3000-work/sf3000toolchain/mipsel-buildroot-linux-gnu_sdk-buildroot/mipsel-buildroot-linux-gnu/sysroot
CC="$TCBIN/mips-mti-linux-gnu-gcc"
CXX="$TCBIN/mips-mti-linux-gnu-g++"
ARCH="-EL -mips32r2 -march=mips32r2 -mtune=74kc -mfp32 -mhard-float --sysroot=$SYSROOT"

cd "$E"
# Compile C sources, then link with g++ so libstdc++ (harfbuzz) links STATICALLY
# (device rootfs has no libstdc++.so.6).
"$CC" $ARCH -O2 -I"$MUPDF/include" -Iport_sf3000 -c reader.c -o reader.o
"$CC" $ARCH -O2 -Iport_sf3000 -c port_sf3000/hwdisp.c -o hwdisp.o
"$CXX" $ARCH reader.o hwdisp.o \
  "$MUPDF/build/release/libmupdf.a" "$MUPDF/build/release/libmupdf-third.a" \
  -static-libstdc++ -static-libgcc -lm -ldl -lpthread \
  -Wl,--gc-sections -o ebook

"$TCBIN/mips-mti-linux-gnu-strip" ebook 2>/dev/null || true
ls -la ebook
file ebook
