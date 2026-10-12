// fuzz_compile.c — libFuzzer target: arbitrary bytes through the whole fakecc
// pipeline (lexer -> parser -> sema -> IR -> opt -> codegen -> ELF emitter).
//
// Rejecting the input is fine, crashing on it is not: ASan/UBSan catch the
// memory errors and undefined behaviour that the fixed test suites never hit
// because they only ever feed fakecc valid programs.
//
// Build: see fuzz/README.md
// Run:   ./build_fuzz/bin/fuzz_compile -max_len=4096 -runs=1000000

#include "fakecc/compiler.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* fakecc leaks on most error paths and calls exit(1) when it cannot allocate;
 * neither is what this target looks for. */
const char *__asan_default_options(void) {
    return "detect_leaks=0";
}

/* one object file per process, overwritten on every iteration */
static const char *obj_path(void) {
    static char path[512];
    static int ready = 0;
    if (!ready) {
        const char *tmp = getenv("TMPDIR");
        snprintf(path, sizeof(path), "%s/fakecc_fuzz_%ld.o",
                 tmp && tmp[0] ? tmp : "/tmp", (long)getpid());
        ready = 1;
    }
    return path;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0 || size > (1u << 20)) return 0;

    char *src = (char *)malloc(size + 1);
    if (!src) return 0;
    memcpy(src, data, size);
    src[size] = '\0';

    FakeccOptions opts;
    fakecc_options_init(&opts);
    /* let the fuzzer steer the flags: both -O levels and -g (debug.c) */
    opts.opt_level = data[0] & 1;
    opts.want_debug = (data[0] >> 1) & 1;
    fakecc_compile_string_to_obj(src, obj_path(), &opts);

    free(src);
    return 0;
}
