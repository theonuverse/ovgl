#!/data/data/com.termux/files/usr/bin/bash
#
# Build script for ovgl - native bionic wrapper with embedded preload
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
GLIBC_PREFIX="${GLIBC_PREFIX:-$PREFIX/glibc}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

info() { printf "${GREEN}[INFO]${NC} %s\n" "$1"; }
warn() { printf "${YELLOW}[WARN]${NC} %s\n" "$1"; }
error() { printf "${RED}[ERROR]${NC} %s\n" "$1" >&2; exit 1; }

cd "$SCRIPT_DIR"

# Check for clang
if ! command -v clang >/dev/null 2>&1; then
    error "clang not found. Please install: pkg install clang"
fi

# ============== Step 1: Build preload library (glibc) ==============

info "Step 1: Building preload library with glibc..."

if [ ! -d "$GLIBC_PREFIX" ]; then
    error "Glibc not found at $GLIBC_PREFIX. Install with: pkg install glibc-runner"
fi

# Clear LD_PRELOAD to avoid interference
LD_PRELOAD="" clang \
    --sysroot="$GLIBC_PREFIX" \
    -shared \
    -fPIC \
    -O2 \
    -Wall \
    -Wextra \
    --target=aarch64-linux-gnu \
    -I"$GLIBC_PREFIX/include" \
    -L"$GLIBC_PREFIX/lib" \
    -Wl,--dynamic-linker="$GLIBC_PREFIX/lib/ld-linux-aarch64.so.1" \
    -Wl,-rpath,"$GLIBC_PREFIX/lib" \
    -o libovgl_preload.so \
    ovgl_preload.c \
    "$GLIBC_PREFIX/lib/libc.so.6" \
    "$GLIBC_PREFIX/lib/libdl.so.2" \
    2>&1 || {
        # Try alternative compilation
        warn "Standard build failed, trying alternative..."
        LD_PRELOAD="" clang \
            --sysroot="$GLIBC_PREFIX" \
            -shared \
            -fPIC \
            -O2 \
            --target=aarch64-linux-gnu \
            -nostdlib \
            -I"$GLIBC_PREFIX/include" \
            -L"$GLIBC_PREFIX/lib" \
            -Wl,--dynamic-linker="$GLIBC_PREFIX/lib/ld-linux-aarch64.so.1" \
            -Wl,-rpath,"$GLIBC_PREFIX/lib" \
            -Wl,--no-as-needed \
            -o libovgl_preload.so \
            ovgl_preload.c \
            -lc -ldl
    }

if [ ! -f "libovgl_preload.so" ]; then
    error "Failed to build libovgl_preload.so"
fi

info "Built libovgl_preload.so ($(stat -c%s libovgl_preload.so) bytes)"

# ============== Step 2: Generate embedded header ==============

info "Step 2: Generating embedded preload header..."

# Generate C array from binary using od (xxd may not be available)
{
    echo "/* Auto-generated - do not edit */"
    echo "/* Embedded preload library data */"
    echo ""
    echo "static const unsigned char preload_so_data[] = {"
    od -An -tx1 -v libovgl_preload.so | sed 's/[0-9a-f]\{2\}/0x&,/g; s/  */ /g; s/^/    /'
    echo "};"
    echo ""
    echo "static const unsigned int preload_so_size = sizeof(preload_so_data);"
} > preload_data.h

info "Generated preload_data.h ($(wc -l < preload_data.h) lines)"

# ============== Step 3: Build main wrapper (bionic) ==============

info "Step 3: Building ovgl wrapper (native bionic)..."

# Build with bionic (default Android toolchain)
clang \
    -O2 \
    -Wall \
    -Wextra \
    -DEMBED_PRELOAD \
    -o ovgl \
    ovgl.c

if [ ! -f "ovgl" ]; then
    error "Failed to build ovgl"
fi

info "Built ovgl ($(stat -c%s ovgl) bytes)"

# ============== Step 4: Verify builds ==============

info "Step 4: Verifying builds..."

echo ""
printf "${BLUE}libovgl_preload.so:${NC}\n"
file libovgl_preload.so
readelf -d libovgl_preload.so 2>/dev/null | grep -E "(NEEDED|RUNPATH|RPATH)" | head -5 || true

echo ""
printf "${BLUE}ovgl:${NC}\n"
file ovgl
readelf -d ovgl 2>/dev/null | grep -E "(NEEDED|INTERP)" | head -5 || true

# ============== Done ==============

echo ""
printf "${GREEN}========================================${NC}\n"
printf "${GREEN}Build complete!${NC}\n"
printf "${GREEN}========================================${NC}\n"
echo ""
echo "Files created:"
echo "  • ovgl                  - Main wrapper binary (bionic)"
echo "  • libovgl_preload.so    - Preload library (glibc)"
echo "  • preload_data.h        - Embedded preload data"
echo ""
echo "Installation:"
echo "  cp ovgl \$PREFIX/bin/"
echo "  chmod +x \$PREFIX/bin/ovgl"
echo ""
echo "Usage:"
echo "  ovgl <binary> [args...]"
echo "  ovgl -h                 # Show help"
echo ""
