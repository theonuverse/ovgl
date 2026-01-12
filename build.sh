#!/data/data/com.termux/files/usr/bin/sh
#
# Build script for ovgl preload library
# Uses clang with glibc sysroot
#

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
GLIBC_PREFIX="${GLIBC_PREFIX:-$PREFIX/glibc}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info() { printf "${GREEN}[INFO]${NC} %s\n" "$1"; }
warn() { printf "${YELLOW}[WARN]${NC} %s\n" "$1"; }
error() { printf "${RED}[ERROR]${NC} %s\n" "$1" >&2; exit 1; }

cd "$SCRIPT_DIR"

# Use clang with glibc sysroot
CC="clang"
SYSROOT="$GLIBC_PREFIX"

# Check if clang exists
if ! command -v clang >/dev/null 2>&1; then
    error "clang not found. Please install: pkg install clang"
fi

info "Using compiler: $CC with sysroot $SYSROOT"
info "Compiling ovgl_preload.c..."

# Clear LD_PRELOAD to avoid bionic interference
LD_PRELOAD="" $CC \
    --sysroot="$SYSROOT" \
    -shared \
    -fPIC \
    -O2 \
    -Wall \
    -Wextra \
    --target=aarch64-linux-gnu \
    -nostdlib \
    -I"$SYSROOT/include" \
    -L"$SYSROOT/lib" \
    -Wl,--dynamic-linker="$SYSROOT/lib/ld-linux-aarch64.so.1" \
    -Wl,-rpath,"$SYSROOT/lib" \
    -o libovgl_preload.so \
    ovgl_preload.c \
    -lc -ldl

if [ -f "libovgl_preload.so" ]; then
    info "Successfully built libovgl_preload.so"
    ls -la libovgl_preload.so
else
    error "Build failed"
fi

# Make the wrapper executable
chmod +x ovgl

info ""
info "Build complete! To use ovgl:"
info "  1. Add to PATH:  export PATH=\"$SCRIPT_DIR:\$PATH\""
info "  2. Run:          ovgl ./your_program"
info ""
info "Or run directly:   $SCRIPT_DIR/ovgl ./your_program"
