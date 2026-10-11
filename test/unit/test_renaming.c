#include "fakecc/cfg.h"
#include "fakecc/domtree.h"
#include "fakecc/phi.h"
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

static void free_insts(IRInstArray *a) {
    free(a->data);
    a->data = NULL;
    a->len = a->cap = 0;
}

/* Build block_stores bitmap for mem2reg_place_phis. */
static char **compute_block_stores(const CFG *cfg, const IRInstArray *insts,
                                    const int *alloca_slots, size_t num_alloca) {
    char **bs = malloc(cfg->num * sizeof(char *));
    for (size_t bi = 0; bi < cfg->num; bi++)
        bs[bi] = calloc(num_alloca, 1);
    for (size_t i = 0; i < insts->len; i++) {
        if (insts->data[i].op != IR_STORE) continue;
        for (size_t si = 0; si < num_alloca; si++) {
            if (insts->data[i].a == alloca_slots[si]) {
                for (size_t bi = 0; bi < cfg->num; bi++) {
                    if (i >= cfg->blocks[bi].start && i < cfg->blocks[bi].end) {
                        bs[bi][si] = 1;
                        break;
                    }
                }
                break;
            }
        }
    }
    return bs;
}

static void free_block_stores(char **bs, size_t num_blocks) {
    for (size_t i = 0; i < num_blocks; i++) free(bs[i]);
    free(bs);
}

/* ================================================================ */
/* Test cases                                                        */
/* ================================================================ */

/* Single alloca: ALLOCA+STORE+LOAD+RET → COPY+RET (no alloca/store/load survive) */
static void test_rename_simple(void) {
    IRInstArray insts = make_insts();
    /* ALLOCA dst=0; CONST dst=1=42; STORE a=0,b=1; LOAD dst=2,a=0; RETURN a=2 */
    PUSH(IR_ALLOCA, 0, -1, -1, 0);
    PUSH(IR_CONST,  1, -1, -1, 42);
    PUSH(IR_STORE, -1,  0,  1, 0);
    PUSH(IR_LOAD,   2,  0, -1, 0);
    PUSH(IR_RETURN, -1,  2, -1, 0);

    CFG cfg;
    cfg_build(&cfg, &insts);

    DomTree dt;
    domtree_build(&dt, &cfg);

    int alloca_slots[] = {0};
    char **block_stores = compute_block_stores(&cfg, &insts, alloca_slots, 1);

    /* φ placement — should find zero φs (single block, no merge) */
    int next_id = 3; /* v0=ALLOCA, v1=CONST, v2=LOAD */
    BlockPhiInfo *bp = mem2reg_place_phis(&cfg, &dt, alloca_slots, 1,
                                           (const char **)block_stores, &next_id);

    /* Rename */
    char *dead = NULL;
    /* Wrap in an IRFunction for mem2reg_rename */
    IRFunction fn;
    fn.name = NULL;
    fn.insts = insts;
    fn.next_value_id = next_id;
    fn.loc = (SourceLoc){NULL, 0, 0};

    mem2reg_rename(&fn, &cfg, &dt, alloca_slots, 1, bp, &dead, 0);

    /* Verify: ALLOCA is dead, STORE is dead, LOAD→COPY, RETURN lives */
    T_ASSERT(dead[0]);  /* ALLOCA dead */
    T_ASSERT(!dead[1]); /* CONST lives */
    T_ASSERT(dead[2]);  /* STORE dead */
    T_ASSERT(!dead[3]); /* former LOAD, now should be COPY */
    T_ASSERT(!dead[4]); /* RETURN lives */

    /* The LOAD at index 3 should be a COPY with a=1 (the CONST's dst) */
    T_ASSERT_EQ_INT(insts.data[3].op, IR_COPY);
    T_ASSERT_EQ_INT(insts.data[3].a, 1); /* reaching value = CONST 42 */

    /* The RETURN should still reference the LOAD's original dst (2) */
    T_ASSERT_EQ_INT(insts.data[4].op, IR_RETURN);
    T_ASSERT_EQ_INT(insts.data[4].a, 2);

    free(dead);
    block_phi_info_free(bp, cfg.num);
    free_block_stores(block_stores, cfg.num);
    domtree_free(&dt);
    cfg_free(&cfg);
    free_insts(&insts);
}

/* Two stores to same alloca: reaching value should be the latest STORE. */
static void test_rename_two_stores(void) {
    IRInstArray insts = make_insts();
    /* ALLOCA dst=0; CONST dst=1=5; STORE a=0,b=1; CONST dst=2=10;
     * STORE a=0,b=2; LOAD dst=3,a=0; RETURN a=3 */
    PUSH(IR_ALLOCA, 0, -1, -1, 0);
    PUSH(IR_CONST,  1, -1, -1, 5);
    PUSH(IR_STORE, -1,  0,  1, 0);
    PUSH(IR_CONST,  2, -1, -1, 10);
    PUSH(IR_STORE, -1,  0,  2, 0);
    PUSH(IR_LOAD,   3,  0, -1, 0);
    PUSH(IR_RETURN, -1,  3, -1, 0);

    CFG cfg;
    cfg_build(&cfg, &insts);

    DomTree dt;
    domtree_build(&dt, &cfg);

    int alloca_slots[] = {0};
    char **block_stores = compute_block_stores(&cfg, &insts, alloca_slots, 1);

    int next_id = 4; /* v0=ALLOCA, v1=5, v2=10, v3=LOAD */
    BlockPhiInfo *bp = mem2reg_place_phis(&cfg, &dt, alloca_slots, 1,
                                           (const char **)block_stores, &next_id);

    char *dead = NULL;
    IRFunction fn;
    fn.name = NULL;
    fn.insts = insts;
    fn.next_value_id = next_id;
    fn.loc = (SourceLoc){NULL, 0, 0};

    mem2reg_rename(&fn, &cfg, &dt, alloca_slots, 1, bp, &dead, 0);

    /* ALLOCA dead, both STOREs dead */
    T_ASSERT(dead[0]);
    T_ASSERT(dead[2]);
    T_ASSERT(dead[4]);

    /* LOAD at index 5 → COPY with a=2 (the second CONST = 10) */
    T_ASSERT_EQ_INT(insts.data[5].op, IR_COPY);
    T_ASSERT_EQ_INT(insts.data[5].a, 2);

    /* RETURN still references dst=3 */
    T_ASSERT_EQ_INT(insts.data[6].op, IR_RETURN);
    T_ASSERT_EQ_INT(insts.data[6].a, 3);

    free(dead);
    block_phi_info_free(bp, cfg.num);
    free_block_stores(block_stores, cfg.num);
    domtree_free(&dt);
    cfg_free(&cfg);
    free_insts(&insts);
}

/* Uninitialized read: int x; return x; → CONST 0 */
static void test_rename_undef_read(void) {
    IRInstArray insts = make_insts();
    /* ALLOCA dst=0; LOAD dst=1,a=0; RETURN a=1 */
    PUSH(IR_ALLOCA, 0, -1, -1, 0);
    PUSH(IR_LOAD,   1,  0, -1, 0);
    PUSH(IR_RETURN, -1,  1, -1, 0);

    CFG cfg;
    cfg_build(&cfg, &insts);

    DomTree dt;
    domtree_build(&dt, &cfg);

    int alloca_slots[] = {0};
    char **block_stores = compute_block_stores(&cfg, &insts, alloca_slots, 1);

    int next_id = 2; /* v0=ALLOCA, v1=LOAD */
    BlockPhiInfo *bp = mem2reg_place_phis(&cfg, &dt, alloca_slots, 1,
                                           (const char **)block_stores, &next_id);

    char *dead = NULL;
    IRFunction fn;
    fn.name = NULL;
    fn.insts = insts;
    fn.next_value_id = next_id;
    fn.loc = (SourceLoc){NULL, 0, 0};

    mem2reg_rename(&fn, &cfg, &dt, alloca_slots, 1, bp, &dead, 0);

    /* ALLOCA dead */
    T_ASSERT(dead[0]);

    /* LOAD at index 1 → CONST 0 (undef read) */
    T_ASSERT_EQ_INT(insts.data[1].op, IR_CONST);
    T_ASSERT_EQ_INT(insts.data[1].imm, 0);

    /* RETURN still references dst=1 */
    T_ASSERT_EQ_INT(insts.data[2].op, IR_RETURN);
    T_ASSERT_EQ_INT(insts.data[2].a, 1);

    free(dead);
    block_phi_info_free(bp, cfg.num);
    free_block_stores(block_stores, cfg.num);
    domtree_free(&dt);
    cfg_free(&cfg);
    free_insts(&insts);
}

/* Diamond (if-else): verify φ args are filled by rename.
 * Each branch stores a different constant; the LOAD in the merge block
 * should reference the φ result instead of the alloca slot. */
static void test_rename_diamond(void) {
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

    CFG cfg;
    cfg_build(&cfg, &insts);
    T_ASSERT_EQ_INT((int)cfg.num, 4);

    DomTree dt;
    domtree_build(&dt, &cfg);

    int alloca_slots[] = {0};
    char **block_stores = compute_block_stores(&cfg, &insts, alloca_slots, 1);

    int next_id = 5; /* v0-4 used */
    BlockPhiInfo *bp = mem2reg_place_phis(&cfg, &dt, alloca_slots, 1,
                                           (const char **)block_stores, &next_id);

    /* merge block should have one φ */
    int merge = cfg_find_label(&cfg, 30);
    T_ASSERT(merge >= 0);
    T_ASSERT_EQ_INT((int)bp[merge].num_phis, 1);

    char *dead = NULL;
    IRFunction fn;
    fn.name = NULL;
    fn.insts = insts;
    fn.next_value_id = next_id;
    fn.loc = (SourceLoc){NULL, 0, 0};

    mem2reg_rename(&fn, &cfg, &dt, alloca_slots, 1, bp, &dead, 0);

    /* The φ in merge should now have 2 args (one from each predecessor) */
    T_ASSERT_EQ_INT((int)bp[merge].phis[0].num_args, 2);

    /* Find the then-block and else-block indices */
    int thenb = cfg_find_label(&cfg, 10);
    int elseb = cfg_find_label(&cfg, 20);
    T_ASSERT(thenb >= 0);
    T_ASSERT(elseb >= 0);

    /* Verify each arg has the correct value from its predecessor */
    for (size_t ai = 0; ai < bp[merge].phis[0].num_args; ai++) {
        PhiArg *arg = &bp[merge].phis[0].args[ai];
        if (arg->pred == thenb)
            T_ASSERT_EQ_INT(arg->val, 2); /* CONST 5 */
        else if (arg->pred == elseb)
            T_ASSERT_EQ_INT(arg->val, 3); /* CONST 10 */
        else
            T_ASSERT(0); /* unexpected predecessor */
    }

    /* The LOAD at index 12 → COPY a=phi_dst */
    T_ASSERT_EQ_INT(insts.data[12].op, IR_COPY);
    T_ASSERT_EQ_INT(insts.data[12].a, bp[merge].phis[0].dst);

    /* STOREs are dead */
    T_ASSERT(dead[5]); /* then-block store */
    T_ASSERT(dead[9]); /* else-block store */

    free(dead);
    block_phi_info_free(bp, cfg.num);
    free_block_stores(block_stores, cfg.num);
    domtree_free(&dt);
    cfg_free(&cfg);
    free_insts(&insts);
}

/* ---- main ---- */

int main(void) {
    test_rename_simple();
    test_rename_two_stores();
    test_rename_undef_read();
    test_rename_diamond();
    return t_finalize();
}
