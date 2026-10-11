#include "fakecc/opt.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers to construct IRModule with one function ---- */

static IRFunction *module_add_fn(IRModule *m, int next_value_id) {
    if (m->functions.len >= m->functions.cap) {
        m->functions.cap = m->functions.cap ? m->functions.cap * 2 : 4;
        m->functions.data = realloc(m->functions.data,
                                     m->functions.cap * sizeof(IRFunction));
        if (!m->functions.data) exit(1);
    }
    IRFunction *fn = &m->functions.data[m->functions.len++];
    memset(fn, 0, sizeof(*fn));
    fn->name = NULL;
    fn->insts.data = NULL;
    fn->insts.len = 0;
    fn->insts.cap = 0;
    fn->next_value_id = next_value_id;
    fn->loc = (SourceLoc){NULL, 0, 0};
    return fn;
}

static void push_inst(IRInstArray *a, IROpcode op, IRValue dst,
                      IRValue arg1, IRValue arg2, int imm) {
    if (a->len >= a->cap) {
        a->cap = a->cap ? a->cap * 2 : 16;
        a->data = realloc(a->data, a->cap * sizeof(IRInst));
        if (!a->data) exit(1);
    }
    IRInst inst;
    memset(&inst, 0, sizeof(inst));
    inst.op = op; inst.dst = dst; inst.a = arg1; inst.b = arg2;
    inst.c = -1; inst.d = -1;
    inst.imm = imm; inst.width = 8; inst.call_callee = -1;
    a->data[a->len++] = inst;
}

#define PUSH(op, dst, a1, a2, imm) push_inst(&fn->insts, (op), (dst), (a1), (a2), (imm))

/* Count instructions with a given opcode. */
static int count_op(const IRInstArray *insts, IROpcode op) {
    int c = 0;
    for (size_t i = 0; i < insts->len; i++)
        if (insts->data[i].op == op) c++;
    return c;
}

/* ================================================================ */
/* Test: opt() eliminates ALLOCA/STORE/LOAD                         */
/* ================================================================ */

static void test_opt_reduces_alloca(void) {
    IRModule mod;
    ir_module_init(&mod);
    IRFunction *fn = module_add_fn(&mod, 3);

    /* int x = 42; return x;
     *   ALLOCA dst=0; CONST dst=1=42; STORE a=0,b=1; LOAD dst=2,a=0; RETURN a=2 */
    PUSH(IR_ALLOCA, 0, -1, -1, 0);
    PUSH(IR_CONST,  1, -1, -1, 42);
    PUSH(IR_STORE, -1,  0,  1, 0);
    PUSH(IR_LOAD,   2,  0, -1, 0);
    PUSH(IR_RETURN, -1,  2, -1, 0);

    opt(&mod, 1, 0);

    /* After opt: no ALLOCA, STORE, or LOAD remain. */
    T_ASSERT_EQ_INT(count_op(&fn->insts, IR_ALLOCA), 0);
    T_ASSERT_EQ_INT(count_op(&fn->insts, IR_STORE), 0);
    T_ASSERT_EQ_INT(count_op(&fn->insts, IR_LOAD), 0);

    ir_module_free(&mod);
}

/* ================================================================ */
/* Test: opt() folds return-of-constant                             */
/* ================================================================ */

static void test_opt_folds_return(void) {
    IRModule mod;
    ir_module_init(&mod);
    IRFunction *fn = module_add_fn(&mod, 3);

    /* int x = 5; return x;
     *   ALLOCA dst=0; CONST dst=1=5; STORE a=0,b=1; LOAD dst=2,a=0; RETURN a=2 */
    PUSH(IR_ALLOCA, 0, -1, -1, 0);
    PUSH(IR_CONST,  1, -1, -1, 5);
    PUSH(IR_STORE, -1,  0,  1, 0);
    PUSH(IR_LOAD,   2,  0, -1, 0);
    PUSH(IR_RETURN, -1,  2, -1, 0);

    opt(&mod, 1, 0);

    /* After opt: RETURN should reference the CONST 5 directly.
     * mem2reg turns LOAD→COPY from the CONST; constfold/peephole eliminate the
     * intermediate COPY, so RETURN a should be the CONST's dst (1). */
    T_ASSERT_EQ_INT(count_op(&fn->insts, IR_ALLOCA), 0);
    T_ASSERT_EQ_INT(count_op(&fn->insts, IR_STORE), 0);
    T_ASSERT_EQ_INT(count_op(&fn->insts, IR_LOAD), 0);

    /* The CONST 5 and RETURN should remain. */
    int found_const5 = 0, found_ret = 0;
    for (size_t i = 0; i < fn->insts.len; i++) {
        IRInst *inst = &fn->insts.data[i];
        if (inst->op == IR_CONST && inst->imm == 5) found_const5 = 1;
        if (inst->op == IR_RETURN) {
            /* RETURN should reference the CONST (dst=1) or a COPY of it. */
            T_ASSERT(inst->a == 1 || count_op(&fn->insts, IR_COPY) > 0);
            found_ret = 1;
        }
    }
    T_ASSERT(found_const5);
    T_ASSERT(found_ret);

    ir_module_free(&mod);
}

/* ================================================================ */
/* Test: opt() eliminates dead code                                 */
/* ================================================================ */

static void test_opt_eliminates_dead(void) {
    IRModule mod;
    ir_module_init(&mod);
    IRFunction *fn = module_add_fn(&mod, 5);

    /* int y = 42;       (dead — y never used)
     * int z = 3 + 4;    (dead — z never used)
     * return 10;
     *
     * ALLOCA dst=0; ALLOCA dst=1;
     * CONST dst=2=42; STORE a=0,b=2;      -- dead
     * CONST dst=3=3; CONST dst=4=4;
     * ADD dst=5,a=3,b=4; STORE a=1,b=5;   -- dead
     * CONST dst=6=10; RETURN a=6 */
    PUSH(IR_ALLOCA, 0, -1, -1, 0);
    PUSH(IR_ALLOCA, 1, -1, -1, 0);
    PUSH(IR_CONST,  2, -1, -1, 42);
    PUSH(IR_STORE, -1,  0,  2, 0);
    PUSH(IR_CONST,  3, -1, -1, 3);
    PUSH(IR_CONST,  4, -1, -1, 4);
    PUSH(IR_ADD,    5,  3,  4, 0);
    PUSH(IR_STORE, -1,  1,  5, 0);
    PUSH(IR_CONST,  6, -1, -1, 10);
    PUSH(IR_RETURN, -1,  6, -1, 0);

    opt(&mod, 1, 0);

    /* mem2reg eliminates ALLOCA/STORE/LOAD.
     * The dead value (ADD dst=5 and CONST dst=2) should be eliminated by DCE.
     * The peephole might fold 3+4→7, but that's still dead and gets removed. */
    T_ASSERT_EQ_INT(count_op(&fn->insts, IR_ALLOCA), 0);
    T_ASSERT_EQ_INT(count_op(&fn->insts, IR_STORE), 0);

    /* Only essential instructions remain: CONST 10, RETURN. */
    /* The ADD (dst=5) should be gone since its result is unused. */
    T_ASSERT_EQ_INT(count_op(&fn->insts, IR_ADD), 0);

    /* CONST 42 should be gone (it was only stored, never loaded, and
     * mem2reg eliminated the store as dead; then DCE removed the CONST
     * since its dst is unused). */
    int found_const42 = 0;
    for (size_t i = 0; i < fn->insts.len; i++)
        if (fn->insts.data[i].op == IR_CONST && fn->insts.data[i].imm == 42)
            found_const42 = 1;
    T_ASSERT(!found_const42);

    /* CONST 10 and RETURN must survive. */
    T_ASSERT(count_op(&fn->insts, IR_CONST) >= 1);
    T_ASSERT_EQ_INT(count_op(&fn->insts, IR_RETURN), 1);

    ir_module_free(&mod);
}

/* ================================================================ */
/* Test: opt() is idempotent — opt(opt(fn)) == opt(fn)              */
/* ================================================================ */

/* Serialize a function's instructions to a string for comparison.
 * Format: "op dst a b imm" per line. */
static char *serialize_insts(const IRInstArray *insts) {
    /* Worst-case: ~80 chars per inst */
    size_t buf_sz = insts->len * 80 + 1;
    char *buf = malloc(buf_sz);
    if (!buf) return NULL;
    buf[0] = '\0';
    size_t off = 0;
    for (size_t i = 0; i < insts->len; i++) {
        IRInst *inst = &insts->data[i];
        int n = snprintf(buf + off, buf_sz - off,
                         "%d %d %d %d\n",
                         (int)inst->op, (int)inst->dst,
                         (int)inst->a, (int)inst->b);
        if (n < 0 || (size_t)n >= buf_sz - off) break;
        off += (size_t)n;
    }
    return buf;
}

static void test_opt_idempotent(void) {
    IRModule mod;
    ir_module_init(&mod);
    IRFunction *fn = module_add_fn(&mod, 3);

    /* int x = 5; return x; */
    PUSH(IR_ALLOCA, 0, -1, -1, 0);
    PUSH(IR_CONST,  1, -1, -1, 5);
    PUSH(IR_STORE, -1,  0,  1, 0);
    PUSH(IR_LOAD,   2,  0, -1, 0);
    PUSH(IR_RETURN, -1,  2, -1, 0);

    opt(&mod, 1, 0);

    /* Serialize after first opt. */
    char *s1 = serialize_insts(&fn->insts);
    T_ASSERT(s1 != NULL);

    /* Second opt should not change anything. */
    opt(&mod, 1, 0);
    char *s2 = serialize_insts(&fn->insts);
    T_ASSERT(s2 != NULL);

    /* Compare — they should be identical. */
    T_ASSERT_STR_EQ(s1, s2);

    free(s1);
    free(s2);
    ir_module_free(&mod);
}

/* ================================================================ */
/* Test: opt() on function with no allocas (just const/return)      */
/* ================================================================ */

static void test_opt_no_allocas(void) {
    IRModule mod;
    ir_module_init(&mod);
    IRFunction *fn = module_add_fn(&mod, 3);

    /* return 1 + 2; →  CONST 0=1; CONST 1=2; ADD 2,0,1; RETURN 2 */
    PUSH(IR_CONST,  0, -1, -1, 1);
    PUSH(IR_CONST,  1, -1, -1, 2);
    PUSH(IR_ADD,    2,  0,  1, 0);
    PUSH(IR_RETURN, -1,  2, -1, 0);

    opt(&mod, 1, 0);

    /* After constfold: 1+2 → 3.  After DCE: CONST 0 and CONST 1 may be
     * eliminated if unused.  The ADD becomes CONST 3. */
    T_ASSERT(fn->insts.len >= 2);

    /* RETURN should exist. */
    int found_ret = 0, found_const3 = 0;
    for (size_t i = 0; i < fn->insts.len; i++) {
        IRInst *inst = &fn->insts.data[i];
        if (inst->op == IR_RETURN) found_ret = 1;
        if (inst->op == IR_CONST && inst->imm == 3) found_const3 = 1;
    }
    T_ASSERT(found_ret);
    T_ASSERT(found_const3);

    ir_module_free(&mod);
}

/* ---- main ---- */

int main(void) {
    test_opt_reduces_alloca();
    test_opt_folds_return();
    test_opt_eliminates_dead();
    test_opt_idempotent();
    test_opt_no_allocas();
    return t_finalize();
}
