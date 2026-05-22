#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

TARGET_TRIPLE="x86_64-pc-windows-msvc"
WINSDK_BASE="${WINSDK_BASE:-/opt/winsdk}"
BUILD_DIR="build"
OUT_EXE="groovie2.exe"

usage() {
    cat <<'EOF'
Usage: ./build.sh [release|debug|linux|macos|clean]

Builds the Windows GUI executable or a CLI-only Unix executable.

Targets:
  release   Build groovie2.exe as a native Win64 GUI program
  debug     Build groovie2-debug.exe as a native Win64 GUI program
  linux     Build build/groovie2-linux as a CLI-only executable
  macos     Build build/groovie2-macos as a CLI-only executable
  clean     Remove build outputs
EOF
}

detect_winsdk() {
    if [[ ! -d "$WINSDK_BASE" ]]; then
        echo "Windows SDK not found at $WINSDK_BASE"
        echo "Install it with xwin first, or set WINSDK_BASE."
        exit 1
    fi

    SDK_INCLUDE=""
    SDK_LIB=""
    CRT_INCLUDE=""
    CRT_LIB=""

    [[ -d "$WINSDK_BASE/sdk/include" ]] && SDK_INCLUDE="$WINSDK_BASE/sdk/include"
    [[ -d "$WINSDK_BASE/sdk/lib" ]] && SDK_LIB="$WINSDK_BASE/sdk/lib"
    [[ -d "$WINSDK_BASE/Include" ]] && SDK_INCLUDE="$WINSDK_BASE/Include"
    [[ -d "$WINSDK_BASE/Lib" ]] && SDK_LIB="$WINSDK_BASE/Lib"
    [[ -d "$WINSDK_BASE/crt/include" ]] && CRT_INCLUDE="$WINSDK_BASE/crt/include"
    [[ -d "$WINSDK_BASE/crt/lib" ]] && CRT_LIB="$WINSDK_BASE/crt/lib"

    if [[ -z "$SDK_INCLUDE" || -z "$SDK_LIB" || -z "$CRT_INCLUDE" || -z "$CRT_LIB" ]]; then
        echo "Could not detect SDK/CRT include and lib folders under $WINSDK_BASE"
        exit 1
    fi

    LIB_ARCH=""
    if [[ -d "$CRT_LIB/x86_64" && -d "$SDK_LIB/um/x86_64" && -d "$SDK_LIB/ucrt/x86_64" ]]; then
        LIB_ARCH="x86_64"
    elif [[ -d "$CRT_LIB/x64" ]]; then
        LIB_ARCH="x64"
    fi
    if [[ -z "$LIB_ARCH" ]]; then
        echo "Could not detect Win64 library architecture under $WINSDK_BASE"
        exit 1
    fi
}

build_windows() {
    local mode="$1"
    detect_winsdk
    mkdir -p "$BUILD_DIR"

    local flags=(
        /nologo
        "--target=$TARGET_TRIPLE"
        -fuse-ld=lld-link
        /std:c++20
        /EHsc
        /MT
        /DWIN32_LEAN_AND_MEAN
        /DNOMINMAX
        /D_CRT_SECURE_NO_WARNINGS
        "-imsvc$CRT_INCLUDE"
        "-imsvc$SDK_INCLUDE/ucrt"
        "-imsvc$SDK_INCLUDE/um"
        "-imsvc$SDK_INCLUDE/shared"
        "-imsvc$SDK_INCLUDE/winrt"
    )

    local link_flags=(
        /subsystem:windows
        /defaultlib:libcmt
        /defaultlib:libucrt
        /nodefaultlib:msvcrt.lib
        /nodefaultlib:ucrt.lib
        "/libpath:$CRT_LIB/$LIB_ARCH"
        "/libpath:$SDK_LIB/um/$LIB_ARCH"
        "/libpath:$SDK_LIB/ucrt/$LIB_ARCH"
        windowscodecs.lib
        ole32.lib
        uuid.lib
        user32.lib
        gdi32.lib
        shell32.lib
        comctl32.lib
        winmm.lib
    )

    if [[ "$mode" == "debug" ]]; then
        flags+=(/Od /Zi)
        link_flags+=(/debug:full)
        OUT_EXE="groovie2-debug.exe"
    else
        flags+=(/O2 /DNDEBUG)
        link_flags+=(/opt:ref /opt:icf)
    fi

    echo "Compiling $OUT_EXE..."
    clang-cl "${flags[@]}" groovie2.cpp config.cpp "/Fe:$OUT_EXE" /link "${link_flags[@]}"
    echo "Built $OUT_EXE"
}

build_linux() {
    mkdir -p "$BUILD_DIR"
    echo "Compiling build/groovie2-linux..."
    "${CXX:-clang++}" ${CXXFLAGS:-} -std=c++20 -Wall -Wextra -pedantic -O2 groovie2.cpp config.cpp \
        ${LDFLAGS:-} -ljpeg -o "$BUILD_DIR/groovie2-linux"
    echo "Built build/groovie2-linux"
}

build_macos() {
    mkdir -p "$BUILD_DIR"

    local cxx="${CXX:-}"
    if [[ -z "$cxx" ]]; then
        cxx="$(xcrun --find c++ 2>/dev/null || true)"
    fi
    if [[ -z "$cxx" ]]; then
        cxx="c++"
    fi

    local extra_cxxflags=()
    local extra_ldflags=()
    if command -v brew >/dev/null 2>&1; then
        local jpeg_prefix
        jpeg_prefix="$(brew --prefix jpeg 2>/dev/null || brew --prefix jpeg-turbo 2>/dev/null || true)"
        if [[ -n "$jpeg_prefix" ]]; then
            extra_cxxflags+=("-I$jpeg_prefix/include")
            extra_ldflags+=("-L$jpeg_prefix/lib")
        fi
    fi

    echo "Compiling build/groovie2-macos..."
    "$cxx" ${CXXFLAGS:-} "${extra_cxxflags[@]}" -std=c++20 -Wall -Wextra -pedantic -O2 \
        groovie2.cpp config.cpp ${LDFLAGS:-} "${extra_ldflags[@]}" -ljpeg -o "$BUILD_DIR/groovie2-macos"
    echo "Built build/groovie2-macos"
}

case "${1:-release}" in
    release)
        build_windows release
        ;;
    debug)
        build_windows debug
        ;;
    linux)
        build_linux
        ;;
    macos|mac|darwin)
        build_macos
        ;;
    clean)
        rm -rf "$BUILD_DIR" groovie2.exe groovie2-debug.exe
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        usage
        exit 2
        ;;
esac
