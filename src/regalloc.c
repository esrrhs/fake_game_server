#include "fakecc/regalloc.h"
#include "fakecc/reg_arm64.h"
#include "fakecc/target.h"
#include "fakecc/cfg.h"
#include "fakecc/common.h"
#include "fakecc/ast.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/* Register-class selection per compilation target                     */
/*                                                                      */
/* The allocation algorithm below is architecture neutral; only the     */
/* register file descriptors and class membership policies differ.      */
/* ================================================================== */

typedef RaRegClass RegClass;

/* -O1 reserves R11 so the inline va_arg sequence cannot clobber a
 * mem2reg-promoted pointer that stays live across the builtin. */
static int ra_reserve_va_scratch = 0;

void ra_set_reserve_va_scratch(int on) {
    ra_reserve_va_scratch = on ? 1 : 0;
}

/* x86-64 SysV classes (the historical defaults):
 *   GP  = the 9-register allocatable set in regalloc.h
 *   XMM = xmm0..xmm13, width 32 (YMM) or 64 (ZMM with -mavx512f),
 *         with the x87 long-double exclusion.
 * When ra_reserve_va_scratch is set, R11 drops out of the GP set. */
static const RaRegClasses *ra_classes_x86(void) {
    static const int GP_NO_R11[] = {
        REG_RSI, REG_RDI,
        REG_R8,  REG_R9,  REG_R10,
        REG_RBX, REG_R12, REG_R13
    };
    static RaRegClasses c = {
        .gp = {
            ALLOCATABLE_REGS,
            REG_ALLOCATABLE,
            GP_CALLER_SAVED_MASK,
            8,
            0,
        },
        .simd = {
            XMM_ALLOCATABLE_REGS,
            REG_XMM_ALLOCATABLE,
            XMM_CALLER_SAVED_MASK,
            32,   /* updated to 64 when -mavx512f is in effect */
            1,    /* width-16 floats are x87 long doubles, not XMM */
        },
    };
    static RaRegClasses c_no_r11 = {
        .gp = {
            GP_NO_R11,
            8,
            0x1Fu, /* RSI/RDI/R8/R9/R10 */
            8,
            0,
        },
        .simd = {
            XMM_ALLOCATABLE_REGS,
            REG_XMM_ALLOCATABLE,
            XMM_CALLER_SAVED_MASK,
            32,
            1,
        },
    };
    int spill = host_has_avx512f() ? 64 : 32;
    c.simd.spill_bytes = spill;
    c_no_r11.simd.spill_bytes = spill;
    return ra_reserve_va_scratch ? &c_no_r11 : &c;
}

const RaRegClasses *ra_classes_current(void) {
    if (target_current()->arch == TARGET_ARCH_ARM64)
        return ra_classes_arm64();
    return ra_classes_x86();
}

/* True if SSA value `v` belongs to the float class in this function.
 * When the function has no per-value float metadata (value_is_float is
 * NULL or value_meta_cap is 0 — e.g. IR hand-built by tests) every value
 * is treated as GP-class. */
static int value_is_float_class(const IRFunction *fn, int v) {
    if (v < 0) return 0;
    if (!fn->value_is_float || fn->value_meta_cap <= 0) return 0;
    if (v >= fn->value_meta_cap) return 0;
    return fn->value_is_float[v];
}

/* True if SSA value `v` is a long double (TY_FLOAT width 16).  Long doubles
 * live in x87 st0 / 16-byte stack slots, NOT in the XMM register file, so
 * they must be excluded from the XMM regalloc class (they are also excluded
 * from GP, since they are float-class). */
static int value_is_ld(const IRFunction *fn, int v) {
    if (!value_is_float_class(fn, v)) return 0;
    if (!fn->value_width || fn->value_meta_cap <= 0) return 0;
    if (v >= fn->value_meta_cap) return 0;
    /* 4 = 16-byte SSE vector (one XMM, SSE+SSEUP).  Not x87 long double. */
    if (fn->value_is_float[v] == 4) return 0;
    return fn->value_width[v] == 16;
}

/* True if SSA value `v` belongs to the register class being allocated in
 * this run: GP (float_class 0) = non-float; SIMD (float_class 1) = float,
 * with width-16 long doubles excluded only when the target keeps them on a
 * special stack path (x86 x87).  Encapsulates the exclusion so a SIMD run
 * leaves such values uncolored (REG_NONE) — their home is a stack slot. */
static int value_in_class(const IRFunction *fn, int v, int float_class,
                          int exclude_16byte_ld) {
    if (float_class)
        return value_is_float_class(fn, v)
            && (!exclude_16byte_ld || !value_is_ld(fn, v));
    return !value_is_float_class(fn, v);
}

/* SSA def map: def[v] = instruction index that defines v, or -1. */
static int *ra_build_defs(const IRFunction *fn) {
    int nv = fn->next_value_id;
    int *def = xmalloc((size_t)(nv > 0 ? nv : 1) * sizeof(int));
    for (int i = 0; i < (nv > 0 ? nv : 1); i++) def[i] = -1;
    for (size_t i = 0; i < fn->insts.len; i++) {
        int d = fn->insts.data[i].dst;
        if (d >= 0 && d < nv) def[d] = (int)i;
    }
    return def;
}

static int def_op_is(const IRFunction *fn, const int *def, IRValue v, IROpcode op) {
    if (v < 0 || v >= fn->next_value_id || def[v] < 0) return 0;
    return fn->insts.data[def[v]].op == op;
}

/* ADDR or (ADDR+const)+… — the bases fold_ptr_off would turn into [rbp+off]. */
static int ra_is_stack_base(const IRFunction *fn, const int *def, IRValue v, int depth) {
    if (depth > 8 || v < 0 || v >= fn->next_value_id || def[v] < 0) return 0;
    const IRInst *d = &fn->insts.data[def[v]];
    if (d->op == IR_ADDR) return 1;
    if (d->op != IR_ADD) return 0;
    if (def_op_is(fn, def, d->b, IR_CONST))
        return ra_is_stack_base(fn, def, d->a, depth + 1);
    if (def_op_is(fn, def, d->a, IR_CONST))
        return ra_is_stack_base(fn, def, d->b, depth + 1);
    return 0;
}

static int ra_is_gaddr(const IRFunction *fn, const int *def, IRValue v) {
    return def_op_is(fn, def, v, IR_GADDR) || def_op_is(fn, def, v, IR_GADDR_TLS);
}

static int ra_ok_index(const IRFunction *fn, const int *def, IRValue v) {
    int depth = 0;
    while (v >= 0 && v < fn->next_value_id && def[v] >= 0 && depth++ < 8) {
        int op = fn->insts.data[def[v]].op;
        if (op == IR_COPY || op == IR_SEXT || op == IR_ZEXT) {
            v = fn->insts.data[def[v]].a;
            continue;
        }
        return op != IR_CONST && op != IR_ADDR && op != IR_GADDR
            && op != IR_GADDR_TLS && op != IR_FADDR;
    }
    return 0;
}

/* Extra SSA operands that must stay live at LOAD_PTR/STORE_PTR so isel
 * can emit [base+index] / [rbp+off+index] without the ADD result.  Does
 * not follow COPY (pointer selects are copies of distinct locals).
 * Only GP scalar widths — vector 16-byte loads/stores are not SIB-folded.
 * Only ADDR/GADDR bases with an integer index, matching isel. */
static int addr_fold_extras(const IRFunction *fn, const int *def, IRValue ptr,
                            int width, IRValue extra[2]) {
    int n = 0;
    if (width != 1 && width != 2 && width != 4 && width != 8) return 0;
    if (!def || ptr < 0 || ptr >= fn->next_value_id || def[ptr] < 0) return 0;
    const IRInst *d = &fn->insts.data[def[ptr]];
    if (d->op != IR_ADD) return 0;
    int a_const = def_op_is(fn, def, d->a, IR_CONST);
    int b_const = def_op_is(fn, def, d->b, IR_CONST);
    int a_stack = ra_is_stack_base(fn, def, d->a, 0);
    int b_stack = ra_is_stack_base(fn, def, d->b, 0);
    /* ADDR+const already folds to [rbp+off]; no extra live regs. */
    if ((a_stack && b_const) || (b_stack && a_const)) return 0;
    if (a_stack && !b_const && ra_ok_index(fn, def, d->b)) {
        extra[n++] = d->b;
        return n;
    }
    if (b_stack && !a_const && ra_ok_index(fn, def, d->a)) {
        extra[n++] = d->a;
        return n;
    }
    if (ra_is_gaddr(fn, def, d->a) && !b_const && ra_ok_index(fn, def, d->b)) {
        extra[n++] = d->a;
        extra[n++] = d->b;
        return n;
    }
    if (ra_is_gaddr(fn, def, d->b) && !a_const && ra_ok_index(fn, def, d->a)) {
        extra[n++] = d->b;
        extra[n++] = d->a;
        return n;
    }
    return 0;
}

/* ================================================================== */
/* Liveness analysis                                                   */
/* ================================================================== */

typedef struct {
    int def_point;         /* instruction index where defined, -1 = never */
    int *use_points;       /* sorted array of use-site instruction indices */
    size_t num_uses;
    size_t cap_uses;
    int live_start;        /* first prog point where value is live */
    int live_end;          /* last prog point where value is live */
} LiveInfo;

static void liv_init(LiveInfo *liv, int n) {
    for (int i = 0; i < n; i++) {
        liv[i].def_point = -1;
        liv[i].use_points = NULL;
        liv[i].num_uses = 0;
        liv[i].cap_uses = 0;
        liv[i].live_start = -1;
        liv[i].live_end = -1;
    }
}

static void liv_free(LiveInfo *liv, int n) {
    for (int i = 0; i < n; i++) free(liv[i].use_points);
    free(liv);
}

static void liv_add_use(LiveInfo *l, int pt) {
    if (l->num_uses >= l->cap_uses) {
        l->cap_uses = l->cap_uses ? l->cap_uses * 2 : 4;
        l->use_points = xrealloc(l->use_points, l->cap_uses * sizeof(int));
    }
    l->use_points[l->num_uses++] = pt;
}

/* Compute liveness for all SSA values in fn.
 * Returns a malloc'd LiveInfo array of length fn->next_value_id. */
static LiveInfo *compute_liveness(const IRFunction *fn) {
    int nv = fn->next_value_id;
    if (nv <= 0) return NULL;

    LiveInfo *liv = xmalloc(nv * sizeof(LiveInfo));
    liv_init(liv, nv);

    for (size_t i = 0; i < fn->insts.len; i++) {
        const IRInst *inst = &fn->insts.data[i];

        /* LABEL/BR carry no value operands.  CBR's `b` is a label id.
         * DBG_VALUE's operand is an observation, not a use. */
        if (inst->op == IR_LABEL || inst->op == IR_BR ||
            inst->op == IR_DBG_VALUE) continue;

        /* Definition */
        if (inst->dst >= 0 && inst->dst < nv) {
            liv[inst->dst].def_point = (int)i;
        }
        /* IR_CALL.b is a second return eightbyte (def), not a use. */
        if (inst->op == IR_CALL && inst->b >= 0 && inst->b < nv)
            liv[inst->b].def_point = (int)i;

        /* Uses */
        if (inst->a >= 0 && inst->a < nv) liv_add_use(&liv[inst->a], (int)i);
        if (inst->op != IR_CBR && inst->op != IR_CALL &&
            inst->b >= 0 && inst->b < nv) liv_add_use(&liv[inst->b], (int)i);

        /* IR_CALL: each call_args[k] is a use. */
        if (inst->op == IR_CALL) {
            for (int k = 0; k < inst->call_nargs; k++) {
                IRValue av = inst->call_args[k];
                if (av >= 0 && av < nv) liv_add_use(&liv[av], (int)i);
            }
        }
    }

    /* Keep ADD operands live through folded [base+index] loads/stores. */
    {
        int *def = ra_build_defs(fn);
        for (size_t i = 0; i < fn->insts.len; i++) {
            const IRInst *inst = &fn->insts.data[i];
            if (inst->op != IR_LOAD_PTR && inst->op != IR_STORE_PTR) continue;
            IRValue extra[2];
            int n = addr_fold_extras(fn, def, inst->a, inst->width, extra);
            for (int k = 0; k < n; k++) {
                if (extra[k] >= 0 && extra[k] < nv)
                    liv_add_use(&liv[extra[k]], (int)i);
            }
        }
        free(def);
    }

    /* Compute live_start / live_end for each value. */
    for (int v = 0; v < nv; v++) {
        if (liv[v].num_uses == 0) {
            /* Dead value — live only at its definition point. */
            liv[v].live_start = liv[v].def_point >= 0 ? liv[v].def_point : 0;
            liv[v].live_end   = liv[v].live_start;
        } else {
            liv[v].live_start = liv[v].def_point >= 0 ? liv[v].def_point
                                : liv[v].use_points[0];
            liv[v].live_end   = liv[v].use_points[liv[v].num_uses - 1];
        }
    }

    return liv;
}

/* ================================================================== */
/* Interference graph (chordal — SSA guarantees this property)         */
/* ================================================================== */

typedef struct {
    int *neighbors;    /* flattened adjacency for vertex v */
    size_t degree;     /* number of neighbors */
    size_t cap;        /* capacity of neighbors array */
} IGANode;

typedef struct {
    IGANode *nodes;    /* nodes[v] for v = 0..n-1 */
    int n;
} InterfGraph;

static void ig_init(InterfGraph *g, int n) {
    g->n = n;
    g->nodes = xmalloc(n * sizeof(IGANode));
    for (int i = 0; i < n; i++) {
        g->nodes[i].neighbors = NULL;
        g->nodes[i].degree = 0;
        g->nodes[i].cap = 0;
    }
}

static void ig_free(InterfGraph *g) {
    for (int i = 0; i < g->n; i++) free(g->nodes[i].neighbors);
    free(g->nodes);
    g->nodes = NULL;
    g->n = 0;
}

static void ig_add_edge(InterfGraph *g, int u, int v) {
    if (u == v) return;

    /* Add v to u's adjacency (avoid duplicates). */
    IGANode *un = &g->nodes[u];
    for (size_t i = 0; i < un->degree; i++)
        if (un->neighbors[i] == v) return;
    if (un->degree >= un->cap) {
        un->cap = un->cap ? un->cap * 2 : 8;
        un->neighbors = xrealloc(un->neighbors, un->cap * sizeof(int));
    }
    un->neighbors[un->degree++] = v;

    /* Add u to v's adjacency. */
    IGANode *vn = &g->nodes[v];
    if (vn->degree >= vn->cap) {
        vn->cap = vn->cap ? vn->cap * 2 : 8;
        vn->neighbors = xrealloc(vn->neighbors, vn->cap * sizeof(int));
    }
    vn->neighbors[vn->degree++] = u;
}

/* ================================================================== */
/* CFG-aware liveness + interference (correct across loop back edges)  */
/* ================================================================== */

/* Bit-set helpers over an nv-bit vector (packed into 64-bit words). */
typedef struct {
    uint64_t *w;         /* words[num_words] */
    int nv;
    int num_words;
} BitSet;

static void bs_init(BitSet *b, int nv) {
    b->nv = nv;
    b->num_words = (nv + 63) / 64;
    b->w = xmalloc((size_t)b->num_words * sizeof(uint64_t));
    memset(b->w, 0, (size_t)b->num_words * sizeof(uint64_t));
}

static void bs_free(BitSet *b) {
    free(b->w);
    b->w = NULL;
}

static void bs_clear(BitSet *b) {
    memset(b->w, 0, (size_t)b->num_words * sizeof(uint64_t));
}

static int bs_test(const BitSet *b, int v) {
    return (int)((b->w[v >> 6] >> (v & 63)) & 1);
}

static void bs_set(BitSet *b, int v) {
    b->w[v >> 6] |= ((uint64_t)1 << (v & 63));
}

static void bs_clr(BitSet *b, int v) {
    b->w[v >> 6] &= ~((uint64_t)1 << (v & 63));
}

/* dst |= src. Returns 1 if dst changed. */
static int bs_or_changed(BitSet *dst, const BitSet *src) {
    int changed = 0;
    for (int i = 0; i < dst->num_words; i++) {
        uint64_t before = dst->w[i];
        uint64_t after = before | src->w[i];
        if (after != before) changed = 1;
        dst->w[i] = after;
    }
    return changed;
}

/* dst = src. */
static void bs_copy(BitSet *dst, const BitSet *src) {
    memcpy(dst->w, src->w, (size_t)dst->num_words * sizeof(uint64_t));
}

/* Iterate set bits in increasing vertex id.  A ctzll scan is equivalent
 * when ctzll is correct, but stage1's __fakecc_ctzll helper has disagreed
 * with gcc's builtin on large graphs and scrambled register coloring. */
#define BS_FOREACH(bs, v) \
    for (int v = 0; v < (bs)->nv; v++) if (bs_test((bs), v))

/* Compute per-block use[b] and def[b] sets.
 *
 * use[b]  = values read in b before being (re)defined in b
 * def[b]  = values written anywhere in b
 *
 * Note: for a value that is both defined and used in b (e.g., x = x + 1),
 * we treat the use as "before def" iff the read appears strictly before
 * the write in the block's instruction order — the standard dataflow
 * definition of upwards-exposed uses. */
static void compute_use_def(const IRFunction *fn, const CFG *cfg,
                            const int *def, BitSet *use_b, BitSet *def_b) {
    for (size_t bi = 0; bi < cfg->num; bi++) {
        bs_clear(&use_b[bi]);
        bs_clear(&def_b[bi]);
        const CFGBlock *blk = &cfg->blocks[bi];
        for (size_t i = blk->start; i < blk->end; i++) {
            const IRInst *inst = &fn->insts.data[i];
            /* LABEL/BR/DBG_VALUE carry no register operands. */
        if (inst->op == IR_LABEL || inst->op == IR_BR ||
            inst->op == IR_DBG_VALUE) continue;

            /* Uses come before def within a single instruction. */
            if (inst->a >= 0 && inst->a < use_b[bi].nv) {
                if (!bs_test(&def_b[bi], inst->a))
                    bs_set(&use_b[bi], inst->a);
            }
            if (inst->op != IR_CBR && inst->op != IR_CALL &&
                inst->b >= 0 && inst->b < use_b[bi].nv) {
                if (!bs_test(&def_b[bi], inst->b))
                    bs_set(&use_b[bi], inst->b);
            }
            if (inst->op == IR_CALL) {
                for (int k = 0; k < inst->call_nargs; k++) {
                    IRValue av = inst->call_args[k];
                    if (av >= 0 && av < use_b[bi].nv &&
                        !bs_test(&def_b[bi], av))
                        bs_set(&use_b[bi], av);
                }
            }
            if (inst->op == IR_LOAD_PTR || inst->op == IR_STORE_PTR) {
                IRValue extra[2];
                int w = inst->width;
                if (w == 1 || w == 2 || w == 4 || w == 8) {
                    int n = addr_fold_extras(fn, def, inst->a, inst->width, extra);
                    for (int k = 0; k < n; k++) {
                        IRValue v = extra[k];
                        if (v >= 0 && v < use_b[bi].nv && !bs_test(&def_b[bi], v))
                            bs_set(&use_b[bi], v);
                    }
                }
            }
            if (inst->dst >= 0 && inst->dst < def_b[bi].nv) {
                bs_set(&def_b[bi], inst->dst);
            }
            /* IR_CALL.b: second return eightbyte. */
            if (inst->op == IR_CALL && inst->b >= 0 &&
                inst->b < def_b[bi].nv)
                bs_set(&def_b[bi], inst->b);
        }
    }
}

/* Fixed-point compute in[b] / out[b]:
 *   in[b]  = use[b] ∪ (out[b] \ def[b])
 *   out[b] = ⋃_{s ∈ succs(b)} in[s]
 *
 * Iterate blocks in reverse order until nothing changes.  This handles
 * loops naturally: the back edge carries values from body's out to head's in. */
static void compute_live_in_out(const CFG *cfg,
                                const BitSet *use_b, const BitSet *def_b,
                                BitSet *in_b, BitSet *out_b) {
    int changed = 1;
    BitSet tmp;
    bs_init(&tmp, use_b[0].nv);

    while (changed) {
        changed = 0;
        /* Process in reverse block order — decent starting heuristic for
         * forward CFGs since terminators come last. */
        for (int bi = (int)cfg->num - 1; bi >= 0; bi--) {
            const CFGBlock *blk = &cfg->blocks[bi];

            /* out[b] = union of in[s] for each succ s */
            bs_clear(&out_b[bi]);
            for (size_t si = 0; si < blk->num_succs; si++) {
                bs_or_changed(&out_b[bi], &in_b[blk->succs[si]]);
            }

            /* in[b] = use[b] ∪ (out[b] \ def[b]) */
            bs_copy(&tmp, &out_b[bi]);
            /* tmp = out \ def */
            for (int wi = 0; wi < tmp.num_words; wi++) {
                tmp.w[wi] &= ~def_b[bi].w[wi];
            }
            /* tmp |= use */
            for (int wi = 0; wi < tmp.num_words; wi++) {
                tmp.w[wi] |= use_b[bi].w[wi];
            }

            /* Did in[bi] change? */
            for (int wi = 0; wi < tmp.num_words; wi++) {
                if (tmp.w[wi] != in_b[bi].w[wi]) {
                    changed = 1;
                    break;
                }
            }
            bs_copy(&in_b[bi], &tmp);
        }
    }
    bs_free(&tmp);
}

/* Build interference graph by walking each block backwards, maintaining
 * the live-set.  At each instruction, dst interferes with every value
 * currently live-out (i.e., in `live` before removing dst).  Then remove
 * dst, add uses.
 *
 * `forbid_mask[v]` (output, k bits wide): OR of color indices this value
 * must NOT be colored to.  Currently set only for values live across an
 * IR_CALL — which cannot occupy a caller-saved register. */
static void build_interf_graph_cfg(const IRFunction *fn, const CFG *cfg,
                                    int nv, InterfGraph *g,
                                    uint32_t *forbid_mask,
                                    int float_class,
                                    const RegClass *cls) {
    ig_init(g, nv);
    for (int v = 0; v < nv; v++) forbid_mask[v] = 0;

    int *ssa_def = ra_build_defs(fn);

    BitSet *use_b = xmalloc(cfg->num * sizeof(BitSet));
    BitSet *def_b = xmalloc(cfg->num * sizeof(BitSet));
    BitSet *in_b  = xmalloc(cfg->num * sizeof(BitSet));
    BitSet *out_b = xmalloc(cfg->num * sizeof(BitSet));
    for (size_t bi = 0; bi < cfg->num; bi++) {
        bs_init(&use_b[bi], nv);
        bs_init(&def_b[bi], nv);
        bs_init(&in_b[bi], nv);
        bs_init(&out_b[bi], nv);
    }

    compute_use_def(fn, cfg, ssa_def, use_b, def_b);
    compute_live_in_out(cfg, use_b, def_b, in_b, out_b);

    BitSet live;
    bs_init(&live, nv);

    /* Pass 1: Walk instructions backwards in each block to collect initial
     * IR_CALL forbid_mask constraints for values live across calls. */
    for (size_t bi = 0; bi < cfg->num; bi++) {
        const CFGBlock *blk = &cfg->blocks[bi];
        bs_copy(&live, &out_b[bi]);

        for (size_t i = blk->end; i > blk->start; i--) {
            const IRInst *inst = &fn->insts.data[i - 1];
            /* LABEL/BR/DBG_VALUE carry no register operands. */
        if (inst->op == IR_LABEL || inst->op == IR_BR ||
            inst->op == IR_DBG_VALUE) continue;

            if (inst->op == IR_CALL) {
                BS_FOREACH(&live, over) {
                    if ((int)over != inst->dst && (int)over != inst->b &&
                        value_in_class(fn, (int)over, float_class, cls->exclude_16byte_ld))
                        forbid_mask[over] |= cls->caller_saved;
                }
            }

            if (inst->dst >= 0 && inst->dst < nv &&
                value_in_class(fn, inst->dst, float_class, cls->exclude_16byte_ld)) {
                if (inst->op != IR_COPY)
                    bs_clr(&live, inst->dst);
            }
            if (inst->op == IR_CALL && inst->b >= 0 && inst->b < nv &&
                value_in_class(fn, inst->b, float_class, cls->exclude_16byte_ld))
                bs_clr(&live, inst->b);

            if (inst->a >= 0 && inst->a < nv &&
                value_in_class(fn, inst->a, float_class, cls->exclude_16byte_ld))
                bs_set(&live, inst->a);
            if (inst->op != IR_CBR && inst->op != IR_CALL &&
                inst->b >= 0 && inst->b < nv &&
                value_in_class(fn, inst->b, float_class, cls->exclude_16byte_ld))
                bs_set(&live, inst->b);
            if (inst->op == IR_CALL) {
                for (int k = 0; k < inst->call_nargs; k++) {
                    IRValue av = inst->call_args[k];
                    if (av >= 0 && av < nv &&
                        value_in_class(fn, av, float_class, cls->exclude_16byte_ld))
                        bs_set(&live, av);
                }
            }
            if (inst->op == IR_LOAD_PTR || inst->op == IR_STORE_PTR) {
                IRValue extra[2];
                int n = addr_fold_extras(fn, ssa_def, inst->a, inst->width, extra);
                for (int k = 0; k < n; k++) {
                    IRValue v = extra[k];
                    if (v >= 0 && v < nv && value_in_class(fn, v, float_class, cls->exclude_16byte_ld))
                        bs_set(&live, v);
                }
            }
        }
    }

    /* Pass 1b: Propagate forbid_mask through IR_COPY (phi-elim merges).
     * Only when -O1 reserved the va_arg scratch: that is the path where
     * mem2reg emits loop-carried COPYs that must share call-survival
     * constraints.  Skipping at -O0 keeps that code stream unchanged. */
    if (ra_reserve_va_scratch) {
        int changed = 1;
        while (changed) {
            changed = 0;
            for (size_t i = 0; i < fn->insts.len; i++) {
                const IRInst *inst = &fn->insts.data[i];
                if (inst->op != IR_COPY) continue;
                int d = inst->dst, a = inst->a;
                if (d < 0 || d >= nv || a < 0 || a >= nv) continue;
                if (!value_in_class(fn, d, float_class, cls->exclude_16byte_ld))
                    continue;
                if (!value_in_class(fn, a, float_class, cls->exclude_16byte_ld))
                    continue;
                uint32_t m = forbid_mask[d] | forbid_mask[a];
                if (forbid_mask[d] != m) { forbid_mask[d] = m; changed = 1; }
                if (forbid_mask[a] != m) { forbid_mask[a] = m; changed = 1; }
            }
        }
    }

    /* Pass 2: Build interference graph by walking instructions backwards. */
    for (size_t bi = 0; bi < cfg->num; bi++) {
        const CFGBlock *blk = &cfg->blocks[bi];
        bs_copy(&live, &out_b[bi]);

        for (size_t i = blk->end; i > blk->start; i--) {
            const IRInst *inst = &fn->insts.data[i - 1];
            /* LABEL/BR/DBG_VALUE carry no register operands. */
        if (inst->op == IR_LABEL || inst->op == IR_BR ||
            inst->op == IR_DBG_VALUE) continue;

            if (inst->dst >= 0 && inst->dst < nv &&
                value_in_class(fn, inst->dst, float_class, cls->exclude_16byte_ld)) {
                BS_FOREACH(&live, other) {
                    if (!value_in_class(fn, (int)other, float_class, cls->exclude_16byte_ld))
                        continue;
                    if ((int)other != inst->dst) {
                        ig_add_edge(g, inst->dst, (int)other);
                    }
                }
                if (inst->op != IR_COPY)
                    bs_clr(&live, inst->dst);
            }
            if (inst->op == IR_CALL && inst->b >= 0 && inst->b < nv &&
                value_in_class(fn, inst->b, float_class, cls->exclude_16byte_ld)) {
                BS_FOREACH(&live, other) {
                    if (!value_in_class(fn, (int)other, float_class, cls->exclude_16byte_ld))
                        continue;
                    if ((int)other != inst->b)
                        ig_add_edge(g, inst->b, (int)other);
                }
                bs_clr(&live, inst->b);
            }

            if (inst->a >= 0 && inst->a < nv &&
                value_in_class(fn, inst->a, float_class, cls->exclude_16byte_ld))
                bs_set(&live, inst->a);
            if (inst->op != IR_CBR && inst->op != IR_CALL &&
                inst->b >= 0 && inst->b < nv &&
                value_in_class(fn, inst->b, float_class, cls->exclude_16byte_ld))
                bs_set(&live, inst->b);
            if (inst->op == IR_CALL) {
                for (int k = 0; k < inst->call_nargs; k++) {
                    IRValue av = inst->call_args[k];
                    if (av >= 0 && av < nv &&
                        value_in_class(fn, av, float_class, cls->exclude_16byte_ld))
                        bs_set(&live, av);
                }
            }
            if (inst->op == IR_LOAD_PTR || inst->op == IR_STORE_PTR) {
                IRValue extra[2];
                int n = addr_fold_extras(fn, ssa_def, inst->a, inst->width, extra);
                for (int k = 0; k < n; k++) {
                    IRValue v = extra[k];
                    if (v >= 0 && v < nv && value_in_class(fn, v, float_class, cls->exclude_16byte_ld))
                        bs_set(&live, v);
                }
            }
        }
    }

    /* Pass 2b (arm64 GP only): all integer arguments arrive simultaneously in
     * x0..x7 and the prologue routes each incoming register to its home.  The
     * normal backward walk only connects a parameter's SSA value to what it
     * meets inside the body, so two parameters whose short live ranges never
     * overlap there could be given the SAME home (e.g. an arg resident in x2
     * and another routed x1->x2); the prologue then overwrites the resident
     * value before the body reads it.  Register-passed parameters therefore
     * pairwise interfere: their homes must be distinct so the entry shuffle is
     * an injective permutation it can realise.  Stack parameters are not in a
     * register and are left out. */
    if (!float_class && target_current()->arch == TARGET_ARCH_ARM64) {
        int gp[8], ngp = 0;
        for (size_t i = 0; i < (size_t)nv && i < fn->insts.len && ngp < 8
                       && fn->insts.data[i].op == IR_PARAM; i++) {
            IRValue pv = fn->insts.data[i].dst;
            /* Any two integer arguments not passed on the stack arrive in two
             * distinct x registers, so their homes must differ too.  The GP
             * class already excludes float/vector params. */
            if (!fn->insts.data[i].force_stack && pv >= 0 && pv < nv &&
                value_in_class(fn, pv, 0, cls->exclude_16byte_ld))
                gp[ngp++] = pv;
        }
        for (int a = 0; a < ngp; a++)
            for (int b = a + 1; b < ngp; b++)
                ig_add_edge(g, gp[a], gp[b]);
    }

    bs_free(&live);
    for (size_t bi = 0; bi < cfg->num; bi++) {
        bs_free(&use_b[bi]);
        bs_free(&def_b[bi]);
        bs_free(&in_b[bi]);
        bs_free(&out_b[bi]);
    }
    free(use_b); free(def_b); free(in_b); free(out_b);
    free(ssa_def);
}


/* ================================================================== */
/* MCS — Maximum Cardinality Search                                    */
/*                                                                      */
/* For chordal graphs, the REVERSE of the MCS order is a Perfect        */
/* Elimination Ordering.  We return the order in which vertices should  */
/* be COLORED (i.e., reverse MCS = PEO).                                */
/* ================================================================== */

static int *compute_mcs_order(const InterfGraph *g) {
    int n = g->n;
    int *order = xmalloc(n * sizeof(int));
    int *weight = xmalloc(n * sizeof(int));
    int *picked = xmalloc(n * sizeof(int));

    memset(weight, 0, n * sizeof(int));
    memset(picked, 0, n * sizeof(int));

    /* Bucket queue: weights are in [0, n), so we keep an array of buckets
     * indexed by weight.  Each bucket is a linked list of vertices that
     * have EVER had that weight.  A vertex may appear in multiple buckets
     * (stale entries) when its weight increases; we skip those at pick
     * time by checking weight[v] == current_bucket.  This makes "pick max
     * weight" O(1) amortized — overall O(n + m) instead of O(n²), which
     * matters for large functions (e.g. 20151204.c's 32KB array init has
     * ~143K virtual registers, where O(n²) would be ~2×10^10 ops). */
    int *head = xmalloc((n + 1) * sizeof(int));  /* head[w] = first vertex, -1 if empty */
    int *lnk  = xmalloc(n * sizeof(int));          /* lnk[v] = next vertex in bucket */
    int *in_bucket = xmalloc(n * sizeof(int));     /* in_bucket[v] = weight bucket v is linked from */
    for (int w = 0; w <= n; w++) head[w] = -1;
    for (int v = 0; v < n; v++) { lnk[v] = -1; in_bucket[v] = -1; }
    /* Initially all vertices have weight 0 → put them in bucket 0. */
    for (int v = 0; v < n; v++) {
        lnk[v] = head[0];
        head[0] = v;
        in_bucket[v] = 0;
    }
    int max_w = 0;  /* current highest non-empty bucket */

    /* Build order from last-coloring to first-coloring.
     * pos goes n-1 → 0, storing vertices to be colored last → first. */
    for (int pos = n - 1; pos >= 0; pos--) {
        /* Find the highest bucket with a valid (unpicked, current-weight) vertex. */
        int best;
        for (;;) {
            while (max_w > 0 && head[max_w] < 0) max_w--;
            if (head[max_w] < 0) {
                /* Should not happen, but fall back to linear scan. */
                best = -1;
                for (int v = 0; v < n; v++) {
                    if (!picked[v]) { best = v; break; }
                }
                break;
            }
            best = head[max_w];
            head[max_w] = lnk[best];  /* remove from front of bucket */
            /* Valid if unpicked AND still in this bucket (weight matches). */
            if (!picked[best] && in_bucket[best] == max_w) {
                break;
            }
            /* Otherwise stale — continue scanning. */
        }

        picked[best] = 1;
        /* "best" was the highest-weight vertex → it goes late in coloring order.
         * We place it at position pos, which goes from n-1 down to 0.
         * So order[0] = first to color (last MCS pick), order[n-1] = last to color. */
        order[pos] = best;

        /* Increment weights of unpicked neighbors. */
        for (size_t j = 0; j < g->nodes[best].degree; j++) {
            int w = g->nodes[best].neighbors[j];
            if (!picked[w]) {
                int nw = ++weight[w];
                /* Add w to front of bucket[nw].  We do NOT remove it from
                 * its old bucket — that entry becomes stale and is skipped
                 * at pick time via the in_bucket check. */
                lnk[w] = head[nw];
                head[nw] = w;
                in_bucket[w] = nw;
                if (nw > max_w) max_w = nw;
            }
        }
    }

    free(head);
    free(lnk);
    free(in_bucket);
    free(weight);
    free(picked);
    return order;
}

/* ================================================================== */
/* Loop-nesting depth (used for spill-cost estimation)                  */
/* ================================================================== */

/* Walk up the dominator tree from a block to compute a simple loop-depth
 * heuristic: a backedge in the CFG means the target block is a loop header,
 * and every block dominated by it is inside the loop.  For this minimal
 * allocator we just check whether any edge from b goes to a dominator of b
 * (a backedge), which implies b is inside a loop. */
static int estimate_loop_depth(int block_id, const CFG *cfg) {
    int depth = 0;
    /* Simple heuristic: if a block has a successor that strictly dominates it,
     * it is inside a loop.  Each such level adds 1 to depth. */
    const CFGBlock *b = &cfg->blocks[block_id];
    for (size_t s = 0; s < b->num_succs; s++) {
        int succ = b->succs[s];
        for (size_t p = 0; p < cfg->blocks[succ].num_preds; p++) {
            /* Check if succ is also a predecessor of another block that
             * reaches back to block_id → backedge heuristic */
            if (succ <= block_id) { depth++; break; }
        }
    }
    return depth > 0 ? 1 : 0;  /* Binary: in-loop or not-in-loop */
}

/* Find the CFG block containing instruction at index inst_idx. */
static int find_block_for_inst(const CFG *cfg, int inst_idx) {
    for (size_t bi = 0; bi < cfg->num; bi++) {
        if ((size_t)inst_idx >= cfg->blocks[bi].start &&
            (size_t)inst_idx < cfg->blocks[bi].end)
            return (int)bi;
    }
    return 0;
}

/* ================================================================== */
/* Spill-cost estimation                                               */
/* ================================================================== */

static int compute_spill_cost(int v, const LiveInfo *liv, const CFG *cfg) {
    const LiveInfo *l = &liv[v];
    int cost = 0;
    for (size_t i = 0; i < l->num_uses; i++) {
        int blk = find_block_for_inst(cfg, l->use_points[i]);
        cost += 1 + estimate_loop_depth(blk, cfg) * 10;
    }
    /* Add cost at def point too. */
    if (l->def_point >= 0) {
        int blk = find_block_for_inst(cfg, l->def_point);
        cost += 1 + estimate_loop_depth(blk, cfg) * 10;
    }
    /* Prevent zero cost (even unused values need some cost). */
    return cost > 0 ? cost : 1;
}

/* ================================================================== */
/* Greedy coloring with spill heuristic                                */
/* ================================================================== */

static void greedy_color(const InterfGraph *g, const int *order,
                         const LiveInfo *liv, const CFG *cfg,
                         const uint32_t *forbid_mask,
                         int *colors, int *spill_slots, int *num_spills,
                         const RegClass *cls) {
    int n = g->n;
    int k = cls->nregs;
    if (k > 32) {
        /* Color masks are 32 bits wide. */
        fprintf(stderr, "fakecc: register class has %d regs (max 32)\n", k);
        exit(1);
    }

    /* Track spill costs for eviction decisions. */
    int *spill_cost = xmalloc(n * sizeof(int));
    for (int v = 0; v < n; v++)
        spill_cost[v] = compute_spill_cost(v, liv, cfg);

    /* color = -1 means "not yet assigned / spilled pending decision"
     * We use -2 to mean "spilled" */
    for (int v = 0; v < n; v++) colors[v] = -1;

    int next_spill = 0;

    for (int i = 0; i < n; i++) {
        int v = order[i];
        if (g->nodes[v].degree == 0 && liv[v].num_uses == 0 && liv[v].def_point < 0) {
            /* Unused value — don't waste a register. */
            colors[v] = -2;
            spill_slots[v] = next_spill++;
            continue;
        }

        /* Find which colors are used by already-colored neighbors, plus
         * any colors this value has been forbidden from. */
        uint32_t used = forbid_mask[v];
        for (size_t j = 0; j < g->nodes[v].degree; j++) {
            int w = g->nodes[v].neighbors[j];
            if (colors[w] >= 0 && colors[w] < k)
                used |= (1u << colors[w]);
        }

        /* Find first free register. */
        int c;
        for (c = 0; c < k; c++)
            if (!(used & (1 << c))) break;

        if (c < k) {
            colors[v] = c;
        } else {
            /* All k registers are in use by neighbors (including those `v` is
             * forbidden from).  Among neighbors, prefer to evict one whose
             * register `v` is actually allowed to take — i.e. a color NOT set
             * in forbid_mask[v] (a callee-saved reg for a value live across a
             * call).  Evicting a neighbor only to hand `v` a forbidden
             * caller-saved reg would clobber `v` at the next call, so that
             * victim is useless; fall through to spilling `v` instead.
             *
             * IMPORTANT: we must ensure the victim's color is NOT also held
             * by another neighbor — otherwise evicting one neighbor still
             * leaves v in conflict with the other.  Only consider colors
             * that appear exactly once among colored, non-forbidden neighbors. */
            int color_count[32] = {0};
            for (size_t j = 0; j < g->nodes[v].degree; j++) {
                int w = g->nodes[v].neighbors[j];
                if (colors[w] >= 0 && colors[w] < k &&
                    !(forbid_mask[v] & (1u << colors[w])))
                    color_count[colors[w]]++;
            }
            int victim = -1, victim_cost = 0x7fffffff;
            for (size_t j = 0; j < g->nodes[v].degree; j++) {
                int w = g->nodes[v].neighbors[j];
                if (colors[w] < 0 || colors[w] >= k) continue;
                if (forbid_mask[v] & (1u << colors[w])) continue; /* v can't use it */
                if (color_count[colors[w]] != 1) continue; /* shared — unsafe */
                if (spill_cost[w] < victim_cost) {
                    victim = w;
                    victim_cost = spill_cost[w];
                }
            }

            if (victim >= 0 && victim_cost < spill_cost[v]) {
                /* Evict victim, give its register to v. */
                colors[v] = colors[victim];
                colors[victim] = -2;
                spill_slots[victim] = next_spill++;
            } else {
                /* Spill v itself. */
                colors[v] = -2;
                spill_slots[v] = next_spill++;
            }
        }
    }

    *num_spills = next_spill;
    free(spill_cost);
}

/* ================================================================== */
/* Coalescing — identifies but does NOT modify redundant COPYs.
 * Codegen uses the RAResult to skip COPYs where src and dst share
 * the same register. */
static void coalesce_copies(IRFunction *fn, int *colors) {
    (void)fn;
    (void)colors;
    /* No IR modification.  Codegen checks ra->reg[dst] == ra->reg[a]
     * for IR_COPY instructions and skips them when coalesced. */
}

/* ================================================================== */
/* Shared per-class allocation                                         */
/* ================================================================== */

/* Run the full allocation pipeline for one register class.  Values not
 * belonging to `float_class` are ignored (they belong to the other
 * class's run).  Returns NULL when the function has no values of the
 * target class — codegen treats a NULL result as "nothing of this class
 * to allocate". */
static RAResult *ra_alloc_class(const IRFunction *fn, int float_class,
                                const RegClass *cls) {
    int nv = fn->next_value_id;
    if (nv <= 0) return NULL;

    /* Short-circuit: if the function has no values of this class, don't
     * bother building a graph.  Keeps ra_xmm NULL for int-only code. */
    {
        int any = 0;
        for (int v = 0; v < nv; v++) {
            if (value_in_class(fn, v, float_class, cls->exclude_16byte_ld)) { any = 1; break; }
        }
        if (!any) return NULL;
    }

    LiveInfo *liv = compute_liveness(fn);
    if (!liv) return NULL;

    /* Build CFG (for spill-cost loop-depth heuristic AND CFG-aware liveness). */
    CFG cfg;
    cfg_build(&cfg, &fn->insts);

    /* Build interference graph using CFG-aware backward walk — restricted
     * to values of the target class.  Correct across loop back edges. */
    InterfGraph g;
    uint32_t *forbid_mask = xmalloc(nv * sizeof(uint32_t));
    build_interf_graph_cfg(fn, &cfg, nv, &g, forbid_mask,
                            float_class, cls);

    /* Compute MCS ordering (reverse = PEO for chordal graph). */
    int *order = compute_mcs_order(&g);

    /* Greedy coloring (color = index into cls->regs). */
    int *colors = xmalloc(nv * sizeof(int));
    int *spill_slots = xmalloc(nv * sizeof(int));
    memset(spill_slots, 0, nv * sizeof(int));
    int num_spills = 0;
    greedy_color(&g, order, liv, &cfg, forbid_mask,
                 colors, spill_slots, &num_spills, cls);
    free(forbid_mask);

    /* Map color indices (0..cls->nregs-1) to the backend's native register
     * codes (x86 ModRM codes or A64 register numbers).  Values not in this
     * class get -1 (REG_NONE == A64_*_NONE), their real home being the
     * other class's result or a stack slot. */
    for (int v = 0; v < nv; v++) {
        if (!value_in_class(fn, v, float_class, cls->exclude_16byte_ld)) {
            colors[v] = REG_NONE;
        } else if (colors[v] >= 0 && colors[v] < cls->nregs) {
            colors[v] = cls->regs[colors[v]];
        } else {
            /* Spilled: keep color as-is (negative), spill_slot valid. */
        }
    }

    /* Coalesce COPYs with same-register src/dst. */
    coalesce_copies((IRFunction *)fn, colors);

    /* Build result. */
    RAResult *ra = xmalloc(sizeof(RAResult));
    ra->reg = colors;
    ra->spill_slot = spill_slots;
    ra->num_spill_slots = num_spills;
    ra->num_values = nv;

    /* SIMD slots are one class-width each (32/64 bytes on x86 with YMM/ZMM,
     * 16 bytes for an arm64 Q register); GP slots are 8 bytes and kept in
     * 16-byte pairs so the frame stays ABI aligned. */
    if (float_class)
        ra->stack_size = cls->spill_bytes * num_spills;
    else {
        int slots = num_spills;
        if (slots % 2 != 0) slots++;
        ra->stack_size = cls->spill_bytes * slots;
    }

    /* Cleanup. */
    free(order);
    ig_free(&g);
    liv_free(liv, nv);
    cfg_free(&cfg);

    return ra;
}

/* ================================================================== */
/* Top-level entry points                                               */
/* ================================================================== */

RAResult *reg_alloc(const IRFunction *fn) {
    const RaRegClasses *cls = ra_classes_current();
    return ra_alloc_class(fn, 0, &cls->gp);
}

RAResult *reg_alloc_xmm(const IRFunction *fn) {
    const RaRegClasses *cls = ra_classes_current();
    return ra_alloc_class(fn, 1, &cls->simd);
}

void ra_result_free(RAResult *ra) {
    if (!ra) return;
    free(ra->reg);
    free(ra->spill_slot);
    free(ra);
}
