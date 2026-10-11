#include "fakecc/mem2reg.h"
#include "test_framework.h"

#include <stdlib.h>
#include <string.h>

/* ---- helpers to construct IRInstArray by hand ---- */

static IRInstArray make_insts(void) {
    IRInstArray a = {NULL, 0, 0};
    return a;
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

#define PUSH(op, dst, a1, a2, imm) push_inst(&insts, (op), (dst), (a1), (a2), (imm))

/* Free the data owned by an IRInstArray (zeroes the struct fields).
 * mem2reg replaces fn->insts with a freshly malloc'd array, so we must
 * free it ourselves. */
static void inst_array_data_free(IRInstArray *a) {
    free(a->data);
    a->data = NULL;
    a->len = 0;
    a->cap = 0;
}

/* Count how many instructions have a given opcode. */
static int count_op(const IRInstArray *insts, IROpcode op) {
    int c = 0;
    for (size_t i = 0; i < insts->len; i++)
        if (insts->data[i].op == op) c++;
    return c;
}

/* ================================================================ */
/* Test cases                                                        */
/* ================================================================ */

/* Single variable: int x = 42; return x;
 *   ALLOCA dst=0; CONST dst=1=42; STORE a=0,b=1; LOAD dst=2,a=0; RETURN a=2
 * After mem2reg: ALLOCA/STORE/LOAD gone. LOAD→COPY referencing CONST. */
static void test_mem2reg_single_var(void) {
    IRInstArray insts = make_insts();
    PUSH(IR_ALLOCA, 0, -1, -1, 0);
    PUSH(IR_CONST,  1, -1, -1, 42);
    PUSH(IR_STORE, -1,  0,  1, 0);
    PUSH(IR_LOAD,   2,  0, -1, 0);
    PUSH(IR_RETURN, -1,  2, -1, 0);

    IRFunction fn;
    memset(&fn, 0, sizeof(fn));
    fn.name = NULL;
    fn.insts = insts;
    fn.next_value_id = 3; /* v0=ALLOCA, v1=CONST, v2=LOAD */
    fn.loc = (SourceLoc){NULL, 0, 0};

    int promoted = opt_mem2reg(&fn, 0);
    T_ASSERT_EQ_INT(promoted, 1);

    /* Verify no ALLOCA/STORE/LOAD remain. */
    T_ASSERT_EQ_INT(count_op(&fn.insts, IR_ALLOCA), 0);
    T_ASSERT_EQ_INT(count_op(&fn.insts, IR_STORE), 0);
    T_ASSERT_EQ_INT(count_op(&fn.insts, IR_LOAD), 0);

    /* Output should have CONST 42, COPY, RETURN. */
    T_ASSERT(fn.insts.len >= 3);

    /* Find the CONST. */
    int found_const = 0, found_copy = 0, found_ret = 0;
    for (size_t i = 0; i < fn.insts.len; i++) {
        IRInst *inst = &fn.insts.data[i];
        if (inst->op == IR_CONST && inst->imm == 42) found_const = 1;
        if (inst->op == IR_COPY && inst->dst == 2 && inst->a == 1) found_copy = 1;
        if (inst->op == IR_RETURN && inst->a == 2) found_ret = 1;
    }
    T_ASSERT(found_const);
    T_ASSERT(found_copy);
    T_ASSERT(found_ret);

    inst_array_data_free(&fn.insts); /* internal helper from mem2reg.c */
}

/* x=5; return x; → after mem2reg, COPY replaces LOAD, referencing CONST 5. */
static void test_mem2reg_const_prop(void) {
    IRInstArray insts = make_insts();
    PUSH(IR_ALLOCA, 0, -1, -1, 0);
    PUSH(IR_CONST,  1, -1, -1, 5);
    PUSH(IR_STORE, -1,  0,  1, 0);
    PUSH(IR_LOAD,   2,  0, -1, 0);
    PUSH(IR_RETURN, -1,  2, -1, 0);

    IRFunction fn;
    memset(&fn, 0, sizeof(fn));
    fn.name = NULL;
    fn.insts = insts;
    fn.next_value_id = 3;
    fn.loc = (SourceLoc){NULL, 0, 0};

    int promoted = opt_mem2reg(&fn, 0);
    T_ASSERT_EQ_INT(promoted, 1);

    T_ASSERT_EQ_INT(count_op(&fn.insts, IR_ALLOCA), 0);
    T_ASSERT_EQ_INT(count_op(&fn.insts, IR_STORE), 0);

    /* The LOAD→COPY should reference the CONST's dst (1). */
    int found_copy_from_const = 0;
    for (size_t i = 0; i < fn.insts.len; i++) {
        IRInst *inst = &fn.insts.data[i];
        if (inst->op == IR_COPY && inst->a == 1)
            found_copy_from_const = 1;
    }
    T_ASSERT(found_copy_from_const);

    inst_array_data_free(&fn.insts);
}

/* Two variables: int x=1; int y=2; return x+y;
 * Verifies ADD uses the correct new SSA values after mem2reg promotion. */
static void test_mem2reg_two_vars(void) {
    IRInstArray insts = make_insts();
    /* ALLOCA x=0; ALLOCA y=1 */
    PUSH(IR_ALLOCA, 0, -1, -1, 0);
    PUSH(IR_ALLOCA, 1, -1, -1, 0);
    /* CONST 2=1; STORE x,2 */
    PUSH(IR_CONST,  2, -1, -1, 1);
    PUSH(IR_STORE, -1,  0,  2, 0);
    /* CONST 3=2; STORE y,3 */
    PUSH(IR_CONST,  3, -1, -1, 2);
    PUSH(IR_STORE, -1,  1,  3, 0);
    /* LOAD 4,x; LOAD 5,y */
    PUSH(IR_LOAD,   4,  0, -1, 0);
    PUSH(IR_LOAD,   5,  1, -1, 0);
    /* ADD 6,4,5; RETURN 6 */
    PUSH(IR_ADD,    6,  4,  5, 0);
    PUSH(IR_RETURN, -1,  6, -1, 0);

    IRFunction fn;
    memset(&fn, 0, sizeof(fn));
    fn.name = NULL;
    fn.insts = insts;
    fn.next_value_id = 7; /* v0-6 used */
    fn.loc = (SourceLoc){NULL, 0, 0};

    int promoted = opt_mem2reg(&fn, 0);
    T_ASSERT_EQ_INT(promoted, 2);

    /* No ALLOCA/STORE/LOAD remain. */
    T_ASSERT_EQ_INT(count_op(&fn.insts, IR_ALLOCA), 0);
    T_ASSERT_EQ_INT(count_op(&fn.insts, IR_STORE), 0);
    T_ASSERT_EQ_INT(count_op(&fn.insts, IR_LOAD), 0);

    /* The ADD should still use dst=4 and dst=5. */
    int found_add = 0;
    for (size_t i = 0; i < fn.insts.len; i++) {
        IRInst *inst = &fn.insts.data[i];
        if (inst->op == IR_ADD && inst->dst == 6) {
            T_ASSERT_EQ_INT(inst->a, 4);
            T_ASSERT_EQ_INT(inst->b, 5);
            found_add = 1;
        }
    }
    T_ASSERT(found_add);

    /* The COPYs that replaced the LOADs should reference the CONSTs.
     * LOAD 4 from x → COPY 4 = a=2 (CONST 1)
     * LOAD 5 from y → COPY 5 = a=3 (CONST 2) */
    int copy4_correct = 0, copy5_correct = 0;
    for (size_t i = 0; i < fn.insts.len; i++) {
        IRInst *inst = &fn.insts.data[i];
        if (inst->op == IR_COPY && inst->dst == 4 && inst->a == 2)
            copy4_correct = 1;
        if (inst->op == IR_COPY && inst->dst == 5 && inst->a == 3)
            copy5_correct = 1;
    }
    T_ASSERT(copy4_correct);
    T_ASSERT(copy5_correct);

    inst_array_data_free(&fn.insts);
}

/* Uninitialized read: int x; return x; → undef read → CONST 0. */
static void test_mem2reg_undef_read(void) {
    IRInstArray insts = make_insts();
    PUSH(IR_ALLOCA, 0, -1, -1, 0);
    PUSH(IR_LOAD,   1,  0, -1, 0);
    PUSH(IR_RETURN, -1,  1, -1, 0);

    IRFunction fn;
    memset(&fn, 0, sizeof(fn));
    fn.name = NULL;
    fn.insts = insts;
    fn.next_value_id = 2; /* v0=ALLOCA, v1=LOAD */
    fn.loc = (SourceLoc){NULL, 0, 0};

    int promoted = opt_mem2reg(&fn, 0);
    T_ASSERT_EQ_INT(promoted, 1);

    /* No ALLOCA. LOAD→CONST 0. */
    T_ASSERT_EQ_INT(count_op(&fn.insts, IR_ALLOCA), 0);
    T_ASSERT_EQ_INT(count_op(&fn.insts, IR_LOAD), 0);

    /* Verify CONST 0 was produced. */
    int found_const0 = 0;
    for (size_t i = 0; i < fn.insts.len; i++) {
        IRInst *inst = &fn.insts.data[i];
        if (inst->op == IR_CONST && inst->imm == 0)
            found_const0 = 1;
    }
    T_ASSERT(found_const0);

    /* RETURN should reference dst=1. */
    int found_ret = 0;
    for (size_t i = 0; i < fn.insts.len; i++) {
        IRInst *inst = &fn.insts.data[i];
        if (inst->op == IR_RETURN && inst->a == 1)
            found_ret = 1;
    }
    T_ASSERT(found_ret);

    inst_array_data_free(&fn.insts);
}

/* No allocas — mem2reg should be a no-op and return 0. */
static void test_mem2reg_no_allocas(void) {
    IRInstArray insts = make_insts();
    /* Just: CONST 0=10; RETURN 0 — no ALLOCA at all. */
    PUSH(IR_CONST,  0, -1, -1, 10);
    PUSH(IR_RETURN, -1,  0, -1, 0);

    IRFunction fn;
    memset(&fn, 0, sizeof(fn));
    fn.name = NULL;
    fn.insts = insts;
    fn.next_value_id = 1;
    fn.loc = (SourceLoc){NULL, 0, 0};

    int promoted = opt_mem2reg(&fn, 0);
    T_ASSERT_EQ_INT(promoted, 0);

    /* Instructions should be unchanged. */
    T_ASSERT_EQ_INT((int)fn.insts.len, 2);
    T_ASSERT_EQ_INT(fn.insts.data[0].op, IR_CONST);
    T_ASSERT_EQ_INT(fn.insts.data[1].op, IR_RETURN);

    inst_array_data_free(&fn.insts);
}

/* Diamond (if-else): verify φ nodes are correctly resolved into COPYs
 * in predecessor blocks. */
static void test_mem2reg_diamond(void) {
    IRInstArray insts = make_insts();
    /* Block 0 (entry):
     *   ALLOCA dst=0; CONST dst=1=1; CBR a=1 true→10 false→20
     * Block 1 (then, label 10):
     *   LABEL 10; CONST dst=2=5; STORE a=0,b=2; BR → 30
     * Block 2 (else, label 20):
     *   LABEL 20; CONST dst=3=10; STORE a=0,b=3; BR → 30
     * Block 3 (merge, label 30):
     *   LABEL 30; LOAD dst=4,a=0; RETURN a=4 */
    PUSH(IR_ALLOCA, 0, -1, -1, 0);
    PUSH(IR_CONST,  1, -1, -1, 1);
    PUSH(IR_CBR,   -1,  1, 20, 10);

    PUSH(IR_LABEL, -1, -1, -1, 10);
    PUSH(IR_CONST,  2, -1, -1, 5);
    PUSH(IR_STORE, -1,  0,  2, 0);
    PUSH(IR_BR,    -1, -1, -1, 30);

    PUSH(IR_LABEL, -1, -1, -1, 20);
    PUSH(IR_CONST,  3, -1, -1, 10);
    PUSH(IR_STORE, -1,  0,  3, 0);
    PUSH(IR_BR,    -1, -1, -1, 30);

    PUSH(IR_LABEL, -1, -1, -1, 30);
    PUSH(IR_LOAD,   4,  0, -1, 0);
    PUSH(IR_RETURN, -1,  4, -1, 0);

    IRFunction fn;
    memset(&fn, 0, sizeof(fn));
    fn.name = NULL;
    fn.insts = insts;
    fn.next_value_id = 5; /* v0-4 used */
    fn.loc = (SourceLoc){NULL, 0, 0};

    int promoted = opt_mem2reg(&fn, 0);
    T_ASSERT_EQ_INT(promoted, 1);

    /* No ALLOCA/STORE/LOAD remain. */
    T_ASSERT_EQ_INT(count_op(&fn.insts, IR_ALLOCA), 0);
    T_ASSERT_EQ_INT(count_op(&fn.insts, IR_STORE), 0);
    T_ASSERT_EQ_INT(count_op(&fn.insts, IR_LOAD), 0);

    /* The φ result dst should appear in COPY instructions in predecessor
     * blocks (then and else). Find φ dst (the value id assigned by mem2reg).
     * The φ was placed in the merge block; its dst consumed a new value id. */
    int found_phi_copy_in_then = 0, found_phi_copy_in_else = 0;
    for (size_t i = 0; i < fn.insts.len; i++) {
        IRInst *inst = &fn.insts.data[i];
        if (inst->op == IR_COPY) {
            /* COPY from CONST 5 (dst=2) → then branch */
            if (inst->a == 2) found_phi_copy_in_then = 1;
            /* COPY from CONST 10 (dst=3) → else branch */
            if (inst->a == 3) found_phi_copy_in_else = 1;
        }
    }
    T_ASSERT(found_phi_copy_in_then);
    T_ASSERT(found_phi_copy_in_else);

    /* RETURN should reference the original LOAD dst (4). */
    T_ASSERT_EQ_INT(fn.insts.data[fn.insts.len - 1].op, IR_RETURN);
    T_ASSERT_EQ_INT(fn.insts.data[fn.insts.len - 1].a, 4);

    inst_array_data_free(&fn.insts);
}

/* ---- main ---- */

int main(void) {
    test_mem2reg_single_var();
    test_mem2reg_const_prop();
    test_mem2reg_two_vars();
    test_mem2reg_undef_read();
    test_mem2reg_no_allocas();
    test_mem2reg_diamond();
    return t_finalize();
}
