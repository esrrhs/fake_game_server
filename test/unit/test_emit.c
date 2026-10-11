#include "fakecc/ast.h"
#include "fakecc/codegen.h"
#include "fakecc/common.h"
#include "fakecc/emit.h"
#include "fakecc/ir.h"
#include "fakecc/lexer.h"
#include "fakecc/parser.h"
#include "fakecc/sema.h"
#include "fakecc/target.h"
#include "fakecc/token.h"
#include "test_framework.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- helper: compile source to temp executable ---- */
static void compile_to_file(const char *src, const char *out_path) {
    TokenArray arr;
    token_array_init(&arr);
    lex(src, "test.c", &arr);

    TranslationUnit tu;
    tu_init(&tu);
    parse(&arr, &tu);
    sema_check(&tu, 1);

    IRModule ir;
    ir_module_init(&ir);
    ir_generate(&tu, &ir, 0);

    EmitModule em;
    emit_module_init(&em);
    codegen(&ir, &em, 0);
    emit_elf(&em, out_path);

    emit_module_free(&em);
    ir_module_free(&ir);
    token_array_free(&arr);
    tu_free(&tu);
}

/* ---- tests ---- */

static void test_elf_magic(void) {
    const char *out = "/tmp/fakecc_test_elf_magic";
    compile_to_file("package main; int main() { return 0; }", out);

    FILE *f = fopen(out, "rb");
    T_ASSERT(f != NULL);
    unsigned char magic[4];
    T_ASSERT_EQ_INT((int)fread(magic, 1, 4, f), 4);
    fclose(f);
    T_ASSERT(magic[0] == 0x7f);
    T_ASSERT(magic[1] == 'E');
    T_ASSERT(magic[2] == 'L');
    T_ASSERT(magic[3] == 'F');
    unlink(out);
}

static void test_elf_machine(void) {
    const char *out = "/tmp/fakecc_test_elf_machine";
    compile_to_file("package main; int main() { return 0; }", out);

    FILE *f = fopen(out, "rb");
    T_ASSERT(f != NULL);
    fseek(f, 18, SEEK_SET); /* e_machine at offset 18 */
    uint16_t machine;
    T_ASSERT_EQ_INT((int)fread(&machine, 2, 1, f), 1);
    fclose(f);
    T_ASSERT_EQ_INT((int)machine, 62); /* EM_X86_64 */
    unlink(out);
}

/* The run-execution cases fork/exec a real x86-64 Linux process, so they
 * can only be exercised natively on an x86-64 Linux host.  Everywhere else
 * (Darwin arm64, Linux aarch64, ...) the artifacts are still produced --
 * the magic/machine checks cover codegen+emission -- but executing them
 * would just fail with ENOEXEC; those cases report SKIP instead. */
#if defined(__linux__) && (defined(__x86_64__) || defined(__amd64__))
#define HOST_RUNS_X86_64_LINUX_ELF 1
#else
#define HOST_RUNS_X86_64_LINUX_ELF 0
#endif

/* Run an emitted executable and return its exit status (-1 if it did not
 * exit normally).  Assumes the caller already verified host support. */
#if HOST_RUNS_X86_64_LINUX_ELF
static int run_elf_exit_code(const char *out) {
    int pid = fork();
    if (pid == 0) {
        execl(out, out, (char *)NULL);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif

/* Compile on every host (that exercises the whole emit pipeline); only
 * execute where the kernel can load the artifact. */
#if HOST_RUNS_X86_64_LINUX_ELF
#define RUN_ELF_CASE(out, expect) \
    T_ASSERT_EQ_INT(run_elf_exit_code(out), (expect))
#else
#define RUN_ELF_CASE(out, expect) do {                                       \
    (void)(expect);                                                          \
    printf("  SKIP run-exec %s (x86-64 Linux ELF needs a Linux x86-64 "      \
           "host)\n", (out));                                                \
} while (0)
#endif

static void test_run_return_0(void) {
    const char *out = "/tmp/fakecc_test_run_0";
    compile_to_file("package main; int main() { return 0; }", out);

    RUN_ELF_CASE(out, 0);
    unlink(out);
}

static void test_run_return_42(void) {
    const char *out = "/tmp/fakecc_test_run_42";
    compile_to_file("package main; int main() { return 42; }", out);

    RUN_ELF_CASE(out, 42);
    unlink(out);
}

static void test_run_return_255(void) {
    const char *out = "/tmp/fakecc_test_run_255";
    compile_to_file("package main; int main() { return 255; }", out);

    RUN_ELF_CASE(out, 255);
    unlink(out);
}

/* ---- main ---- */

int main(void) {
    /* Pins the x86-64 ELF backend; the run-execution cases execute only on
     * a Linux x86-64 host and report SKIP elsewhere. */
    target_set_current(target_x86_64_linux());
    test_elf_magic();
    test_elf_machine();
    test_run_return_0();
    test_run_return_42();
    test_run_return_255();
    return t_finalize();
}
