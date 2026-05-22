#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

case "${1:-build}" in
    build)
        ./build.sh macos
        ;;
    clean)
        rm -rf build/groovie2-macos
        ;;
    rebuild)
        rm -rf build/groovie2-macos
        ./build.sh macos
        ;;
    -h|--help|help)
        cat <<'EOF'
Usage: ./build.command [build|clean|rebuild]

Builds build/groovie2-macos as a CLI-only macOS executable.
EOF
        ;;
    *)
        echo "Unknown command: $1"
        echo "Usage: ./build.command [build|clean|rebuild]"
        exit 2
        ;;
esac
