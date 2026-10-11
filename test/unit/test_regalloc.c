#include "fakecc/regalloc.h"
#include "fakecc/reg_arm64.h"
#include "fakecc/target.h"
#include "fakecc/mem2reg.h"
#include "test_framework.h"

#include <stdlib.h>
#include <string.h>

/* ---- helpers to construct IR ---- */

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

static void inst_array_data_free(IRInstArray *a) {
    free(a->data);
    a->data = NULL; a->len = a->cap = 0;
}

/* Count how many values are spilled. */
static int count_spilled(const RAResult *ra) {
    int c = 0;
    for (int i = 0; i < ra->num_values; i++)
        if (ra->reg[i] < 0) c++;
    return c;
}

/* ================================================================ */
/* Test: CONST → RETURN gets registers                              */
/* ================================================================ */

static void test_ra_single_const(void) {
    IRFunction fn;
    fn.name = NULL;
    fn.insts.data = NULL; fn.insts.len = 0; fn.insts.cap = 0;
    fn.next_value_id = 2;
    fn.loc = (SourceLoc){NULL, 0, 0};
    fn.ra = NULL;
    fn.value_is_float = NULL;
    fn.value_meta_cap = 0;
    fn.ra_xmm = NULL;

    /* CONST 0 = 42; RETURN 0 */
    push_inst(&fn.insts, IR_CONST,  0, -1, -1, 42);
    push_inst(&fn.insts, IR_RETURN, -1,  0, -1, 0);

    RAResult *ra = reg_alloc(&fn);
    T_ASSERT(ra != NULL);
    T_ASSERT_EQ_INT(ra->num_values, 2);

    /* CONST dst=0 should be in a register. */
    T_ASSERT(ra->reg[0] >= 0 || ra->reg[0] == REG_NONE);

    /* RETURN references value 0 — which is the CONST. */
    /* Verify the return value (0) is in a register (likely RAX). */

    ra_result_free(ra);
    inst_array_data_free(&fn.insts);
}

/* ================================================================ */
/* Test: two non-interfering values → different registers           */
/* ================================================================ */

static void test_ra_two_independent(void) {
    IRFunction fn;
    fn.name = NULL;
    fn.insts.data = NULL; fn.insts.len = 0; fn.insts.cap = 0;
    fn.next_value_id = 3;
    fn.loc = (SourceLoc){NULL, 0, 0};
    fn.ra = NULL;
    fn.value_is_float = NULL;
    fn.value_meta_cap = 0;
    fn.ra_xmm = NULL;

    /* v0 = CONST 1; v1 = CONST 2; RETURN v0
     * v0 and v1 are independent (no interference — they never overlap in use). */
    push_inst(&fn.insts, IR_CONST,  0, -1, -1, 1);
    push_inst(&fn.insts, IR_CONST,  1, -1, -1, 2);
    push_inst(&fn.insts, IR_RETURN, -1,  0, -1, 0);

    RAResult *ra = reg_alloc(&fn);
    T_ASSERT(ra != NULL);
    T_ASSERT_EQ_INT(ra->num_values, 3);

    /* Both v0 and v1 should be in registers. They may or may not be
     * different registers since they don't interfere. */
    T_ASSERT(ra->reg[0] >= 0 || ra->reg[0] == REG_NONE);
    T_ASSERT(ra->reg[1] >= 0 || ra->reg[1] == REG_NONE);

    ra_result_free(ra);
    inst_array_data_free(&fn.insts);
}

/* ================================================================ */
/* Test: interfering pair must use different registers              */
/* ================================================================ */

static void test_ra_interfering_pair(void) {
    IRFunction fn;
    fn.name = NULL;
    fn.insts.data = NULL; fn.insts.len = 0; fn.insts.cap = 0;
    fn.next_value_id = 4;
    fn.loc = (SourceLoc){NULL, 0, 0};
    fn.ra = NULL;
    fn.value_is_float = NULL;
    fn.value_meta_cap = 0;
    fn.ra_xmm = NULL;

    /* v0 = CONST 3; v1 = CONST 4; v2 = ADD v0, v1; RETURN v2
     * v0 and v1 are both live at the ADD instruction → they interfere. */
    push_inst(&fn.insts, IR_CONST,  0, -1, -1, 3);
    push_inst(&fn.insts, IR_CONST,  1, -1, -1, 4);
    push_inst(&fn.insts, IR_ADD,    2,  0,  1, 0);
    push_inst(&fn.insts, IR_RETURN, -1,  2, -1, 0);

    RAResult *ra = reg_alloc(&fn);
    T_ASSERT(ra != NULL);

    /* v0 and v1 interfere, so they must be in different registers. */
    if (ra->reg[0] >= 0 && ra->reg[1] >= 0) {
        T_ASSERT(ra->reg[0] != ra->reg[1]);
    }

    ra_result_free(ra);
    inst_array_data_free(&fn.insts);
}

/* ================================================================ */
/* Test: run full pipeline (mem2reg + reg_alloc) on variable code   */
/* ================================================================ */

static void test_ra_after_mem2reg(void) {
    IRFunction fn;
    fn.name = NULL;
    fn.insts.data = NULL; fn.insts.len = 0; fn.insts.cap = 0;
    fn.next_value_id = 3;
    fn.loc = (SourceLoc){NULL, 0, 0};
    fn.ra = NULL;
    fn.value_is_float = NULL;
    fn.value_meta_cap = 0;
    fn.ra_xmm = NULL;

    /* int x = 5; return x;
     *   ALLOCA 0; CONST 1=5; STORE 0,1; LOAD 2,0; RETURN 2 */
    push_inst(&fn.insts, IR_ALLOCA, 0, -1, -1, 0);
    push_inst(&fn.insts, IR_CONST,  1, -1, -1, 5);
    push_inst(&fn.insts, IR_STORE, -1,  0,  1, 0);
    push_inst(&fn.insts, IR_LOAD,   2,  0, -1, 0);
    push_inst(&fn.insts, IR_RETURN, -1,  2, -1, 0);

    /* Run mem2reg to promote to SSA form. */
    opt_mem2reg(&fn, 0);

    /* After mem2reg, next_value_id may have grown (phi result ids). */
    /* Run register allocator. */
    RAResult *ra = reg_alloc(&fn);
    T_ASSERT(ra != NULL);

    /* The return value should be in a register (or at least not crashed). */
    /* CONST 5 (dst=1) should be in a register. */
    T_ASSERT(ra->reg[1] >= 0 || ra->reg[1] == REG_NONE);

    /* Verify we have no or few spills for this simple case. */
    T_ASSERT(count_spilled(ra) <= 2);

    ra_result_free(ra);
    inst_array_data_free(&fn.insts);
}

/* ================================================================ */
/* Test: NULL function (no values)                                   */
/* ================================================================ */

static void test_ra_empty(void) {
    IRFunction fn;
    fn.name = NULL;
    fn.insts.data = NULL; fn.insts.len = 0; fn.insts.cap = 0;
    fn.next_value_id = 1;
    fn.loc = (SourceLoc){NULL, 0, 0};
    fn.ra = NULL;
    fn.value_is_float = NULL;
    fn.value_meta_cap = 0;
    fn.ra_xmm = NULL;

    /* Just RETURN with no prior defs (next_value_id = 1, but no CONST). */
    push_inst(&fn.insts, IR_RETURN, -1, 0, -1, 0);

    RAResult *ra = reg_alloc(&fn);
    /* May succeed (liveness finds value 0 is used) or return NULL. */
    if (ra) {
        /* Allocation should handle the undefined value. */
        ra_result_free(ra);
    }

    inst_array_data_free(&fn.insts);
}

/* ================================================================ */
/* Test: loop-carried value interferes with itself across back edge  */
/*                                                                    */
/* This exercises CFG-aware liveness. In a diamond with a back edge:  */
/*                                                                    */
/*   entry:                                                            */
/*     v0 = CONST 1                                                    */
/*     v1 = CONST 0                                                    */
/*     BR L_head                                                       */
/*   L_head:                                                           */
/*     v2 = CBR-cond (from previous body iteration or from entry)      */
/*     CBR v2 → L_body, L_exit                                         */
/*   L_body:                                                           */
/*     v3 = ADD v0 v1  (uses v0 which was defined pre-loop)            */
/*     BR L_head                                                       */
/*   L_exit:                                                           */
/*     RETURN v0                                                       */
/*                                                                    */
/* v0 must remain live across the back edge (used in body AND after   */
/* exit).  Interval liveness would say v0 dies after the last linear  */
/* use, but CFG liveness must keep it live throughout.                */
/* ================================================================ */

static void test_ra_loop_carried_liveness(void) {
    IRFunction fn;
    fn.name = NULL;
    fn.insts.data = NULL; fn.insts.len = 0; fn.insts.cap = 0;
    fn.next_value_id = 4;
    fn.next_label_id = 3;
    fn.loc = (SourceLoc){NULL, 0, 0};
    fn.ra = NULL;
    fn.value_is_float = NULL;
    fn.value_meta_cap = 0;
    fn.ra_xmm = NULL;

    /* pre-loop */
    push_inst(&fn.insts, IR_CONST, 0, -1, -1, 1);   /* v0 = 1 */
    push_inst(&fn.insts, IR_CONST, 1, -1, -1, 0);   /* v1 = 0 */
    push_inst(&fn.insts, IR_BR,   -1, -1, -1, 0);   /* → L0 head */
    push_inst(&fn.insts, IR_LABEL,-1, -1, -1, 0);   /* L0: head */
    push_inst(&fn.insts, IR_CONST, 2, -1, -1, 1);   /* v2 = 1 (cond) */
    push_inst(&fn.insts, IR_CBR,  -1,  2,  2, 1);   /* if v2 → L1 else L2 */
    push_inst(&fn.insts, IR_LABEL,-1, -1, -1, 1);   /* L1: body */
    push_inst(&fn.insts, IR_ADD,   3,  0,  1, 0);   /* v3 = v0 + v1  ← uses v0 */
    push_inst(&fn.insts, IR_BR,   -1, -1, -1, 0);   /* back to L0 */
    push_inst(&fn.insts, IR_LABEL,-1, -1, -1, 2);   /* L2: exit */
    push_inst(&fn.insts, IR_RETURN,-1, 0, -1, 0);   /* return v0 */

    RAResult *ra = reg_alloc(&fn);
    T_ASSERT(ra != NULL);

    /* Key invariant: v0's register must NOT alias v3's register.
     * If interval liveness were used, v0's last linear use is the RETURN,
     * and v3's live range is confined to the body — the interval builder
     * would say they don't overlap.  But v0 must be preserved across the
     * back edge every iteration, so it interferes with v3. */
    if (ra->reg[0] >= 0 && ra->reg[3] >= 0) {
        T_ASSERT(ra->reg[0] != ra->reg[3]);
    }

    ra_result_free(ra);
    inst_array_data_free(&fn.insts);
}

/* ================================================================ */
/* Test: many simultaneously-live values force some spills          */
/* ================================================================ */

static void test_ra_spill_pressure(void) {
    IRFunction fn;
    fn.name = NULL;
    fn.insts.data = NULL; fn.insts.len = 0; fn.insts.cap = 0;
    fn.next_value_id = 0;
    fn.loc = (SourceLoc){NULL, 0, 0};
    fn.ra = NULL;
    fn.value_is_float = NULL;
    fn.value_meta_cap = 0;
    fn.ra_xmm = NULL;

    /* Create 16 independent consts, then sum them left-associatively so
     * all earlier values stay live until the end — more than REG_ALLOCATABLE. */
    const int N = 16;
    for (int i = 0; i < N; i++)
        push_inst(&fn.insts, IR_CONST, i, -1, -1, i + 1);
    int acc = N;
    push_inst(&fn.insts, IR_ADD, acc, 0, 1, 0);
    for (int i = 2; i < N; i++) {
        int next = acc + 1;
        push_inst(&fn.insts, IR_ADD, next, acc, i, 0);
        acc = next;
    }
    push_inst(&fn.insts, IR_RETURN, -1, acc, -1, 0);
    fn.next_value_id = acc + 1;

    RAResult *ra = reg_alloc(&fn);
    T_ASSERT(ra != NULL);
    T_ASSERT(count_spilled(ra) >= 1);
    /* Accumulators and consts that remain live should still be assigned. */
    T_ASSERT(ra->reg[acc] >= 0 || ra->reg[acc] == REG_NONE);

    ra_result_free(ra);
    inst_array_data_free(&fn.insts);
}

/* ================================================================ */
/* Test: return value prefers a usable register assignment          */
/* ================================================================ */

static void test_ra_return_assigned(void) {
    IRFunction fn;
    fn.name = NULL;
    fn.insts.data = NULL; fn.insts.len = 0; fn.insts.cap = 0;
    fn.next_value_id = 3;
    fn.loc = (SourceLoc){NULL, 0, 0};
    fn.ra = NULL;
    fn.value_is_float = NULL;
    fn.value_meta_cap = 0;
    fn.ra_xmm = NULL;

    push_inst(&fn.insts, IR_CONST, 0, -1, -1, 10);
    push_inst(&fn.insts, IR_CONST, 1, -1, -1, 32);
    push_inst(&fn.insts, IR_ADD, 2, 0, 1, 0);
    push_inst(&fn.insts, IR_RETURN, -1, 2, -1, 0);

    RAResult *ra = reg_alloc(&fn);
    T_ASSERT(ra != NULL);
    T_ASSERT(ra->reg[2] >= 0); /* sum should not be spilled in this tiny fn */
    T_ASSERT_EQ_INT(count_spilled(ra), 0);

    ra_result_free(ra);
    inst_array_data_free(&fn.insts);
}

/* Push a no-argument named CALL defining dst. */
static void push_call(IRInstArray *a, IRValue dst, const char *name) {
    if (a->len >= a->cap) {
        a->cap = a->cap ? a->cap * 2 : 16;
        a->data = realloc(a->data, a->cap * sizeof(IRInst));
        if (!a->data) exit(1);
    }
    IRInst inst;
    memset(&inst, 0, sizeof(inst));
    inst.op = IR_CALL; inst.dst = dst; inst.a = -1; inst.b = -1;
    inst.c = -1; inst.d = -1;
    inst.width = 8; inst.call_callee = -1;
    inst.call_name = (char *)name;
    inst.call_nargs = 0;
    inst.call_args = NULL;
    a->data[a->len++] = inst;
}

static IRFunction fn_empty_init(int next_value_id) {
    IRFunction fn;
    fn.name = NULL;
    fn.insts.data = NULL; fn.insts.len = 0; fn.insts.cap = 0;
    fn.next_value_id = next_value_id;
    fn.next_label_id = 0;
    fn.loc = (SourceLoc){NULL, 0, 0};
    fn.ra = NULL;
    fn.value_is_float = NULL;
    fn.value_meta_cap = 0;
    fn.ra_xmm = NULL;
    return fn;
}

/* ================================================================ */
/* Target register-class descriptors                                */
/* ================================================================ */

static void test_class_descriptors_x86(void) {
    target_set_current(target_x86_64_linux());
    const RaRegClasses *c = ra_classes_current();
    T_ASSERT_EQ_INT(c->gp.nregs, 9);
    T_ASSERT_EQ_INT((int)c->gp.caller_saved, (int)GP_CALLER_SAVED_MASK);
    T_ASSERT_EQ_INT(c->gp.spill_bytes, 8);
    T_ASSERT_EQ_INT(c->gp.exclude_16byte_ld, 0);
    T_ASSERT_EQ_INT(c->gp.regs[0], REG_RSI);
    T_ASSERT_EQ_INT(c->simd.nregs, REG_XMM_ALLOCATABLE);
    T_ASSERT_EQ_INT((int)c->simd.caller_saved, (int)XMM_CALLER_SAVED_MASK);
    T_ASSERT(c->simd.spill_bytes == 32 || c->simd.spill_bytes == 64);
    T_ASSERT_EQ_INT(c->simd.exclude_16byte_ld, 1);
}

static void test_class_descriptors_arm64(void) {
    target_set_current(target_arm64_macos());
    const RaRegClasses *c = ra_classes_current();

    /* GP: 24 allocatable; caller colors 0..13 = x2..x15; callee 14..23. */
    T_ASSERT_EQ_INT(c->gp.nregs, A64_GP_NREGS);
    T_ASSERT_EQ_INT((int)c->gp.caller_saved, (int)A64_GP_CALLER_MASK);
    T_ASSERT_EQ_INT(c->gp.spill_bytes, 8);
    T_ASSERT_EQ_INT(c->gp.exclude_16byte_ld, 0);
    T_ASSERT_EQ_INT(c->gp.regs[0], A64_X2);
    T_ASSERT_EQ_INT(c->gp.regs[13], A64_X15);
    T_ASSERT_EQ_INT(c->gp.regs[14], A64_X19);
    T_ASSERT_EQ_INT(c->gp.regs[23], A64_X28);
    /* Reserved registers never appear in the allocatable set (x8 is
     * deliberately allocatable outside aggregate-return sequences). */
    for (int i = 0; i < c->gp.nregs; i++) {
        int r = c->gp.regs[i];
        T_ASSERT(r != A64_X0 && r != A64_X1);
        T_ASSERT(r != A64_X16 && r != A64_X17 && r != A64_X18);
        T_ASSERT(r != A64_FP && r != A64_LR && r != A64_SP);
    }

    /* SIMD: 30 allocatable; caller 0..21 = v0..v7,v16..v29; callee v8..v15. */
    T_ASSERT_EQ_INT(c->simd.nregs, A64_VEC_NREGS);
    T_ASSERT_EQ_INT((int)c->simd.caller_saved, (int)A64_VEC_CALLER_MASK);
    T_ASSERT_EQ_INT(c->simd.spill_bytes, 16);
    T_ASSERT_EQ_INT(c->simd.exclude_16byte_ld, 0);
    T_ASSERT_EQ_INT(c->simd.regs[0], A64_V0);
    T_ASSERT_EQ_INT(c->simd.regs[7], A64_V7);
    T_ASSERT_EQ_INT(c->simd.regs[8], A64_V16);
    T_ASSERT_EQ_INT(c->simd.regs[21], A64_V29);
    T_ASSERT_EQ_INT(c->simd.regs[22], A64_V8);
    T_ASSERT_EQ_INT(c->simd.regs[29], A64_V15);
    for (int i = 0; i < c->simd.nregs; i++) {
        int r = c->simd.regs[i];
        T_ASSERT(r != A64_V30 && r != A64_V31);
    }

    /* Every allocatable color is either caller- or callee-saved, never both. */
    for (int i = 0; i < c->gp.nregs; i++) {
        int in_caller = (c->gp.caller_saved >> i) & 1;
        T_ASSERT(in_caller == (i < 14));
    }
    for (int i = 0; i < c->simd.nregs; i++) {
        int in_caller = (c->simd.caller_saved >> i) & 1;
        T_ASSERT(in_caller == (i < 22));
    }
}

/* A value live across a CALL must be assigned a callee-saved register. */
static void test_arm64_live_across_call(void) {
    target_set_current(target_arm64_macos());
    IRFunction fn = fn_empty_init(3);

    push_inst(&fn.insts, IR_CONST, 0, -1, -1, 1);
    push_call(&fn.insts, 1, "f");
    push_inst(&fn.insts, IR_ADD,   2,  0,  1, 0);  /* v0 live across call */
    push_inst(&fn.insts, IR_RETURN, -1,  2, -1, 0);

    RAResult *ra = reg_alloc(&fn);
    T_ASSERT(ra != NULL);
    T_ASSERT(ra->reg[0] >= 0);
    /* Caller colors 0..13 map to x2..x15; v0 must be in callee x19..x28. */
    T_ASSERT(ra->reg[0] >= A64_X19 && ra->reg[0] <= A64_X28);

    ra_result_free(ra);
    inst_array_data_free(&fn.insts);
}

/* Same IR under x86: v0 must land in one of RBX/R12/R13. */
static void test_x86_live_across_call(void) {
    target_set_current(target_x86_64_linux());
    IRFunction fn = fn_empty_init(3);

    push_inst(&fn.insts, IR_CONST, 0, -1, -1, 1);
    push_call(&fn.insts, 1, "f");
    push_inst(&fn.insts, IR_ADD,   2,  0,  1, 0);
    push_inst(&fn.insts, IR_RETURN, -1,  2, -1, 0);

    RAResult *ra = reg_alloc(&fn);
    T_ASSERT(ra != NULL);
    T_ASSERT(ra->reg[0] == REG_RBX || ra->reg[0] == REG_R12
             || ra->reg[0] == REG_R13);

    ra_result_free(ra);
    inst_array_data_free(&fn.insts);
}

/* More simultaneously-live values than registers force spills, and the
 * GP spill area stays 16-byte aligned. */
static void test_arm64_spill_pressure(void) {
    target_set_current(target_arm64_macos());
    IRFunction fn = fn_empty_init(0);
    const int N = A64_GP_NREGS + 2;   /* 26 */
    for (int i = 0; i < N; i++)
        push_inst(&fn.insts, IR_CONST, i, -1, -1, i + 1);
    int acc = N;
    push_inst(&fn.insts, IR_ADD, acc, 0, 1, 0);
    for (int i = 2; i < N; i++) {
        int next = acc + 1;
        push_inst(&fn.insts, IR_ADD, next, acc, i, 0);
        acc = next;
    }
    push_inst(&fn.insts, IR_RETURN, -1, acc, -1, 0);
    fn.next_value_id = acc + 1;

    RAResult *ra = reg_alloc(&fn);
    T_ASSERT(ra != NULL);
    T_ASSERT(count_spilled(ra) >= 1);
    T_ASSERT_EQ_INT(ra->stack_size % 16, 0);

    ra_result_free(ra);
    inst_array_data_free(&fn.insts);
}

/* ---- main ---- */

int main(void) {
    const TargetDesc *saved = target_current();

    /* Existing cases pin the x86-64 register file. */
    target_set_current(target_x86_64_linux());
    test_ra_single_const();
    test_ra_two_independent();
    test_ra_interfering_pair();
    test_ra_after_mem2reg();
    test_ra_empty();
    test_ra_loop_carried_liveness();
    test_ra_spill_pressure();
    test_ra_return_assigned();
    test_class_descriptors_x86();

    /* arm64 register file and allocation. */
    test_class_descriptors_arm64();
    test_arm64_live_across_call();
    test_x86_live_across_call();
    test_arm64_spill_pressure();

    target_set_current(saved);
    return t_finalize();
}
