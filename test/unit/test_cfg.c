#include "fakecc/cfg.h"
#include <string.h>
#include "test_framework.h"

#include <stdlib.h>

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

static void free_insts(IRInstArray *a) {
    free(a->data);
    a->data = NULL;
    a->len = a->cap = 0;
}

/* ---- tests ---- */

static void test_cfg_single_block(void) {
    IRInstArray insts = make_insts();
    /* CONST v0=42; RETURN v0 */
    push_inst(&insts, IR_CONST, 0, -1, -1, 42);
    push_inst(&insts, IR_RETURN, -1, 0, -1, 0);

    CFG cfg;
    cfg_build(&cfg, &insts);

    T_ASSERT_EQ_INT((int)cfg.num, 1);
    T_ASSERT_EQ_INT(cfg.entry, 0);
    T_ASSERT_EQ_INT((int)cfg.blocks[0].num_preds, 0);
    T_ASSERT_EQ_INT((int)cfg.blocks[0].num_succs, 0);
    T_ASSERT_EQ_INT((int)cfg.blocks[0].start, 0);
    T_ASSERT_EQ_INT((int)cfg.blocks[0].end, 2);
    T_ASSERT_EQ_INT(cfg.blocks[0].id, 0);
    T_ASSERT_EQ_INT(cfg.blocks[0].label, -1);

    /* RPO: single block → rpo[0] = 0 */
    int *rpo = cfg_rpo(&cfg);
    T_ASSERT(rpo != NULL);
    T_ASSERT_EQ_INT(rpo[0], 0);
    free(rpo);

    cfg_free(&cfg);
    free_insts(&insts);
}

static void test_cfg_two_blocks_fallthrough(void) {
    IRInstArray insts = make_insts();
    /* Block 0: CONST; CONST (no terminator → falls through) */
    push_inst(&insts, IR_CONST, 0, -1, -1, 1);
    push_inst(&insts, IR_CONST, 1, -1, -1, 2);
    /* Label 10 starts block 1 */
    push_inst(&insts, IR_LABEL, -1, -1, -1, 10);
    push_inst(&insts, IR_RETURN, -1, 0, -1, 0);

    CFG cfg;
    cfg_build(&cfg, &insts);

    T_ASSERT_EQ_INT((int)cfg.num, 2);
    T_ASSERT_EQ_INT(cfg.blocks[0].label, -1);
    T_ASSERT_EQ_INT(cfg.blocks[1].label, 10);

    /* Block 0 falls through to block 1 */
    T_ASSERT_EQ_INT((int)cfg.blocks[0].num_succs, 1);
    T_ASSERT_EQ_INT(cfg.blocks[0].succs[0], 1);
    T_ASSERT_EQ_INT((int)cfg.blocks[1].num_preds, 1);
    T_ASSERT_EQ_INT(cfg.blocks[1].preds[0], 0);

    /* Block 1 has RETURN → no successors */
    T_ASSERT_EQ_INT((int)cfg.blocks[1].num_succs, 0);

    cfg_free(&cfg);
    free_insts(&insts);
}

static void test_cfg_label_and_br(void) {
    IRInstArray insts = make_insts();
    /* Block 0 (entry): BR to label 10 */
    push_inst(&insts, IR_BR, -1, -1, -1, 10);
    /* Block 1: LABEL 20; RETURN */
    push_inst(&insts, IR_LABEL, -1, -1, -1, 20);
    push_inst(&insts, IR_RETURN, -1, 0, -1, 0);
    /* Block 2: LABEL 10; RETURN */
    push_inst(&insts, IR_LABEL, -1, -1, -1, 10);
    push_inst(&insts, IR_RETURN, -1, 1, -1, 0);

    CFG cfg;
    cfg_build(&cfg, &insts);

    /* 3 blocks: entry, label 20, label 10 */
    T_ASSERT_EQ_INT((int)cfg.num, 3);

    /* Block 0 branches to label 10 */
    int target = cfg_find_label(&cfg, 10);
    T_ASSERT(target >= 0);
    T_ASSERT_EQ_INT((int)cfg.blocks[0].num_succs, 1);
    T_ASSERT_EQ_INT(cfg.blocks[0].succs[0], target);
    T_ASSERT_EQ_INT((int)cfg.blocks[target].num_preds, 1);
    T_ASSERT_EQ_INT(cfg.blocks[target].preds[0], 0);

    /* Block 0 should NOT have a fallthrough edge to block 1 (BR is unconditional) */
    for (size_t i = 0; i < cfg.blocks[0].num_succs; i++)
        T_ASSERT(cfg.blocks[0].succs[i] != 1);

    cfg_free(&cfg);
    free_insts(&insts);
}

static void test_cfg_cbr_diamond(void) {
    IRInstArray insts = make_insts();
    /* A diamond CFG:
     *   block 0: CONST; CBR (cond, true→label 10, false→label 20)
     *   block 1: LABEL 10; BR → label 30
     *   block 2: LABEL 20; BR → label 30
     *   block 3: LABEL 30; RETURN
     *
     * Order in stream: LABEL 20 first, then LABEL 10 (so block order
     * differs from label order — stresses cfg_find_label). */
    push_inst(&insts, IR_CONST, 0, -1, -1, 1);
    /* CBR: a=cond(v0), imm=true_label=10, b=false_label=20 */
    push_inst(&insts, IR_CBR, -1, 0, 20, 10);
    push_inst(&insts, IR_LABEL, -1, -1, -1, 20);   /* false path first in stream */
    push_inst(&insts, IR_BR, -1, -1, -1, 30);
    push_inst(&insts, IR_LABEL, -1, -1, -1, 10);   /* true path second */
    push_inst(&insts, IR_BR, -1, -1, -1, 30);
    push_inst(&insts, IR_LABEL, -1, -1, -1, 30);   /* merge */
    push_inst(&insts, IR_RETURN, -1, 0, -1, 0);

    CFG cfg;
    cfg_build(&cfg, &insts);

    /* 4 blocks: entry, label20, label10, label30 */
    T_ASSERT_EQ_INT((int)cfg.num, 4);

    /* Block 0 has 2 successors (CBR) */
    T_ASSERT_EQ_INT((int)cfg.blocks[0].num_succs, 2);

    /* Verify both targets exist and have block 0 as predecessor */
    int t10 = cfg_find_label(&cfg, 10);
    int t20 = cfg_find_label(&cfg, 20);
    int t30 = cfg_find_label(&cfg, 30);
    T_ASSERT(t10 >= 0);
    T_ASSERT(t20 >= 0);
    T_ASSERT(t30 >= 0);

    /* The two CBR targets each have block 0 as a predecessor */
    int t10_has_pred0 = 0, t20_has_pred0 = 0;
    for (size_t i = 0; i < cfg.blocks[t10].num_preds; i++)
        if (cfg.blocks[t10].preds[i] == 0) t10_has_pred0 = 1;
    for (size_t i = 0; i < cfg.blocks[t20].num_preds; i++)
        if (cfg.blocks[t20].preds[i] == 0) t20_has_pred0 = 1;
    T_ASSERT(t10_has_pred0);
    T_ASSERT(t20_has_pred0);

    /* Both branches jump to merge */
    T_ASSERT_EQ_INT((int)cfg.blocks[t10].num_succs, 1);
    T_ASSERT_EQ_INT(cfg.blocks[t10].succs[0], t30);
    T_ASSERT_EQ_INT((int)cfg.blocks[t20].num_succs, 1);
    T_ASSERT_EQ_INT(cfg.blocks[t20].succs[0], t30);

    /* Merge has 2 predecessors */
    T_ASSERT_EQ_INT((int)cfg.blocks[t30].num_preds, 2);

    cfg_free(&cfg);
    free_insts(&insts);
}

static void test_cfg_rpo_order(void) {
    IRInstArray insts = make_insts();
    /* Simple linear chain: 3 blocks
     *   block 0: CONST; CBR → true=10, false=20
     *   block 1: LABEL 10; BR → 30
     *   block 2: LABEL 20; BR → 30
     *   block 3: LABEL 30; RETURN
     * Same diamond shape as above. */
    push_inst(&insts, IR_CONST, 0, -1, -1, 1);
    push_inst(&insts, IR_CBR, -1, 0, 20, 10);
    push_inst(&insts, IR_LABEL, -1, -1, -1, 10);
    push_inst(&insts, IR_BR, -1, -1, -1, 30);
    push_inst(&insts, IR_LABEL, -1, -1, -1, 20);
    push_inst(&insts, IR_BR, -1, -1, -1, 30);
    push_inst(&insts, IR_LABEL, -1, -1, -1, 30);
    push_inst(&insts, IR_RETURN, -1, 0, -1, 0);

    CFG cfg;
    cfg_build(&cfg, &insts);

    int *rpo = cfg_rpo(&cfg);
    T_ASSERT(rpo != NULL);

    /* Entry block must have RPO 0 (smallest) */
    T_ASSERT_EQ_INT(rpo[cfg.entry], 0);

    /* Every block reachable from entry must have RPO >= 0 */
    for (size_t i = 0; i < cfg.num; i++)
        T_ASSERT(rpo[i] >= 0);

    /* For each edge u→v where v has a single predecessor (so it's not a
     * merge node), rpo[u] should be < rpo[v] for forward edges.  For the
     * diamond: entry→{t10,t20}, t10→t30, t20→t30.
     * Entry→children must be forward. */
    for (size_t i = 0; i < cfg.blocks[cfg.entry].num_succs; i++) {
        int s = cfg.blocks[cfg.entry].succs[i];
        T_ASSERT(rpo[s] > rpo[cfg.entry]);
    }

    free(rpo);
    cfg_free(&cfg);
    free_insts(&insts);
}

static void test_cfg_empty_function(void) {
    IRInstArray insts = make_insts();

    CFG cfg;
    cfg_build(&cfg, &insts);

    /* Empty instruction array should produce 1 empty block */
    T_ASSERT_EQ_INT((int)cfg.num, 1);
    T_ASSERT_EQ_INT(cfg.entry, 0);
    T_ASSERT_EQ_INT((int)cfg.blocks[0].num_preds, 0);
    T_ASSERT_EQ_INT((int)cfg.blocks[0].num_succs, 0);
    T_ASSERT_EQ_INT((int)cfg.blocks[0].start, 0);
    T_ASSERT_EQ_INT((int)cfg.blocks[0].end, 0);

    /* RPO on empty (single empty block) */
    int *rpo = cfg_rpo(&cfg);
    T_ASSERT(rpo != NULL);
    T_ASSERT_EQ_INT(rpo[0], 0);
    free(rpo);

    cfg_free(&cfg);
    free_insts(&insts);
}

static void test_cfg_find_label_not_found(void) {
    IRInstArray insts = make_insts();
    push_inst(&insts, IR_CONST, 0, -1, -1, 1);
    push_inst(&insts, IR_RETURN, -1, 0, -1, 0);

    CFG cfg;
    cfg_build(&cfg, &insts);

    T_ASSERT_EQ_INT(cfg_find_label(&cfg, 99), -1);
    T_ASSERT_EQ_INT(cfg_find_label(&cfg, 0), -1);

    cfg_free(&cfg);
    free_insts(&insts);
}

/* ---- main ---- */

int main(void) {
    test_cfg_single_block();
    test_cfg_empty_function();
    test_cfg_two_blocks_fallthrough();
    test_cfg_label_and_br();
    test_cfg_cbr_diamond();
    test_cfg_rpo_order();
    test_cfg_find_label_not_found();
    return t_finalize();
}
