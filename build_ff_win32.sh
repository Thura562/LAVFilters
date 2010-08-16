#!/bin/bash 

if [ ! -d bin_Win32/lib ]
then
mkdir -p bin_Win32/lib
fi

cd ffmpeg

make distclean

OPTIONS="
--enable-shared \
--enable-memalign-hack \
--enable-gpl \
--enable-w32threads \
--disable-runtime-cpudetect \
--disable-asm \
--disable-postproc \
--enable-zlib \
--disable-static \
--disable-altivec \
--disable-muxers \
--disable-encoders \
--disable-debug \
--enable-ffplay \
--disable-ffserver \
--disable-ffmpeg \
--enable-ffprobe \
--disable-devices \
--disable-filters \
--disable-avfilter \
--disable-swscale \
--disable-avdevice \
--disable-hwaccels \
--disable-bsfs \
--arch=x86 --target-os=mingw32"

./configure --extra-cflags="-U__STRICT_ANSI__ -fno-strict-aliasing -march=i686 -mmmx -msse -mfpmath=sse" ${OPTIONS} &&
 
make -j4 &&
cp lib*/*-*.dll ../bin_Win32 &&
cp lib*/*.lib ../bin_Win32/lib &&
cp ffprobe.exe ../bin_Win32

cd ..
