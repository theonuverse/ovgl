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

First, make sure Termux is fully updated:

```bash
yes | pkg up
```

### Step 2: Install Required Packages

Install the glibc repository and necessary tools:

```bash
pkg ins glibc-repo wget clang git -y
pkg ins glibc -y
```

**Installed packages:**
| Package | Purpose |
|---------|---------|
| `glibc-repo` | Adds the glibc package repository |
| `glibc` | GNU C Library for Termux |
| `clang` | Compiler to build libovgl_preload.so |
| `wget` | Download files |
| `git` | For cloning the repository |

### Step 3: Fix glibc libc.so Symlink

Termux's glibc package has a `libc.so` that's a linker script, which can cause issues. We need to replace it with a symlink to the actual library:

```bash
mv $PREFIX/glibc/lib/libc.so $PREFIX/glibc/lib/libc.so.script
ln -s $PREFIX/glibc/lib/libc.so.6 $PREFIX/glibc/lib/libc.so
```

**Why is this needed?**
The original `libc.so` is a linker script that references multiple libraries. Some programs don't handle this correctly when loaded through the dynamic linker directly. The symlink ensures `libc.so` points directly to the actual shared library.

### Step 4: Install ovgl

#### Download/Clone
```bash
cd ~ && git clone https://github.com/theonuverse/ovgl.git
```

### Step 5: Build ovgl

```bash
cd ~/ovgl && sh build.sh
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
cat >> ~/.bashrc << 'EOF'

# ovgl - glibc binary runner
export PATH="$HOME/ovgl:$PATH"
EOF

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

**ovgl advantages:**
- No modification to binaries
- Works with all glibc binaries automatically
- Handles child processes
- Binaries remain portable

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
