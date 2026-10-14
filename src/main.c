#include "fakecc/codegen.h"
#include "fakecc/common.h"
#include "fakecc/emit.h"
#include "fakecc/ir.h"
#include "fakecc/lexer.h"
#include "fakecc/opt.h"
#include "fakecc/parser.h"
#include "fakecc/pkg.h"
#include "fakecc/sema.h"
#include "fakecc/target.h"
#include "fakecc/token.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__SANITIZE_ADDRESS__) || defined(__has_feature)
const char *__asan_default_options(void) {
    return "detect_leaks=0";
}
#endif

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "fakecc: cannot open '%s'\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        fprintf(stderr, "fakecc: out of memory\n");
        exit(1);
    }
    size_t nread = fread(buf, 1, (size_t)size, f);
    buf[nread] = '\0';
    fclose(f);
    return buf;
}

/* Lex + parse one source file into `tu` (sema/codegen happen later).
 * Returns FAKECC_OK / FAKECC_ERR; on error the CLI should exit. */
static int parse_source(const char *source, const char *filename,
                        TranslationUnit *tu, PkgContext *pkg) {
    TokenArray tokens;
    token_array_init(&tokens);
    if (lex(source, filename, &tokens) != FAKECC_OK) {
        token_array_free(&tokens);
        return FAKECC_ERR;
    }
    tu_init(tu);
    int rc = parse_in_pkg(&tokens, tu, pkg);
    token_array_free(&tokens);
    return rc;
}

/* Sema → IR → opt → codegen for an already-parsed TU. */
static int lower_tu(TranslationUnit *tu, const char *filename,
                    EmitModule *out, int opt_level, int want_debug,
                    PkgContext *pkg) {
    if (sema_check_in_pkg(tu, 0, pkg) != FAKECC_OK)
        return FAKECC_ERR;

    if (sema_has_errors())
        return FAKECC_ERR;

    IRModule ir;
    ir_module_init(&ir);
    if (ir_generate(tu, &ir, opt_level == 0) != FAKECC_OK) {
        ir_module_free(&ir);
        return FAKECC_ERR;
    }

    opt(&ir, opt_level, want_debug);

    emit_module_init(out);
    if (want_debug)
        out->dbg_tu_name = xstrdup(filename);
    codegen(&ir, out, want_debug);

    ir_module_free(&ir);
    if (fakecc_had_error())
        return FAKECC_ERR;
    return FAKECC_OK;
}

static void module_free(EmitModule *m) {
    emit_module_free(m);
}

static void usage(void) {
    fprintf(stderr,
            "usage: fakecc [-c] [-shared] [-g] [-O0|-O1] [-mavx|-mno-avx] [-mavx512f|-mno-avx512f]\n"
            "              [-nostdlib] [-nodefaultlibs]\n"
            "              [-LDIR]... [-lLIB]... <input...> -o <output>\n"
            "  (default)       link builtin runtime/ (freestanding; no DT_NEEDED)\n"
            "  -shared         produce a shared object (.so) library\n"
            "  -g              emit DWARF debug info (line numbers, variables);\n"
            "                  independent of -O, never changes generated code\n"
            "  -O0             keep locals in memory (skip SSA promotion)\n"
            "  -O1             default: SSA promotion + folding + DCE\n"
            "  -mno-avx        pass 32-byte vectors in memory (gcc -mno-avx)\n"
            "  -mavx           default: 32-byte vectors in one YMM\n"
            "  -mavx512f       pass 64-byte vectors in one ZMM (gcc -mavx512f)\n"
            "  -mno-avx512f    default: 64-byte vectors in memory\n"
            "  -nostdlib       do not link builtin runtime/; use -l for system libs\n"
            "  -lLIB           link against libLIB.so (DT_NEEDED; optional interop)\n"
            "  -l:SONAME       link against exact soname SONAME\n"
            "  -LDIR           add DIR to the shared-library search path\n"
            "                  (link-time check for -l; also DT_RUNPATH)\n"
            "  -nodefaultlibs  accepted for compatibility (default already skips libc)\n"
            "  --target=T      code generation target: x86_64-linux (default on\n"
            "                  Linux hosts) or arm64-macos (default on Apple Silicon;\n"
            "                  accepted spellings: x86_64-linux, arm64-apple-darwin, ...)\n"
            "  FAKECC_RT       override path to the runtime/ directory\n"
            "  FAKECC_PKG      colon-separated package search path\n");
    exit(1);
}

static int ends_with(const char *s, const char *suf) {
    size_t n = strlen(s), m = strlen(suf);
    return n >= m && strcmp(s + n - m, suf) == 0;
}

static int is_system_soname(const char *soname) {
    return strcmp(soname, "libc.so.6") == 0
        || strcmp(soname, "libm.so.6") == 0
        || strcmp(soname, "libdl.so.2") == 0
        || strcmp(soname, "libpthread.so.0") == 0
        || strcmp(soname, "libSystem.B.dylib") == 0;
}

/* Map a -l argument to a DT_NEEDED/LC_LOAD_DYLIB soname string (caller frees).
 * On Mach-O targets, use .dylib extension; on ELF, use .so. */
static char *soname_from_l(const char *arg, int is_macho) {
    if (!arg || !*arg) {
        fprintf(stderr, "fakecc: -l requires a library name\n");
        exit(1);
    }
    if (arg[0] == ':')
        return xstrdup(arg + 1);
    /* Common system library sonames — platform-specific. */
    if (is_macho) {
        /* macOS: libSystem.B.dylib covers libc, libm, libpthread, libdl. */
        if (strcmp(arg, "c") == 0) return xstrdup("libSystem.B.dylib");
        if (strcmp(arg, "m") == 0) return xstrdup("libSystem.B.dylib");
        if (strcmp(arg, "dl") == 0) return xstrdup("libSystem.B.dylib");
        if (strcmp(arg, "pthread") == 0) return xstrdup("libSystem.B.dylib");
    } else {
        /* Linux: glibc ABI sonames. */
        if (strcmp(arg, "c") == 0) return xstrdup("libc.so.6");
        if (strcmp(arg, "m") == 0) return xstrdup("libm.so.6");
        if (strcmp(arg, "dl") == 0) return xstrdup("libdl.so.2");
        if (strcmp(arg, "pthread") == 0) return xstrdup("libpthread.so.0");
    }
    {
        size_t n = strlen(arg);
        const char *ext = is_macho ? ".dylib" : ".so";
        size_t ext_len = strlen(ext);
        char *s = malloc(3 + n + ext_len + 1);
        if (!s) { fprintf(stderr, "fakecc: out of memory\n"); exit(1); }
        memcpy(s, "lib", 3);
        memcpy(s + 3, arg, n);
        memcpy(s + 3 + n, ext, ext_len + 1);
        return s;
    }
}

static char *soname_from_so_path(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') base = p + 1;
    return xstrdup(base);
}

/* Directory containing path ("" for basename-only). Caller frees. */
static char *dir_of(const char *path) {
    const char *slash = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '/') slash = p;
    if (!slash) return xstrdup(".");
    if (slash == path) return xstrdup("/");
    {
        size_t n = (size_t)(slash - path);
        char *d = malloc(n + 1);
        if (!d) { fprintf(stderr, "fakecc: out of memory\n"); exit(1); }
        memcpy(d, path, n);
        d[n] = '\0';
        return d;
    }
}

static void paths_add(char ***paths, int *n, const char *dir) {
    for (int i = 0; i < *n; i++)
        if (strcmp((*paths)[i], dir) == 0) return;
    *paths = realloc(*paths, ((size_t)*n + 1) * sizeof(char *));
    if (!*paths) { fprintf(stderr, "fakecc: out of memory\n"); exit(1); }
    (*paths)[(*n)++] = xstrdup(dir);
}

/* True if DIR/SONAME exists as a regular readable file. */
static int lib_in_dir(const char *dir, const char *soname) {
    size_t nd = strlen(dir), ns = strlen(soname);
    char *path = malloc(nd + 1 + ns + 1);
    if (!path) { fprintf(stderr, "fakecc: out of memory\n"); exit(1); }
    memcpy(path, dir, nd);
    path[nd] = '/';
    memcpy(path + nd + 1, soname, ns + 1);
    FILE *f = fopen(path, "rb");
    int ok = f != NULL;
    if (f) fclose(f);
    free(path);
    return ok;
}

/* For non-system -l libs, require a hit in one of the -L dirs when any
 * -L was given (gcc-like "cannot find -lfoo"). */
static void require_libs_found(char **needed, int num_needed,
                               char **lib_paths, int num_lib_paths) {
    if (num_lib_paths == 0) return;
    for (int i = 0; i < num_needed; i++) {
        if (is_system_soname(needed[i])) continue;
        int found = 0;
        for (int d = 0; d < num_lib_paths; d++) {
            if (lib_in_dir(lib_paths[d], needed[i])) { found = 1; break; }
        }
        if (!found) {
            fprintf(stderr, "fakecc: cannot find -l library '%s'\n", needed[i]);
            exit(1);
        }
    }
}

static int file_readable(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static char *path_join(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    int slash = (na > 0 && a[na - 1] != '/');
    char *p = malloc(na + (size_t)slash + nb + 1);
    if (!p) { fprintf(stderr, "fakecc: out of memory\n"); exit(1); }
    memcpy(p, a, na);
    if (slash) p[na++] = '/';
    memcpy(p + na, b, nb + 1);
    return p;
}

/* Builtin freestanding runtime: one package, directory runtime/. */
static const char *rt_pkg_name(void) { return "runtime"; }

/* Locate runtime/: FAKECC_RT, ./runtime, <argv0-dir>/runtime, <argv0-dir>/../runtime. */
static char *find_rt_dir(const char *argv0) {
    const char *env = getenv("FAKECC_RT");
    if (env && env[0]) {
        char *probe = path_join(env, "string.c");
        int ok = file_readable(probe);
        free(probe);
        if (ok) return xstrdup(env);
    }
    if (file_readable("runtime/string.c")) return xstrdup("runtime");

    char *basedir = dir_of(argv0);
    {
        char *cand = path_join(basedir, "runtime");
        char *probe = path_join(cand, "string.c");
        int ok = file_readable(probe);
        free(probe);
        if (ok) {
            free(basedir);
            return cand;
        }
        free(cand);
    }
    {
        char *cand = path_join(basedir, "../runtime");
        char *probe = path_join(cand, "string.c");
        int ok = file_readable(probe);
        free(probe);
        if (ok) {
            free(basedir);
            return cand;
        }
        free(cand);
    }
    {
        char *cand = path_join(basedir, "../share/fakecc/runtime");
        char *probe = path_join(cand, "string.c");
        int ok = file_readable(probe);
        free(probe);
        if (ok) {
            free(basedir);
            return cand;
        }
        free(cand);
    }
    free(basedir);
    return NULL;
}

/* FAKECC_PKG is a colon-separated list of package-root directories. */
static void add_pkg_env_paths(PkgContext *ctx) {
    const char *env = getenv("FAKECC_PKG");
    if (!env || !env[0]) return;
    char *copy = xstrdup(env);
    char *p = copy;
    while (*p) {
        char *colon = strchr(p, ':');
        if (colon) *colon = '\0';
        if (*p) pkg_ctx_add_path(ctx, p);
        if (!colon) break;
        p = colon + 1;
    }
    free(copy);
}

int main(int argc, char **argv) {
    int compile_only = 0;
    int is_shared = 0;
    int nodefaultlibs = 0;
    int nostdlib = 0;
    int want_debug = 0;
    int opt_level = 1;
    const char *output_path = NULL;
    const char **inputs = NULL;
    int ninputs = 0;
    char **needed = NULL;
    int num_needed = 0;
    char **lib_paths = NULL;
    int num_lib_paths = 0;
    const char *target_arg = NULL;   /* explicit --target value, or host default */
    int x86_vec_enable = 0;          /* -mavx / -mavx512f explicitly requested */

    /* Pre-scan for --target so library name resolution (-l, .so/.dylib) can be
     * target-aware before the main argument parsing loop. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--target") == 0 ||
            strcmp(argv[i], "-target") == 0) {
            if (i + 1 < argc) target_arg = argv[i + 1];
        } else if (strncmp(argv[i], "--target=", 9) == 0) {
            target_arg = argv[i] + 9;
        } else if (strncmp(argv[i], "-target=", 8) == 0) {
            target_arg = argv[i] + 8;
        }
    }
    /* Resolve target early for library extension selection. */
    const TargetDesc *target;
    if (target_arg) {
        target = target_parse(target_arg);
        if (!target) {
            fprintf(stderr,
                    "fakecc: unrecognized target triple '%s' "
                    "(supported: x86_64-linux, arm64-macos)\n",
                    target_arg);
            exit(1);
        }
    } else {
        target = target_default();
    }
    target_set_current(target);
    int is_macho = (target->objfmt == TARGET_OBJFMT_MACHO);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            compile_only = 1;
        } else if (strcmp(argv[i], "-shared") == 0) {
            is_shared = 1;
        } else if (strcmp(argv[i], "-nodefaultlibs") == 0) {
            nodefaultlibs = 1;
        } else if (strcmp(argv[i], "-finstrument-functions") == 0) {
            g_instrument_functions = 1;
        } else if (strcmp(argv[i], "-fno-builtin") == 0) {
            g_no_builtin = 1;
        } else if (strncmp(argv[i], "-fno-builtin-", 13) == 0) {
            ir_disable_builtin(argv[i] + 13);
        } else if (strcmp(argv[i], "-fsanitize=address") == 0) {
            g_sanitize_address = 1;
        } else if (strncmp(argv[i], "-fsanitize=", 11) == 0) {
            /* Accept standard sanitize flags */
            if (strstr(argv[i], "address")) g_sanitize_address = 1;
        } else if (strcmp(argv[i], "-g") == 0) {
            want_debug = 1;
        } else if (strcmp(argv[i], "-pthread") == 0 ||
                   strcmp(argv[i], "-pthreads") == 0) {
            /* Accepted and ignored: nothing to do on a host where threads are
             * part of libc (Darwin keeps them in libSystem).  Taking it as an
             * error only pushed callers to spell the flag differently. */
        } else if (strcmp(argv[i], "-mno-avx") == 0) {
            g_no_avx = 1;
            g_avx512f = 0;
        } else if (strcmp(argv[i], "-mavx") == 0) {
            g_no_avx = 0;
            x86_vec_enable = 1;
        } else if (strcmp(argv[i], "-mno-avx512f") == 0) {
            g_avx512f = 0;
        } else if (strcmp(argv[i], "-mavx512f") == 0) {
            g_avx512f = 1;
            g_no_avx = 0;
            x86_vec_enable = 1;
        } else if (strcmp(argv[i], "--target") == 0 ||
                   strcmp(argv[i], "-target") == 0) {
            if (i + 1 >= argc) usage();
            target_arg = argv[++i];
        } else if (strncmp(argv[i], "--target=", 9) == 0) {
            target_arg = argv[i] + 9;
        } else if (strncmp(argv[i], "-target=", 8) == 0) {
            target_arg = argv[i] + 8;
        } else if (strcmp(argv[i], "-O0") == 0) {
            opt_level = 0;
        } else if (argv[i][0] == '-' && argv[i][1] == 'O' && argv[i][2] != '\0') {
            /* -O1/-O2/-O3/-Os/-Ofast all map to the single pipeline fakecc has. */
            opt_level = 1;
        } else if (strcmp(argv[i], "-nostdlib") == 0) {
            nostdlib = 1;
        } else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) usage();
            output_path = argv[++i];
        } else if (strcmp(argv[i], "-L") == 0) {
            if (i + 1 >= argc) usage();
            paths_add(&lib_paths, &num_lib_paths, argv[++i]);
        } else if (argv[i][0] == '-' && argv[i][1] == 'L') {
            paths_add(&lib_paths, &num_lib_paths, argv[i] + 2);
        } else if (strcmp(argv[i], "-l") == 0) {
            if (i + 1 >= argc) usage();
            needed = realloc(needed, ((size_t)num_needed + 1) * sizeof(char *));
            needed[num_needed++] = soname_from_l(argv[++i], is_macho);
        } else if (argv[i][0] == '-' && argv[i][1] == 'l') {
            needed = realloc(needed, ((size_t)num_needed + 1) * sizeof(char *));
            needed[num_needed++] = soname_from_l(argv[i] + 2, is_macho);
        } else if (ends_with(argv[i], ".so") || ends_with(argv[i], ".dylib")) {
            /* Pass a .so/.dylib path: DT_NEEDED/LC_LOAD_DYLIB = basename;
             * its dir joins -L/RUNPATH/LC_RPATH. */
            needed = realloc(needed, ((size_t)num_needed + 1) * sizeof(char *));
            needed[num_needed++] = soname_from_so_path(argv[i]);
            {
                char *d = dir_of(argv[i]);
                paths_add(&lib_paths, &num_lib_paths, d);
                free(d);
            }
        } else if (strncmp(argv[i], "-Wl,", 4) == 0) {
            /* Pass linker options through: -Wl,-rpath,/path etc. */
            const char *opt = argv[i] + 4;
            /* Parse comma-separated linker options. */
            while (*opt) {
                if (strncmp(opt, "-rpath,", 7) == 0) {
                    opt += 7;
                    /* rpath value runs until next comma or end. */
                    const char *end = strchr(opt, ',');
                    size_t len = end ? (size_t)(end - opt) : strlen(opt);
                    char *rpath = malloc(len + 1);
                    if (!rpath) { fprintf(stderr, "fakecc: out of memory\n"); exit(1); }
                    memcpy(rpath, opt, len);
                    rpath[len] = '\0';
                    /* Store rpath for later use (add to lib_paths for RUNPATH). */
                    paths_add(&lib_paths, &num_lib_paths, rpath);
                    free(rpath);
                    opt += len;
                    if (*opt == ',') opt++;
                } else {
                    /* Skip unknown linker options silently (gcc-like). */
                    const char *end = strchr(opt, ',');
                    if (end) opt = end + 1;
                    else break;
                }
            }
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "fakecc: unknown option '%s'\n", argv[i]);
            usage();
        } else {
            inputs = realloc(inputs, (ninputs + 1) * sizeof(char *));
            inputs[ninputs++] = argv[i];
        }
    }

    if (ninputs == 0 || output_path == NULL) usage();

    /* x86-only vector-width switches: arm64 always passes 16-byte NEON
     * vectors; -mavx/-mavx512f are meaningless there.  The -mno-* forms
     * are accepted as no-ops (gcc-like). */
    if (target->arch == TARGET_ARCH_ARM64 && x86_vec_enable) {
        fprintf(stderr,
                "fakecc: -mavx/-mavx512f apply only to the x86-64 target; "
                "arm64 always uses 16-byte NEON vectors\n");
        exit(1);
    }

    /* Fail early while the arm64/Mach-O backend is being built out, rather
     * than emitting an ELF file under the wrong assumption. */
    if (!target_backend_ready(target)) {
        fprintf(stderr,
                "fakecc: target '%s' is not supported by this build yet\n",
                target->triple);
        exit(1);
    }

    PkgContext pkg;
    pkg_ctx_init(&pkg);
    add_pkg_env_paths(&pkg);

    char *rt_dir = find_rt_dir(argv[0]);
    /* import runtime looks up a directory named runtime next to each search path. */
    if (rt_dir) {
        char *parent = dir_of(rt_dir);
        pkg_ctx_add_path(&pkg, parent);
        free(parent);
    }
    /* Also search next to each input so user packages resolve. */
    for (int i = 0; i < ninputs; i++) {
        if (ends_with(inputs[i], ".o")) continue;
        char *d = dir_of(inputs[i]);
        pkg_ctx_add_path(&pkg, d);
        free(d);
    }

    if (compile_only) {
        if (ninputs != 1) {
            fprintf(stderr, "fakecc: -c requires exactly one input\n");
            exit(1);
        }
        if (num_needed > 0 || num_lib_paths > 0 || nodefaultlibs || nostdlib) {
            fprintf(stderr,
                    "fakecc: -l / -L / -nostdlib / -nodefaultlibs are link-time options\n");
            exit(1);
        }
        TranslationUnit tu;
        char *src = read_file(inputs[0]);
        if (parse_source(src, inputs[0], &tu, &pkg) != FAKECC_OK) {
            free(src);
            exit(1);
        }
        free(src);
        EmitModule em;
        emit_set_object_mode(1);
        int low = lower_tu(&tu, inputs[0], &em, opt_level, want_debug, &pkg);
        emit_set_object_mode(0);
        if (low != FAKECC_OK) {
            tu_free(&tu);
            exit(1);
        }
        tu_free(&tu);
        emit_obj(&em, output_path);
        module_free(&em);
        free(inputs);
        free(rt_dir);
        pkg_ctx_free(&pkg);
        return 0;
    }

    if (is_shared && !nostdlib) {
        /* Unless the user explicitly requested standard library or packages,
         * a shared library is freestanding and does not auto-link runtime/ */
        nostdlib = 1;
    }

    require_libs_found(needed, num_needed, lib_paths, num_lib_paths);

    int nrt = 0;
    if (!nostdlib) {
        if (!rt_dir) {
            fprintf(stderr,
                    "fakecc: cannot find runtime/ (set FAKECC_RT or run from the source tree)\n");
            exit(1);
        }
        nrt = 1;
    }

    /* Preload builtin rt so its TUs are parsed once and cached. */
    SourceLoc zloc = {0};
    if (nrt)
        pkg_load(&pkg, rt_pkg_name(), zloc);

    TranslationUnit *user_tus = malloc((size_t)ninputs * sizeof(TranslationUnit));
    if (!user_tus) {
        fprintf(stderr, "fakecc: out of memory\n");
        exit(1);
    }

    /* Phase 1: parse user sources, then fold each TU into its package so the
     * next file's parse can see sibling typedefs/structs.  Same-package types
     * are visible via is_type_start -> pkg_find_struct/typedef regardless of
     * parse order. */
    TokenArray *user_tokens = malloc((size_t)ninputs * sizeof(TokenArray));
    if (!user_tokens) {
        fprintf(stderr, "fakecc: out of memory\n");
        exit(1);
    }
    for (int i = 0; i < ninputs; i++) {
        size_t len = strlen(inputs[i]);
        tu_init(&user_tus[i]);
        if (len >= 2 && inputs[i][len - 2] == '.' && inputs[i][len - 1] == 'o') {
            token_array_init(&user_tokens[i]);
        } else {
            char *src = read_file(inputs[i]);
            token_array_init(&user_tokens[i]);
            if (lex(src, inputs[i], &user_tokens[i]) != FAKECC_OK) {
                free(src);
                exit(1);
            }
            free(src);
        }
    }

    for (int i = 0; i < ninputs; i++) {
        size_t len = strlen(inputs[i]);
        if (len >= 2 && inputs[i][len - 2] == '.' && inputs[i][len - 1] == 'o') {
            /* .o inputs have no TU; mark package.name NULL as a sentinel. */
            memset(&user_tus[i], 0, sizeof(user_tus[i]));
        } else {
            if (parse_in_pkg(&user_tokens[i], &user_tus[i], &pkg) != FAKECC_OK)
                exit(1);
            if (user_tus[i].package.name) {
                TranslationUnit *one = &user_tus[i];
                pkg_register_tus(&pkg, user_tus[i].package.name, &one, 1);
            }
        }
        token_array_free(&user_tokens[i]);
    }
    free(user_tokens);

    /* Count package files to link: every loaded package except the user's own.
     * A package is "the user's own" when owns_files == 0 — its TUs were the CLI
     * inputs, already folded into the package as shallow copies that we dropped
     * after build_exports (pkg_register_tus), and codegen'd in Phase 2.  The
     * builtin rt plus any package pulled in by `import` (transitively, since
     * pkg_load resolves imports recursively) have owns_files == 1 and must be
     * codegen'd here. */
    int nlinked_files = 0;
    for (size_t p = 0; p < pkg.npkgs; p++) {
        Package *pp = pkg.pkgs[p];
        if (!pp->owns_files) continue;
        nlinked_files += (int)pp->nfiles;
    }
    /* The Darwin runtime is linked by default.  It has to be: without it a
     * program has no printf, no malloc, no string functions, and e2e -O1
     * scores 2474 against 2536 with it (see docs/t16-darwin-runtime-port.md
     * for how that gap closed).  FAKECC_ARM64_RUNTIME=0 still links
     * freestanding user code, which is the older behaviour and useful when
     * bisecting a runtime problem.  Both gates below read this one value --
     * when they were separate expressions they drifted, and the second one
     * filling modules the first had not reserved produced a segfault rather
     * than anything resembling the real fault. */
    const char *arm64_rt = getenv("FAKECC_ARM64_RUNTIME");
    int arm64_link_runtime = !arm64_rt || strcmp(arm64_rt, "0") != 0;
    if (target_current()->arch == TARGET_ARCH_ARM64
        && !arm64_link_runtime)
        nlinked_files = 0;

    /* -shared means something different on Mach-O, and this backend cannot
     * build it: a dylib is MH_DYLIB with an LC_ID_DYLIB and no entry stub,
     * its exports reach dyld through an LC_DYLD_INFO export trie, and --
     * the part that decides whether it is worth building at all -- nothing
     * could consume the result: the Mach-O link path has no -l, and the
     * runtime exposes no dlopen.  Refused here, before any compilation,
     * because otherwise the message the user sees is "no 'main' function
     * found", which sends them looking for a main function that was never
     * the problem. */
    if (is_shared && target_current()->objfmt == TARGET_OBJFMT_MACHO
        && !compile_only) {
        fprintf(stderr,
                "fakecc: -shared is not implemented for the arm64-macos "
                "target: it needs an MH_DYLIB with LC_ID_DYLIB and an export "
                "trie, and there is no -l or dlopen here to consume it\n");
        exit(1);
    }

    int nmods = ninputs + nlinked_files;
    /* A TU that defines no main is legal as soon as more than one module
     * contributes to the link -- the arm64 backend emits its entry stub in
     * whichever TU owns main, so it must not reject the others.  Count only
     * real source TUs: a prebuilt .o input never carries one. */
    int nsource_tus = 0;
    for (int i = 0; i < ninputs; i++) {
        size_t len = strlen(inputs[i]);
        if (len >= 2 && inputs[i][len - 2] == '.' && inputs[i][len - 1] == 'o')
            continue;
        nsource_tus++;
    }
    emit_set_multi_tu(nsource_tus + nlinked_files > 1);

    EmitModule *mods = malloc((size_t)nmods * sizeof(EmitModule));
    EmitModule **mod_ptrs = malloc((size_t)nmods * sizeof(EmitModule *));
    if (!mods || !mod_ptrs) {
        fprintf(stderr, "fakecc: out of memory\n");
        exit(1);
    }

    /* Phase 2: sema + codegen user modules. */
    for (int i = 0; i < ninputs; i++) {
        size_t len = strlen(inputs[i]);
        if (len >= 2 && inputs[i][len - 2] == '.' && inputs[i][len - 1] == 'o') {
            if (emit_obj_read(inputs[i], &mods[i]) != 0) exit(1);
        } else {
            if (lower_tu(&user_tus[i], inputs[i], &mods[i], opt_level, want_debug, &pkg) != FAKECC_OK)
                exit(1);
            tu_free(&user_tus[i]);
        }
        mod_ptrs[i] = &mods[i];
        /* A definition here outranks the same name from a package, so a
         * program can replace memcpy or fputs with its own. */
        mods[i].is_user_tu = 1;
    }
    free(user_tus);

    /* Must agree exactly with the nlinked_files reservation above.  The two
     * used to differ (that one was unconditional, this one carried
     * !nostdlib), so arm64 + -nostdlib let Phase 3 fill modules into an array
     * that was never sized for them and the tail reached emit_link
     * uninitialized. */
    int arm64_freestanding =
        target_current()->arch == TARGET_ARCH_ARM64
        && !nostdlib
        && !arm64_link_runtime;

    /* Phase 3: codegen already-parsed package files (builtin rt + any package
     * pulled in by `import`).  The user's own package is skipped: it has
     * owns_files == 0 (its TUs were the CLI inputs, codegen'd in Phase 2). */
    int mi = ninputs;
    for (size_t p = 0; p < pkg.npkgs; p++) {
        Package *pp = pkg.pkgs[p];
        if (!pp->owns_files) continue;
        for (size_t f = 0; f < pp->nfiles; f++) {
            if (arm64_freestanding) break;     /* runtime ported in T16+ */
            char *fake = path_join(pp->dir, "_.c");
            if (lower_tu(&pp->files[f], fake, &mods[mi], opt_level, want_debug, &pkg) != FAKECC_OK)
                exit(1);
            free(fake);
            mod_ptrs[mi] = &mods[mi];
            mi++;
        }
    }

    emit_link(mod_ptrs, (size_t)nmods, output_path,
              (const char **)needed, (size_t)num_needed,
              nodefaultlibs || nostdlib,
              (const char **)lib_paths, (size_t)num_lib_paths, want_debug,
              is_shared);

    for (int i = 0; i < nmods; i++) module_free(&mods[i]);
    free(mod_ptrs);
    free(mods);
    free(inputs);
    free(rt_dir);
    for (int i = 0; i < num_needed; i++) free(needed[i]);
    free(needed);
    for (int i = 0; i < num_lib_paths; i++) free(lib_paths[i]);
    free(lib_paths);
    pkg_ctx_free(&pkg);
    return 0;
}
