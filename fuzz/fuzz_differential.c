// fuzz_differential.c — libFuzzer target: fakecc must not accept what gcc
// rejects.  gcc runs with -fsyntax-only, so this compares diagnostics, not
// generated code; comparing values is test/fuzz's job, which feeds both
// compilers complete programs instead of random bytes.
//
// The check is one-directional: fakecc implements a subset of gcc's C, so
// rejecting more than gcc does is expected and never reported.
//
// Build: see fuzz/README.md
// Run:   ./build_fuzz/bin/fuzz_differential -max_len=2048 -runs=100000

#include "fakecc/compiler.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

const char *__asan_default_options(void) {
    return "detect_leaks=0";
}

static void tmp_path(char *buf, size_t n, const char *suffix) {
    const char *tmp = getenv("TMPDIR");
    snprintf(buf, n, "%s/fakecc_fdiff_%ld%s", tmp && tmp[0] ? tmp : "/tmp",
             (long)getpid(), suffix);
}

/* gcc cannot parse fakecc's own dialect (package/import/runtime.*), so such a
 * program can never agree with gcc.  The package declaration need not be the
 * very first token — a leading comment is enough to hide it from a
 * prefix check — so look for the keywords anywhere. */
static int is_fakecc_dialect(const char *s) {
    return strstr(s, "package") || strstr(s, "import") || strstr(s, "runtime.");
}

/* gcc is only a meaningful oracle for text */
static int looks_like_text(const uint8_t *d, size_t n) {
    size_t check = n < 256 ? n : 256;
    for (size_t i = 0; i < check; i++) {
        unsigned char c = d[i];
        if (c == 0) return 0;
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') return 0;
    }
    return 1;
}

/* gcc 14 promoted several long-standing warnings to errors; keep them warnings
 * so that only real disagreements are reported.
 * Returns 1 = accepts, 0 = rejects, -1 = unknown (no gcc, or it timed out). */
static int gcc_verdict(const char *src) {
    char path[512], cmd[1200];
    tmp_path(path, sizeof(path), ".c");
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(src, 1, strlen(src), f);
    fclose(f);

    snprintf(cmd, sizeof(cmd),
             "timeout 10 gcc -fsyntax-only -w -std=gnu11"
             " -Wno-error=implicit-function-declaration -Wno-error=implicit-int"
             " -Wno-error=int-conversion -Wno-error=incompatible-pointer-types"
             " -Wno-error=return-mismatch %s >/dev/null 2>&1", path);
    int status = system(cmd);
    if (status < 0 || !WIFEXITED(status)) return -1;
    if (WEXITSTATUS(status) == 124) return -1;   /* timeout(1) killed gcc */
    return WEXITSTATUS(status) == 0 ? 1 : 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0 || size > (1u << 16)) return 0;
    if (!looks_like_text(data, size)) return 0;

    char *src = (char *)malloc(size + 1);
    if (!src) return 0;
    memcpy(src, data, size);
    src[size] = '\0';

    FakeccOptions opts;
    fakecc_options_init(&opts);
    opts.opt_level = data[0] & 1;
    char obj[512];
    tmp_path(obj, sizeof(obj), ".o");
    int fcc_accepts = fakecc_compile_string_to_obj(src, obj, &opts) == 0;

    if (fcc_accepts && !is_fakecc_dialect(src) && gcc_verdict(src) == 0) {
        fprintf(stderr, "fakecc accepted a program that gcc rejects:\n%s\n", src);
        abort();
    }

    free(src);
    return 0;
}
