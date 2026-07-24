#!/bin/sh
# Cross-build libmupdf (+ bundled thirdparty) as static libs for the mipsel
# handheld. MuPDF renders epub/mobi/pdf/cbz/fb2 to RGB bitmaps; the reader glue
# (reader.c) blits those via driver.so. generated/ ships pre-built font/cmap
# dumps so there is NO host codegen step to cross-confuse. GUI/network/OCR off.
set -e
MUPDF=/home/tomaszz/sf3000-work/mupdf
TCBIN=/home/tomaszz/sf3000-work/sf3000toolchain/mipsel-buildroot-linux-gnu_sdk-buildroot/opt/ext-toolchain/bin
SYSROOT=/home/tomaszz/sf3000-work/sf3000toolchain/mipsel-buildroot-linux-gnu_sdk-buildroot/mipsel-buildroot-linux-gnu/sysroot
PFX="$TCBIN/mips-mti-linux-gnu-"
ARCH="-EL -mips32r2 -march=mips32r2 -mtune=74kc -mfp32 -mhard-float --sysroot=$SYSROOT"

cd "$MUPDF"
make -j"$(nproc)" \
  build=release \
  HAVE_X11=no HAVE_GLUT=no HAVE_CURL=no USE_TESSERACT=no HAVE_OBJCOPY=no \
  OS=Linux verbose=yes \
  CC="${PFX}gcc" CXX="${PFX}g++" AR="${PFX}ar" LD="${PFX}ld" \
  XCFLAGS="$ARCH" XCXXFLAGS="$ARCH" \
  libs 2>&1 | tail -25

echo "=== outputs ==="
ls -la "$MUPDF"/build/release/libmupdf.a "$MUPDF"/build/release/libmupdf-third.a 2>&1
