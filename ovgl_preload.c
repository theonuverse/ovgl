/*
 * ovgl_preload.c - Intercept exec* calls to run glibc binaries on Termux
 * 
 * This library hooks execve/execveat to detect glibc ELF binaries and
 * rewrites the call to use the glibc dynamic linker.
 *
 * Compile with glibc:
 *   $PREFIX/glibc/bin/gcc -shared -fPIC -o libovgl_preload.so ovgl_preload.c -ldl
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

// Environment variable for the glibc lib path
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
            // Get interpreter path
            if (phdr[i].p_offset + phdr[i].p_filesz > (size_t)n) return 0;
            
            char *interp = (char *)(buf + phdr[i].p_offset);
            
            debug_print("Found interpreter: %s", interp);
            
            // Check if it's a standard Linux glibc interpreter (not Android's)
            if (strstr(interp, "ld-linux") != NULL || 
                strstr(interp, "ld-musl") != NULL) {
                // Make sure it's NOT the Termux glibc loader (already set up)
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
    // Count original argv
    int argc = 0;
    while (argv[argc]) argc++;
    
    // New argv: loader --library-path lib binary [original args...]
    // We need: loader, --library-path, lib_path, --argv0, argv[0], binary, argv[1..n], NULL
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
    // Count original envp
    int envc = 0;
    while (envp[envc]) envc++;
    
    // Allocate space for original + OVGL_ORIG_EXE + NULL
    char **new_envp = malloc((envc + 2) * sizeof(char *));
    if (!new_envp) return NULL;
    
    int j = 0;
    for (int i = 0; i < envc; i++) {
        // Skip bionic LD_PRELOAD that might interfere
        if (strncmp(envp[i], "LD_PRELOAD=", 11) == 0) {
            // Check if it contains termux-exec (bionic library)
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
    
    // Resolve the path
    char resolved[PATH_MAX];
    resolve_path(pathname, resolved);
    
    debug_print("execve intercepted: %s -> %s", pathname, resolved);
    
    // Check if it's a glibc binary
    int is_glibc = is_glibc_elf(resolved);
    if (is_glibc != 1) {
        debug_print("Not a glibc binary (result=%d), passing through", is_glibc);
        return real_execve(pathname, argv, envp);
    }
    
    debug_print("Detected glibc binary, rewriting execve");
    
    // Build new argv with loader
    char **new_argv = build_loader_argv(glibc_loader, glibc_lib, resolved, argv);
    if (!new_argv) {
        debug_print("Failed to build new argv");
        return real_execve(pathname, argv, envp);
    }
    
    // Build new envp
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

// Also hook execv (calls execve internally usually, but let's be safe)
int execv(const char *pathname, char *const argv[]) {
    return execve(pathname, argv, environ);
}

// Hook execvp - need to resolve PATH
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
    
    // Check if reading /proc/self/exe
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
    
    // Check if reading /proc/self/exe
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

// Constructor - runs when library is loaded
__attribute__((constructor))
static void init(void) {
    debug_print("ovgl_preload loaded");
}
