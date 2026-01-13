/*
 * ovgl - OpenVGL Wrapper for running glibc binaries on Termux
 * 
 * A native bionic executable that:
 * - Detects binary architecture (arm64 vs x86_64)
 * - Uses glibc loader for arm64 glibc binaries
 * - Uses box64 for x86_64 binaries
 * - Embeds the preload library for seamless operation
 *
 * Compile with clang (bionic):
 *   clang -O2 -o ovgl ovgl.c -DEMBED_PRELOAD
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <elf.h>
#include <sys/stat.h>
#include <limits.h>
#include <libgen.h>

/* ============== Configuration ============== */

#define GLIBC_PREFIX "/data/data/com.termux/files/usr/glibc"
#define GLIBC_LIB GLIBC_PREFIX "/lib"
#define GLIBC_LIB_X86_64 GLIBC_PREFIX "/lib_x86_64"
#define GLIBC_LOADER GLIBC_LIB "/ld-linux-aarch64.so.1"
#define BOX64_X86_LIBS_DEFAULT "glibc/lib_x86_64"

#define COLOR_RED     "\033[0;31m"
#define COLOR_GREEN   "\033[0;32m"
#define COLOR_YELLOW  "\033[1;33m"
#define COLOR_BLUE    "\033[0;34m"
#define COLOR_RESET   "\033[0m"

/* ============== Embedded Preload Library ============== */

#ifdef EMBED_PRELOAD
#include "preload_data.h"
#else
/* Fallback: no embedded preload, will look for external file */
static const unsigned char preload_so_data[] = {0};
static const unsigned int preload_so_size = 0;
#endif

/* ============== Architecture Detection ============== */

typedef enum {
    ARCH_UNKNOWN = 0,
    ARCH_AARCH64,
    ARCH_X86_64,
    ARCH_NOT_ELF,
    ARCH_ERROR
} elf_arch_t;

typedef enum {
    INTERP_NONE = 0,
    INTERP_GLIBC,
    INTERP_BIONIC,
    INTERP_MUSL,
    INTERP_OTHER
} interp_type_t;

typedef struct {
    elf_arch_t arch;
    interp_type_t interp;
    char interp_path[256];
} binary_info_t;

static binary_info_t analyze_binary(const char *path) {
    binary_info_t info = {0};
    info.arch = ARCH_ERROR;
    
    int fd = open(path, O_RDONLY);
    if (fd < 0) return info;
    
    unsigned char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    
    if (n < (ssize_t)sizeof(Elf64_Ehdr)) {
        info.arch = ARCH_NOT_ELF;
        return info;
    }
    
    /* Check ELF magic */
    if (buf[0] != 0x7f || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F') {
        info.arch = ARCH_NOT_ELF;
        return info;
    }
    
    /* Only handle 64-bit ELF */
    if (buf[EI_CLASS] != ELFCLASS64) {
        info.arch = ARCH_UNKNOWN;
        return info;
    }
    
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)buf;
    
    /* Detect architecture */
    switch (ehdr->e_machine) {
        case EM_AARCH64:
            info.arch = ARCH_AARCH64;
            break;
        case EM_X86_64:
            info.arch = ARCH_X86_64;
            break;
        default:
            info.arch = ARCH_UNKNOWN;
            return info;
    }
    
    /* Find interpreter */
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        info.interp = INTERP_NONE;
        return info;
    }
    
    if (ehdr->e_phoff + ehdr->e_phnum * sizeof(Elf64_Phdr) > (size_t)n) {
        return info;
    }
    
    Elf64_Phdr *phdr = (Elf64_Phdr *)(buf + ehdr->e_phoff);
    
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_INTERP) {
            if (phdr[i].p_offset + phdr[i].p_filesz > (size_t)n) break;
            if (phdr[i].p_filesz >= sizeof(info.interp_path)) break;
            
            memcpy(info.interp_path, buf + phdr[i].p_offset, phdr[i].p_filesz);
            info.interp_path[phdr[i].p_filesz] = '\0';
            
            /* Classify interpreter */
            if (strstr(info.interp_path, "ld-linux") != NULL) {
                info.interp = INTERP_GLIBC;
            } else if (strstr(info.interp_path, "linker64") != NULL ||
                       strstr(info.interp_path, "linker") != NULL) {
                info.interp = INTERP_BIONIC;
            } else if (strstr(info.interp_path, "ld-musl") != NULL) {
                info.interp = INTERP_MUSL;
            } else {
                info.interp = INTERP_OTHER;
            }
            break;
        }
    }
    
    return info;
}

/* ============== Path Resolution ============== */

static char *find_in_path(const char *name, char *resolved, size_t size) {
    /* If it contains a slash, treat as path */
    if (strchr(name, '/') != NULL) {
        if (name[0] == '/') {
            strncpy(resolved, name, size - 1);
            resolved[size - 1] = '\0';
        } else {
            char cwd[PATH_MAX];
            if (getcwd(cwd, sizeof(cwd))) {
                snprintf(resolved, size, "%s/%s", cwd, name);
            } else {
                strncpy(resolved, name, size - 1);
                resolved[size - 1] = '\0';
            }
        }
        if (access(resolved, X_OK) == 0) return resolved;
        return NULL;
    }
    
    /* First, check current directory */
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) {
        snprintf(resolved, size, "%s/%s", cwd, name);
        if (access(resolved, X_OK) == 0) return resolved;
    }
    
    /* Search in PATH */
    char *path_env = getenv("PATH");
    if (!path_env) return NULL;
    
    char *path_copy = strdup(path_env);
    if (!path_copy) return NULL;
    
    char *saveptr;
    char *dir = strtok_r(path_copy, ":", &saveptr);
    
    while (dir) {
        snprintf(resolved, size, "%s/%s", dir, name);
        if (access(resolved, X_OK) == 0) {
            free(path_copy);
            return resolved;
        }
        dir = strtok_r(NULL, ":", &saveptr);
    }
    
    free(path_copy);
    return NULL;
}

static char *find_box64(char *resolved, size_t size) {
    /* Prefer system box64 at $PREFIX/bin over any other location */
    const char *prefix = getenv("PREFIX");
    if (!prefix) prefix = "/data/data/com.termux/files/usr";
    
    char sys_box64[PATH_MAX];
    snprintf(sys_box64, sizeof(sys_box64), "%s/bin/box64", prefix);
    
    if (access(sys_box64, X_OK) == 0) {
        char *rp = realpath(sys_box64, resolved);
        if (rp) return rp;
    }
    
    /* Fallback to PATH search */
    return find_in_path("box64", resolved, size);
}

/* ============== Box64 Wrapper for Child Process Support ============== */

/* When a x86_64 binary forks and execs another x86_64 binary, box64 looks 
 * for itself in BOX64_PATH. Since box64 is a glibc binary that needs to run 
 * through the glibc loader, we create a wrapper script at $PREFIX/glibc/bin/box64
 * that invokes box64 correctly. */
static void ensure_box64_wrapper(const char *real_box64_path) {
    char wrapper_path[PATH_MAX];
    snprintf(wrapper_path, sizeof(wrapper_path), "%s/bin/box64", GLIBC_PREFIX);
    
    /* Check if wrapper already exists and is correct */
    struct stat st;
    if (stat(wrapper_path, &st) == 0) {
        /* Check if it's a regular file (script) vs symlink */
        if (S_ISREG(st.st_mode) && (st.st_mode & S_IXUSR)) {
            /* Already exists as executable script - check if it has correct content */
            FILE *f = fopen(wrapper_path, "r");
            if (f) {
                char buf[512];
                if (fgets(buf, sizeof(buf), f) && strstr(buf, "#!/")) {
                    /* Has shebang, check for box64 path */
                    if (fgets(buf, sizeof(buf), f)) {
                        if (strstr(buf, real_box64_path)) {
                            fclose(f);
                            return; /* Already correct */
                        }
                    }
                }
                fclose(f);
            }
        }
        /* Remove old file/symlink */
        unlink(wrapper_path);
    }
    
    /* Create wrapper script that invokes box64 through glibc loader */
    FILE *f = fopen(wrapper_path, "w");
    if (!f) return;
    
    fprintf(f, "#!/data/data/com.termux/files/usr/bin/sh\n");
    fprintf(f, "exec %s --library-path %s %s \"$@\"\n", 
            GLIBC_LOADER, GLIBC_LIB, real_box64_path);
    fclose(f);
    
    /* Make executable */
    chmod(wrapper_path, 0755);
}

/* ============== Preload Library Extraction ============== */

static char *extract_preload_library(char *path_buf, size_t path_size) {
#ifdef EMBED_PRELOAD
    if (preload_so_size == 0) {
        return NULL;
    }
    
    /* Write to $PREFIX/glibc/lib/ */
    snprintf(path_buf, path_size, "%s/libovgl_preload.so", GLIBC_LIB);
    
    /* Check if file already exists with correct size */
    struct stat st;
    if (stat(path_buf, &st) == 0 && (size_t)st.st_size == preload_so_size) {
        return path_buf;
    }
    
    /* Write the file */
    int fd = open(path_buf, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd >= 0) {
        ssize_t written = write(fd, preload_so_data, preload_so_size);
        close(fd);
        if (written == (ssize_t)preload_so_size) {
            return path_buf;
        }
    }
    
    return NULL;
#else
    /* No embedded preload - look for external file in glibc lib */
    snprintf(path_buf, path_size, "%s/libovgl_preload.so", GLIBC_LIB);
    if (access(path_buf, R_OK) == 0) return path_buf;
    
    return NULL;
#endif
}

/* ============== Environment Setup ============== */

static char **build_environment(const char *preload_path, int for_box64, int use_preload, const char *orig_binary, int debug) {
    extern char **environ;
    
    /* Count existing env vars */
    int envc = 0;
    while (environ[envc]) envc++;
    
    /* Allocate space for existing + new vars + NULL */
    char **new_env = malloc((envc + 15) * sizeof(char *));
    if (!new_env) return NULL;
    
    int j = 0;
    const char *home = getenv("HOME");
    (void)home; /* May be unused now */
    
    /* Copy existing env, filtering some vars */
    for (int i = 0; i < envc; i++) {
        /* Skip vars we'll override */
        if (strncmp(environ[i], "LD_PRELOAD=", 11) == 0) continue;
        if (strncmp(environ[i], "OVGL_GLIBC_LIB=", 15) == 0) continue;
        if (strncmp(environ[i], "OVGL_GLIBC_LOADER=", 18) == 0) continue;
        if (strncmp(environ[i], "OVGL_ORIG_EXE=", 14) == 0) continue;
        if (strncmp(environ[i], "BOX64_LD_PRELOAD=", 17) == 0) continue;
        if (strncmp(environ[i], "BOX64_PATH=", 11) == 0) continue;
        /* Keep user's BOX64_LD_LIBRARY_PATH if set, otherwise we'll add default */
        if (!for_box64 && strncmp(environ[i], "BOX64_LD_LIBRARY_PATH=", 22) == 0) continue;
        
        new_env[j++] = strdup(environ[i]);
    }
    
    /* Add OVGL environment variables */
    char buf[PATH_MAX + 64];
    
    snprintf(buf, sizeof(buf), "OVGL_GLIBC_LIB=%s", GLIBC_LIB);
    new_env[j++] = strdup(buf);
    
    snprintf(buf, sizeof(buf), "OVGL_GLIBC_LOADER=%s", GLIBC_LOADER);
    new_env[j++] = strdup(buf);
    
    /* Set original binary path for /proc/self/exe fix */
    if (orig_binary) {
        snprintf(buf, sizeof(buf), "OVGL_ORIG_EXE=%s", orig_binary);
        new_env[j++] = strdup(buf);
    }
    
    if (for_box64) {
        /* For box64: set BOX64_LD_LIBRARY_PATH if not already set by user */
        if (!getenv("BOX64_LD_LIBRARY_PATH")) {
            snprintf(buf, sizeof(buf), "BOX64_LD_LIBRARY_PATH=%s", GLIBC_LIB_X86_64);
            new_env[j++] = strdup(buf);
        }
        
        /* Set BOX64_PATH so box64 can find the wrapper for fork/exec */
        const char *prefix = getenv("PREFIX");
        if (!prefix) prefix = "/data/data/com.termux/files/usr";
        snprintf(buf, sizeof(buf), "BOX64_PATH=%s/glibc/bin/:%s/bin/", prefix, prefix);
        new_env[j++] = strdup(buf);
        
        if (preload_path && use_preload) {
            snprintf(buf, sizeof(buf), "BOX64_LD_PRELOAD=%s", preload_path);
            new_env[j++] = strdup(buf);
        }
        
        /* Fake uname to report x86_64 so launchers don't bail out */
        new_env[j++] = strdup("BOX64_UNAME=x86_64");
        
        /* Clear LD_PRELOAD for the native box64 binary itself */
        new_env[j++] = strdup("LD_PRELOAD=");
    } else {
        /* For native arm64 glibc: set LD_PRELOAD only if requested */
        if (preload_path && use_preload) {
            snprintf(buf, sizeof(buf), "LD_PRELOAD=%s", preload_path);
            new_env[j++] = strdup(buf);
        } else {
            new_env[j++] = strdup("LD_PRELOAD=");
        }
    }
    
    /* Enable debug output for preload if debug mode */
    if (debug) {
        new_env[j++] = strdup("OVGL_DEBUG=1");
    }
    
    new_env[j] = NULL;
    return new_env;
}

/* Change to binary's directory */
static void change_to_binary_dir(const char *binary_path) {
    char *path_copy = strdup(binary_path);
    if (!path_copy) return;
    
    char *dir = dirname(path_copy);
    if (dir && strlen(dir) > 0 && strcmp(dir, ".") != 0) {
        chdir(dir);
    }
    free(path_copy);
}

/* ============== Usage and Error Messages ============== */

static void print_usage(const char *prog) {
    fprintf(stderr, 
        COLOR_BLUE "ovgl" COLOR_RESET " - Run glibc/x86_64 binaries on Termux\n\n"
        COLOR_YELLOW "Usage:" COLOR_RESET " %s [options] <binary> [args...]\n\n"
        COLOR_YELLOW "Options:" COLOR_RESET "\n"
        "  -h, --help        Show this help message\n"
        "  -d, --debug       Enable debug output\n"
        "  -n, --no-preload  Don't use preload library (for simple binaries)\n"
        "  -v, --version     Show version\n\n"
        COLOR_YELLOW "Examples:" COLOR_RESET "\n"
        "  %s ./my_glibc_app           # Run arm64 glibc binary\n"
        "  %s ./x86_64_binary          # Run x86_64 binary via box64\n"
        "  %s geekbench6               # Run from PATH\n"
        "  %s -n geekbench6            # Run without preload\n\n"
        COLOR_YELLOW "Supported architectures:" COLOR_RESET "\n"
        "  - arm64 (aarch64) - uses glibc loader directly\n"
        "  - x86_64          - uses box64 emulator\n\n"
        COLOR_YELLOW "Environment:" COLOR_RESET "\n"
        "  OVGL_GLIBC_LIB     Glibc library path (default: %s)\n"
        "  OVGL_GLIBC_LOADER  Glibc loader path (default: %s)\n"
        "  BOX64_LD_LIBRARY_PATH  x86_64 library path (default: $HOME/%s)\n\n",
        prog, prog, prog, prog, prog, GLIBC_LIB, GLIBC_LOADER, BOX64_X86_LIBS_DEFAULT);
}

static void print_version(void) {
    printf("ovgl version 1.0.0\n");
    printf("Glibc path: %s\n", GLIBC_LIB);
    printf("Loader: %s\n", GLIBC_LOADER);
#ifdef EMBED_PRELOAD
    printf("Preload library: embedded (%u bytes)\n", preload_so_size);
#else
    printf("Preload library: external\n");
#endif
}

/* ============== Main ============== */

int main(int argc, char *argv[]) {
    int debug = 0;
    int use_preload = 1;
    int arg_start = 1;
    
    /* Parse options */
    while (arg_start < argc && argv[arg_start][0] == '-') {
        if (strcmp(argv[arg_start], "-h") == 0 || strcmp(argv[arg_start], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[arg_start], "-v") == 0 || strcmp(argv[arg_start], "--version") == 0) {
            print_version();
            return 0;
        }
        if (strcmp(argv[arg_start], "-d") == 0 || strcmp(argv[arg_start], "--debug") == 0) {
            debug = 1;
            arg_start++;
            continue;
        }
        if (strcmp(argv[arg_start], "-n") == 0 || strcmp(argv[arg_start], "--no-preload") == 0) {
            use_preload = 0;
            arg_start++;
            continue;
        }
        if (strcmp(argv[arg_start], "--") == 0) {
            arg_start++;
            break;
        }
        fprintf(stderr, COLOR_RED "ovgl:" COLOR_RESET " Unknown option: %s\n", argv[arg_start]);
        return 1;
    }
    
    if (arg_start >= argc) {
        print_usage(argv[0]);
        return 1;
    }
    
    const char *binary_name = argv[arg_start];
    
    /* Resolve binary path */
    char binary_path[PATH_MAX];
    if (!find_in_path(binary_name, binary_path, sizeof(binary_path))) {
        fprintf(stderr, COLOR_RED "ovgl:" COLOR_RESET " Binary not found: %s\n", binary_name);
        return 127;
    }
    
    if (debug) {
        fprintf(stderr, COLOR_BLUE "ovgl:" COLOR_RESET " Resolved binary: %s\n", binary_path);
    }
    
    /* Analyze the binary */
    binary_info_t info = analyze_binary(binary_path);
    
    if (info.arch == ARCH_ERROR) {
        fprintf(stderr, COLOR_RED "ovgl:" COLOR_RESET " Cannot read binary: %s\n", binary_path);
        return 1;
    }
    
    if (info.arch == ARCH_NOT_ELF) {
        fprintf(stderr, COLOR_RED "ovgl:" COLOR_RESET " Not an ELF binary: %s\n", binary_path);
        return 1;
    }
    
    if (info.arch == ARCH_UNKNOWN) {
        fprintf(stderr, COLOR_RED "ovgl:" COLOR_RESET " Unsupported architecture in: %s\n", binary_path);
        return 1;
    }
    
    if (debug) {
        const char *arch_str = (info.arch == ARCH_AARCH64) ? "arm64" : "x86_64";
        const char *interp_str = "unknown";
        switch (info.interp) {
            case INTERP_GLIBC: interp_str = "glibc"; break;
            case INTERP_BIONIC: interp_str = "bionic"; break;
            case INTERP_MUSL: interp_str = "musl"; break;
            case INTERP_NONE: interp_str = "none"; break;
            case INTERP_OTHER: interp_str = "other"; break;
        }
        fprintf(stderr, COLOR_BLUE "ovgl:" COLOR_RESET " Architecture: %s, Interpreter: %s\n", arch_str, interp_str);
        if (info.interp_path[0]) {
            fprintf(stderr, COLOR_BLUE "ovgl:" COLOR_RESET " Interpreter path: %s\n", info.interp_path);
        }
    }
    
    /* Extract preload library */
    char preload_path[PATH_MAX];
    char *preload = extract_preload_library(preload_path, sizeof(preload_path));
    
    if (debug) {
        if (preload) {
            fprintf(stderr, COLOR_BLUE "ovgl:" COLOR_RESET " Preload library: %s\n", preload);
        } else {
            fprintf(stderr, COLOR_YELLOW "ovgl:" COLOR_RESET " No preload library available\n");
        }
    }
    
    /* Handle based on architecture */
    if (info.arch == ARCH_X86_64) {
        /* x86_64 binary - need box64 */
        char box64_path[PATH_MAX];
        if (!find_box64(box64_path, sizeof(box64_path))) {
            fprintf(stderr, COLOR_RED "ovgl:" COLOR_RESET " box64 is required for x86_64 binaries but not found!\n");
            fprintf(stderr, COLOR_YELLOW "hint:" COLOR_RESET " Install box64 or add it to your PATH\n");
            return 127;
        }
        
        /* Create wrapper script so box64 can find itself for child x86_64 processes */
        ensure_box64_wrapper(box64_path);
        
        if (debug) {
            fprintf(stderr, COLOR_BLUE "ovgl:" COLOR_RESET " Using box64: %s\n", box64_path);
        }
        
        /* Check if box64 itself needs glibc loader */
        binary_info_t box64_info = analyze_binary(box64_path);
        int box64_needs_glibc = (box64_info.interp == INTERP_GLIBC);
        
        if (debug) {
            fprintf(stderr, COLOR_BLUE "ovgl:" COLOR_RESET " box64 needs glibc loader: %s\n", box64_needs_glibc ? "yes" : "no");
        }
        
        /* Build environment - for x86_64, original binary is what box64 runs */
        char **new_env = build_environment(preload, 1, use_preload, binary_path, debug);
        if (!new_env) {
            perror("build_environment");
            return 1;
        }
        
        /* Change to binary's directory */
        change_to_binary_dir(binary_path);
        
        if (box64_needs_glibc) {
            /* box64 is a glibc binary - run through loader */
            /* argv: loader --library-path lib --argv0 box64 box64 binary [args...] */
            int orig_argc = argc - arg_start;
            char **new_argv = malloc((orig_argc + 8) * sizeof(char *));
            if (!new_argv) {
                perror("malloc");
                return 1;
            }
            
            int i = 0;
            new_argv[i++] = (char *)GLIBC_LOADER;
            new_argv[i++] = "--library-path";
            new_argv[i++] = (char *)GLIBC_LIB;
            new_argv[i++] = "--argv0";
            new_argv[i++] = "box64";
            new_argv[i++] = box64_path;
            new_argv[i++] = binary_path;
            
            for (int j = 1; j < orig_argc; j++) {
                new_argv[i++] = argv[arg_start + j];
            }
            new_argv[i] = NULL;
            
            if (debug) {
                fprintf(stderr, COLOR_BLUE "ovgl:" COLOR_RESET " Executing: %s ... %s %s\n", GLIBC_LOADER, box64_path, binary_path);
            }
            
            execve(GLIBC_LOADER, new_argv, new_env);
            fprintf(stderr, COLOR_RED "ovgl:" COLOR_RESET " execve failed: %s\n", strerror(errno));
            return 1;
        } else {
            /* box64 is native bionic - run directly */
            int orig_argc = argc - arg_start;
            char **new_argv = malloc((orig_argc + 2) * sizeof(char *));
            if (!new_argv) {
                perror("malloc");
                return 1;
            }
            
            new_argv[0] = box64_path;
            new_argv[1] = binary_path;
            for (int i = 1; i < orig_argc; i++) {
                new_argv[i + 1] = argv[arg_start + i];
            }
            new_argv[orig_argc + 1] = NULL;
            
            if (debug) {
                fprintf(stderr, COLOR_BLUE "ovgl:" COLOR_RESET " Executing: %s %s\n", box64_path, binary_path);
            }
            
            execve(box64_path, new_argv, new_env);
            fprintf(stderr, COLOR_RED "ovgl:" COLOR_RESET " execve failed: %s\n", strerror(errno));
            return 1;
        }
        
    } else if (info.arch == ARCH_AARCH64) {
        /* arm64 binary */
        
        /* Check if it needs glibc loader */
        if (info.interp == INTERP_BIONIC) {
            /* Native bionic binary - just run it directly */
            if (debug) {
                fprintf(stderr, COLOR_BLUE "ovgl:" COLOR_RESET " Native bionic binary, running directly\n");
            }
            execv(binary_path, &argv[arg_start]);
            perror("execv");
            return 1;
        }
        
        /* Check if glibc loader exists */
        if (access(GLIBC_LOADER, X_OK) != 0) {
            fprintf(stderr, COLOR_RED "ovgl:" COLOR_RESET " Glibc loader not found: %s\n", GLIBC_LOADER);
            fprintf(stderr, COLOR_YELLOW "hint:" COLOR_RESET " Install glibc-runner package: pkg install glibc-runner\n");
            return 1;
        }
        
        /* Check if glibc lib directory exists */
        if (access(GLIBC_LIB, F_OK) != 0) {
            fprintf(stderr, COLOR_RED "ovgl:" COLOR_RESET " Glibc lib not found: %s\n", GLIBC_LIB);
            return 1;
        }
        
        /* Build argv for glibc loader */
        int orig_argc = argc - arg_start;
        char **new_argv = malloc((orig_argc + 6) * sizeof(char *));
        if (!new_argv) {
            perror("malloc");
            return 1;
        }
        
        int i = 0;
        new_argv[i++] = (char *)GLIBC_LOADER;
        new_argv[i++] = "--library-path";
        new_argv[i++] = (char *)GLIBC_LIB;
        new_argv[i++] = "--argv0";
        new_argv[i++] = argv[arg_start];
        new_argv[i++] = binary_path;
        
        for (int j = 1; j < orig_argc; j++) {
            new_argv[i++] = argv[arg_start + j];
        }
        new_argv[i] = NULL;
        
        /* Build environment */
        char **new_env = build_environment(preload, 0, use_preload, binary_path, debug);
        if (!new_env) {
            perror("build_environment");
            return 1;
        }
        
        /* Change to binary's directory so it can find its resources */
        change_to_binary_dir(binary_path);
        
        if (debug) {
            fprintf(stderr, COLOR_BLUE "ovgl:" COLOR_RESET " Executing: %s --library-path %s %s\n", GLIBC_LOADER, GLIBC_LIB, binary_path);
        }
        
        execve(GLIBC_LOADER, new_argv, new_env);
        fprintf(stderr, COLOR_RED "ovgl:" COLOR_RESET " execve failed: %s\n", strerror(errno));
        return 1;
    }
    
    fprintf(stderr, COLOR_RED "ovgl:" COLOR_RESET " Unexpected error\n");
    return 1;
}