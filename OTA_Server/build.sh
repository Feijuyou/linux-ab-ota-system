set -e

export RDK_SYSROOT="/home/bryce/rdk-sysroot"

cmake -S . -B build-rdk \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake \
    -DRDK_SYSROOT="$RDK_SYSROOT" \
    -DCMAKE_BUILD_TYPE=Release

cmake --build build-rdk -j"$(nproc)"

file build-rdk/ota_client