#include "fakecc/cfg.h"
#include <string.h>
#include "fakecc/domtree.h"
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

static void test_dom_single_block(void) {
    IRInstArray insts = make_insts();
    push_inst(&insts, IR_CONST, 0, -1, -1, 42);
    push_inst(&insts, IR_RETURN, -1, 0, -1, 0);

    CFG cfg;
    cfg_build(&cfg, &insts);

    DomTree dt;
    domtree_build(&dt, &cfg);

    T_ASSERT_EQ_INT((int)dt.n, 1);
    /* entry's idom is -1 (no parent) */
    T_ASSERT_EQ_INT(dt.idom[0], -1);
    /* DF is empty for single block */
    T_ASSERT_EQ_INT((int)dt.df_len[0], 0);
    /* Entry dominates itself */
    T_ASSERT(domtree_dominates(&dt, 0, 0));

    domtree_free(&dt);
    cfg_free(&cfg);
    free_insts(&insts);
}

static void test_dom_two_blocks(void) {
    /* Block 0: CONST; CONST (no terminator, fallthrough)
     * Block 1: LABEL 10; RETURN */
    IRInstArray insts = make_insts();
    push_inst(&insts, IR_CONST, 0, -1, -1, 1);
    push_inst(&insts, IR_CONST, 1, -1, -1, 2);
    push_inst(&insts, IR_LABEL, -1, -1, -1, 10);
    push_inst(&insts, IR_RETURN, -1, 0, -1, 0);

    CFG cfg;
    cfg_build(&cfg, &insts);

    DomTree dt;
    domtree_build(&dt, &cfg);

    T_ASSERT_EQ_INT((int)dt.n, 2);
    T_ASSERT_EQ_INT(dt.idom[0], -1);
    T_ASSERT_EQ_INT(dt.idom[1], 0);

    /* Block 0 dominates block 1 */
    T_ASSERT(domtree_dominates(&dt, 0, 1));
    /* Block 1 does NOT dominate block 0 */
    T_ASSERT(!domtree_dominates(&dt, 1, 0));

    domtree_free(&dt);
    cfg_free(&cfg);
    free_insts(&insts);
}

static void test_dom_if_then(void) {
    /* Simple if-then:
     *   block 0 (entry): CONST; CBR cond → true=10, false=20
     *   block 1 (then):  LABEL 10; BR → 20
     *   block 2 (merge): LABEL 20; RETURN
     *
     * In stream order (LABEL 20 must appear last so it's the "next"
     * direction):
     *   CONST; CBR(true=10,false=20)
     *   LABEL 10; BR 20
     *   LABEL 20; RETURN  */
    IRInstArray insts = make_insts();
    push_inst(&insts, IR_CONST, 0, -1, -1, 1);
    push_inst(&insts, IR_CBR, -1, 0, 20, 10);  /* true→10, false→20 */
    push_inst(&insts, IR_LABEL, -1, -1, -1, 10);
    push_inst(&insts, IR_BR, -1, -1, -1, 20);
    push_inst(&insts, IR_LABEL, -1, -1, -1, 20);
    push_inst(&insts, IR_RETURN, -1, 0, -1, 0);

    CFG cfg;
    cfg_build(&cfg, &insts);

    DomTree dt;
    domtree_build(&dt, &cfg);

    /* 3 blocks: entry, then (label 10), merge (label 20) */
    T_ASSERT_EQ_INT((int)dt.n, 3);

    /* Entry dominates everyone */
    T_ASSERT_EQ_INT(dt.idom[cfg.entry], -1);
    for (size_t i = 1; i < cfg.num; i++)
        T_ASSERT(domtree_dominates(&dt, cfg.entry, (int)i));

    /* Both the then-block and the merge are directly idom'd by entry
     * (this is an if-then: the merge has entry + then as predecessors,
     * so entry dominates the merge, and the then block is not a dominator). */
    int merge = cfg_find_label(&cfg, 20);
    T_ASSERT(merge >= 0);
    T_ASSERT_EQ_INT(dt.idom[merge], 0);

    /* DF of entry: merge should be in there (merge-before-merge semantics
     * not applicable; entry's DF contains blocks where it no longer strictly
     * dominates — actually entry always strictly dominates everything except
     * itself in a reachable CFG. DF[entry] should be empty!). */
    /* Actually in an if-then, merge has 2 preds (then + entry via CBR),
     * so DF of the then block should contain merge. */
    int thenb = cfg_find_label(&cfg, 10);
    T_ASSERT(thenb >= 0);
    /* DF[thenb] should contain merge */
    int found = 0;
    for (size_t k = 0; k < dt.df_len[thenb]; k++)
        if (dt.df[thenb][k] == merge) { found = 1; break; }
    T_ASSERT(found);

    domtree_free(&dt);
    cfg_free(&cfg);
    free_insts(&insts);
}

static void test_dom_diamond(void) {
    /* Diamond (if-else):
     *   block 0 (entry): CONST; CBR cond → true=10, false=20
     *   block 1 (then):  LABEL 10; BR → 30
     *   block 2 (else):  LABEL 20; BR → 30
     *   block 3 (merge): LABEL 30; RETURN */
    IRInstArray insts = make_insts();
    push_inst(&insts, IR_CONST, 0, -1, -1, 1);
    push_inst(&insts, IR_CBR, -1, 0, 20, 10);  /* true→10, false→20 */
    push_inst(&insts, IR_LABEL, -1, -1, -1, 10);
    push_inst(&insts, IR_BR, -1, -1, -1, 30);
    push_inst(&insts, IR_LABEL, -1, -1, -1, 20);
    push_inst(&insts, IR_BR, -1, -1, -1, 30);
    push_inst(&insts, IR_LABEL, -1, -1, -1, 30);
    push_inst(&insts, IR_RETURN, -1, 0, -1, 0);

    CFG cfg;
    cfg_build(&cfg, &insts);

    DomTree dt;
    domtree_build(&dt, &cfg);

    T_ASSERT_EQ_INT((int)dt.n, 4);

    /* idom[entry] = -1 */
    T_ASSERT_EQ_INT(dt.idom[cfg.entry], -1);

    /* idom of both branch blocks should be entry */
    int t10 = cfg_find_label(&cfg, 10);
    int t20 = cfg_find_label(&cfg, 20);
    T_ASSERT(t10 >= 0); T_ASSERT(t20 >= 0);
    T_ASSERT_EQ_INT(dt.idom[t10], cfg.entry);
    T_ASSERT_EQ_INT(dt.idom[t20], cfg.entry);

    /* idom[merge] should also be entry (entry dominates merge because
     * both branches flow to it, and entry dominates both branches) */
    int t30 = cfg_find_label(&cfg, 30);
    T_ASSERT(t30 >= 0);
    T_ASSERT_EQ_INT(dt.idom[t30], cfg.entry);

    /* Entry dominates everyone */
    for (size_t i = 1; i < cfg.num; i++)
        T_ASSERT(domtree_dominates(&dt, cfg.entry, (int)i));

    /* Neither branch dominates the merge (they only dominate themselves) */
    T_ASSERT(!domtree_dominates(&dt, t10, t30));
    T_ASSERT(!domtree_dominates(&dt, t20, t30));

    /* DF[then] contains merge, DF[else] contains merge */
    int merge_in_df_then = 0, merge_in_df_else = 0;
    for (size_t k = 0; k < dt.df_len[t10]; k++)
        if (dt.df[t10][k] == t30) merge_in_df_then = 1;
    for (size_t k = 0; k < dt.df_len[t20]; k++)
        if (dt.df[t20][k] == t30) merge_in_df_else = 1;
    T_ASSERT(merge_in_df_then);
    T_ASSERT(merge_in_df_else);

    domtree_free(&dt);
    cfg_free(&cfg);
    free_insts(&insts);
}

static void test_dom_loop(void) {
    /* Simple loop:
     *   block 0 (entry):  CONST; BR → 10     (jump to header)
     *   block 1 (header): LABEL 10; CBR cond → true=20, false=10
     *   block 2 (body):   LABEL 20; BR → 10  (back to header)
     *
     * Edges: 0→1, 1→{2, 1} (CBR true→20/label20, false→10/header),
     *        2→1
     *
     * Instruction stream:
     *   BR 10        (block 0)
     *   LABEL 10; CBR(true→20, false→10)  (block 1)
     *   LABEL 20; BR 10                    (block 2) */
    IRInstArray insts = make_insts();
    push_inst(&insts, IR_BR, -1, -1, -1, 10);

    /* Header: LABEL 10 */
    push_inst(&insts, IR_LABEL, -1, -1, -1, 10);
    push_inst(&insts, IR_CONST, 0, -1, -1, 1);
    /* CBR: true→label 20 (body), false→label 10 (header/loop exit) */
    push_inst(&insts, IR_CBR, -1, 0, 10, 20);

    /* Body: LABEL 20; BR back to 10 */
    push_inst(&insts, IR_LABEL, -1, -1, -1, 20);
    push_inst(&insts, IR_BR, -1, -1, -1, 10);

    CFG cfg;
    cfg_build(&cfg, &insts);

    DomTree dt;
    domtree_build(&dt, &cfg);

    /* 3 blocks: entry(0), header(1), body(2) */
    T_ASSERT_EQ_INT((int)dt.n, 3);

    /* Entry dominates everyone */
    for (size_t i = 1; i < cfg.num; i++)
        T_ASSERT(domtree_dominates(&dt, cfg.entry, (int)i));

    /* Header dominates the body (header is idom of body) */
    int hdr = cfg_find_label(&cfg, 10);
    int body = cfg_find_label(&cfg, 20);
    T_ASSERT(hdr >= 0); T_ASSERT(body >= 0);
    T_ASSERT(domtree_dominates(&dt, hdr, body));

    /* Body does NOT dominate header */
    T_ASSERT(!domtree_dominates(&dt, body, hdr));

    /* DF[body] or DF[entry] should contain header (the loop header has
     * a back-edge from body, so it's in DF[body]). */
    int hdr_in_df_body = 0;
    for (size_t k = 0; k < dt.df_len[body]; k++)
        if (dt.df[body][k] == hdr) { hdr_in_df_body = 1; break; }
    T_ASSERT(hdr_in_df_body);

    domtree_free(&dt);
    cfg_free(&cfg);
    free_insts(&insts);
}

/* ---- main ---- */

int main(void) {
    test_dom_single_block();
    test_dom_two_blocks();
    test_dom_if_then();
    test_dom_diamond();
    test_dom_loop();
    return t_finalize();
}
