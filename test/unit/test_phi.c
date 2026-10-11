#include "fakecc/cfg.h"
#include "fakecc/domtree.h"
#include "fakecc/phi.h"
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

/* Build the block_stores bitmap: block_stores[bi][si] = has store?
 * Returns a char** array of cfg->num pointers, each malloc'd num_alloca bytes. */
static char **compute_block_stores(const CFG *cfg, const IRInstArray *insts,
                                    const int *alloca_slots, size_t num_alloca) {
    char **bs = malloc(cfg->num * sizeof(char *));
    for (size_t bi = 0; bi < cfg->num; bi++) {
        bs[bi] = calloc(num_alloca, 1);
    }
    for (size_t i = 0; i < insts->len; i++) {
        if (insts->data[i].op != IR_STORE) continue;
        /* Find which alloca slot this store targets. */
        for (size_t si = 0; si < num_alloca; si++) {
            if (insts->data[i].a == alloca_slots[si]) {
                /* Find which block contains this instruction. */
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

/* Count total φ nodes across all blocks. */
static size_t count_total_phis(const BlockPhiInfo *bp, size_t num_blocks) {
    size_t n = 0;
    for (size_t i = 0; i < num_blocks; i++) n += bp[i].num_phis;
    return n;
}

/* Count how many φ nodes in a given block are for a given alloca slot. */
static size_t block_phis_for_slot(const BlockPhiInfo *bp, int block_id, int slot) {
    size_t n = 0;
    for (size_t i = 0; i < bp[block_id].num_phis; i++)
        if (bp[block_id].phis[i].alloca_slot == slot) n++;
    return n;
}

/* ---- test cases ---- */

/* No STORE instructions → zero φ nodes. */
static void test_phi_no_store(void) {
    IRInstArray insts = make_insts();
    /* ALLOCA dst=0; CONST dst=1=42; RETURN a=1 */
    PUSH(IR_ALLOCA, 0, -1, -1, 0);
    PUSH(IR_CONST,  1, -1, -1, 42);
    PUSH(IR_RETURN, -1,  1, -1, 0);

    CFG cfg;
    cfg_build(&cfg, &insts);

    DomTree dt;
    domtree_build(&dt, &cfg);

    int alloca_slots[] = {0};
    char **block_stores = compute_block_stores(&cfg, &insts, alloca_slots, 1);

    int next_id = 2; /* ALLOCA=0, CONST=1 already used */
    BlockPhiInfo *bp = mem2reg_place_phis(&cfg, &dt, alloca_slots, 1,
                                           (const char **)block_stores, &next_id);

    T_ASSERT_EQ_INT((int)count_total_phis(bp, cfg.num), 0);

    block_phi_info_free(bp, cfg.num);
    free_block_stores(block_stores, cfg.num);
    domtree_free(&dt);
    cfg_free(&cfg);
    free_insts(&insts);
}

/* Single STORE in a single block — no merge point, zero φ nodes. */
static void test_phi_single_store(void) {
    IRInstArray insts = make_insts();
    /* ALLOCA dst=0; CONST dst=1=5; STORE a=0,b=1; LOAD dst=2,a=0; RETURN a=2 */
    PUSH(IR_ALLOCA, 0, -1, -1, 0);
    PUSH(IR_CONST,  1, -1, -1, 5);
    PUSH(IR_STORE, -1,  0,  1, 0);
    PUSH(IR_LOAD,   2,  0, -1, 0);
    PUSH(IR_RETURN, -1,  2, -1, 0);

    CFG cfg;
    cfg_build(&cfg, &insts);

    DomTree dt;
    domtree_build(&dt, &cfg);

    int alloca_slots[] = {0};
    char **block_stores = compute_block_stores(&cfg, &insts, alloca_slots, 1);

    int next_id = 3; /* 0=ALLOCA, 1=CONST, 2=LOAD */
    BlockPhiInfo *bp = mem2reg_place_phis(&cfg, &dt, alloca_slots, 1,
                                           (const char **)block_stores, &next_id);

    T_ASSERT_EQ_INT((int)count_total_phis(bp, cfg.num), 0);

    block_phi_info_free(bp, cfg.num);
    free_block_stores(block_stores, cfg.num);
    domtree_free(&dt);
    cfg_free(&cfg);
    free_insts(&insts);
}

/* Diamond (if-else): two branches each store, merge block gets one φ. */
static void test_phi_diamond(void) {
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
    PUSH(IR_CBR,   -1,  1, 20, 10);  /* true→10, false→20 */

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

    /* Verify we have 4 blocks */
    T_ASSERT_EQ_INT((int)cfg.num, 4);

    DomTree dt;
    domtree_build(&dt, &cfg);

    /* Verify merge is in DF of both branch blocks */
    int merge = cfg_find_label(&cfg, 30);
    T_ASSERT(merge >= 0);

    int thenb = cfg_find_label(&cfg, 10);
    T_ASSERT(thenb >= 0);
    int then_df_has_merge = 0;
    for (size_t k = 0; k < dt.df_len[thenb]; k++)
        if (dt.df[thenb][k] == merge) { then_df_has_merge = 1; break; }
    T_ASSERT(then_df_has_merge);

    int elseb = cfg_find_label(&cfg, 20);
    T_ASSERT(elseb >= 0);
    int else_df_has_merge = 0;
    for (size_t k = 0; k < dt.df_len[elseb]; k++)
        if (dt.df[elseb][k] == merge) { else_df_has_merge = 1; break; }
    T_ASSERT(else_df_has_merge);

    /* Place φ nodes */
    int alloca_slots[] = {0};
    char **block_stores = compute_block_stores(&cfg, &insts, alloca_slots, 1);

    int next_id = 5; /* 0-4 used */
    BlockPhiInfo *bp = mem2reg_place_phis(&cfg, &dt, alloca_slots, 1,
                                           (const char **)block_stores, &next_id);

    /* Should have exactly 1 φ in the merge block */
    T_ASSERT_EQ_INT((int)count_total_phis(bp, cfg.num), 1);
    T_ASSERT_EQ_INT((int)block_phis_for_slot(bp, merge, 0), 1);
    T_ASSERT_EQ_INT(bp[merge].phis[0].alloca_slot, 0);
    /* φ result should have a new SSA value */
    T_ASSERT(bp[merge].phis[0].dst >= 5);

    block_phi_info_free(bp, cfg.num);
    free_block_stores(block_stores, cfg.num);
    domtree_free(&dt);
    cfg_free(&cfg);
    free_insts(&insts);
}

/* Nested: outer if-then-else with inner if-then-else in the then-branch.
 * The store is in a deeply nested block; φ should propagate through
 * intermediate dominance frontiers up to the final merge. */
static void test_phi_nested(void) {
    IRInstArray insts = make_insts();
    /* Block 0: ALLOCA; CONST(cond); CBR → 10, 20
     * Block 1 (label 10, outer-then): STORE; CONST(cond); CBR → 30, 40
     * Block 2 (label 20, outer-else): CONST(cond); CBR → 30, 40
     * Block 3 (label 30, inner-then): BR → 50
     * Block 4 (label 40, inner-else): BR → 50
     * Block 5 (label 50, merge): RETURN
     *
     * The STORE in block 1 means DF[1] = {3, 4} → φ placed in 3 and 4.
     * Then DF[3] = {5} and DF[4] = {5} → φ also placed in 5. */
    PUSH(IR_ALLOCA, 0, -1, -1, 0);
    PUSH(IR_CONST,  1, -1, -1, 1);       /* v1 = 1 (cond) */
    PUSH(IR_CBR,   -1,  1, 20, 10);      /* if v1 →10 else →20 */

    /* Block 1: label 10 */
    PUSH(IR_LABEL, -1, -1, -1, 10);
    PUSH(IR_CONST,  2, -1, -1, 42);      /* v2 = 42 */
    PUSH(IR_STORE, -1,  0,  2, 0);       /* store v2 → slot 0 */
    PUSH(IR_CONST,  3, -1, -1, 1);       /* v3 = 1 (cond) */
    PUSH(IR_CBR,   -1,  3, 40, 30);      /* if v3 →30 else →40 */

    /* Block 2: label 20 */
    PUSH(IR_LABEL, -1, -1, -1, 20);
    PUSH(IR_CONST,  4, -1, -1, 1);       /* v4 = 1 (cond) */
    PUSH(IR_CBR,   -1,  4, 40, 30);      /* if v4 →30 else →40 */

    /* Block 3: label 30 */
    PUSH(IR_LABEL, -1, -1, -1, 30);
    PUSH(IR_BR,    -1, -1, -1, 50);

    /* Block 4: label 40 */
    PUSH(IR_LABEL, -1, -1, -1, 40);
    PUSH(IR_BR,    -1, -1, -1, 50);

    /* Block 5: label 50 (merge) */
    PUSH(IR_LABEL, -1, -1, -1, 50);
    PUSH(IR_LOAD,   5,  0, -1, 0);        /* v5 = load slot 0 */
    PUSH(IR_RETURN, -1,  5, -1, 0);

    CFG cfg;
    cfg_build(&cfg, &insts);
    T_ASSERT_EQ_INT((int)cfg.num, 6);

    DomTree dt;
    domtree_build(&dt, &cfg);

    int alloca_slots[] = {0};
    char **block_stores = compute_block_stores(&cfg, &insts, alloca_slots, 1);

    /* Verify the store is in block 1 (label 10) */
    int thenb = cfg_find_label(&cfg, 10);
    T_ASSERT(thenb >= 0);
    T_ASSERT(block_stores[thenb][0]);

    int next_id = 6; /* v0..v5 used */
    BlockPhiInfo *bp = mem2reg_place_phis(&cfg, &dt, alloca_slots, 1,
                                           (const char **)block_stores, &next_id);

    /* We expect φ nodes at:
     *   - blocks 3 and 4 (inner merge targets, DF of block 1)
     *   - block 5 (outer merge, DF of blocks 3 and 4)
     * Total: at least 2 (in 3,4) and at least 1 in 5 */
    T_ASSERT((int)count_total_phis(bp, cfg.num) >= 2);

    int inner_then = cfg_find_label(&cfg, 30);
    int inner_else = cfg_find_label(&cfg, 40);
    int merge5 = cfg_find_label(&cfg, 50);
    T_ASSERT(inner_then >= 0);
    T_ASSERT(inner_else >= 0);
    T_ASSERT(merge5 >= 0);

    T_ASSERT((int)block_phis_for_slot(bp, merge5, 0) >= 1);

    block_phi_info_free(bp, cfg.num);
    free_block_stores(block_stores, cfg.num);
    domtree_free(&dt);
    cfg_free(&cfg);
    free_insts(&insts);
}

/* Two alloca variables each stored in different branches of a diamond.
 * The merge block should receive a φ for each variable. */
static void test_phi_multiple_alloca(void) {
    IRInstArray insts = make_insts();
    /* Block 0: ALLOCA dst=0; ALLOCA dst=1; CONST(cond); CBR →10,20
     * Block 1 (label 10): STORE a=0,b=42; STORE a=1,b=99; BR →30
     * Block 2 (label 20): CONST 5; STORE a=0,b=5; CONST 7; STORE a=1,b=7; BR →30
     * Block 3 (label 30, merge): RETURN */
    PUSH(IR_ALLOCA, 0, -1, -1, 0);
    PUSH(IR_ALLOCA, 1, -1, -1, 0);
    PUSH(IR_CONST,  2, -1, -1, 1);       /* v2 = 1 (cond) */
    PUSH(IR_CBR,   -1,  2, 20, 10);      /* CBR true→10, false→20 */

    /* Block 1: then */
    PUSH(IR_LABEL, -1, -1, -1, 10);
    PUSH(IR_CONST,  3, -1, -1, 42);      /* v3 = 42 */
    PUSH(IR_STORE, -1,  0,  3, 0);       /* slot 0 ← 42 */
    PUSH(IR_CONST,  4, -1, -1, 99);      /* v4 = 99 */
    PUSH(IR_STORE, -1,  1,  4, 0);       /* slot 1 ← 99 */
    PUSH(IR_BR,    -1, -1, -1, 30);

    /* Block 2: else */
    PUSH(IR_LABEL, -1, -1, -1, 20);
    PUSH(IR_CONST,  5, -1, -1, 5);       /* v5 = 5 */
    PUSH(IR_STORE, -1,  0,  5, 0);       /* slot 0 ← 5 */
    PUSH(IR_CONST,  6, -1, -1, 7);       /* v6 = 7 */
    PUSH(IR_STORE, -1,  1,  6, 0);       /* slot 1 ← 7 */
    PUSH(IR_BR,    -1, -1, -1, 30);

    /* Block 3: merge */
    PUSH(IR_LABEL, -1, -1, -1, 30);
    PUSH(IR_RETURN, -1,  0, -1, 0);

    CFG cfg;
    cfg_build(&cfg, &insts);

    DomTree dt;
    domtree_build(&dt, &cfg);

    int alloca_slots[] = {0, 1};
    char **block_stores = compute_block_stores(&cfg, &insts, alloca_slots, 2);

    /* Verify both blocks have stores for both slots */
    int thenb = cfg_find_label(&cfg, 10);
    int elseb = cfg_find_label(&cfg, 20);
    T_ASSERT(thenb >= 0);
    T_ASSERT(elseb >= 0);
    T_ASSERT(block_stores[thenb][0]);
    T_ASSERT(block_stores[thenb][1]);
    T_ASSERT(block_stores[elseb][0]);
    T_ASSERT(block_stores[elseb][1]);

    int next_id = 7; /* v0..v6 used */
    BlockPhiInfo *bp = mem2reg_place_phis(&cfg, &dt, alloca_slots, 2,
                                           (const char **)block_stores, &next_id);

    /* Merge block should have 2 φ nodes (one per alloca) */
    int merge = cfg_find_label(&cfg, 30);
    T_ASSERT(merge >= 0);
    T_ASSERT_EQ_INT((int)count_total_phis(bp, cfg.num), 2);
    T_ASSERT_EQ_INT((int)block_phis_for_slot(bp, merge, 0), 1);
    T_ASSERT_EQ_INT((int)block_phis_for_slot(bp, merge, 1), 1);

    /* The two φ results should have different SSA values */
    int phi0_dst = bp[merge].phis[0].dst;
    int phi1_dst = bp[merge].phis[1].dst;
    T_ASSERT(phi0_dst != phi1_dst);

    block_phi_info_free(bp, cfg.num);
    free_block_stores(block_stores, cfg.num);
    domtree_free(&dt);
    cfg_free(&cfg);
    free_insts(&insts);
}

/* ---- main ---- */

int main(void) {
    test_phi_no_store();
    test_phi_single_store();
    test_phi_diamond();
    test_phi_nested();
    test_phi_multiple_alloca();
    return t_finalize();
}
