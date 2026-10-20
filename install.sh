#!/usr/bin/env bash
set -euo pipefail
# Validate the selected CMake packages without login-shell dylib overrides.
unset DYLD_LIBRARY_PATH DYLD_FRAMEWORK_PATH DYLD_FALLBACK_LIBRARY_PATH \
    DYLD_VERSIONED_LIBRARY_PATH DYLD_VERSIONED_FRAMEWORK_PATH
if [[ $# -ne 0 ]]; then
    echo "Usage: INSTALL_PREFIX=... CMAKE_PREFIX_PATH=... QT_PREFIX_PATH=... ./install.sh" >&2
    exit 2
fi
source_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$source_dir/build"
mkdir -p "$build_dir/tmp" "$build_dir/ccache"
export TMPDIR="$build_dir/tmp"
export CCACHE_DIR="$build_dir/ccache"
install_prefix="${INSTALL_PREFIX:-$HOME/.local/SDK/iiMultimodalAgent}"
prefix_path="${CMAKE_PREFIX_PATH:-}"
for dependency in iiSDKCore iiFileProvider iiDecision iiLocalLLM iiLocalDiffusion iiAudioGen iiVoiceOver; do
    dependency_prefix="$HOME/.local/SDK/$dependency"
    if [[ -d "$dependency_prefix" ]]; then
        prefix_path="${prefix_path:+$prefix_path;}$dependency_prefix"
    fi
done
qt_prefix="${QT_PREFIX_PATH:-}"
if [[ -z "$qt_prefix" && -d /Volumes/Storage/Qt/6.8.3/macos ]]; then
    qt_prefix=/Volumes/Storage/Qt/6.8.3/macos
fi
if [[ -n "$qt_prefix" ]]; then
    prefix_path="$qt_prefix${prefix_path:+;$prefix_path}"
fi
cmake -S "$source_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=ON "-DCMAKE_PREFIX_PATH=$prefix_path" \
    "-DCMAKE_INSTALL_PREFIX=$install_prefix"
cmake --build "$build_dir" --config Release --parallel "${BUILD_JOBS:-4}"
ctest --test-dir "$build_dir" -C Release --parallel 1 --output-on-failure
cmake --install "$build_dir" --config Release
consumer_dir="$build_dir/consumer/build"
cmake -S "$source_dir/tests/consumer" -B "$consumer_dir" \
    -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$install_prefix${prefix_path:+;$prefix_path}" \
    "-DiiMultimodalAgent_DIR=$install_prefix/lib/cmake/iiMultimodalAgent"
cmake --build "$consumer_dir" --config Release --parallel "${BUILD_JOBS:-4}"
ctest --test-dir "$consumer_dir" -C Release --parallel 1 --output-on-failure
