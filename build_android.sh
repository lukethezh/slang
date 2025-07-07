#!/bin/bash

export ANDROID_NDK=$ANDROID_NDK_HOME
export TOOLCHAIN=$ANDROID_NDK/toolchains/llvm/prebuilt/windows-x86_64

export CC=$TOOLCHAIN/bin/aarch64-linux-android24-clang
export CXX=$TOOLCHAIN/bin/aarch64-linux-android24-clang++
export PATH=$ANDROID_NDK/toolchains/llvm/prebuilt/windows-x86_64/sysroot:$PATH

cmake --preset default \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-24 \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
    -DSLANG_LIB_TYPE=STATIC

cmake --build --preset releaseWithDebugInfo