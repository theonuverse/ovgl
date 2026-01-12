# ovgl - Run glibc Binaries on Termux Without Patching

**ovgl** (onuverse Glibc Loader) is a lightweight solution to run **unpatched** glibc ARM64 binaries on Termux **without proot or any container**.

## Table of Contents

- [Overview](#overview)
- [How It Works](#how-it-works)
- [Complete Setup Guide](#complete-setup-guide)
  - [Step 1: Update Termux](#step-1-update-termux)
  - [Step 2: Install Required Packages](#step-2-install-required-packages)
  - [Step 3: Fix glibc libc.so Symlink](#step-3-fix-glibc-libcso-symlink)
  - [Step 4: Install ovgl](#step-4-install-ovgl)
  - [Step 5: Build ovgl](#step-5-build-ovgl)
  - [Step 6: Configure Shell](#step-6-configure-shell)
- [Usage](#usage)
- [Example: Running Geekbench 6](#example-running-geekbench-6)
- [Environment Variables](#environment-variables)
- [Technical Details](#technical-details)
- [Troubleshooting](#troubleshooting)
- [The Problem ovgl Solves](#the-problem-ovgl-solves)
- [Alternative: Manual Patching (Old Method)](#alternative-manual-patching-old-method)
- [License](#license)

---

## Overview

Termux on Android uses **Bionic libc** (Android's C library), but many Linux binaries are compiled against **glibc** (GNU C Library). These binaries won't run directly on Termux because:

1. The ELF interpreter path (`/lib/ld-linux-aarch64.so.1`) doesn't exist on Android
2. Android's kernel execve() syscall fails to find the interpreter
3. Even if you invoke the loader manually, child processes spawned by the binary will fail

**ovgl** solves all these problems by:
- Invoking the glibc dynamic linker directly
- Intercepting `execve()` calls to redirect child processes through the glibc loader
- Fixing `/proc/self/exe` so binaries can find their resources

---

## How It Works

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              ovgl Architecture                               │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   User runs: ovgl ./geekbench6                                              │
│                     │                                                       │
│                     ▼                                                       │
│   ┌─────────────────────────────────────────────────────────────────┐      │
│   │  ovgl shell script                                               │      │
│   │  - Resolves binary path                                          │      │
│   │  - Sets OVGL_* environment variables                             │      │
│   │  - Clears bionic LD_PRELOAD                                      │      │
│   │  - Invokes glibc loader with LD_PRELOAD=libovgl_preload.so      │      │
│   └─────────────────────────────────────────────────────────────────┘      │
│                     │                                                       │
│                     ▼                                                       │
│   ┌─────────────────────────────────────────────────────────────────┐      │
│   │  ld-linux-aarch64.so.1 (glibc dynamic linker)                   │      │
│   │  - Loads geekbench6 with --library-path $PREFIX/glibc/lib       │      │
│   │  - Also loads libovgl_preload.so into process                   │      │
│   └─────────────────────────────────────────────────────────────────┘      │
│                     │                                                       │
│                     ▼                                                       │
│   ┌─────────────────────────────────────────────────────────────────┐      │
│   │  geekbench6 (main binary)                                        │      │
│   │  - Reads /proc/self/exe → libovgl_preload intercepts            │      │
│   │    Returns real binary path instead of loader path               │      │
│   │  - Calls execve("geekbench_aarch64") → libovgl_preload intercepts│      │
│   └─────────────────────────────────────────────────────────────────┘      │
│                     │                                                       │
│                     ▼                                                       │
│   ┌─────────────────────────────────────────────────────────────────┐      │
│   │  libovgl_preload.so (execve hook)                                │      │
│   │  - Detects geekbench_aarch64 is a glibc binary (ELF parsing)    │      │
│   │  - Rewrites execve to: ld-linux.so --library-path ... binary    │      │
│   │  - Child process also gets LD_PRELOAD=libovgl_preload.so        │      │
│   └─────────────────────────────────────────────────────────────────┘      │
│                     │                                                       │
│                     ▼                                                       │
│   ┌─────────────────────────────────────────────────────────────────┐      │
│   │  geekbench_aarch64 (child binary) - RUNS SUCCESSFULLY!          │      │
│   └─────────────────────────────────────────────────────────────────┘      │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Complete Setup Guide

### Step 1: Update Termux

First, make sure Termux is using a good mirror and is fully updated:

```bash
# Change to a fast mirror (select a mirror close to you)
termux-change-repo

# Update all packages
yes | pkg update && pkg upgrade -y
```

### Step 2: Install Required Packages

Install the glibc repository and necessary tools:

```bash
# Install glibc repository and basic tools
pkg install glibc-repo wget strace file clang -y

# Install glibc and patchelf (patchelf is optional but useful for debugging)
pkg install glibc patchelf -y
```

**Installed packages:**
| Package | Purpose |
|---------|---------|
| `glibc-repo` | Adds the glibc package repository |
| `glibc` | GNU C Library for Termux |
| `clang` | Compiler to build libovgl_preload.so |
| `wget` | Download files |
| `strace` | Debug system calls (optional) |
| `file` | Identify file types (optional) |
| `patchelf` | ELF patching tool (optional, for old method) |

### Step 3: Fix glibc libc.so Symlink

Termux's glibc package has a `libc.so` that's a linker script, which can cause issues. We need to replace it with a symlink to the actual library:

```bash
# Backup the linker script
mv $PREFIX/glibc/lib/libc.so $PREFIX/glibc/lib/libc.so.script

# Create symlink to the real library
ln -s $PREFIX/glibc/lib/libc.so.6 $PREFIX/glibc/lib/libc.so
```

**Why is this needed?**
The original `libc.so` is a linker script that references multiple libraries. Some programs don't handle this correctly when loaded through the dynamic linker directly. The symlink ensures `libc.so` points directly to the actual shared library.

### Step 4: Install ovgl

You can either clone from a repository or create the files manually:

#### Option A: Download/Clone
```bash
cd ~
git clone https://github.com/theonuverse/ovgl.git
# OR download and extract
```

#### Option B: Create Manually

Create the directory:
```bash
mkdir -p ~/ovgl
cd ~/ovgl
```

Create `ovgl_preload.c`:
```bash
cat > ovgl_preload.c << 'EOF'
/*
 * ovgl_preload.c - Intercept exec* calls to run glibc binaries on Termux
 * 
 * This library hooks execve/execveat to detect glibc ELF binaries and
 * rewrites the call to use the glibc dynamic linker.
 *
 * Compile with:
 *   clang --sysroot=$PREFIX/glibc -shared -fPIC -O2 -o libovgl_preload.so ovgl_preload.c -lc -ldl
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <errno.h>
#include <elf.h>
#include <sys/stat.h>
#include <limits.h>
#include <libgen.h>

// Environment variable names
#define GLIBC_LIB_ENV "OVGL_GLIBC_LIB"
#define GLIBC_LOADER_ENV "OVGL_GLIBC_LOADER"
#define OVGL_DEBUG_ENV "OVGL_DEBUG"
#define OVGL_ORIG_EXE_ENV "OVGL_ORIG_EXE"

static int debug_enabled = -1;

static void debug_print(const char *fmt, ...) {
    if (debug_enabled == -1) {
        debug_enabled = (getenv(OVGL_DEBUG_ENV) != NULL);
    }
    if (debug_enabled) {
        va_list args;
        va_start(args, fmt);
        fprintf(stderr, "[ovgl] ");
        vfprintf(stderr, fmt, args);
        fprintf(stderr, "\n");
        va_end(args);
    }
}

// Check if file is a glibc ELF that needs the loader
// Returns: 1 if glibc binary, 0 if not, -1 on error
static int is_glibc_elf(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    
    unsigned char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    
    if (n < (ssize_t)sizeof(Elf64_Ehdr)) return 0;
    
    // Check ELF magic
    if (buf[0] != 0x7f || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F') {
        return 0;
    }
    
    // Only handle 64-bit ELF
    if (buf[EI_CLASS] != ELFCLASS64) return 0;
    
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)buf;
    
    // Must be executable or shared object
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) return 0;
    
    // Find PT_INTERP
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) return 0;
    if (ehdr->e_phoff + ehdr->e_phnum * sizeof(Elf64_Phdr) > (size_t)n) return 0;
    
    Elf64_Phdr *phdr = (Elf64_Phdr *)(buf + ehdr->e_phoff);
    
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_INTERP) {
            if (phdr[i].p_offset + phdr[i].p_filesz > (size_t)n) return 0;
            
            char *interp = (char *)(buf + phdr[i].p_offset);
            
            debug_print("Found interpreter: %s", interp);
            
            // Check if it's a standard Linux glibc interpreter
            if (strstr(interp, "ld-linux") != NULL || 
                strstr(interp, "ld-musl") != NULL) {
                // Make sure it's NOT the Termux glibc loader
                const char *glibc_lib = getenv(GLIBC_LIB_ENV);
                if (glibc_lib && strstr(interp, glibc_lib) != NULL) {
                    debug_print("Already using Termux glibc loader");
                    return 0;
                }
                return 1;
            }
            return 0;
        }
    }
    
    return 0;
}

// Resolve path to absolute
static char *resolve_path(const char *path, char *resolved) {
    if (path[0] == '/') {
        strncpy(resolved, path, PATH_MAX - 1);
        resolved[PATH_MAX - 1] = '\0';
        return resolved;
    }
    
    // Try PATH lookup for non-absolute paths without /
    if (strchr(path, '/') == NULL) {
        char *path_env = getenv("PATH");
        if (path_env) {
            char *path_copy = strdup(path_env);
            char *dir = strtok(path_copy, ":");
            while (dir) {
                snprintf(resolved, PATH_MAX, "%s/%s", dir, path);
                if (access(resolved, X_OK) == 0) {
                    free(path_copy);
                    return resolved;
                }
                dir = strtok(NULL, ":");
            }
            free(path_copy);
        }
    }
    
    // Relative path - resolve from cwd
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) {
        snprintf(resolved, PATH_MAX, "%s/%s", cwd, path);
        return resolved;
    }
    
    strncpy(resolved, path, PATH_MAX - 1);
    resolved[PATH_MAX - 1] = '\0';
    return resolved;
}

// Real execve function pointer
static int (*real_execve)(const char *, char *const[], char *const[]) = NULL;

// Build new argv for loader invocation
static char **build_loader_argv(const char *loader, const char *lib_path, 
                                 const char *binary, char *const argv[]) {
    int argc = 0;
    while (argv[argc]) argc++;
    
    // New argv: loader --library-path lib --argv0 argv[0] binary [original args...]
    int new_argc = 6 + (argc > 0 ? argc - 1 : 0);
    char **new_argv = malloc((new_argc + 1) * sizeof(char *));
    if (!new_argv) return NULL;
    
    int i = 0;
    new_argv[i++] = strdup(loader);
    new_argv[i++] = strdup("--library-path");
    new_argv[i++] = strdup(lib_path);
    new_argv[i++] = strdup("--argv0");
    new_argv[i++] = strdup(argv[0] ? argv[0] : binary);
    new_argv[i++] = strdup(binary);
    
    // Copy remaining args (skip argv[0])
    for (int j = 1; j < argc; j++) {
        new_argv[i++] = strdup(argv[j]);
    }
    new_argv[i] = NULL;
    
    return new_argv;
}

// Build new envp with OVGL vars and clean LD_PRELOAD for bionic
static char **build_new_envp(char *const envp[], const char *orig_exe) {
    int envc = 0;
    while (envp[envc]) envc++;
    
    char **new_envp = malloc((envc + 2) * sizeof(char *));
    if (!new_envp) return NULL;
    
    int j = 0;
    for (int i = 0; i < envc; i++) {
        // Skip bionic LD_PRELOAD that might interfere
        if (strncmp(envp[i], "LD_PRELOAD=", 11) == 0) {
            if (strstr(envp[i], "libtermux-exec") != NULL) {
                debug_print("Removing bionic LD_PRELOAD: %s", envp[i]);
                continue;
            }
        }
        // Skip existing OVGL_ORIG_EXE
        if (strncmp(envp[i], OVGL_ORIG_EXE_ENV "=", sizeof(OVGL_ORIG_EXE_ENV)) == 0) {
            continue;
        }
        new_envp[j++] = strdup(envp[i]);
    }
    
    // Add original exe path
    char *orig_exe_env = malloc(strlen(OVGL_ORIG_EXE_ENV) + strlen(orig_exe) + 2);
    sprintf(orig_exe_env, "%s=%s", OVGL_ORIG_EXE_ENV, orig_exe);
    new_envp[j++] = orig_exe_env;
    
    new_envp[j] = NULL;
    return new_envp;
}

static void free_strarray(char **arr) {
    if (!arr) return;
    for (int i = 0; arr[i]; i++) {
        free(arr[i]);
    }
    free(arr);
}

// Hooked execve
int execve(const char *pathname, char *const argv[], char *const envp[]) {
    if (!real_execve) {
        real_execve = dlsym(RTLD_NEXT, "execve");
        if (!real_execve) {
            errno = ENOSYS;
            return -1;
        }
    }
    
    const char *glibc_lib = getenv(GLIBC_LIB_ENV);
    const char *glibc_loader = getenv(GLIBC_LOADER_ENV);
    
    if (!glibc_lib || !glibc_loader) {
        debug_print("OVGL env vars not set, passing through");
        return real_execve(pathname, argv, envp);
    }
    
    char resolved[PATH_MAX];
    resolve_path(pathname, resolved);
    
    debug_print("execve intercepted: %s -> %s", pathname, resolved);
    
    int is_glibc = is_glibc_elf(resolved);
    if (is_glibc != 1) {
        debug_print("Not a glibc binary (result=%d), passing through", is_glibc);
        return real_execve(pathname, argv, envp);
    }
    
    debug_print("Detected glibc binary, rewriting execve");
    
    char **new_argv = build_loader_argv(glibc_loader, glibc_lib, resolved, argv);
    if (!new_argv) {
        debug_print("Failed to build new argv");
        return real_execve(pathname, argv, envp);
    }
    
    char **new_envp = build_new_envp(envp, resolved);
    if (!new_envp) {
        free_strarray(new_argv);
        return real_execve(pathname, argv, envp);
    }
    
    debug_print("Executing: %s %s %s %s %s %s", 
                new_argv[0], new_argv[1], new_argv[2], 
                new_argv[3], new_argv[4], new_argv[5]);
    
    int ret = real_execve(glibc_loader, new_argv, new_envp);
    int saved_errno = errno;
    
    free_strarray(new_argv);
    free_strarray(new_envp);
    
    errno = saved_errno;
    return ret;
}

// Hook execv
int execv(const char *pathname, char *const argv[]) {
    return execve(pathname, argv, environ);
}

// Hook execvp
int execvp(const char *file, char *const argv[]) {
    char resolved[PATH_MAX];
    resolve_path(file, resolved);
    return execve(resolved, argv, environ);
}

// Hook execvpe
int execvpe(const char *file, char *const argv[], char *const envp[]) {
    char resolved[PATH_MAX];
    resolve_path(file, resolved);
    return execve(resolved, argv, envp);
}

// Hook readlink to fix /proc/self/exe
static ssize_t (*real_readlink)(const char *, char *, size_t) = NULL;

ssize_t readlink(const char *pathname, char *buf, size_t bufsiz) {
    if (!real_readlink) {
        real_readlink = dlsym(RTLD_NEXT, "readlink");
    }
    
    ssize_t ret = real_readlink(pathname, buf, bufsiz);
    
    if (ret > 0 && strcmp(pathname, "/proc/self/exe") == 0) {
        const char *orig_exe = getenv(OVGL_ORIG_EXE_ENV);
        if (orig_exe) {
            size_t len = strlen(orig_exe);
            if (len < bufsiz) {
                memcpy(buf, orig_exe, len);
                debug_print("readlink(/proc/self/exe) -> %s", orig_exe);
                return len;
            }
        }
    }
    
    return ret;
}

// Hook readlinkat
static ssize_t (*real_readlinkat)(int, const char *, char *, size_t) = NULL;

ssize_t readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz) {
    if (!real_readlinkat) {
        real_readlinkat = dlsym(RTLD_NEXT, "readlinkat");
    }
    
    ssize_t ret = real_readlinkat(dirfd, pathname, buf, bufsiz);
    
    if (ret > 0 && strcmp(pathname, "/proc/self/exe") == 0) {
        const char *orig_exe = getenv(OVGL_ORIG_EXE_ENV);
        if (orig_exe) {
            size_t len = strlen(orig_exe);
            if (len < bufsiz) {
                memcpy(buf, orig_exe, len);
                debug_print("readlinkat(/proc/self/exe) -> %s", orig_exe);
                return len;
            }
        }
    }
    
    return ret;
}

// Constructor
__attribute__((constructor))
static void init(void) {
    debug_print("ovgl_preload loaded");
}
EOF
```

Create `build.sh`:
```bash
cat > build.sh << 'EOF'
#!/data/data/com.termux/files/usr/bin/sh
#
# Build script for ovgl preload library
#

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
GLIBC_PREFIX="${GLIBC_PREFIX:-$PREFIX/glibc}"

info() { printf "[INFO] %s\n" "$1"; }
error() { printf "[ERROR] %s\n" "$1" >&2; exit 1; }

cd "$SCRIPT_DIR"

CC="clang"
SYSROOT="$GLIBC_PREFIX"

if ! command -v clang >/dev/null 2>&1; then
    error "clang not found. Please install: pkg install clang"
fi

info "Using compiler: $CC with sysroot $SYSROOT"
info "Compiling ovgl_preload.c..."

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

chmod +x ovgl

info ""
info "Build complete! Usage: ovgl ./your_program"
EOF
chmod +x build.sh
```

Create `ovgl` wrapper script:
```bash
cat > ovgl << 'EOF'
#!/data/data/com.termux/files/usr/bin/sh
#
# ovgl - Run glibc binaries on Termux without patching
#

OVGL_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
GLIBC_PREFIX="${GLIBC_PREFIX:-$PREFIX/glibc}"
GLIBC_LIB="$GLIBC_PREFIX/lib"
GLIBC_LOADER="$GLIBC_LIB/ld-linux-aarch64.so.1"
PRELOAD_LIB="$OVGL_DIR/libovgl_preload.so"

usage() {
    echo "Usage: ovgl [options] <program> [args...]"
    echo ""
    echo "Run glibc ARM64 binaries on Termux without patching."
    echo ""
    echo "Options:"
    echo "    -h, --help      Show this help message"
    echo "    -d, --debug     Enable debug output"
    exit 0
}

error() {
    echo "Error: $1" >&2
    exit 1
}

DEBUG=""
while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage ;;
        -d|--debug) DEBUG=1; shift ;;
        --) shift; break ;;
        -*) error "Unknown option: $1" ;;
        *) break ;;
    esac
done

if [ $# -eq 0 ]; then
    error "No program specified. Use 'ovgl --help' for usage."
fi

PROGRAM="$1"
shift

# Resolve program path
if [ "${PROGRAM#/}" = "$PROGRAM" ]; then
    if [ "${PROGRAM#./}" != "$PROGRAM" ] || [ "${PROGRAM#*/}" != "$PROGRAM" ]; then
        PROGRAM="$(pwd)/$PROGRAM"
    else
        if [ -x "./$PROGRAM" ]; then
            PROGRAM="$(pwd)/$PROGRAM"
        else
            OLDIFS="$IFS"
            IFS=':'
            FOUND=""
            for p in $PATH; do
                if [ -x "$p/$PROGRAM" ]; then
                    FOUND="$p/$PROGRAM"
                    break
                fi
            done
            IFS="$OLDIFS"
            if [ -z "$FOUND" ]; then
                error "Program not found: $PROGRAM"
            fi
            PROGRAM="$FOUND"
        fi
    fi
fi

[ ! -f "$PROGRAM" ] && error "Program not found: $PROGRAM"
[ ! -x "$PROGRAM" ] && error "Program is not executable: $PROGRAM"
[ ! -f "$GLIBC_LOADER" ] && error "glibc loader not found: $GLIBC_LOADER"
[ ! -f "$PRELOAD_LIB" ] && error "Preload library not found: $PRELOAD_LIB"

export OVGL_GLIBC_LIB="$GLIBC_LIB"
export OVGL_GLIBC_LOADER="$GLIBC_LOADER"
export OVGL_ORIG_EXE="$PROGRAM"

if [ -n "$DEBUG" ] || [ -n "$OVGL_DEBUG" ]; then
    export OVGL_DEBUG=1
    echo "ovgl: Running $PROGRAM"
    echo "ovgl: GLIBC_LIB=$GLIBC_LIB"
    echo "ovgl: GLIBC_LOADER=$GLIBC_LOADER"
    echo "ovgl: PRELOAD_LIB=$PRELOAD_LIB"
fi

exec env LD_PRELOAD="$PRELOAD_LIB" \
    "$GLIBC_LOADER" \
    --library-path "$GLIBC_LIB" \
    --argv0 "$PROGRAM" \
    "$PROGRAM" \
    "$@"
EOF
chmod +x ovgl
```

### Step 5: Build ovgl

```bash
cd ~/ovgl

# Clear LD_PRELOAD to avoid bionic interference during build
LD_PRELOAD="" sh build.sh
```

Expected output:
```
[INFO] Using compiler: clang with sysroot /data/data/com.termux/files/usr/glibc
[INFO] Compiling ovgl_preload.c...
[INFO] Successfully built libovgl_preload.so
-rwx------ 1 u0_a293 u0_a293 10504 Jan 12 19:16 libovgl_preload.so
[INFO] 
[INFO] Build complete! Usage: ovgl ./your_program
```

### Step 6: Configure Shell

Add ovgl to your PATH and create an alias to avoid LD_PRELOAD issues:

```bash
# Add to ~/.bashrc or ~/.profile
cat >> ~/.bashrc << 'EOF'

# ovgl - glibc binary runner
export PATH="$HOME/ovgl:$PATH"
alias ovgl='LD_PRELOAD="" ovgl'
EOF

# Reload
source ~/.bashrc
```

---

## Usage

### Basic Usage

```bash
# Run a glibc binary
ovgl ./program

# Run with arguments
ovgl ./program --arg1 value1 --arg2

# With debug output
ovgl -d ./program

# Or with environment variable
OVGL_DEBUG=1 ovgl ./program
```

### Alias (Recommended)

If you set up the alias in Step 6, you can simply run:
```bash
ovgl ./program
```

Without the alias, you need:
```bash
LD_PRELOAD="" ovgl ./program
```

---

## Example: Running Geekbench 6

```bash
# Download Geekbench
cd ~
wget https://cdn.geekbench.com/Geekbench-6.5.0-LinuxARMPreview.tar.gz
tar -xvf Geekbench-6.5.0-LinuxARMPreview.tar.gz
rm Geekbench-6.5.0-LinuxARMPreview.tar.gz
cd Geekbench-6.5.0-LinuxARMPreview

# Run without any patching!
ovgl ./geekbench6
```

**With debug output:**
```bash
ovgl -d ./geekbench6
```

You should see:
```
ovgl: Running /data/data/com.termux/files/home/Geekbench-6.5.0-LinuxARMPreview/./geekbench6
ovgl: GLIBC_LIB=/data/data/com.termux/files/usr/glibc/lib
ovgl: GLIBC_LOADER=/data/data/com.termux/files/usr/glibc/lib/ld-linux-aarch64.so.1
ovgl: PRELOAD_LIB=/data/data/com.termux/files/home/ovgl/libovgl_preload.so
[ovgl] ovgl_preload loaded
[ovgl] readlink(/proc/self/exe) -> /data/data/com.termux/files/home/Geekbench-6.5.0-LinuxARMPreview/./geekbench6
[ovgl] execve intercepted: ...geekbench_aarch64 -> ...geekbench_aarch64
[ovgl] Found interpreter: /lib/ld-linux-aarch64.so.1
[ovgl] Detected glibc binary, rewriting execve
[ovgl] Executing: ld-linux-aarch64.so.1 --library-path ... --argv0 ... geekbench_aarch64
[ovgl] ovgl_preload loaded
Geekbench 6.5.0 Preview : https://www.geekbench.com/
...
```

---

## Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `GLIBC_PREFIX` | Path to glibc installation | `$PREFIX/glibc` |
| `OVGL_DEBUG` | Set to `1` to enable debug output | unset |
| `OVGL_GLIBC_LIB` | (Internal) Path to glibc libraries | auto |
| `OVGL_GLIBC_LOADER` | (Internal) Path to glibc loader | auto |
| `OVGL_ORIG_EXE` | (Internal) Original executable path | auto |

---

## Technical Details

### Hooked Functions

The preload library intercepts:

| Function | Purpose |
|----------|---------|
| `execve()` | Main exec hook - rewrites glibc binary execution |
| `execv()` | Wrapper, calls execve() |
| `execvp()` | PATH resolution + execve() |
| `execvpe()` | PATH resolution + execve() with custom envp |
| `readlink()` | Fixes `/proc/self/exe` |
| `readlinkat()` | Fixes `/proc/self/exe` (fd variant) |

### ELF Detection

The library parses ELF headers to detect glibc binaries:

1. Check ELF magic bytes (`\x7fELF`)
2. Verify 64-bit ELF (`ELFCLASS64`)
3. Check executable type (`ET_EXEC` or `ET_DYN`)
4. Find `PT_INTERP` program header
5. Check if interpreter contains `ld-linux` or `ld-musl`
6. Exclude already-patched binaries (Termux glibc path)

### execve Rewrite

When a glibc binary is detected:

```
Original: execve("./program", ["./program", "arg1"], envp)

Rewritten: execve(
    "/data/data/com.termux/files/usr/glibc/lib/ld-linux-aarch64.so.1",
    [
        "ld-linux-aarch64.so.1",
        "--library-path", "/data/data/com.termux/files/usr/glibc/lib",
        "--argv0", "./program",
        "/full/path/to/program",
        "arg1"
    ],
    envp_with_OVGL_vars
)
```

---

## Troubleshooting

### "version `LIBC' not found"

This happens when glibc bash loads with bionic LD_PRELOAD:

```bash
# Solution: Clear LD_PRELOAD before running
LD_PRELOAD="" ovgl ./program

# Or use the alias
alias ovgl='LD_PRELOAD="" ovgl'
```

### Exit code 127 / Silent failure

Usually means the loader or interpreter can't be found:

```bash
# Run with debug
ovgl -d ./program

# Or use strace
LD_PRELOAD="" strace -f ovgl ./program 2>&1 | grep -E "execve|ENOENT"
```

### Missing shared libraries

```bash
# Check what the binary needs
readelf -d ./program | grep NEEDED

# List available glibc libraries
ls $PREFIX/glibc/lib/*.so*
```

### Child process fails

Check debug output for:
```
[ovgl] execve intercepted: ...
[ovgl] Detected glibc binary, rewriting execve
```

If you don't see this, the preload library isn't being loaded in child processes.

### Build fails

Make sure you have clang and clear LD_PRELOAD:
```bash
pkg install clang
cd ~/ovgl
LD_PRELOAD="" sh build.sh
```

---

## The Problem ovgl Solves

### Why can't glibc binaries run directly on Termux?

1. **ELF Interpreter Path**: Linux glibc binaries have `/lib/ld-linux-aarch64.so.1` as their interpreter. This path doesn't exist on Android.

2. **Kernel execve()**: When you run a binary, the kernel reads the ELF interpreter path and tries to load it. On Android, this fails with ENOENT.

3. **Manual loader invocation**: You can run:
   ```bash
   $PREFIX/glibc/lib/ld-linux-aarch64.so.1 --library-path $PREFIX/glibc/lib ./program
   ```
   But if `program` spawns child processes, those fail because they go through the kernel's execve again.

4. **`/proc/self/exe` issue**: Many programs read `/proc/self/exe` to find their installation directory. When running through the loader, this returns the loader's path, not the actual program's path.

### How ovgl solves these problems:

1. **Direct loader invocation**: ovgl calls the glibc loader directly with the correct library path.

2. **execve hook**: The preload library intercepts all exec* calls and rewrites them to use the loader.

3. **`/proc/self/exe` fix**: The preload library hooks readlink to return the correct binary path.

4. **Environment propagation**: The preload library is inherited by child processes, so the hooks work recursively.

---

## Alternative: Manual Patching (Old Method)

If you prefer to patch binaries instead of using ovgl:

```bash
export G_LIB="$PREFIX/glibc/lib"
export G_LDR="$PREFIX/glibc/lib/ld-linux-aarch64.so.1"

# Patch the binary
patchelf --set-interpreter "$G_LDR" --set-rpath "$G_LIB" ./program

# Run directly (no ovgl needed)
LD_PRELOAD="" ./program
```

**Downsides of patching:**
- Modifies the binary permanently
- Need to patch every binary
- Need to re-patch after updates
- Can't run the same binary on other systems

**ovgl advantages:**
- No modification to binaries
- Works with all glibc binaries automatically
- Handles child processes
- Binaries remain portable

---

## Quick Reference

```bash
# One-time setup
termux-change-repo
yes | pkg update && pkg upgrade -y
pkg install glibc-repo wget clang -y
pkg install glibc -y

# Fix libc.so
mv $PREFIX/glibc/lib/libc.so $PREFIX/glibc/lib/libc.so.script
ln -s $PREFIX/glibc/lib/libc.so.6 $PREFIX/glibc/lib/libc.so

# Install ovgl (download or create files as shown above)
cd ~/ovgl
LD_PRELOAD="" sh build.sh

# Add to ~/.bashrc
echo 'export PATH="$HOME/ovgl:$PATH"' >> ~/.bashrc
echo 'alias ovgl="LD_PRELOAD=\"\" ovgl"' >> ~/.bashrc
source ~/.bashrc

# Usage
ovgl ./any_glibc_binary
```

---

## License

MIT License

Copyright (c) 2026

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
