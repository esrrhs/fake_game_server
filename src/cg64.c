#include "fakecc/cg64.h"
#include "fakecc/a64.h"
#include "fakecc/macho.h"
#include "fakecc/reg_arm64.h"
#include "fakecc/common.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/* arm64 (Darwin AAPCS64) instruction selector.                        */
/*                                                                      */
/* Frame:  [fp+0]    saved x29/x30                                      */
/*         [fp+16..] callee-saved register pair saves                  */
/*         [fp-...]  locals: pinned allocas, GP spills, call area      */
/* x16/x17 (IP0/IP1) are scratch; the allocator never assigns them.   */
/* ================================================================== */

#define SCR0 A64_X16
#define SCR1 A64_X17

/* GNU __builtin_apply_args / __builtin_apply block, AAPCS64 register file.
 * Only self-consistency is required -- both ends are fakecc -- but the shape
 * mirrors gcc's arm64 apply_args_size(): the outgoing stack arguments sit at
 * the front, then the integer registers, then the SIMD ones.
 *
 *   [0,  64)  outgoing stack arguments, copied to the callee frame on apply
 *   [64, 128)  x0..x7
 *   [128,256)  v0..v7, 16 bytes each
 *
 * The result block repeats the layout and adds a scratch tail for the three
 * operands of __builtin_apply (which may themselves live in x0-x7) plus the
 * saved sp.  APPLY_STACK_MAX bounds the `size` an apply can carry on the
 * stack; the copy below is unrolled to exactly that many bytes. */
#define APPLY_STACK_MAX   64
#define APPLY_GP_OFF      64
#define APPLY_V_OFF       128
#define APPLY_BLOCK_SIZE  256
/* APPLY_SCRATCH_OFF is the byte offset of that tail *inside* the block.
 * APPLY_RES_SCRATCH is the block's total size; using it as the scratch
 * offset lands one past the end of the block, which is exactly where the
 * apply_args block sits in the frame -- the stashed function pointer was
 * written over the outgoing stack arguments and then read back as the call
 * target. */
#define APPLY_SCRATCH_OFF  APPLY_BLOCK_SIZE
#define APPLY_RES_SCRATCH (APPLY_BLOCK_SIZE + 32)


typedef struct {
    A64Asm           *as;
    const IRModule   *ir;
    const IRFunction *fn;
    const RAResult   *ra;
    const RAResult   *ra_fp;     /* SIMD homes for scalar float/double */
    int              *def;         /* SSA value -> defining inst index  */
    int              *alloca_off;  /* ALLOCA SSA id -> fp-relative off  */
    int              *alloca_aln;  /* ALLOCA SSA id -> alignment (8..64) */
    int              *spill_off;   /* spill slot -> fp-relative offset  */
    int              *fp_spill_off; /* SIMD spill slot -> fp-relative    */
    int              *vlabels;     /* IR label id -> a64 label id       */
    int              *fn_label;    /* function index -> a64 label id    */
    int               frame_locals;
    int               call_area;
    int               save_total;  /* fp/lr + callee-saved saves bytes  */
    int               vararg_off;  /* fp offset of the first anonymous arg */
    /* fp-relative slot holding the SP a dynamic alloca left behind, or -1 when
     * the function does not allocate dynamically.  emit_call lowers SP past the
     * outgoing area before a branch and restores it afterwards, so neither the
     * stack arguments nor the callee's frame land on the allocated block.
     * IR_DYN_ALLOCA rewrites the slot rather than the call site snapshotting
     * SP: a loop can alloca more than once and SP keeps moving down. */
    int               dyn_sp_off;
    /* GNU __builtin_apply_args / __builtin_apply blocks, fp-relative; 0 when
     * this function does not use them. */
    int               apply_args_off;
    int               apply_result_off;
    int               epilog_label; /* function's epilogue, for __builtin_return */
    /* Module globals: section (G_RO/G_DATA/G_BSS) and in-section offset. */
    int              *gsect;
    size_t           *goff;
    /* ADRP+ADD pairs targeting globals; patched once the final section
     * placement (which depends on the total text length) is known. */
    struct GFix { uint32_t at; int gidx; const char *name; int value; int addend; int got; int tlvp; } *gfix;
    size_t            ngfix, capgfix;
    /* First gfix entry belonging to the function being compiled.  IR value
     * ids are allocated per function and collide across functions, so
     * fold_page_addend may only match entries inside this range; entries of
     * earlier functions stay queued because their relocs are emitted once
     * every function has been compiled. */
    size_t            fn_gfix_start;
    /* Pointer slots inside global initializers.  Resolved into dyld
     * rebases once every section base is known. */
    struct PFix {
        int         gidx;
        int         slot_off;
        const char *sym;
        const char *sub;
        int         addend;
    }                *pfix;
    size_t            npfix, cappfix;
    /* Local copy of runtime/int128.c's restoring division.  The arm64
     * image does not link the Linux runtime, and external BL is T14. */
    int               udiv_label;
    /* Exec-mode TLV descriptors live in __data.  -1 means this global
     * has no descriptor.  The image has one template, not a copy per thread. */
    int              *tlv_off;
    int               tlv_thunk;
    /* Exec-mode slot in __bss that holds the process envp.  -1 in an object. */
    int               environ_off;
    /* Object-file calls to functions that are not in this TU.
     * Weak calls stay a BRANCH26 so a missing target can return 0.
     * Any other external call goes through one local stub per symbol. */
    struct XCall { uint32_t at; const char *name; } *xcall;
    size_t            nxcall, capxcall;
    struct Stub { const char *name; int lab; } *stub;
    size_t            nstub, capstub;
    /* &&label sites.  Object files cannot bake the address: the linker
     * prepends an entry stub, which changes the page offset. */
    struct LRel { int a64lab; char *name; } *lrel;
    size_t            nlrel, caplrel;
} C64;

enum { G_RO = 1, G_DATA = 2, G_BSS = 3, G_COMMON = 4 };

static const char *note_label_sym(C64 *c, int ir_id, int a64lab) {
    const char *fn = (c->fn && c->fn->name) ? c->fn->name : "fn";
    size_t n = strlen(fn) + 24;
    char *buf = xmalloc(n);
    snprintf(buf, n, ".L%s_%d", fn, ir_id);
    for (size_t i = 0; i < c->nlrel; i++) {
        if (strcmp(c->lrel[i].name, buf) == 0) {
            free(buf);
            return c->lrel[i].name;
        }
    }
    if (c->nlrel == c->caplrel) {
        c->caplrel = c->caplrel ? c->caplrel * 2 : 8;
        c->lrel = realloc(c->lrel, c->caplrel * sizeof *c->lrel);
        if (!c->lrel) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
    }
    c->lrel[c->nlrel].a64lab = a64lab;
    c->lrel[c->nlrel].name = buf;
    c->nlrel++;
    return buf;
}

static void note_page_reloc(C64 *c, uint32_t at, int gidx, const char *name,
                            int value) {
    if (c->ngfix == c->capgfix) {
        c->capgfix = c->capgfix ? c->capgfix * 2 : 64;
        c->gfix = realloc(c->gfix, c->capgfix * sizeof *c->gfix);
        if (!c->gfix) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
    }
    c->gfix[c->ngfix].at = at;
    c->gfix[c->ngfix].gidx = gidx;
    c->gfix[c->ngfix].name = name;
    c->gfix[c->ngfix].value = value;
    c->gfix[c->ngfix].addend = 0;
    c->gfix[c->ngfix].got = 0;
    c->gfix[c->ngfix].tlvp = 0;
    c->ngfix++;
}

/* ADRP+ADD of the exec-mode envp slot.  Patched with the other page relocs. */
static void emit_environ_addr(C64 *c, int rd) {
    A64Asm *a = c->as;
    note_page_reloc(c, (uint32_t)a->code.len, -2, NULL, -1);
    a64_word(a, 0x90000000u | (uint32_t)(rd & 31));
    a64_word(a, 0x91000000u | ((uint32_t)(rd & 31) << 5) | (uint32_t)(rd & 31));
}

/* Symbols the arm64 backend knows no definition for, and which a program is
 * allowed to do without.  runtime/thread.c is built around Linux's clone(),
 * futex() and friends; there is no Darwin equivalent to lower them to, so
 * rather than fail the whole link -- which would take printf and every other
 * runtime service down with it -- resolve them to address 0 and let the
 * caller observe the failure.  pthread_create then returns an error instead
 * of the program being unbuildable.
 *
 * This is a stand-in until the runtime grows a real Darwin thread layer; it
 * is deliberately narrow, so a genuine missing symbol still fails loudly. */
static const char *const c64_absent_on_darwin[] = {
    "__clone",
    "__clone3",
};

static int c64_is_weak_ref(const IRModule *ir, const char *name) {
    if (!name) return 0;
    for (size_t i = 0; i < sizeof c64_absent_on_darwin
                            / sizeof *c64_absent_on_darwin; i++)
        if (strcmp(c64_absent_on_darwin[i], name) == 0) return 1;
    if (!ir) return 0;
    for (size_t i = 0; i < ir->n_weak_refs; i++)
        if (ir->weak_refs[i] && strcmp(ir->weak_refs[i], name) == 0)
            return 1;
    return 0;
}

static int c64_stub_label(C64 *c, const char *name) {
    for (size_t i = 0; i < c->nstub; i++)
        if (c->stub[i].name && strcmp(c->stub[i].name, name) == 0)
            return c->stub[i].lab;
    if (c->nstub == c->capstub) {
        c->capstub = c->capstub ? c->capstub * 2 : 4;
        c->stub = xrealloc(c->stub, c->capstub * sizeof *c->stub);
    }
    c->stub[c->nstub].name = name;
    c->stub[c->nstub].lab = a64_new_label(c->as);
    return c->stub[c->nstub++].lab;
}

static int c64_undef_sym(EmitModule *out, const IRModule *ir, const char *name) {
    int si = emit_module_find_symbol(out, name);
    if (si >= 0) return si;
    if (c64_is_weak_ref(ir, name))
        return emit_module_add_symbol(out, name, 2, 0, (uint16_t)SECT_UNDEF, 0, 0);
    return emit_module_add_undefined(out, name);
}

static void c64_die(C64 *c, const IRInst *s, const char *what) {
    const char *f = c->fn->loc.file ? c->fn->loc.file : "<arm64>";
    int line = s ? s->loc.line : c->fn->loc.line;
    die_at(f, line, s ? s->loc.col : 0,
           "arm64 backend: %s not supported yet", what);
}

/* ── Value locations ──────────────────────────────────────────────── */

static int vw(C64 *c, IRValue v) {
    if (v >= 0 && c->fn->value_width &&
        v < c->fn->next_value_id && c->fn->value_width[v])
        return c->fn->value_width[v];
    return 8;
}

/* The allocator stores NATIVE hardware register codes in ra->reg[]
 * (regalloc maps palette colors through a64_gp_allocatable once; see
 * test_regalloc's A64_X19..A64_X28 assertion).  Indexing the palette
 * table a second time here used to be a no-op only for low colors and
 * ran out of bounds (into the adjacent SIMD table) for colors >= 20,
 * aliasing distinct values onto x0..x4 and misclassifying saves. */
static int home_reg(C64 *c, IRValue v) {
    if (!c->ra || v < 0 || v >= c->ra->num_values) return -1;
    int hw = c->ra->reg[v];
    return hw >= 0 ? hw : -1;
}

/* A negative reg[] is not by itself "spilled": REG_NONE also marks a value
 * that belongs to the *other* register class, whose home is that class's
 * result rather than a slot here.  Only a value the allocator gave a real
 * spill slot has one, so ask the slot, not the register. */
static int has_spill_slot(C64 *c, IRValue v) {
    if (!c->ra || v < 0 || v >= c->ra->num_values) return 0;
    int slot = c->ra->spill_slot[v];
    if (slot < 0 || slot >= c->ra->num_spill_slots) return 0;
    return c->spill_off != NULL;
}

static int spill_off(C64 *c, IRValue v) {
    if (!has_spill_slot(c, v)) return 0;
    return c->spill_off[c->ra->spill_slot[v]];
}

/* Forward declarations: frame/base memory accessors (defined below). */
static void emit_load(C64 *c, int rt, int base, int width, int uns);
static void emit_store(C64 *c, int rt, int base, int width);
static void frame_load(C64 *c, int rt, int off, int width, int uns, int aln);
static void frame_store(C64 *c, int valrt, int off, int width, int aln);
static void emit_fp_addr(C64 *c, int rd, int off);
static void emit_fp_addr_aligned(C64 *c, int rd, int off, int aln);
static int alloca_align_of(const C64 *c, IRValue slot, int nv);
static void emit_mov_imm_w(A64Asm *a, int rd, int64_t imm, int is64);
static void fmov_q(A64Asm *a, int rd, int rn);
static void str_q(A64Asm *a, int rt, int rn, int byte_off);
static void ldr_q(A64Asm *a, int rt, int rn, int byte_off);

/* Destination register for v: its home, or SCR0 when spilled. */
static int dst_reg(C64 *c, IRValue v) {
    int r = home_reg(c, v);
    return r >= 0 ? r : SCR0;
}

static void commit(C64 *c, IRValue v, int srcreg) {
    if (home_reg(c, v) >= 0) return;
    frame_store(c, srcreg, spill_off(c, v), 8, 0);
}

/* IEEE scalar float/double (value_is_float == 1, width 4 or 8).
 * long double, decimal and NEON vectors stay out of this path. */
static int scalar_fp_val(const C64 *c, IRValue v) {
    const IRFunction *fn = c->fn;
    if (!fn || v < 0 || !fn->value_is_float || v >= fn->value_meta_cap)
        return 0;
    if (fn->value_is_float[v] != 1) return 0;
    int w = (fn->value_width && v < fn->next_value_id && fn->value_width[v])
            ? fn->value_width[v] : 8;
    return w == 4 || w == 8;
}

/* Darwin vector_size(16): one Q register (value_is_float == 4, width 16). */
static int vec16_val(const C64 *c, IRValue v) {
    const IRFunction *fn = c->fn;
    if (!fn || v < 0 || !fn->value_is_float || v >= fn->value_meta_cap)
        return 0;
    if (fn->value_is_float[v] != 4) return 0;
    int w = (fn->value_width && v < fn->next_value_id) ? fn->value_width[v] : 0;
    return w == 16;
}

static int fp_home(const C64 *c, IRValue v) {
    if (!c->ra_fp || v < 0 || v >= c->ra_fp->num_values) return -1;
    int hw = c->ra_fp->reg[v];
    return hw >= 0 ? hw : -1;
}

/* Same distinction as has_spill_slot, for the SIMD class. */
static int fp_spill(const C64 *c, IRValue v) {
    if (!c->ra_fp || v < 0 || v >= c->ra_fp->num_values) return 0;
    int slot = c->ra_fp->spill_slot[v];
    if (slot < 0 || slot >= c->ra_fp->num_spill_slots) return 0;
    if (!c->fp_spill_off) return 0;
    return c->fp_spill_off[slot];
}

/* v30/v31 are outside the allocatable set. */
#define FSCR A64_V31

static void fp_frame(C64 *c, int vt, int off, int is_double, int store, int aln) {
    emit_fp_addr_aligned(c, SCR1, off, aln);
    if (store) {
        if (is_double) a64_str_d(c->as, vt, SCR1, 0);
        else a64_str_s(c->as, vt, SCR1, 0);
    } else {
        if (is_double) a64_ldr_d(c->as, vt, SCR1, 0);
        else a64_ldr_s(c->as, vt, SCR1, 0);
    }
}

static void commit_fp(C64 *c, IRValue v, int src) {
    int h = fp_home(c, v);
    if (h >= 0) {
        if (h != src) a64_fmov_reg(c->as, h, src, vw(c, v) == 8);
        return;
    }
    fp_frame(c, src, fp_spill(c, v), vw(c, v) == 8, 1, 0);
}

/* Materialize v into a V register.  `avoid` is a V reg that must stay intact. */
static int load_fp(C64 *c, IRValue v, int avoid) {
    int h = fp_home(c, v);
    if (h >= 0) return h;
    int tmp = (avoid == FSCR) ? A64_V30 : FSCR;
    const IRInst *d = (v >= 0 && c->def && c->def[v] >= 0)
                      ? &c->fn->insts.data[c->def[v]] : NULL;
    if (d && d->op == IR_CONST) {
        int isd = vw(c, v) == 8;
        emit_mov_imm_w(c->as, SCR0, d->float_imm, isd);
        a64_fmov_gp(c->as, tmp, SCR0, 1, isd);
        return tmp;
    }
    fp_frame(c, tmp, fp_spill(c, v), vw(c, v) == 8, 0, 0);
    return tmp;
}

static void commit_q(C64 *c, IRValue v, int src) {
    int h = fp_home(c, v);
    if (h >= 0) {
        fmov_q(c->as, h, src);
        return;
    }
    emit_fp_addr(c, SCR1, fp_spill(c, v));
    str_q(c->as, src, SCR1, 0);
}

static int load_q(C64 *c, IRValue v, int avoid) {
    int h = fp_home(c, v);
    if (h >= 0) return h;
    int tmp = (avoid == FSCR) ? A64_V30 : FSCR;
    emit_fp_addr(c, SCR1, fp_spill(c, v));
    ldr_q(c->as, tmp, SCR1, 0);
    return tmp;
}

static void emit_mov_imm_w(A64Asm *a, int rd, int64_t imm, int is64) {
    if (is64) {
        a64_mov_imm64(a, rd, (uint64_t)imm);
        return;
    }
    uint32_t w = (uint32_t)imm;
    a64_movz(a, rd, w & 0xFFFF, 0, 0);
    if (w >> 16) a64_movk(a, rd, w >> 16, 1, 0);
}

/* Defining instruction, or NULL when v was never written (a -1 slot
 * in the def map).  Indexing that slot used to walk off the front of
 * the instruction array. */
static const IRInst *def_inst(C64 *c, IRValue v) {
    if (!c->def || v < 0 || v >= c->fn->next_value_id) return NULL;
    int di = c->def[v];
    if (di < 0 || (size_t)di >= c->fn->insts.len) return NULL;
    return &c->fn->insts.data[di];
}

/* Load operand v into a register; `other` is the register already
 * holding the other operand (scratch choice must not collide). */
static int load_op(C64 *c, IRValue v, int other) {
    int scratch = (other == SCR0) ? SCR1 : SCR0;
    const IRInst *d = def_inst(c, v);
    if (d && d->op == IR_CONST) {
        int hr = home_reg(c, v);
        if (hr >= 0) return hr;  /* materialized at its CONST definition */
        emit_mov_imm_w(c->as, scratch, d->imm, vw(c, v) == 8);
        return scratch;
    }
    int r = home_reg(c, v);
    if (r >= 0) return r;
    frame_load(c, scratch, spill_off(c, v), 8, 1, 0);
    return scratch;
}

static int load_ptrv(C64 *c, IRValue v, int other) {
    int r = home_reg(c, v);
    if (r >= 0) return r;
    int scratch = (other == SCR0) ? SCR1 : SCR0;
    frame_load(c, scratch, spill_off(c, v), 8, 1, 0);
    return scratch;
}

/* SP is not encodable in ORR (Rm/Rn=31 means XZR): move via ADD #0. */
static void mov_sp_like(A64Asm *a, int rd, int rn) {
    a64_add_imm12(a, rd, rn, 0, 0, 1, 0);
}

/* rd = fp + off (off usually negative).  The offset is materialized into
 * rd itself when it does not fit imm12, so no second scratch is needed
 * (callers may have data in x16/x17). */
static void emit_fp_addr(C64 *c, int rd, int off) {
    if (off == 0) {
        a64_mov_reg(c->as, rd, A64_FP, 1);
    } else if (off < 0 && off >= -4095) {
        a64_sub_imm12(c->as, rd, A64_FP, (unsigned)(-off), 0, 1, 0);
    } else if (off > 0 && off <= 4095) {
        a64_add_imm12(c->as, rd, A64_FP, (unsigned)off, 0, 1, 0);
    } else {
        emit_mov_imm_w(c->as, rd, off, 1);
        a64_add_reg(c->as, rd, A64_FP, rd, A64_LSL, 0, 1, 0);
    }
}

/* rd = align_down(fp + off, aln) -- an over-aligned alloca's real address.
 * AAPCS64 only guarantees a 16-byte aligned SP on entry, so a slot asking for
 * 32 or more cannot be reached by a static fp-relative offset: round the
 * address down at run time instead.  The frame plan rounds *every* offset up
 * to the frame's single maximum alignment, so every over-aligned slot is
 * masked by the same amount and they keep their relative positions -- mixing
 * per-slot alignments here would let a 64-byte slot and a 32-byte slot overlap
 * whenever fp lands mid-way between the two.  aln is a power of two, so
 * ~(aln-1) is exactly -aln, and -32/-64 are both encodable as logical
 * immediates (a contiguous run of ones). */
static void emit_fp_addr_aligned(C64 *c, int rd, int off, int aln) {
    emit_fp_addr(c, rd, off);
    if (aln > 16) a64_and_imm(c->as, rd, rd, ~(uint64_t)(aln - 1), 1);
}

/* Alignment recorded for alloca `slot` (0 when it is not a pinned slot). */
static int alloca_align_of(const C64 *c, IRValue slot, int nv) {
    if (!c->alloca_aln || slot < 0 || slot >= nv) return 0;
    return c->alloca_aln[slot];
}

/* fp-relative loads/stores.  LDUR/STUR cover the imm9 window.  Outside
 * it the address is built in the destination itself and then loaded
 * (`ldr rt, [rt]`): the other scratch often already holds the first
 * operand of a binary op, and reusing it as an address base drops that
 * value (seen as a wrong sum once spill slots sit below fp-256). */
static void frame_load(C64 *c, int rt, int off, int width, int uns, int aln) {
    A64Asm *a = c->as;
    if (aln <= 16 && off >= -256 && off <= 255) {
        switch (width) {
        case 1:
            if (uns)
                a64_ldur8(a, rt, A64_FP, off);
            else
                a64_ldursb64(a, rt, A64_FP, off);
            break;
        case 2:
            if (uns)
                a64_ldur16(a, rt, A64_FP, off);
            else
                a64_ldursh64(a, rt, A64_FP, off);
            break;
        case 4:
            if (uns)
                a64_ldur32(a, rt, A64_FP, off);
            else
                a64_ldursw(a, rt, A64_FP, off);
            break;
        default: a64_ldur64(a, rt, A64_FP, off); break;
        }
        return;
    }
    emit_fp_addr_aligned(c, rt, off, aln);
    emit_load(c, rt, rt, width, uns);
}

static void frame_store(C64 *c, int valrt, int off, int width, int aln) {
    A64Asm *a = c->as;
    if (aln <= 16 && off >= -256 && off <= 255) {
        switch (width) {
        case 1: a64_stur8(a, valrt, A64_FP, off); break;
        case 2: a64_stur16(a, valrt, A64_FP, off); break;
        case 4: a64_stur32(a, valrt, A64_FP, off); break;
        default: a64_stur64(a, valrt, A64_FP, off); break;
        }
        return;
    }
    int at = (valrt == SCR0) ? SCR1 : SCR0;
    emit_fp_addr_aligned(c, at, off, aln);
    emit_store(c, valrt, at, width);
}

static void emit_load(C64 *c, int rt, int base, int width, int uns) {
    switch (width) {
    case 1:
        if (uns) a64_ldr8(c->as, rt, base, 0);
        else a64_ldrsb64(c->as, rt, base, 0);
        break;
    case 2:
        if (uns) a64_ldr16(c->as, rt, base, 0);
        else a64_ldrsh64(c->as, rt, base, 0);
        break;
    case 4:
        if (uns) a64_ldr32(c->as, rt, base, 0);
        else a64_ldrsw(c->as, rt, base, 0);
        break;
    default:
        a64_ldr64(c->as, rt, base, 0);
        break;
    }
}

static void emit_store(C64 *c, int rt, int base, int width) {
    switch (width) {
    case 1: a64_str8(c->as, rt, base, 0); break;
    case 2: a64_str16(c->as, rt, base, 0); break;
    case 4: a64_str32(c->as, rt, base, 0); break;
    default: a64_str64(c->as, rt, base, 0); break;
    }
}

/* ── Calls ────────────────────────────────────────────────────────── */

static int find_global_idx(const IRModule *ir, const char *name) {
    for (size_t k = 0; k < ir->globals.len; k++)
        if (strcmp(ir->globals.data[k].name, name) == 0)
            return (int)k;
    return -1;
}

static int find_function(const IRModule *ir, const char *name, int *idx_out) {
    for (size_t i = 0; i < ir->functions.len; i++)
        if (strcmp(ir->functions.data[i].name, name) == 0) {
            *idx_out = (int)i;
            return 0;
        }
    return -1;
}

static void emit_syscall(C64 *c, const IRInst *s);
static int emit_pac_builtin(C64 *c, const IRInst *s);

/* malloc/free/calloc via Darwin mmap/munmap when this TU does not define
 * them.  The block stores its mapped length in the 16 bytes before the
 * returned pointer so free can munmap the same range.  calloc checks that
 * the element count and size do not wrap. */
static int emit_alloc_builtin(C64 *c, const char *name) {
    int is_malloc = strcmp(name, "malloc") == 0;
    int is_free = strcmp(name, "free") == 0;
    int is_calloc = strcmp(name, "calloc") == 0;
    int is_realloc = strcmp(name, "realloc") == 0;
    if (!is_malloc && !is_free && !is_calloc && !is_realloc) return 0;
    int defined = 0;
    if (find_function(c->ir, name, &defined) == 0) return 0;
    A64Asm *a = c->as;
    if (is_free) {
        int done = a64_new_label(a);
        a64_cbz(a, A64_X0, done, 1);
        a64_sub_imm12(a, A64_X0, A64_X0, 16, 0, 1, 0);
        a64_ldr64(a, A64_X1, A64_X0, 0);
        a64_movz(a, A64_X16, 73, 0, 1); /* munmap */
        a64_svc(a, 0x80);
        a64_bind(a, done);
        return 1;
    }
    int nonzero = a64_new_label(a);
    int fail = a64_new_label(a);
    int done = a64_new_label(a);
    int zloop = a64_new_label(a);
    int after = a64_new_label(a);
    int have_size = a64_new_label(a);
    if (is_realloc) {
        int as_malloc = a64_new_label(a);
        int as_free = a64_new_label(a);
        int use_new = a64_new_label(a);
        a64_cbz(a, A64_X0, as_malloc, 1);
        a64_cbz(a, A64_X1, as_free, 1);
        a64_mov_reg(a, A64_X10, A64_X0, 1);
        a64_mov_reg(a, A64_X12, A64_X1, 1);
        a64_sub_imm12(a, A64_X11, A64_X0, 16, 0, 1, 0);
        a64_ldr64(a, A64_X11, A64_X11, 0);
        a64_sub_imm12(a, A64_X11, A64_X11, 16, 0, 1, 0);
        a64_cmp_reg(a, A64_X11, A64_X12, 1);
        a64_bcond(a, A64_LS, use_new);
        a64_mov_reg(a, A64_X11, A64_X12, 1);
        a64_bind(a, use_new);
        a64_movz(a, A64_X14, 1, 0, 1);
        a64_mov_reg(a, A64_X0, A64_X12, 1);
        a64_movz(a, A64_X7, 0, 0, 1);
        a64_b(a, have_size);
        a64_bind(a, as_malloc);
        a64_mov_reg(a, A64_X0, A64_X1, 1);
        a64_movz(a, A64_X7, 0, 0, 1);
        a64_movz(a, A64_X14, 0, 0, 1);
        a64_b(a, have_size);
        a64_bind(a, as_free);
        a64_sub_imm12(a, A64_X0, A64_X0, 16, 0, 1, 0);
        a64_ldr64(a, A64_X1, A64_X0, 0);
        a64_movz(a, A64_X16, 73, 0, 1);
        a64_svc(a, 0x80);
        a64_movz(a, A64_X0, 0, 0, 1);
        a64_b(a, done);
    }
    if (is_calloc) {
        int nzero = a64_new_label(a);
        a64_mul(a, A64_X2, A64_X0, A64_X1, 1);
        a64_cbz(a, A64_X0, nzero, 1);
        a64_udiv(a, A64_X3, A64_X2, A64_X0, 1);
        a64_cmp_reg(a, A64_X3, A64_X1, 1);
        a64_bcond(a, A64_NE, fail);
        a64_bind(a, nzero);
        a64_mov_reg(a, A64_X7, A64_X2, 1);
        a64_mov_reg(a, A64_X0, A64_X2, 1);
        a64_movz(a, A64_X14, 0, 0, 1);
    } else {
        a64_movz(a, A64_X7, 0, 0, 1);
        a64_movz(a, A64_X14, 0, 0, 1);
    }
    a64_bind(a, have_size);
    a64_cbnz(a, A64_X0, nonzero, 1);
    a64_movz(a, A64_X0, 1, 0, 1);
    a64_bind(a, nonzero);
    a64_add_imm12(a, A64_X0, A64_X0, 31, 0, 1, 0);
    a64_lsr_imm(a, A64_X0, A64_X0, 4, 1);
    a64_lsl_imm(a, A64_X0, A64_X0, 4, 1);
    a64_mov_reg(a, A64_X1, A64_X0, 1);
    a64_movz(a, A64_X0, 0, 0, 1);
    a64_movz(a, A64_X2, 3, 0, 1);
    a64_movz(a, A64_X3, 0x1002, 0, 1);
    a64_movn(a, A64_X4, 0, 0, 1);
    a64_movz(a, A64_X5, 0, 0, 1);
    a64_movz(a, A64_X16, 197, 0, 1); /* mmap */
    a64_svc(a, 0x80);
    a64_cset(a, A64_X9, A64_CS, 1);
    a64_cbnz(a, A64_X9, fail, 1);
    a64_str64(a, A64_X1, A64_X0, 0);
    a64_add_imm12(a, A64_X0, A64_X0, 16, 0, 1, 0);
    a64_mov_reg(a, A64_X2, A64_X0, 1);
    a64_mov_reg(a, A64_X3, A64_X7, 1);
    a64_bind(a, zloop);
    a64_cbz(a, A64_X3, after, 1);
    a64_str8(a, 31, A64_X2, 0); /* strb wzr */
    a64_add_imm12(a, A64_X2, A64_X2, 1, 0, 1, 0);
    a64_sub_imm12(a, A64_X3, A64_X3, 1, 0, 1, 0);
    a64_b(a, zloop);
    a64_bind(a, fail);
    a64_movz(a, A64_X0, 0, 0, 1);
    a64_b(a, done);
    a64_bind(a, after);
    {
        int cloop = a64_new_label(a);
        int copied = a64_new_label(a);
        a64_cbz(a, A64_X14, done, 1);
        a64_mov_reg(a, A64_X13, A64_X0, 1);
        a64_mov_reg(a, A64_X2, A64_X13, 1);
        a64_mov_reg(a, A64_X3, A64_X10, 1);
        a64_mov_reg(a, A64_X4, A64_X11, 1);
        a64_bind(a, cloop);
        a64_cbz(a, A64_X4, copied, 1);
        a64_ldr8(a, A64_X5, A64_X3, 0);
        a64_str8(a, A64_X5, A64_X2, 0);
        a64_add_imm12(a, A64_X2, A64_X2, 1, 0, 1, 0);
        a64_add_imm12(a, A64_X3, A64_X3, 1, 0, 1, 0);
        a64_sub_imm12(a, A64_X4, A64_X4, 1, 0, 1, 0);
        a64_b(a, cloop);
        a64_bind(a, copied);
        a64_mov_reg(a, A64_X0, A64_X10, 1);
        a64_sub_imm12(a, A64_X0, A64_X0, 16, 0, 1, 0);
        a64_ldr64(a, A64_X1, A64_X0, 0);
        a64_movz(a, A64_X16, 73, 0, 1);
        a64_svc(a, 0x80);
        a64_mov_reg(a, A64_X0, A64_X13, 1);
    }
    a64_bind(a, done);
    return 1;
}

/* Freestanding memcpy/memmove/memset.  Struct assignment above 64 bytes
 * lowers to a call, and the Darwin runtime is not linked yet (T16).
 * A user-defined function of the same name still wins.  The three
 * arguments are already in x0/x1/x2; the original destination is
 * returned in x0.  Byte loops stay correct for odd sizes and (for
 * memmove) overlapping ranges. */
static int emit_mem_builtin(C64 *c, const char *name) {
    int is_memcpy = strcmp(name, "memcpy") == 0;
    int is_memmove = strcmp(name, "memmove") == 0;
    int is_memset = strcmp(name, "memset") == 0;
    int is_bzero = strcmp(name, "bzero") == 0;
    int is_mempcpy = strcmp(name, "mempcpy") == 0;
    int is_bcopy = strcmp(name, "bcopy") == 0;
    int is_memccpy = strcmp(name, "memccpy") == 0;
    if (!is_memcpy && !is_memmove && !is_memset && !is_bzero && !is_mempcpy
        && !is_bcopy && !is_memccpy)
        return 0;
    int defined = 0;
    if (find_function(c->ir, name, &defined) == 0) return 0;

    A64Asm *a = c->as;
    if (is_memccpy) {
        /* memccpy(dst, src, c, n).  The stop byte is copied.  Return the
         * address just past it, or 0 when n runs out first. */
        int Lloop = a64_new_label(a);
        int Lmiss = a64_new_label(a);
        int Ldone = a64_new_label(a);
        a64_and_imm(a, A64_X2, A64_X2, 0xff, 0);
        a64_bind(a, Lloop);
        a64_cbz(a, A64_X3, Lmiss, 1);
        a64_ldr8(a, A64_X4, A64_X1, 0);
        a64_str8(a, A64_X4, A64_X0, 0);
        a64_add_imm12(a, A64_X0, A64_X0, 1, 0, 1, 0);
        a64_add_imm12(a, A64_X1, A64_X1, 1, 0, 1, 0);
        a64_sub_imm12(a, A64_X3, A64_X3, 1, 0, 1, 0);
        a64_cmp_reg(a, A64_X4, A64_X2, 0);
        a64_bcond(a, A64_EQ, Ldone);
        a64_b(a, Lloop);
        a64_bind(a, Lmiss);
        a64_movz(a, A64_X0, 0, 0, 1);
        a64_bind(a, Ldone);
        return 1;
    }
    if (is_bcopy) {
        /* bcopy(src, dst, n): opposite of memmove, and overlap-safe. */
        a64_mov_reg(a, SCR0, A64_X0, 1);
        a64_mov_reg(a, A64_X0, A64_X1, 1);
        a64_mov_reg(a, A64_X1, SCR0, 1);
        is_memmove = 1;
    }
    if (is_bzero) {
        /* bzero(dst, n): the count arrives in x1, the fill byte is 0. */
        a64_mov_reg(a, A64_X2, A64_X1, 1);
        a64_movz(a, A64_X1, 0, 0, 1);
        is_memset = 1;
    }
    if (is_mempcpy) is_memcpy = 1;
    int Lfwd = a64_new_label(a);
    int Lback = a64_new_label(a);
    int Lback_loop = a64_new_label(a);
    int Ldone = a64_new_label(a);

    a64_mov_reg(a, SCR1, A64_X0, 1);                 /* remember dst */
    if (is_memmove) {
        a64_cmp_reg(a, A64_X0, A64_X1, 1);
        a64_bcond(a, A64_CS, Lback);                 /* dst >= src: backward */
    }
    a64_bind(a, Lfwd);
    a64_cbz(a, A64_X2, Ldone, 1);
    if (is_memset) {
        a64_str8(a, A64_X1, A64_X0, 0);
    } else {
        a64_ldr8(a, SCR0, A64_X1, 0);
        a64_str8(a, SCR0, A64_X0, 0);
        a64_add_imm12(a, A64_X1, A64_X1, 1, 0, 1, 0);
    }
    a64_add_imm12(a, A64_X0, A64_X0, 1, 0, 1, 0);
    a64_sub_imm12(a, A64_X2, A64_X2, 1, 0, 1, 0);
    a64_b(a, Lfwd);
    if (is_memmove) {
        a64_bind(a, Lback);
        a64_add_reg(a, A64_X0, A64_X0, A64_X2, A64_LSL, 0, 1, 0);
        a64_add_reg(a, A64_X1, A64_X1, A64_X2, A64_LSL, 0, 1, 0);
        a64_bind(a, Lback_loop);
        a64_cbz(a, A64_X2, Ldone, 1);
        a64_sub_imm12(a, A64_X0, A64_X0, 1, 0, 1, 0);
        a64_sub_imm12(a, A64_X1, A64_X1, 1, 0, 1, 0);
        a64_ldr8(a, SCR0, A64_X1, 0);
        a64_str8(a, SCR0, A64_X0, 0);
        a64_sub_imm12(a, A64_X2, A64_X2, 1, 0, 1, 0);
        a64_b(a, Lback_loop);
    }
    a64_bind(a, Ldone);
    /* mempcpy returns dest+n, which is where the loop stopped. */
    if (!is_mempcpy)
        a64_mov_reg(a, A64_X0, SCR1, 1);
    return 1;
}

/* Map A-Z onto a-z.  NUL and every other byte stay put. */
static void fold_ascii_upper(A64Asm *a, int reg) {
    int Lskip = a64_new_label(a);
    a64_cmp_imm12(a, reg, 65, 0, 0);
    a64_bcond(a, A64_CC, Lskip);
    a64_cmp_imm12(a, reg, 90, 0, 0);
    a64_bcond(a, A64_HI, Lskip);
    a64_add_imm12(a, reg, reg, 32, 0, 0, 0);
    a64_bind(a, Lskip);
}

/* memcmp/strcmp/strlen/strncmp.  Same reason as memcpy: the Darwin
 * runtime is not linked yet, and a user definition still wins.
 * Arguments are already in x0/x1(/x2).  The signed byte difference
 * (or the length) is left in x0.  x3/x4 are caller-saved. */
static int emit_scan_builtin(C64 *c, const char *name) {
    int is_memcmp = strcmp(name, "memcmp") == 0 || strcmp(name, "bcmp") == 0;
    int is_strcmp = strcmp(name, "strcmp") == 0;
    int is_strncmp = strcmp(name, "strncmp") == 0;
    int is_strlen = strcmp(name, "strlen") == 0;
    int is_strcasecmp = strcmp(name, "strcasecmp") == 0;
    int is_strncasecmp = strcmp(name, "strncasecmp") == 0;
    int fold = is_strcasecmp || is_strncasecmp;
    if (is_strcasecmp) is_strcmp = 1;
    if (is_strncasecmp) is_strncmp = 1;
    if (!is_memcmp && !is_strcmp && !is_strncmp && !is_strlen) return 0;
    int defined = 0;
    if (find_function(c->ir, name, &defined) == 0) return 0;

    A64Asm *a = c->as;
    int Lloop = a64_new_label(a);
    int Ldiff = a64_new_label(a);
    int Lzero = a64_new_label(a);
    int Ldone = a64_new_label(a);

    if (is_strlen) {
        a64_mov_reg(a, A64_X1, A64_X0, 1);
        a64_bind(a, Lloop);
        a64_ldr8(a, A64_X2, A64_X0, 0);
        a64_cbz(a, A64_X2, Ldone, 0);
        a64_add_imm12(a, A64_X0, A64_X0, 1, 0, 1, 0);
        a64_b(a, Lloop);
        a64_bind(a, Ldone);
        a64_sub_reg(a, A64_X0, A64_X0, A64_X1, A64_LSL, 0, 1, 0);
        return 1;
    }

    a64_bind(a, Lloop);
    if (is_memcmp || is_strncmp)
        a64_cbz(a, A64_X2, Lzero, 1);
    a64_ldr8(a, A64_X3, A64_X0, 0);
    a64_ldr8(a, A64_X4, A64_X1, 0);
    if (fold) {
        fold_ascii_upper(a, A64_X3);
        fold_ascii_upper(a, A64_X4);
    }
    a64_cmp_reg(a, A64_X3, A64_X4, 0);
    a64_bcond(a, A64_NE, Ldiff);
    if (is_strcmp || is_strncmp)
        a64_cbz(a, A64_X3, Lzero, 0);
    a64_add_imm12(a, A64_X0, A64_X0, 1, 0, 1, 0);
    a64_add_imm12(a, A64_X1, A64_X1, 1, 0, 1, 0);
    if (is_memcmp || is_strncmp)
        a64_sub_imm12(a, A64_X2, A64_X2, 1, 0, 1, 0);
    a64_b(a, Lloop);
    a64_bind(a, Lzero);
    a64_movz(a, A64_X0, 0, 0, 1);
    a64_b(a, Ldone);
    a64_bind(a, Ldiff);
    /* Unsigned bytes.  Sign-extend so a 64-bit compare still sees < 0. */
    a64_sub_reg(a, A64_X0, A64_X3, A64_X4, A64_LSL, 0, 0, 0);
    a64_sxt(a, A64_X0, A64_X0, 4, 1);
    a64_bind(a, Ldone);
    return 1;
}

/* memchr/strchr/strrchr/strnlen.  Pointer or length left in x0.
 * The searched byte is masked to 8 bits.  x3/x4 are caller-saved. */
static int emit_find_builtin(C64 *c, const char *name) {
    int is_memchr = strcmp(name, "memchr") == 0;
    int is_memrchr = strcmp(name, "memrchr") == 0;
    int is_strchr = strcmp(name, "strchr") == 0 || strcmp(name, "index") == 0;
    int is_strrchr = strcmp(name, "strrchr") == 0 || strcmp(name, "rindex") == 0;
    int is_strnlen = strcmp(name, "strnlen") == 0;
    int is_strchrnul = strcmp(name, "strchrnul") == 0;
    int is_rawmemchr = strcmp(name, "rawmemchr") == 0;
    if (!is_memchr && !is_memrchr && !is_strchr && !is_strrchr && !is_strnlen
        && !is_strchrnul && !is_rawmemchr)
        return 0;
    int defined = 0;
    if (find_function(c->ir, name, &defined) == 0) return 0;

    A64Asm *a = c->as;
    int Lloop = a64_new_label(a);
    int Ldone = a64_new_label(a);
    int Lmiss = a64_new_label(a);

    if (is_memrchr) {
        /* Walk backward from s+n.  x4 is the cursor. */
        int Lhit = a64_new_label(a);
        a64_and_imm(a, A64_X1, A64_X1, 0xff, 0);
        a64_add_reg(a, A64_X4, A64_X0, A64_X2, A64_LSL, 0, 1, 0);
        a64_bind(a, Lloop);
        a64_cmp_reg(a, A64_X4, A64_X0, 1);
        a64_bcond(a, A64_EQ, Lmiss);
        a64_sub_imm12(a, A64_X4, A64_X4, 1, 0, 1, 0);
        a64_ldr8(a, A64_X3, A64_X4, 0);
        a64_cmp_reg(a, A64_X3, A64_X1, 0);
        a64_bcond(a, A64_EQ, Lhit);
        a64_b(a, Lloop);
        a64_bind(a, Lhit);
        a64_mov_reg(a, A64_X0, A64_X4, 1);
        a64_b(a, Ldone);
        a64_bind(a, Lmiss);
        a64_movz(a, A64_X0, 0, 0, 1);
        a64_bind(a, Ldone);
        return 1;
    }
    if (is_strchrnul || is_rawmemchr) {
        a64_and_imm(a, A64_X1, A64_X1, 0xff, 0);
        a64_bind(a, Lloop);
        a64_ldr8(a, A64_X3, A64_X0, 0);
        a64_cmp_reg(a, A64_X3, A64_X1, 0);
        a64_bcond(a, A64_EQ, Ldone);
        if (!is_rawmemchr)
            a64_cbz(a, A64_X3, Ldone, 0);
        a64_add_imm12(a, A64_X0, A64_X0, 1, 0, 1, 0);
        a64_b(a, Lloop);
        a64_bind(a, Ldone);
        return 1;
    }

    if (is_strnlen) {
        a64_mov_reg(a, A64_X3, A64_X0, 1);
        a64_bind(a, Lloop);
        a64_cbz(a, A64_X1, Ldone, 1);
        a64_ldr8(a, A64_X4, A64_X0, 0);
        a64_cbz(a, A64_X4, Ldone, 0);
        a64_add_imm12(a, A64_X0, A64_X0, 1, 0, 1, 0);
        a64_sub_imm12(a, A64_X1, A64_X1, 1, 0, 1, 0);
        a64_b(a, Lloop);
        a64_bind(a, Ldone);
        a64_sub_reg(a, A64_X0, A64_X0, A64_X3, A64_LSL, 0, 1, 0);
        return 1;
    }

    a64_and_imm(a, A64_X1, A64_X1, 0xff, 0);
    if (is_strrchr)
        a64_movz(a, A64_X4, 0, 0, 1);
    a64_bind(a, Lloop);
    if (is_memchr)
        a64_cbz(a, A64_X2, Lmiss, 1);
    a64_ldr8(a, A64_X3, A64_X0, 0);
    a64_cmp_reg(a, A64_X3, A64_X1, 0);
    if (is_strrchr) {
        int Lnext = a64_new_label(a);
        a64_bcond(a, A64_NE, Lnext);
        a64_mov_reg(a, A64_X4, A64_X0, 1);
        a64_bind(a, Lnext);
        a64_cbz(a, A64_X3, Ldone, 0);
        a64_add_imm12(a, A64_X0, A64_X0, 1, 0, 1, 0);
        a64_b(a, Lloop);
        a64_bind(a, Ldone);
        a64_mov_reg(a, A64_X0, A64_X4, 1);
        return 1;
    }
    a64_bcond(a, A64_EQ, Ldone);
    if (!is_memchr)
        a64_cbz(a, A64_X3, Lmiss, 0);
    a64_add_imm12(a, A64_X0, A64_X0, 1, 0, 1, 0);
    if (is_memchr)
        a64_sub_imm12(a, A64_X2, A64_X2, 1, 0, 1, 0);
    a64_b(a, Lloop);
    a64_bind(a, Lmiss);
    a64_movz(a, A64_X0, 0, 0, 1);
    a64_bind(a, Ldone);
    return 1;
}

/* strspn/strcspn.  The span length is left in x0.  x2–x5 are caller-saved.
 * A user definition of the same name still wins. */
static int emit_span_builtin(C64 *c, const char *name) {
    int is_spn = strcmp(name, "strspn") == 0;
    int is_cspn = strcmp(name, "strcspn") == 0;
    int is_pbrk = strcmp(name, "strpbrk") == 0;
    if (!is_spn && !is_cspn && !is_pbrk) return 0;
    int defined = 0;
    if (find_function(c->ir, name, &defined) == 0) return 0;

    A64Asm *a = c->as;
    int Louter = a64_new_label(a);
    int Linner = a64_new_label(a);
    int Ladvance = a64_new_label(a);
    int Ldone = a64_new_label(a);

    int Lmiss = a64_new_label(a);
    int Lhit = a64_new_label(a);
    int Lend = a64_new_label(a);
    a64_mov_reg(a, A64_X4, A64_X0, 1);
    a64_bind(a, Louter);
    a64_ldr8(a, A64_X2, A64_X0, 0);
    a64_cbz(a, A64_X2, is_pbrk ? Lmiss : Ldone, 0);
    a64_mov_reg(a, A64_X5, A64_X1, 1);
    a64_bind(a, Linner);
    a64_ldr8(a, A64_X3, A64_X5, 0);
    /* strspn stops when accept runs out.  strcspn and strpbrk advance. */
    a64_cbz(a, A64_X3, is_spn ? Ldone : Ladvance, 0);
    a64_cmp_reg(a, A64_X2, A64_X3, 0);
    a64_bcond(a, A64_EQ, is_spn ? Ladvance : (is_pbrk ? Lhit : Ldone));
    a64_add_imm12(a, A64_X5, A64_X5, 1, 0, 1, 0);
    a64_b(a, Linner);
    a64_bind(a, Ladvance);
    a64_add_imm12(a, A64_X0, A64_X0, 1, 0, 1, 0);
    a64_b(a, Louter);
    if (is_pbrk) {
        /* x0 already addresses the matching byte. */
        a64_bind(a, Lhit);
        a64_b(a, Lend);
        a64_bind(a, Lmiss);
        a64_movz(a, A64_X0, 0, 0, 1);
        a64_bind(a, Lend);
        return 1;
    }
    a64_bind(a, Ldone);
    a64_sub_reg(a, A64_X0, A64_X0, A64_X4, A64_LSL, 0, 1, 0);
    return 1;
}

/* __builtin_clear_cache(start, end).  Apple Silicon will execute a stale
 * instruction line unless the data cache is cleaned and the instruction
 * cache is invalidated to the point of unification.  Reading CTR_EL0 is
 * SIGILL in user mode on this OS, so the step is 64 bytes: this machine's
 * line is 128, and a shorter step still hits every line.  Args are already
 * in x0 and x1.  An empty or reversed range does nothing. */
static int emit_cache_builtin(C64 *c, const char *name) {
    if (!name || strcmp(name, "clear_cache") != 0) return 0;
    int defined = 0;
    if (find_function(c->ir, name, &defined) == 0) return 0;
    A64Asm *a = c->as;
    int Ldone = a64_new_label(a);
    int Ldc = a64_new_label(a);
    int Ldc_end = a64_new_label(a);
    int Lic = a64_new_label(a);
    int Lic_end = a64_new_label(a);
    a64_cmp_reg(a, A64_X0, A64_X1, 1);
    a64_bcond(a, A64_CS, Ldone);
    a64_movz(a, A64_X17, 64, 0, 1);
    a64_sub_imm12(a, A64_X2, A64_X17, 1, 0, 1, 0);
    a64_mvn(a, A64_X2, A64_X2, 1);
    a64_and_reg(a, A64_X9, A64_X0, A64_X2, 1);
    a64_bind(a, Ldc);
    a64_cmp_reg(a, A64_X9, A64_X1, 1);
    a64_bcond(a, A64_CS, Ldc_end);
    a64_word(a, 0xD50B7B20u | A64_X9);
    a64_add_reg(a, A64_X9, A64_X9, A64_X17, A64_LSL, 0, 1, 0);
    a64_b(a, Ldc);
    a64_bind(a, Ldc_end);
    a64_word(a, 0xD5033B9Fu);
    a64_and_reg(a, A64_X9, A64_X0, A64_X2, 1);
    a64_bind(a, Lic);
    a64_cmp_reg(a, A64_X9, A64_X1, 1);
    a64_bcond(a, A64_CS, Lic_end);
    a64_word(a, 0xD50B7520u | A64_X9);
    a64_add_reg(a, A64_X9, A64_X9, A64_X17, A64_LSL, 0, 1, 0);
    a64_b(a, Lic);
    a64_bind(a, Lic_end);
    a64_word(a, 0xD5033B9Fu);
    a64_word(a, 0xD5033FDFu);
    a64_bind(a, Ldone);
    return 1;
}

/* strcpy/stpcpy/strncpy/strcat/strncat/strstr.  Result pointer in x0.
 * x3–x5 are caller-saved.  A user definition of the same name still wins. */
/* strlcpy/strlcat return the length they tried to create.  strsep
 * rewrites the caller's pointer and does not allocate. */
static int emit_bound_builtin(C64 *c, const char *name) {
    int is_strlcpy = name && strcmp(name, "strlcpy") == 0;
    int is_strlcat = name && strcmp(name, "strlcat") == 0;
    int is_strsep = name && strcmp(name, "strsep") == 0;
    if (!is_strlcpy && !is_strlcat && !is_strsep) return 0;
    int defined = 0;
    if (find_function(c->ir, name, &defined) == 0) return 0;
    A64Asm *a = c->as;
    if (is_strsep) {
        int Lscan = a64_new_label(a);
        int Ldelim = a64_new_label(a);
        int Lnext = a64_new_label(a);
        int Lhit = a64_new_label(a);
        int Lend = a64_new_label(a);
        int Lret = a64_new_label(a);
        int Lnull = a64_new_label(a);
        a64_ldr64(a, A64_X2, A64_X0, 0);
        a64_cbz(a, A64_X2, Lnull, 1);
        a64_mov_reg(a, A64_X3, A64_X2, 1);
        a64_bind(a, Lscan);
        a64_ldr8(a, A64_X4, A64_X2, 0);
        a64_cbz(a, A64_X4, Lend, 0);
        a64_mov_reg(a, A64_X5, A64_X1, 1);
        a64_bind(a, Ldelim);
        a64_ldr8(a, A64_X6, A64_X5, 0);
        a64_cbz(a, A64_X6, Lnext, 0);
        a64_cmp_reg(a, A64_X4, A64_X6, 0);
        a64_bcond(a, A64_EQ, Lhit);
        a64_add_imm12(a, A64_X5, A64_X5, 1, 0, 1, 0);
        a64_b(a, Ldelim);
        a64_bind(a, Lnext);
        a64_add_imm12(a, A64_X2, A64_X2, 1, 0, 1, 0);
        a64_b(a, Lscan);
        a64_bind(a, Lhit);
        a64_str8(a, 31, A64_X2, 0);
        a64_add_imm12(a, A64_X2, A64_X2, 1, 0, 1, 0);
        a64_str64(a, A64_X2, A64_X0, 0);
        a64_mov_reg(a, A64_X0, A64_X3, 1);
        a64_b(a, Lret);
        a64_bind(a, Lend);
        a64_str64(a, 31, A64_X0, 0);
        a64_mov_reg(a, A64_X0, A64_X3, 1);
        a64_b(a, Lret);
        a64_bind(a, Lnull);
        a64_movz(a, A64_X0, 0, 0, 1);
        a64_bind(a, Lret);
        return 1;
    }
    int Lcopy = a64_new_label(a);
    int Lnul = a64_new_label(a);
    int Lterm = a64_new_label(a);
    int Lmeas = a64_new_label(a);
    int Llen = a64_new_label(a);
    int Ldone = a64_new_label(a);
    a64_movz(a, A64_X6, 0, 0, 1);
    if (is_strlcat) {
        int Lfind = a64_new_label(a);
        int Lapp = a64_new_label(a);
        int Llong = a64_new_label(a);
        a64_mov_reg(a, A64_X4, A64_X0, 1);
        a64_mov_reg(a, A64_X5, A64_X2, 1);
        a64_bind(a, Lfind);
        a64_cbz(a, A64_X5, Llong, 1);
        a64_ldr8(a, A64_X3, A64_X0, 0);
        a64_cbz(a, A64_X3, Lapp, 0);
        a64_add_imm12(a, A64_X0, A64_X0, 1, 0, 1, 0);
        a64_sub_imm12(a, A64_X5, A64_X5, 1, 0, 1, 0);
        a64_b(a, Lfind);
        a64_bind(a, Llong);
        a64_mov_reg(a, A64_X6, A64_X2, 1);
        a64_mov_reg(a, A64_X4, A64_X1, 1);
        a64_b(a, Lmeas);
        a64_bind(a, Lapp);
        a64_sub_reg(a, A64_X6, A64_X0, A64_X4, A64_LSL, 0, 1, 0);
        a64_mov_reg(a, A64_X2, A64_X5, 1);
    }
    a64_mov_reg(a, A64_X4, A64_X1, 1);
    a64_cbz(a, A64_X2, Lmeas, 1);
    a64_bind(a, Lcopy);
    a64_cmp_imm12(a, A64_X2, 1, 0, 1);
    a64_bcond(a, A64_EQ, Lterm);
    a64_ldr8(a, A64_X3, A64_X1, 0);
    a64_cbz(a, A64_X3, Lnul, 0);
    a64_str8(a, A64_X3, A64_X0, 0);
    a64_add_imm12(a, A64_X0, A64_X0, 1, 0, 1, 0);
    a64_add_imm12(a, A64_X1, A64_X1, 1, 0, 1, 0);
    a64_sub_imm12(a, A64_X2, A64_X2, 1, 0, 1, 0);
    a64_b(a, Lcopy);
    a64_bind(a, Lnul);
    a64_str8(a, 31, A64_X0, 0);
    a64_sub_reg(a, A64_X0, A64_X1, A64_X4, A64_LSL, 0, 1, 0);
    a64_b(a, Ldone);
    a64_bind(a, Lterm);
    a64_str8(a, 31, A64_X0, 0);
    a64_bind(a, Lmeas);
    a64_ldr8(a, A64_X3, A64_X1, 0);
    a64_cbz(a, A64_X3, Llen, 0);
    a64_add_imm12(a, A64_X1, A64_X1, 1, 0, 1, 0);
    a64_b(a, Lmeas);
    a64_bind(a, Llen);
    a64_sub_reg(a, A64_X0, A64_X1, A64_X4, A64_LSL, 0, 1, 0);
    a64_bind(a, Ldone);
    a64_add_reg(a, A64_X0, A64_X0, A64_X6, A64_LSL, 0, 1, 0);
    return 1;
}

static int emit_copy_builtin(C64 *c, const char *name) {
    int is_strcpy = strcmp(name, "strcpy") == 0;
    int is_stpcpy = strcmp(name, "stpcpy") == 0;
    int is_strncpy = strcmp(name, "strncpy") == 0;
    int is_stpncpy = strcmp(name, "stpncpy") == 0;
    int is_strcat = strcmp(name, "strcat") == 0;
    int is_strncat = strcmp(name, "strncat") == 0;
    int is_strstr = strcmp(name, "strstr") == 0;
    if (!is_strcpy && !is_stpcpy && !is_strncpy && !is_stpncpy && !is_strcat
        && !is_strncat && !is_strstr)
        return 0;
    int defined = 0;
    if (find_function(c->ir, name, &defined) == 0) return 0;

    A64Asm *a = c->as;
    if (is_strstr) {
        int Louter = a64_new_label(a);
        int Linner = a64_new_label(a);
        int Lnext = a64_new_label(a);
        int Lmiss = a64_new_label(a);
        int Ldone = a64_new_label(a);
        a64_ldr8(a, A64_X2, A64_X1, 0);
        a64_cbz(a, A64_X2, Ldone, 0);
        a64_bind(a, Louter);
        a64_ldr8(a, A64_X3, A64_X0, 0);
        a64_cbz(a, A64_X3, Lmiss, 0);
        a64_mov_reg(a, A64_X4, A64_X0, 1);
        a64_mov_reg(a, A64_X5, A64_X1, 1);
        a64_bind(a, Linner);
        a64_ldr8(a, A64_X2, A64_X5, 0);
        a64_cbz(a, A64_X2, Ldone, 0);
        a64_ldr8(a, A64_X3, A64_X4, 0);
        a64_cmp_reg(a, A64_X2, A64_X3, 0);
        a64_bcond(a, A64_NE, Lnext);
        a64_add_imm12(a, A64_X4, A64_X4, 1, 0, 1, 0);
        a64_add_imm12(a, A64_X5, A64_X5, 1, 0, 1, 0);
        a64_b(a, Linner);
        a64_bind(a, Lnext);
        a64_add_imm12(a, A64_X0, A64_X0, 1, 0, 1, 0);
        a64_b(a, Louter);
        a64_bind(a, Lmiss);
        a64_movz(a, A64_X0, 0, 0, 1);
        a64_bind(a, Ldone);
        return 1;
    }

    if (is_strncpy || is_stpncpy) {
        int Lloop = a64_new_label(a);
        int Lpad = a64_new_label(a);
        int Lnul = a64_new_label(a);
        int Ldone = a64_new_label(a);
        int Lend = a64_new_label(a);
        a64_mov_reg(a, A64_X4, A64_X0, 1);
        a64_movz(a, A64_X5, 0, 0, 1); /* address of the first NUL, or 0 */
        a64_bind(a, Lloop);
        a64_cbz(a, A64_X2, Ldone, 1);
        a64_ldr8(a, A64_X3, A64_X1, 0);
        a64_str8(a, A64_X3, A64_X0, 0);
        a64_add_imm12(a, A64_X0, A64_X0, 1, 0, 1, 0);
        a64_sub_imm12(a, A64_X2, A64_X2, 1, 0, 1, 0);
        a64_cbz(a, A64_X3, Lnul, 0);
        a64_add_imm12(a, A64_X1, A64_X1, 1, 0, 1, 0);
        a64_b(a, Lloop);
        a64_bind(a, Lnul);
        a64_sub_imm12(a, A64_X5, A64_X0, 1, 0, 1, 0);
        a64_bind(a, Lpad);
        a64_cbz(a, A64_X2, Ldone, 1);
        a64_str8(a, 31, A64_X0, 0);
        a64_add_imm12(a, A64_X0, A64_X0, 1, 0, 1, 0);
        a64_sub_imm12(a, A64_X2, A64_X2, 1, 0, 1, 0);
        a64_b(a, Lpad);
        a64_bind(a, Ldone);
        if (is_stpncpy) {
            /* No NUL inside n bytes: return dest+n (x0).  Otherwise the
             * first NUL, saved in x5. */
            a64_cbz(a, A64_X5, Lend, 1);
            a64_mov_reg(a, A64_X0, A64_X5, 1);
        } else {
            a64_mov_reg(a, A64_X0, A64_X4, 1);
        }
        a64_bind(a, Lend);
        return 1;
    }

    int Lfind = a64_new_label(a);
    int Lcopy = a64_new_label(a);
    int Lterm = a64_new_label(a);
    int Ldone = a64_new_label(a);
    a64_mov_reg(a, A64_X4, A64_X0, 1);
    if (is_strcat || is_strncat) {
        a64_bind(a, Lfind);
        a64_ldr8(a, A64_X3, A64_X0, 0);
        a64_cbz(a, A64_X3, Lcopy, 0);
        a64_add_imm12(a, A64_X0, A64_X0, 1, 0, 1, 0);
        a64_b(a, Lfind);
    }
    a64_bind(a, Lcopy);
    if (is_strncat) {
        a64_cbz(a, A64_X2, Lterm, 1);
        a64_ldr8(a, A64_X3, A64_X1, 0);
        a64_cbz(a, A64_X3, Lterm, 0);
        a64_str8(a, A64_X3, A64_X0, 0);
        a64_add_imm12(a, A64_X0, A64_X0, 1, 0, 1, 0);
        a64_add_imm12(a, A64_X1, A64_X1, 1, 0, 1, 0);
        a64_sub_imm12(a, A64_X2, A64_X2, 1, 0, 1, 0);
        a64_b(a, Lcopy);
        a64_bind(a, Lterm);
        a64_str8(a, 31, A64_X0, 0);
        a64_mov_reg(a, A64_X0, A64_X4, 1);
        return 1;
    }
    a64_ldr8(a, A64_X3, A64_X1, 0);
    a64_str8(a, A64_X3, A64_X0, 0);
    a64_add_imm12(a, A64_X0, A64_X0, 1, 0, 1, 0);
    a64_add_imm12(a, A64_X1, A64_X1, 1, 0, 1, 0);
    a64_cbnz(a, A64_X3, Lcopy, 0);
    a64_bind(a, Ldone);
    if (is_stpcpy)
        a64_sub_imm12(a, A64_X0, A64_X0, 1, 0, 1, 0);
    else
        a64_mov_reg(a, A64_X0, A64_X4, 1);
    return 1;
}

/* x0 is the user byte count.  On success x0 is the payload pointer;
 * on failure x0 is 0.  Clobbers x1-x5, x9 and x16.  Same mapping as
 * malloc: 16-byte header holding the mapped length. */
static void emit_mmap_bytes(A64Asm *a) {
    int nonzero = a64_new_label(a);
    int fail = a64_new_label(a);
    int done = a64_new_label(a);
    a64_cbnz(a, A64_X0, nonzero, 1);
    a64_movz(a, A64_X0, 1, 0, 1);
    a64_bind(a, nonzero);
    a64_add_imm12(a, A64_X0, A64_X0, 31, 0, 1, 0);
    a64_lsr_imm(a, A64_X0, A64_X0, 4, 1);
    a64_lsl_imm(a, A64_X0, A64_X0, 4, 1);
    a64_mov_reg(a, A64_X1, A64_X0, 1);
    a64_movz(a, A64_X0, 0, 0, 1);
    a64_movz(a, A64_X2, 3, 0, 1);
    a64_movz(a, A64_X3, 0x1002, 0, 1);
    a64_movn(a, A64_X4, 0, 0, 1);
    a64_movz(a, A64_X5, 0, 0, 1);
    a64_movz(a, A64_X16, 197, 0, 1);
    a64_svc(a, 0x80);
    a64_cset(a, A64_X9, A64_CS, 1);
    a64_cbnz(a, A64_X9, fail, 1);
    a64_str64(a, A64_X1, A64_X0, 0);
    a64_add_imm12(a, A64_X0, A64_X0, 16, 0, 1, 0);
    a64_b(a, done);
    a64_bind(a, fail);
    a64_movz(a, A64_X0, 0, 0, 1);
    a64_bind(a, done);
}

/* strdup/strndup.  A same-TU definition wins.  The copy is allocated
 * with a same-TU malloc when one exists, otherwise with mmap.  The
 * source and the copied length are spilled around that call. */
static int emit_dup_builtin(C64 *c, const char *name) {
    int is_strdup = strcmp(name, "strdup") == 0;
    int is_strndup = strcmp(name, "strndup") == 0;
    if (!is_strdup && !is_strndup) return 0;
    int defined = 0;
    if (find_function(c->ir, name, &defined) == 0) return 0;

    A64Asm *a = c->as;
    int Lcount = a64_new_label(a);
    int Lalloc = a64_new_label(a);
    int Lcopy = a64_new_label(a);
    int Lnul = a64_new_label(a);
    int Lfail = a64_new_label(a);
    int Ldone = a64_new_label(a);

    a64_sub_imm12(a, A64_SP, A64_SP, 16, 0, 1, 0);
    a64_str64(a, A64_X0, A64_SP, 0);
    if (is_strndup)
        a64_str64(a, A64_X1, A64_SP, 8);
    a64_movz(a, A64_X13, 0, 0, 1);
    a64_bind(a, Lcount);
    if (is_strndup) {
        a64_ldr64(a, A64_X1, A64_SP, 8);
        a64_cmp_reg(a, A64_X13, A64_X1, 1);
        a64_bcond(a, A64_EQ, Lalloc);
    }
    a64_ldr8(a, A64_X2, A64_X0, 0);
    a64_cbz(a, A64_X2, Lalloc, 0);
    a64_add_imm12(a, A64_X0, A64_X0, 1, 0, 1, 0);
    a64_add_imm12(a, A64_X13, A64_X13, 1, 0, 1, 0);
    a64_b(a, Lcount);
    a64_bind(a, Lalloc);
    a64_str64(a, A64_X13, A64_SP, 8);
    a64_add_imm12(a, A64_X0, A64_X13, 1, 0, 1, 0);
    {
        int mfi = 0;
        if (find_function(c->ir, "malloc", &mfi) == 0)
            a64_bl(a, c->fn_label[mfi]);
        else
            emit_mmap_bytes(a);
    }
    a64_cbz(a, A64_X0, Lfail, 1);
    a64_mov_reg(a, A64_X14, A64_X0, 1);
    a64_ldr64(a, A64_X1, A64_SP, 0);
    a64_ldr64(a, A64_X13, A64_SP, 8);
    a64_mov_reg(a, A64_X0, A64_X14, 1);
    a64_bind(a, Lcopy);
    a64_cbz(a, A64_X13, Lnul, 1);
    a64_ldr8(a, A64_X2, A64_X1, 0);
    a64_str8(a, A64_X2, A64_X0, 0);
    a64_add_imm12(a, A64_X0, A64_X0, 1, 0, 1, 0);
    a64_add_imm12(a, A64_X1, A64_X1, 1, 0, 1, 0);
    a64_sub_imm12(a, A64_X13, A64_X13, 1, 0, 1, 0);
    a64_b(a, Lcopy);
    a64_bind(a, Lnul);
    a64_str8(a, 31, A64_X0, 0);
    a64_mov_reg(a, A64_X0, A64_X14, 1);
    a64_b(a, Ldone);
    a64_bind(a, Lfail);
    a64_movz(a, A64_X0, 0, 0, 1);
    a64_bind(a, Ldone);
    a64_add_imm12(a, A64_SP, A64_SP, 16, 0, 1, 0);
    return 1;
}

static void emit_store_errno(C64 *c, int egi);

/* pipe returns the two descriptors in x0 and x1, not through the
 * pointer argument.  They are stored into the caller's int[2]. */
static int emit_pipe_builtin(C64 *c, const char *name) {
    if (!name || strcmp(name, "pipe") != 0) return 0;
    int defined = 0;
    if (find_function(c->ir, name, &defined) == 0) return 0;
    A64Asm *a = c->as;
    int egi = find_global_idx(c->ir, "errno");
    int fail = a64_new_label(a);
    int done = a64_new_label(a);
    a64_mov_reg(a, A64_X12, A64_X0, 1);
    a64_movz(a, A64_X16, 42, 0, 1);
    a64_svc(a, 0x80);
    a64_cset(a, A64_X9, A64_CS, 1);
    a64_cbnz(a, A64_X9, fail, 1);
    a64_str32(a, A64_X0, A64_X12, 0);
    a64_str32(a, A64_X1, A64_X12, 4);
    if (egi >= 0) {
        note_page_reloc(c, (uint32_t)a->code.len, egi, NULL, -1);
        a64_word(a, 0x90000000u | (uint32_t)A64_X16);
        a64_word(a, 0x91000000u | ((uint32_t)A64_X16 << 5) | (uint32_t)A64_X16);
        a64_str32(a, 31, A64_X16, 0);
    }
    a64_movz(a, A64_X0, 0, 0, 1);
    a64_b(a, done);
    a64_bind(a, fail);
    emit_store_errno(c, egi);
    a64_neg(a, A64_X0, A64_X0, 1);
    a64_bind(a, done);
    return 1;
}

/* read/write/pread/preadv/pwrite/open/close/dup/dup2/fsync/fdatasync/chflags/fchflags/link/unlink/chdir/chmod/access/symlink/readlink/rename/getpid/getppid/getuid/geteuid/getgid/getegid/umask/gettimeofday/issetugid/getentropy/getrlimit/setrlimit/getpgrp/getpgid/setpgid/setsid/kill/getitimer/setitimer/sigprocmask/sigpending/minherit/getsid/fchdir/flock/getgroups/getdtablesize/mkfifo/pathconf/fpathconf/utimes/futimes/chown/fchown/lchown/socketpair/readv/writev/socket/shutdown/getsockopt/setsockopt/bind/listen/getsockname/connect/accept/sendto/sendmsg/recvmsg/recvfrom/getpeername/poll/kqueue/kevent/fchmod/madvise/mincore/mprotect/msync/mlock/munlock/getpriority/setpriority/getrusage/openat/unlinkat/mkdirat/renameat/faccessat/symlinkat/readlinkat/linkat/fchmodat/fchownat/mkfifoat/mkdir/rmdir/lseek/truncate/ftruncate.
 * Arguments are already in x0..x5.  Darwin numbers, with the same
 * carry-to-negative errno fix as __syscall.  A same-TU int errno is
 * updated with the positive code, or cleared on success.  A same-TU
 * definition still wins. */
static int emit_io_builtin(C64 *c, const char *name) {
    int num = 0;
    if (strcmp(name, "read") == 0) num = 3;
    else if (strcmp(name, "write") == 0) num = 4;
    else if (strcmp(name, "pread") == 0) num = 153;
    else if (strcmp(name, "preadv") == 0) num = 540;
    else if (strcmp(name, "pwrite") == 0) num = 154;
    else if (strcmp(name, "open") == 0) num = 5;
    else if (strcmp(name, "close") == 0) num = 6;
    else if (strcmp(name, "dup") == 0) num = 41;
    else if (strcmp(name, "dup2") == 0) num = 90;
    else if (strcmp(name, "fsync") == 0) num = 95;
    else if (strcmp(name, "fdatasync") == 0) num = 187;
    else if (strcmp(name, "chflags") == 0) num = 34;
    else if (strcmp(name, "fchflags") == 0) num = 35;
    else if (strcmp(name, "flock") == 0) num = 131;
    else if (strcmp(name, "link") == 0) num = 9;
    else if (strcmp(name, "unlink") == 0) num = 10;
    else if (strcmp(name, "chdir") == 0) num = 12;
    else if (strcmp(name, "fchdir") == 0) num = 13;
    else if (strcmp(name, "chmod") == 0) num = 15;
    else if (strcmp(name, "access") == 0) num = 33;
    else if (strcmp(name, "symlink") == 0) num = 57;
    else if (strcmp(name, "readlink") == 0) num = 58;
    else if (strcmp(name, "rename") == 0) num = 128;
    else if (strcmp(name, "getpid") == 0) num = 20;
    else if (strcmp(name, "getuid") == 0) num = 24;
    else if (strcmp(name, "geteuid") == 0) num = 25;
    else if (strcmp(name, "getppid") == 0) num = 39;
    else if (strcmp(name, "getegid") == 0) num = 43;
    else if (strcmp(name, "getgid") == 0) num = 47;
    else if (strcmp(name, "umask") == 0) num = 60;
    else if (strcmp(name, "gettimeofday") == 0) num = 116;
    else if (strcmp(name, "issetugid") == 0) num = 327;
    else if (strcmp(name, "getentropy") == 0) num = 500;
    else if (strcmp(name, "getrlimit") == 0) num = 194;
    else if (strcmp(name, "setrlimit") == 0) num = 195;
    else if (strcmp(name, "getpgrp") == 0) num = 81;
    else if (strcmp(name, "getpgid") == 0) num = 151;
    else if (strcmp(name, "setpgid") == 0) num = 82;
    else if (strcmp(name, "setsid") == 0) num = 147;
    else if (strcmp(name, "kill") == 0) num = 37;
    else if (strcmp(name, "getitimer") == 0) num = 86;
    else if (strcmp(name, "setitimer") == 0) num = 83;
    else if (strcmp(name, "sigprocmask") == 0) num = 48;
    else if (strcmp(name, "sigpending") == 0) num = 52;
    else if (strcmp(name, "minherit") == 0) num = 250;
    else if (strcmp(name, "getsid") == 0) num = 310;
    else if (strcmp(name, "getgroups") == 0) num = 79;
    else if (strcmp(name, "getdtablesize") == 0) num = 89;
    else if (strcmp(name, "mkdir") == 0) num = 136;
    else if (strcmp(name, "mkfifo") == 0) num = 132;
    else if (strcmp(name, "pathconf") == 0) num = 191;
    else if (strcmp(name, "fpathconf") == 0) num = 192;
    else if (strcmp(name, "utimes") == 0) num = 138;
    else if (strcmp(name, "futimes") == 0) num = 139;
    else if (strcmp(name, "chown") == 0) num = 16;
    else if (strcmp(name, "fchown") == 0) num = 123;
    else if (strcmp(name, "lchown") == 0) num = 364;
    else if (strcmp(name, "socketpair") == 0) num = 135;
    else if (strcmp(name, "readv") == 0) num = 120;
    else if (strcmp(name, "writev") == 0) num = 121;
    else if (strcmp(name, "socket") == 0) num = 97;
    else if (strcmp(name, "shutdown") == 0) num = 134;
    else if (strcmp(name, "getsockopt") == 0) num = 118;
    else if (strcmp(name, "setsockopt") == 0) num = 105;
    else if (strcmp(name, "bind") == 0) num = 104;
    else if (strcmp(name, "listen") == 0) num = 106;
    else if (strcmp(name, "getsockname") == 0) num = 32;
    else if (strcmp(name, "connect") == 0) num = 98;
    else if (strcmp(name, "accept") == 0) num = 30;
    else if (strcmp(name, "sendto") == 0) num = 133;
    else if (strcmp(name, "sendmsg") == 0) num = 28;
    else if (strcmp(name, "recvmsg") == 0) num = 27;
    else if (strcmp(name, "recvfrom") == 0) num = 29;
    else if (strcmp(name, "getpeername") == 0) num = 31;
    else if (strcmp(name, "poll") == 0) num = 230;
    else if (strcmp(name, "kqueue") == 0) num = 362;
    else if (strcmp(name, "kevent") == 0) num = 363;
    else if (strcmp(name, "fchmod") == 0) num = 124;
    else if (strcmp(name, "madvise") == 0) num = 75;
    else if (strcmp(name, "mincore") == 0) num = 78;
    else if (strcmp(name, "mprotect") == 0) num = 74;
    else if (strcmp(name, "munmap") == 0) num = 73;
    else if (strcmp(name, "msync") == 0) num = 65;
    else if (strcmp(name, "mlock") == 0) num = 203;
    else if (strcmp(name, "munlock") == 0) num = 204;
    else if (strcmp(name, "getpriority") == 0) num = 100;
    else if (strcmp(name, "setpriority") == 0) num = 96;
    else if (strcmp(name, "getrusage") == 0) num = 117;
    else if (strcmp(name, "openat") == 0) num = 463;
    else if (strcmp(name, "unlinkat") == 0) num = 472;
    else if (strcmp(name, "mkdirat") == 0) num = 475;
    else if (strcmp(name, "renameat") == 0) num = 465;
    else if (strcmp(name, "faccessat") == 0) num = 466;
    else if (strcmp(name, "symlinkat") == 0) num = 474;
    else if (strcmp(name, "readlinkat") == 0) num = 473;
    else if (strcmp(name, "linkat") == 0) num = 471;
    else if (strcmp(name, "fchmodat") == 0) num = 467;
    else if (strcmp(name, "fchownat") == 0) num = 468;
    else if (strcmp(name, "mkfifoat") == 0) num = 553;
    else if (strcmp(name, "rmdir") == 0) num = 137;
    else if (strcmp(name, "lseek") == 0) num = 199;
    else if (strcmp(name, "truncate") == 0) num = 200;
    else if (strcmp(name, "ftruncate") == 0) num = 201;
    else return 0;
    int defined = 0;
    if (find_function(c->ir, name, &defined) == 0) return 0;
    A64Asm *a = c->as;
    int egi = find_global_idx(c->ir, "errno");
    /* Darwin's libc ORs the POSIX resource with 0x1000 before the syscall. */
    if (num == 194 || num == 195)
        a64_or_imm(a, A64_X0, A64_X0, 0x1000, 1);
    /* The public kill stub sets the POSIX flag in x2 before syscall 37. */
    if (num == 37)
        a64_movz(a, A64_X2, 1, 0, 1);
    a64_movz(a, A64_X16, (unsigned)num, 0, 1);
    a64_svc(a, 0x80);
    {
        int ok = a64_new_label(a);
        a64_cset(a, A64_X9, A64_CS, 1);
        if (egi >= 0) {
            int clear = a64_new_label(a);
            int stored = a64_new_label(a);
            note_page_reloc(c, (uint32_t)a->code.len, egi, NULL, -1);
            a64_word(a, 0x90000000u | (uint32_t)A64_X16);
            a64_word(a, 0x91000000u | ((uint32_t)A64_X16 << 5) | (uint32_t)A64_X16);
            a64_cbz(a, A64_X9, clear, 1);
            a64_str32(a, A64_X0, A64_X16, 0);
            a64_b(a, stored);
            a64_bind(a, clear);
            a64_str32(a, 31, A64_X16, 0);
            a64_bind(a, stored);
        }
        a64_cbz(a, A64_X9, ok, 1);
        /* How failure is reported depends on what the caller believes this
         * name is, so the two linking modes answer it differently.
         *
         * A lone freestanding TU has no libc at all: the body synthesised
         * here is the *only* implementation of write/open/... and it is a
         * raw syscall wrapper, so it reports the way __syscall does -- the
         * negated errno.  test_cg64_native pins that contract for every name
         * in the table (write(-1,..) == -9 and friends).
         *
         * Once another module joins the link there is a libc in the picture
         * and callers expect POSIX: -1, with the reason in errno.  The e2e
         * runtime tests check exactly that (chmod on a missing path).  errno
         * was stored above from the positive code, so collapsing the return
         * value to -1 loses nothing either way. */
        if (emit_multi_tu())
            a64_mov_imm64(a, A64_X0, (uint64_t)-1);
        else
            a64_neg(a, A64_X0, A64_X0, 1);
        a64_bind(a, ok);
    }
    return 1;
}

/* x0 holds a positive errno.  A same-TU int errno receives it. */
static void emit_store_errno(C64 *c, int egi) {
    if (egi < 0) return;
    A64Asm *a = c->as;
    note_page_reloc(c, (uint32_t)a->code.len, egi, NULL, -1);
    a64_word(a, 0x90000000u | (uint32_t)A64_X16);
    a64_word(a, 0x91000000u | ((uint32_t)A64_X16 << 5) | (uint32_t)A64_X16);
    a64_str32(a, A64_X0, A64_X16, 0);
}

/* Process termination: exit / _exit / _Exit / abort.
 *
 * Darwin has a single `exit` syscall (1) that takes the status in x0 and
 * never returns; `abort` is SIGABRT.  These are libc-level names rather than
 * syscalls, so the freestanding arm64 backend has to supply them itself or
 * every torture case that ends with exit() fails to compile.  A same-TU
 * definition still wins, so a program that implements its own exit keeps it.
 *
 * The emitted body cannot fall through, so a dummy return is appended: the
 * syscall is unreachable and the epilogue only exists to keep the function
 * well-formed for disassembly and for the branch-density bookkeeping. */
static int emit_exit_builtin(C64 *c, const char *name) {
    if (!name) return 0;
    int is_exit = strcmp(name, "exit") == 0;
    int is_atexit = strcmp(name, "_exit") == 0 || strcmp(name, "_Exit") == 0;
    int is_abort = strcmp(name, "abort") == 0;
    if (!is_exit && !is_atexit && !is_abort) return 0;
    int defined = 0;
    if (find_function(c->ir, name, &defined) == 0) return 0;
    A64Asm *a = c->as;
    if (is_abort) {
        /* Darwin abort() raises SIGABRT.  kill(getpid(), SIGABRT) is the
         * faithful lowering, and the pid has to be the real one: a hard-coded
         * 1 would signal whatever process happens to own pid 1 instead of us,
         * and the call would fall through to the _exit fallback below with the
         * wrong status. */
        a64_movz(a, A64_X16, 20, 0, 1); /* getpid */
        a64_svc(a, 0x80);
        /* getpid cannot fail, so x0 is the pid; Darwin kill takes (pid, sig)
         * in x0/x1. */
        a64_movz(a, A64_X16, 37, 0, 1); /* kill */
        a64_movz(a, A64_X1, 6, 0, 1);   /* SIGABRT */
        a64_svc(a, 0x80);
        /* kill only returns if the signal was blocked or ignored; fall back
         * to _exit(134) so abort() can never return to its caller and the
         * wait status still reads as SIGABRT. */
        a64_movz(a, A64_X16, 1, 0, 1);
        a64_movz(a, A64_X0, 134, 0, 1);
        a64_svc(a, 0x80);
        a64_ret(a, A64_LR);
        return 1;
    }
    /* exit/_exit/_Exit all take the status in x0 and trap straight into the
     * kernel, so atexit handlers do not run - matching _exit, and matching
     * what a freestanding program without libc can promise. */
    a64_movz(a, A64_X16, 1, 0, 1);
    a64_svc(a, 0x80);
    a64_ret(a, A64_LR);
    return 1;
}

/* getcwd.  Darwin writes the path with fcntl(F_GETPATH) into a 1024-byte
 * scratch buffer, then copies it if the caller has room.  A zero size is
 * EINVAL and a short buffer is ERANGE.  A same-TU definition still wins. */
static int emit_getcwd(C64 *c, const char *name) {
    if (!name || strcmp(name, "getcwd") != 0) return 0;
    int defined = 0;
    if (find_function(c->ir, name, &defined) == 0) return 0;

    A64Asm *a = c->as;
    int egi = find_global_idx(c->ir, "errno");
    int Lbadsz = a64_new_label(a);
    int Lopen_fail = a64_new_label(a);
    int Lctl_fail = a64_new_label(a);
    int Lrange = a64_new_label(a);
    int Lmeas = a64_new_label(a);
    int Lgot = a64_new_label(a);
    int Lcopy = a64_new_label(a);
    int Lcopied = a64_new_label(a);
    int Ldone = a64_new_label(a);

    a64_mov_reg(a, A64_X12, A64_X0, 1);
    a64_mov_reg(a, A64_X13, A64_X1, 1);
    a64_cbz(a, A64_X13, Lbadsz, 1);
    a64_sub_imm12(a, A64_SP, A64_SP, 1040, 0, 1, 0);
    a64_movz(a, A64_X2, 46, 0, 0); /* '.' */
    a64_str8(a, A64_X2, A64_SP, 0);
    a64_str8(a, 31, A64_SP, 1);
    a64_add_imm12(a, A64_X0, A64_SP, 0, 0, 1, 0); /* SP, not ORR/XZR */
    a64_movz(a, A64_X1, 0, 0, 1);
    a64_movz(a, A64_X2, 0, 0, 1);
    a64_movz(a, A64_X16, 5, 0, 1); /* open */
    a64_svc(a, 0x80);
    a64_cset(a, A64_X9, A64_CS, 1);
    a64_cbnz(a, A64_X9, Lopen_fail, 1);
    a64_mov_reg(a, A64_X14, A64_X0, 1);
    a64_mov_reg(a, A64_X0, A64_X14, 1);
    a64_movz(a, A64_X1, 50, 0, 1); /* F_GETPATH */
    a64_add_imm12(a, A64_X2, A64_SP, 16, 0, 1, 0);
    a64_movz(a, A64_X16, 92, 0, 1); /* fcntl */
    a64_svc(a, 0x80);
    a64_cset(a, A64_X9, A64_CS, 1);
    a64_cbnz(a, A64_X9, Lctl_fail, 1);
    a64_add_imm12(a, A64_X2, A64_SP, 16, 0, 1, 0);
    a64_movz(a, A64_X3, 0, 0, 1);
    a64_bind(a, Lmeas);
    a64_ldr8(a, A64_X4, A64_X2, 0);
    a64_cbz(a, A64_X4, Lgot, 0);
    a64_add_imm12(a, A64_X2, A64_X2, 1, 0, 1, 0);
    a64_add_imm12(a, A64_X3, A64_X3, 1, 0, 1, 0);
    a64_b(a, Lmeas);
    a64_bind(a, Lgot);
    a64_add_imm12(a, A64_X4, A64_X3, 1, 0, 1, 0);
    a64_cmp_reg(a, A64_X4, A64_X13, 1);
    a64_bcond(a, A64_HI, Lrange);
    a64_add_imm12(a, A64_X2, A64_SP, 16, 0, 1, 0);
    a64_mov_reg(a, A64_X5, A64_X12, 1);
    a64_mov_reg(a, A64_X6, A64_X4, 1);
    a64_bind(a, Lcopy);
    a64_cbz(a, A64_X6, Lcopied, 1);
    a64_ldr8(a, A64_X7, A64_X2, 0);
    a64_str8(a, A64_X7, A64_X5, 0);
    a64_add_imm12(a, A64_X2, A64_X2, 1, 0, 1, 0);
    a64_add_imm12(a, A64_X5, A64_X5, 1, 0, 1, 0);
    a64_sub_imm12(a, A64_X6, A64_X6, 1, 0, 1, 0);
    a64_b(a, Lcopy);
    a64_bind(a, Lcopied);
    a64_mov_reg(a, A64_X0, A64_X14, 1);
    a64_movz(a, A64_X16, 6, 0, 1); /* close */
    a64_svc(a, 0x80);
    a64_mov_reg(a, A64_X0, A64_X12, 1);
    a64_b(a, Ldone);
    a64_bind(a, Lopen_fail);
    emit_store_errno(c, egi);
    a64_movz(a, A64_X0, 0, 0, 1);
    a64_b(a, Ldone);
    a64_bind(a, Lctl_fail);
    a64_str64(a, A64_X0, A64_SP, 8);
    a64_mov_reg(a, A64_X0, A64_X14, 1);
    a64_movz(a, A64_X16, 6, 0, 1);
    a64_svc(a, 0x80);
    a64_ldr64(a, A64_X0, A64_SP, 8);
    emit_store_errno(c, egi);
    a64_movz(a, A64_X0, 0, 0, 1);
    a64_b(a, Ldone);
    a64_bind(a, Lrange);
    a64_mov_reg(a, A64_X0, A64_X14, 1);
    a64_movz(a, A64_X16, 6, 0, 1);
    a64_svc(a, 0x80);
    a64_movz(a, A64_X0, 34, 0, 1); /* ERANGE */
    emit_store_errno(c, egi);
    a64_movz(a, A64_X0, 0, 0, 1);
    a64_bind(a, Ldone);
    a64_add_imm12(a, A64_SP, A64_SP, 1040, 0, 1, 0);
    int Lret = a64_new_label(a);
    a64_b(a, Lret);
    a64_bind(a, Lbadsz);
    a64_movz(a, A64_X0, 22, 0, 1); /* EINVAL */
    emit_store_errno(c, egi);
    a64_movz(a, A64_X0, 0, 0, 1);
    a64_bind(a, Lret);
    return 1;
}

/* getenv.  The entry stub stores dyld's envp in the bss slot.  A same-TU
 * definition still wins.  Object files keep the call external. */
/* mmap / mmap2.
 *
 * Syscall 197 takes six arguments and reports failure the way libc does --
 * by returning MAP_FAILED, not -1 -- so it cannot join emit_io_builtin's
 * three-argument table.  It is variadic too: mmap's sixth argument (offset)
 * is absent on Darwin, whose kernel always maps whole pages, so the caller's
 * x5 is simply not forwarded.
 *
 * MAP_FAILED is (void *)-1, which is what a caller compares against.  errno
 * gets the positive code. */
static int emit_mmap(C64 *c, const char *name) {
    if (!name) return 0;
    int is_mmap = strcmp(name, "mmap") == 0;
    int is_mmap2 = strcmp(name, "mmap2") == 0;
    if (!is_mmap && !is_mmap2) return 0;
    int defined = 0;
    if (find_function(c->ir, name, &defined) == 0) return 0;

    A64Asm *a = c->as;
    int egi = find_global_idx(c->ir, "errno");
    int ok = a64_new_label(a);
    a64_movz(a, A64_X16, 197, 0, 1);
    a64_svc(a, 0x80);
    a64_cset(a, A64_X9, A64_CS, 1);
    if (egi >= 0) {
        int clear = a64_new_label(a);
        int stored = a64_new_label(a);
        note_page_reloc(c, (uint32_t)a->code.len, egi, NULL, -1);
        a64_word(a, 0x90000000u | (uint32_t)A64_X16);
        a64_word(a, 0x91000000u | ((uint32_t)A64_X16 << 5) | (uint32_t)A64_X16);
        a64_cbz(a, A64_X9, clear, 1);
        a64_str32(a, A64_X0, A64_X16, 0);
        a64_b(a, stored);
        a64_bind(a, clear);
        a64_str32(a, 31, A64_X16, 0);
        a64_bind(a, stored);
    }
    /* The kernel already returns -1 on failure, which is MAP_FAILED, so
     * there is nothing to rewrite here -- unlike emit_io_builtin, whose
     * return value is the negated errno. */
    a64_cbz(a, A64_X9, ok, 1);
    a64_mov_imm64(a, A64_X0, (uint64_t)-1);
    a64_bind(a, ok);
    (void)is_mmap2;
    return 1;
}

static int emit_getenv(C64 *c, const char *name) {
    if (!name || strcmp(name, "getenv") != 0) return 0;
    int defined = 0;
    if (find_function(c->ir, name, &defined) == 0) return 0;
    if (emit_object_mode() || c->environ_off < 0) return 0;

    A64Asm *a = c->as;
    int Lloop = a64_new_label(a);
    int Lcmp = a64_new_label(a);
    int Leq = a64_new_label(a);
    int Lnext = a64_new_label(a);
    int Lmiss = a64_new_label(a);
    int Ldone = a64_new_label(a);

    a64_mov_reg(a, A64_X1, A64_X0, 1);
    emit_environ_addr(c, A64_X0);
    a64_ldr64(a, A64_X0, A64_X0, 0);
    a64_cbz(a, A64_X0, Lmiss, 1);
    a64_bind(a, Lloop);
    a64_ldr64(a, A64_X2, A64_X0, 0);
    a64_cbz(a, A64_X2, Lmiss, 1);
    a64_mov_reg(a, A64_X4, A64_X2, 1);
    a64_mov_reg(a, A64_X5, A64_X1, 1);
    a64_bind(a, Lcmp);
    a64_ldr8(a, A64_X6, A64_X5, 0);
    a64_ldr8(a, A64_X7, A64_X4, 0);
    a64_cbz(a, A64_X6, Leq, 0);
    a64_cmp_reg(a, A64_X6, A64_X7, 0);
    a64_bcond(a, A64_NE, Lnext);
    a64_add_imm12(a, A64_X4, A64_X4, 1, 0, 1, 0);
    a64_add_imm12(a, A64_X5, A64_X5, 1, 0, 1, 0);
    a64_b(a, Lcmp);
    a64_bind(a, Leq);
    a64_cmp_imm12(a, A64_X7, 61, 0, 0);
    a64_bcond(a, A64_NE, Lnext);
    a64_add_imm12(a, A64_X0, A64_X4, 1, 0, 1, 0);
    a64_b(a, Ldone);
    a64_bind(a, Lnext);
    a64_add_imm12(a, A64_X0, A64_X0, 8, 0, 1, 0);
    a64_b(a, Lloop);
    a64_bind(a, Lmiss);
    a64_movz(a, A64_X0, 0, 0, 1);
    a64_bind(a, Ldone);
    return 1;
}

/* Bytes of outgoing stack traffic for one call: 8 per stack slot, plus
 * 16 when call_args[0] must be stashed before it is written to x8. */
static int is_va_builtin(const char *name) {
    return name && (strcmp(name, "va_start") == 0 || strcmp(name, "va_arg") == 0
                    || strcmp(name, "va_end") == 0);
}

/* GCC integer bit builtins stay named calls so x86 can emit lzcnt/tzcnt.
 * On arm64 they are instructions, not libc helpers.  `long` is 64-bit. */
static int is_bit_builtin(const char *bn) {
    if (!bn || strncmp(bn, "__builtin_", 10) != 0) return 0;
    return strstr(bn, "clz") || strstr(bn, "ctz") || strstr(bn, "ffs")
        || strstr(bn, "popcount") || strstr(bn, "parity")
        || strstr(bn, "clrsb") || strstr(bn, "bswap");
}

static int bit_is64(const char *bn) {
    if (strstr(bn, "bswap16") || strstr(bn, "bswap32")) return 0;
    if (strstr(bn, "bswap64")) return 1;
    size_t n = strlen(bn);
    if (n >= 2 && bn[n - 2] == 'l' && bn[n - 1] == 'l') return 1;
    if (n >= 1 && bn[n - 1] == 'l') return 1;
    return 0;
}

static int safe_tmp(int a, int b, int c, int d);

static void bit1(A64Asm *a, uint32_t base, int rd, int rn) {
    a64_word(a, base | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
}

/* CNT + ADDV into V31, which the allocator never assigns. */
static void emit_popc(A64Asm *a, int dst, int src, int is64) {
    a64_fmov_gp(a, FSCR, src, 1, is64);
    a64_word(a, 0x0E205800u | ((uint32_t)FSCR << 5) | (uint32_t)FSCR);
    a64_word(a, 0x0E31B800u | ((uint32_t)FSCR << 5) | (uint32_t)FSCR);
    a64_fmov_gp(a, dst, FSCR, 0, 0);
}

static void emit_bit_builtin(C64 *c, const IRInst *s) {
    A64Asm *a = c->as;
    const char *bn = s->call_name;
    if (s->dst < 0 || s->call_nargs < 1) return;
    int is64 = bit_is64(bn);
    int dst = dst_reg(c, s->dst);
    int src = load_op(c, s->call_args[0], dst);
    uint32_t clz = is64 ? 0xDAC01000u : 0x5AC01000u;
    uint32_t rbit = is64 ? 0xDAC00000u : 0x5AC00000u;
    if (strstr(bn, "bswap")) {
        if (strstr(bn, "16")) {
            /* rev + lsr 16 keeps only the swapped low half. */
            bit1(a, 0x5AC00800u, dst, src);
            a64_lsr_imm(a, dst, dst, 16, 0);
        } else if (is64) {
            bit1(a, 0xDAC00C00u, dst, src);
        } else {
            bit1(a, 0x5AC00800u, dst, src);
        }
    } else if (strstr(bn, "clrsb")) {
        bit1(a, is64 ? 0xDAC01400u : 0x5AC01400u, dst, src);
    } else if (strstr(bn, "ffs")) {
        /* 0 if src is zero, otherwise ctz(src)+1. */
        int tmp = safe_tmp(src, -1, -1, -1);
        bit1(a, rbit, tmp, src);
        bit1(a, clz, tmp, tmp);
        a64_cmp_imm12(a, src, 0, 0, is64);
        a64_word(a, 0x1A8007E0u | ((uint32_t)(tmp & 31) << 16)
                                | (uint32_t)(dst & 31));
    } else if (strstr(bn, "ctz")) {
        bit1(a, rbit, dst, src);
        bit1(a, clz, dst, dst);
    } else if (strstr(bn, "clz")) {
        bit1(a, clz, dst, src);
    } else if (strstr(bn, "popcount") || strstr(bn, "parity")) {
        emit_popc(a, dst, src, is64);
        if (strstr(bn, "parity"))
            a64_and_imm(a, dst, dst, 1, 0);
    }
    commit(c, s->dst, dst);
}

/* rotateleftN / rotaterightN arrive with the __builtin_ prefix stripped.
 * 32- and 64-bit counts are masked by RORV.  8- and 16-bit rotates are
 * built from shifts so the high bits of a W register stay out of it. */
static int rotate_width(const char *bn) {
    if (!bn) return 0;
    int left = strncmp(bn, "rotateleft", 10) == 0;
    int right = strncmp(bn, "rotateright", 11) == 0;
    if (!left && !right) return 0;
    const char *p = bn + (left ? 10 : 11);
    if (strcmp(p, "8") == 0) return 8;
    if (strcmp(p, "16") == 0) return 16;
    if (strcmp(p, "32") == 0) return 32;
    if (strcmp(p, "64") == 0) return 64;
    return 0;
}

static int bitrev_width(const char *bn) {
    if (!bn || strncmp(bn, "bitreverse", 10) != 0) return 0;
    const char *p = bn + 10;
    if (strcmp(p, "8") == 0) return 8;
    if (strcmp(p, "16") == 0) return 16;
    if (strcmp(p, "32") == 0) return 32;
    if (strcmp(p, "64") == 0) return 64;
    return 0;
}

static void emit_bitrev_builtin(C64 *c, const IRInst *s, int bits) {
    if (s->dst < 0 || s->call_nargs < 1) return;
    int dst = dst_reg(c, s->dst);
    int src = load_op(c, s->call_args[0], dst);
    bit1(c->as, bits == 64 ? 0xDAC00000u : 0x5AC00000u, dst, src);
    if (bits == 8) a64_lsr_imm(c->as, dst, dst, 24, 0);
    else if (bits == 16) a64_lsr_imm(c->as, dst, dst, 16, 0);
    commit(c, s->dst, dst);
}

static void emit_rotate_n(C64 *c, const IRInst *s, int width, int right) {
    if (s->dst < 0 || s->call_nargs < 2) return;
    A64Asm *a = c->as;
    int is64 = width == 64;
    int dst = dst_reg(c, s->dst);
    int src = load_op(c, s->call_args[0], dst);
    int sh = load_op(c, s->call_args[1], src);
    if (width >= 32) {
        if (right) {
            a64_rorv(a, dst, src, sh, is64);
        } else {
            int nreg = safe_tmp(src, sh, dst, -1);
            a64_neg(a, nreg, sh, is64);
            a64_rorv(a, dst, src, nreg, is64);
        }
        commit(c, s->dst, dst);
        return;
    }
    uint64_t mask = width == 8 ? 0xffu : 0xffffu;
    int vreg = safe_tmp(src, sh, dst, -1);
    int nreg = safe_tmp(src, sh, dst, vreg);
    a64_and_imm(a, vreg, src, mask, 0);
    a64_and_imm(a, nreg, sh, (uint64_t)(width - 1), 0);
    int wreg = safe_tmp(vreg, nreg, dst, -1);
    a64_movz(a, wreg, (unsigned)width, 0, 0);
    a64_sub_reg(a, wreg, wreg, nreg, A64_LSL, 0, 0, 0);
    if (right) {
        a64_lsrv(a, dst, vreg, nreg, 0);
        a64_lslv(a, vreg, vreg, wreg, 0);
    } else {
        a64_lslv(a, dst, vreg, nreg, 0);
        a64_lsrv(a, vreg, vreg, wreg, 0);
    }
    a64_or_reg(a, dst, dst, vreg, 0);
    a64_and_imm(a, dst, dst, mask, 0);
    commit(c, s->dst, dst);
}

static void emit_rotate_builtin(C64 *c, const IRInst *s, int width) {
    int right = s->call_name && strstr(s->call_name, "right") != NULL;
    emit_rotate_n(c, s, width, right);
}

/* C23 stdc_* bit ops, names after the __builtin_ prefix is stripped.
 * The width is the argument's width, not the register width: an
 * unsigned char zero has 8 leading zeros, not 32. */
static int stdc_op(const char *n) {
    if (!n || strncmp(n, "stdc_", 5) != 0) return 0;
    const char *p = n + 5;
    if (strcmp(p, "leading_zeros") == 0) return 1;
    if (strcmp(p, "leading_ones") == 0) return 2;
    if (strcmp(p, "trailing_zeros") == 0) return 3;
    if (strcmp(p, "trailing_ones") == 0) return 4;
    if (strcmp(p, "first_leading_zero") == 0) return 5;
    if (strcmp(p, "first_leading_one") == 0) return 6;
    if (strcmp(p, "first_trailing_zero") == 0) return 7;
    if (strcmp(p, "first_trailing_one") == 0) return 8;
    if (strcmp(p, "count_ones") == 0) return 9;
    if (strcmp(p, "count_zeros") == 0) return 10;
    if (strcmp(p, "has_single_bit") == 0) return 11;
    if (strcmp(p, "bit_width") == 0) return 12;
    if (strcmp(p, "bit_floor") == 0) return 13;
    if (strcmp(p, "bit_ceil") == 0) return 14;
    if (strcmp(p, "rotate_left") == 0) return 15;
    if (strcmp(p, "rotate_right") == 0) return 16;
    return 0;
}

static void stdc_mask(A64Asm *a, int dst, int src, int bits) {
    if (bits >= 32) {
        if (dst != src) a64_mov_reg(a, dst, src, bits >= 64);
        return;
    }
    a64_and_imm(a, dst, src, (1ull << bits) - 1, 0);
}

static void stdc_lz(A64Asm *a, int dst, int src, int bits) {
    if (bits >= 64) {
        bit1(a, 0xDAC01000u, dst, src);
        return;
    }
    bit1(a, 0x5AC01000u, dst, src);
    if (bits < 32)
        a64_sub_imm12(a, dst, dst, (unsigned)(32 - bits), 0, 0, 0);
}

/* rbit+clz.  A zero input yields the register width (32 or 64). */
static void stdc_tz_raw(A64Asm *a, int dst, int src, int bits) {
    int is64 = bits >= 64;
    bit1(a, is64 ? 0xDAC00000u : 0x5AC00000u, dst, src);
    bit1(a, is64 ? 0xDAC01000u : 0x5AC01000u, dst, dst);
}

static void stdc_tz(A64Asm *a, int dst, int src, int bits) {
    if (bits >= 32) {
        stdc_tz_raw(a, dst, src, bits);
        return;
    }
    int Lnz = a64_new_label(a);
    int Lend = a64_new_label(a);
    a64_cbnz(a, src, Lnz, 0);
    a64_movz(a, dst, (unsigned)bits, 0, 0);
    a64_b(a, Lend);
    a64_bind(a, Lnz);
    stdc_tz_raw(a, dst, src, bits);
    a64_bind(a, Lend);
}

static void stdc_width_minus(A64Asm *a, int dst, int count, int bits) {
    if (count == dst) {
        int tmp = safe_tmp(dst, -1, -1, -1);
        a64_movz(a, tmp, (unsigned)bits, 0, 0);
        a64_sub_reg(a, dst, tmp, count, A64_LSL, 0, 0, 0);
    } else {
        a64_movz(a, dst, (unsigned)bits, 0, 0);
        a64_sub_reg(a, dst, dst, count, A64_LSL, 0, 0, 0);
    }
}

static void stdc_not_masked(A64Asm *a, int dst, int src, int bits) {
    a64_mvn(a, dst, src, bits >= 64);
    stdc_mask(a, dst, dst, bits);
}

static int emit_stdc_builtin(C64 *c, const IRInst *s) {
    int op = stdc_op(s->call_name);
    if (!op || s->dst < 0) return 0;
    int bits = vw(c, s->call_args[0]) * 8;
    if (bits <= 0) bits = 32;
    if (op == 15 || op == 16) {
        if (s->call_nargs < 2) return 0;
        if (bits != 8 && bits != 16 && bits != 32 && bits != 64) bits = 32;
        emit_rotate_n(c, s, bits, op == 16);
        return 1;
    }
    if (s->call_nargs < 1) return 0;
    A64Asm *a = c->as;
    int dst = dst_reg(c, s->dst);
    int src = load_op(c, s->call_args[0], dst);
    int is64 = bits >= 64;
    int tmp = safe_tmp(src, dst, -1, -1);
    if (op == 1) {
        stdc_mask(a, tmp, src, bits);
        stdc_lz(a, dst, bits >= 32 ? src : tmp, bits);
    } else if (op == 2 || op == 5) {
        stdc_not_masked(a, tmp, src, bits);
        stdc_lz(a, dst, tmp, bits);
        if (op == 5) stdc_width_minus(a, dst, dst, bits);
    } else if (op == 3) {
        stdc_mask(a, tmp, src, bits);
        stdc_tz(a, dst, bits >= 32 ? src : tmp, bits);
    } else if (op == 4 || op == 7) {
        stdc_not_masked(a, tmp, src, bits);
        stdc_tz(a, dst, tmp, bits);
        if (op == 7) {
            int Ladd = a64_new_label(a);
            int Lend = a64_new_label(a);
            a64_cmp_imm12(a, dst, (unsigned)bits, 0, 0);
            a64_bcond(a, A64_NE, Ladd);
            a64_movz(a, dst, 0, 0, 0);
            a64_b(a, Lend);
            a64_bind(a, Ladd);
            a64_add_imm12(a, dst, dst, 1, 0, 0, 0);
            a64_bind(a, Lend);
        }
    } else if (op == 6 || op == 12) {
        stdc_mask(a, tmp, src, bits);
        stdc_lz(a, dst, bits >= 32 ? src : tmp, bits);
        stdc_width_minus(a, dst, dst, bits);
    } else if (op == 8) {
        int Lz = a64_new_label(a);
        int Lend = a64_new_label(a);
        stdc_mask(a, tmp, src, bits);
        a64_cbz(a, tmp, Lz, is64);
        stdc_tz_raw(a, dst, tmp, bits);
        a64_add_imm12(a, dst, dst, 1, 0, 0, 0);
        a64_b(a, Lend);
        a64_bind(a, Lz);
        a64_movz(a, dst, 0, 0, 0);
        a64_bind(a, Lend);
    } else if (op == 9 || op == 10 || op == 11) {
        stdc_mask(a, tmp, src, bits);
        emit_popc(a, dst, bits >= 32 ? src : tmp, is64);
        if (op == 10) stdc_width_minus(a, dst, dst, bits);
        else if (op == 11) {
            a64_cmp_imm12(a, dst, 1, 0, 0);
            a64_cset(a, dst, A64_EQ, 0);
        }
    } else if (op == 13 || op == 14) {
        /* bit_floor is 0 for a zero input.  bit_ceil(0) is 1, and a
         * power of two that does not fit the type becomes 0. */
        int Lz = a64_new_label(a);
        int Lpow = a64_new_label(a);
        int Lzero = a64_new_label(a);
        int Lend = a64_new_label(a);
        int sh = safe_tmp(src, dst, tmp, -1);
        stdc_mask(a, tmp, src, bits);
        a64_cbz(a, tmp, Lz, is64);
        if (op == 14) {
            a64_sub_imm12(a, sh, tmp, 1, 0, is64, 0);
            a64_and_reg(a, sh, tmp, sh, is64);
            a64_cbz(a, sh, Lpow, is64);
            stdc_lz(a, dst, tmp, bits);
            stdc_width_minus(a, sh, dst, bits);
            a64_cmp_imm12(a, sh, (unsigned)bits, 0, 0);
            a64_bcond(a, A64_EQ, Lzero);
            a64_movz(a, tmp, 1, 0, is64);
            a64_lslv(a, dst, tmp, sh, is64);
            a64_b(a, Lend);
            a64_bind(a, Lpow);
            if (tmp != dst) a64_mov_reg(a, dst, tmp, is64);
            a64_b(a, Lend);
        } else {
            stdc_lz(a, dst, tmp, bits);
            stdc_width_minus(a, sh, dst, bits);
            a64_sub_imm12(a, sh, sh, 1, 0, 0, 0);
            a64_movz(a, tmp, 1, 0, is64);
            a64_lslv(a, dst, tmp, sh, is64);
            a64_b(a, Lend);
        }
        a64_bind(a, Lz);
        a64_movz(a, dst, op == 14 ? 1u : 0u, 0, is64);
        a64_b(a, Lend);
        a64_bind(a, Lzero);
        a64_movz(a, dst, 0, 0, is64);
        a64_bind(a, Lend);
    }
    commit(c, s->dst, dst);
    return 1;
}

/* __builtin_clzg/ctzg/clrsbg/ffsg/popcountg/parityg.  The width is the
 * argument's, and clzg/ctzg take an optional value to return for zero. */
static int bitg_kind(const char *n) {
    if (!n) return 0;
    if (strcmp(n, "__builtin_clzg") == 0) return 1;
    if (strcmp(n, "__builtin_ctzg") == 0) return 2;
    if (strcmp(n, "__builtin_clrsbg") == 0) return 3;
    if (strcmp(n, "__builtin_ffsg") == 0) return 4;
    if (strcmp(n, "__builtin_popcountg") == 0) return 5;
    if (strcmp(n, "__builtin_parityg") == 0) return 6;
    return 0;
}

static void emit_bitg_builtin(C64 *c, const IRInst *s) {
    int kind = bitg_kind(s->call_name);
    if (!kind || s->dst < 0 || s->call_nargs < 1) return;
    A64Asm *a = c->as;
    int bits = vw(c, s->call_args[0]) * 8;
    if (bits <= 0) bits = 32;
    int is64 = bits >= 64;
    int dst = dst_reg(c, s->dst);
    int src = load_op(c, s->call_args[0], dst);
    int fb = -1;
    if ((kind == 1 || kind == 2) && s->call_nargs >= 2)
        fb = load_op(c, s->call_args[1], dst);
    int tmp = safe_tmp(src, dst, fb, -1);
    stdc_mask(a, tmp, src, bits);
    if ((kind == 1 || kind == 2) && s->call_nargs >= 2) {
        int Lnz = a64_new_label(a);
        int Lend = a64_new_label(a);
        a64_cbnz(a, tmp, Lnz, is64);
        if (fb != dst) a64_mov_reg(a, dst, fb, 0);
        a64_b(a, Lend);
        a64_bind(a, Lnz);
        if (kind == 1) stdc_lz(a, dst, tmp, bits);
        else stdc_tz(a, dst, tmp, bits);
        a64_bind(a, Lend);
    } else if (kind == 1) {
        stdc_lz(a, dst, tmp, bits);
    } else if (kind == 2) {
        stdc_tz(a, dst, tmp, bits);
    } else if (kind == 3) {
        if (bits >= 64) {
            bit1(a, 0xDAC01400u, dst, tmp);
        } else if (bits >= 32) {
            bit1(a, 0x5AC01400u, dst, tmp);
        } else {
            int Lend = a64_new_label(a);
            a64_lsl_imm(a, dst, tmp, (unsigned)(32 - bits), 0);
            bit1(a, 0x5AC01400u, dst, dst);
            a64_cmp_imm12(a, dst, (unsigned)(bits - 1), 0, 0);
            a64_bcond(a, A64_LS, Lend);
            a64_movz(a, dst, (unsigned)(bits - 1), 0, 0);
            a64_bind(a, Lend);
        }
    } else if (kind == 4) {
        uint32_t rbit = is64 ? 0xDAC00000u : 0x5AC00000u;
        uint32_t clz = is64 ? 0xDAC01000u : 0x5AC01000u;
        int t2 = safe_tmp(src, dst, tmp, fb);
        bit1(a, rbit, t2, tmp);
        bit1(a, clz, t2, t2);
        a64_cmp_imm12(a, tmp, 0, 0, is64);
        a64_word(a, 0x1A8007E0u | ((uint32_t)(t2 & 31) << 16) | (uint32_t)(dst & 31));
    } else {
        emit_popc(a, dst, tmp, is64);
        if (kind == 6) a64_and_imm(a, dst, dst, 1, 0);
    }
    commit(c, s->dst, dst);
}

/* add_sat/sub_sat/mul_sat.  The range is the argument type: an unsigned
 * char stops at 255, and a signed char stops at -128..127. */
static int sat_kind(const char *n) {
    if (!n) return 0;
    if (strcmp(n, "add_sat") == 0) return 1;
    if (strcmp(n, "sub_sat") == 0) return 2;
    if (strcmp(n, "mul_sat") == 0) return 3;
    return 0;
}

static void sat_limit(A64Asm *a, int rd, int bits, int is_u, int want_min) {
    int is64 = bits >= 64;
    if (is_u) {
        if (want_min) a64_movz(a, rd, 0, 0, is64);
        else if (bits >= 64) a64_movn(a, rd, 0, 0, 1);
        else if (bits >= 32) a64_movn(a, rd, 0, 0, 0);
        else a64_movz(a, rd, (unsigned)((1u << bits) - 1), 0, 0);
        return;
    }
    if (bits >= 64) {
        if (want_min) a64_movz(a, rd, 0x8000, 3, 1);
        else a64_movn(a, rd, 0x8000, 3, 1);
    } else if (bits >= 32) {
        if (want_min) a64_movz(a, rd, 0x8000, 1, 0);
        else a64_movn(a, rd, 0x8000, 1, 0);
    } else if (bits >= 16) {
        if (want_min) a64_movn(a, rd, 32767, 0, 0);
        else a64_movz(a, rd, 32767, 0, 0);
    } else {
        if (want_min) a64_movn(a, rd, 127, 0, 0);
        else a64_movz(a, rd, 127, 0, 0);
    }
}

static void sat_prep(A64Asm *a, int dst, int src, int bits, int is_u) {
    if (bits >= 64) {
        if (dst != src) a64_mov_reg(a, dst, src, 1);
    } else if (bits >= 32) {
        if (dst != src) a64_mov_reg(a, dst, src, 0);
    } else if (is_u) {
        a64_uxt(a, dst, src, (unsigned)(bits / 8), 0);
    } else {
        a64_sxt(a, dst, src, (unsigned)(bits / 8), 0);
    }
}

static void sat_clamp(A64Asm *a, int dst, int val, int bits, int is_u) {
    int is64 = bits >= 64;
    int mx = safe_tmp(dst, val, -1, -1);
    int Lok = a64_new_label(a);
    int Lend = a64_new_label(a);
    sat_limit(a, mx, bits, is_u, 0);
    if (is_u) {
        a64_cmp_reg(a, val, mx, is64);
        a64_bcond(a, A64_LS, Lok);
        a64_mov_reg(a, dst, mx, is64);
        a64_b(a, Lend);
    } else {
        int mn = safe_tmp(dst, val, mx, -1);
        int Lhi = a64_new_label(a);
        sat_limit(a, mn, bits, 0, 1);
        a64_cmp_reg(a, val, mx, is64);
        a64_bcond(a, A64_GT, Lhi);
        a64_cmp_reg(a, val, mn, is64);
        a64_bcond(a, A64_GE, Lok);
        a64_mov_reg(a, dst, mn, is64);
        a64_b(a, Lend);
        a64_bind(a, Lhi);
        a64_mov_reg(a, dst, mx, is64);
        a64_b(a, Lend);
    }
    a64_bind(a, Lok);
    if (dst != val) a64_mov_reg(a, dst, val, is64);
    a64_bind(a, Lend);
}

static void sat_mull(A64Asm *a, int rd, int rn, int rm, int is_signed) {
    uint32_t base = is_signed ? 0x9B207C00u : 0x9BA07C00u;
    a64_word(a, base | ((uint32_t)(rm & 31) << 16)
                    | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
}

static void sat_mulh(A64Asm *a, int rd, int rn, int rm, int is_signed) {
    uint32_t base = is_signed ? 0x9B407C00u : 0x9BC07C00u;
    a64_word(a, base | ((uint32_t)(rm & 31) << 16)
                    | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
}

static void emit_sat_builtin(C64 *c, const IRInst *s) {
    int kind = sat_kind(s->call_name);
    if (!kind || s->dst < 0 || s->call_nargs < 2) return;
    A64Asm *a = c->as;
    int bits = s->width > 0 ? s->width * 8 : 32;
    if (bits != 8 && bits != 16 && bits != 32 && bits != 64) bits = 32;
    int is_u = s->is_unsigned;
    int is64 = bits >= 64;
    int dst = dst_reg(c, s->dst);
    int va = load_op(c, s->call_args[0], dst);
    int vb = load_op(c, s->call_args[1], va);
    int ra = safe_tmp(dst, va, vb, -1);
    int rb = safe_tmp(dst, va, vb, ra);
    sat_prep(a, ra, va, bits, is_u);
    sat_prep(a, rb, vb, bits, is_u);
    if (bits < 32) {
        int tmp = safe_tmp(dst, ra, rb, -1);
        if (kind == 2 && is_u) {
            int Lzero = a64_new_label(a);
            int Lend = a64_new_label(a);
            a64_cmp_reg(a, ra, rb, 0);
            a64_bcond(a, A64_CC, Lzero);
            a64_sub_reg(a, dst, ra, rb, A64_LSL, 0, 0, 0);
            a64_b(a, Lend);
            a64_bind(a, Lzero);
            a64_movz(a, dst, 0, 0, 0);
            a64_bind(a, Lend);
        } else {
            if (kind == 1) a64_add_reg(a, tmp, ra, rb, A64_LSL, 0, 0, 0);
            else if (kind == 2) a64_sub_reg(a, tmp, ra, rb, A64_LSL, 0, 0, 0);
            else a64_mul(a, tmp, ra, rb, 0);
            sat_clamp(a, dst, tmp, bits, is_u);
        }
        commit(c, s->dst, dst);
        return;
    }
    if (kind == 3) {
        int prod = safe_tmp(dst, ra, rb, -1);
        int Lsat = a64_new_label(a);
        int Lend = a64_new_label(a);
        int sgn = prod;
        if (bits == 32) {
            sat_mull(a, prod, ra, rb, !is_u);
            if (is_u) {
                int hi = safe_tmp(dst, prod, ra, rb);
                a64_lsr_imm(a, hi, prod, 32, 1);
                a64_cbnz(a, hi, Lsat, 1);
            } else {
                int ext = safe_tmp(dst, prod, ra, rb);
                a64_sxt(a, ext, prod, 4, 1);
                a64_cmp_reg(a, ext, prod, 1);
                a64_bcond(a, A64_NE, Lsat);
            }
            a64_mov_reg(a, dst, prod, 0);
            a64_b(a, Lend);
        } else {
            int hi = safe_tmp(dst, prod, ra, rb);
            a64_mul(a, prod, ra, rb, 1);
            sat_mulh(a, hi, ra, rb, !is_u);
            sgn = hi;
            if (is_u) {
                a64_cbnz(a, hi, Lsat, 1);
            } else {
                int sign = safe_tmp(dst, prod, hi, ra);
                a64_asr_imm(a, sign, prod, 63, 1);
                a64_cmp_reg(a, hi, sign, 1);
                a64_bcond(a, A64_NE, Lsat);
            }
            a64_mov_reg(a, dst, prod, 1);
            a64_b(a, Lend);
        }
        a64_bind(a, Lsat);
        if (is_u) {
            sat_limit(a, dst, bits, 1, 0);
        } else {
            int Lmin = a64_new_label(a);
            a64_cmp_imm12(a, sgn, 0, 0, 1);
            a64_bcond(a, A64_MI, Lmin);
            sat_limit(a, dst, bits, 0, 0);
            a64_b(a, Lend);
            a64_bind(a, Lmin);
            sat_limit(a, dst, bits, 0, 1);
        }
        a64_bind(a, Lend);
        commit(c, s->dst, dst);
        return;
    }
    {
        int tmp = safe_tmp(dst, ra, rb, -1);
        int Lsat = a64_new_label(a);
        int Lok = a64_new_label(a);
        int Lend = a64_new_label(a);
        if (kind == 1) a64_add_reg(a, tmp, ra, rb, A64_LSL, 0, is64, 1);
        else a64_sub_reg(a, tmp, ra, rb, A64_LSL, 0, is64, 1);
        if (is_u) {
            a64_bcond(a, kind == 1 ? A64_CS : A64_CC, Lsat);
            a64_mov_reg(a, dst, tmp, is64);
            a64_b(a, Lend);
            a64_bind(a, Lsat);
            if (kind == 1) sat_limit(a, dst, bits, 1, 0);
            else a64_movz(a, dst, 0, 0, is64);
        } else {
            int Lmin = a64_new_label(a);
            a64_bcond(a, A64_VC, Lok);
            a64_cmp_imm12(a, ra, 0, 0, is64);
            a64_bcond(a, A64_MI, Lmin);
            sat_limit(a, dst, bits, 0, 0);
            a64_b(a, Lend);
            a64_bind(a, Lmin);
            sat_limit(a, dst, bits, 0, 1);
            a64_b(a, Lend);
            a64_bind(a, Lok);
            a64_mov_reg(a, dst, tmp, is64);
        }
        a64_bind(a, Lend);
    }
    commit(c, s->dst, dst);
}

/* Byte count of a stack argument that reaches memory as a pointer to the
 * object, or 0 when this one does not.  Two flags mean that: CALL_ARG_BLOB
 * for a MEMORY-class aggregate and CALL_ARG_AGG for an aggregate or vector
 * passed as an anonymous variadic argument.  ir.c hands over the size; a
 * missing one falls back to 1 so the copy still happens. */

/* Alignment the outgoing stack slot for a copy-in argument must carry.
 * AAPCS64 rounds an anonymous argument's slot up to the argument's own
 * alignment, and the callee's va_arg does the same rounding when it reads,
 * so both sides have to agree -- CALL_ARG_AGG arguments take their alignment
 * from ir.c, while a MEMORY blob parameter aligns its slot to 16. */
static int copy_arg_align(unsigned char f) {
    if (f & CALL_ARG_ALIGN64) return 64;
    if (f & CALL_ARG_ALIGN32) return 32;
    if (f & CALL_ARG_ALIGN16) return 16;
    if (f & CALL_ARG_BLOB) return 16;
    return 0;
}

static int call_arg_copy_bytes(const IRInst *inst, int k) {
    if (!inst->call_arg_on_stack)
        return 0;
    unsigned char f = inst->call_arg_on_stack[k];
    if (!(f & (CALL_ARG_BLOB | CALL_ARG_AGG)))
        return 0;
    if (inst->call_arg_nbytes && inst->call_arg_nbytes[k] > 0)
        return inst->call_arg_nbytes[k];
    return 1;
}

/* True when this stack argument is an object the caller copies into the
 * outgoing area rather than passing a pointer to it.  AAPCS64 C.12 draws the
 * line at 16 bytes: anything wider travels as a pointer to the object. */
static int call_arg_is_inline_copy(const IRInst *inst, int k) {
    int n = call_arg_copy_bytes(inst, k);
    return n > 0 && n <= 16;
}

static int outgoing_stack_bytes(C64 *c, const IRInst *s) {
    if (!s->call_nargs) return s->align16 == A64_MARK_SRET ? 16 : 0;
    if (s->call_name && strcmp(s->call_name, "__syscall") == 0) return 0;
    if (is_va_builtin(s->call_name)) return 0;
    int start = s->align16 == A64_MARK_SRET ? 1 : 0;
    int gp = 0, fp = 0, st = 0;
    for (int i = start; i < s->call_nargs; i++) {
        unsigned f = s->call_arg_on_stack ? s->call_arg_on_stack[i] : 0;
        int scalar = scalar_fp_val(c, s->call_args[i]);
        int qv = vec16_val(c, s->call_args[i]);
        if ((scalar || qv) && !(f & CALL_ARG_STACK) && fp < 8) {
            fp++;
            continue;
        }
        if ((f & CALL_ARG_HFA) && !(f & CALL_ARG_STACK) && fp < 8) {
            fp++;
            continue;
        }
        int cpy = call_arg_copy_bytes(s, i);
        if (cpy) {
            /* Must match the reservation in the call sequence exactly, or the
             * outgoing area comes out smaller than the callee expects and the
             * object lands somewhere the callee never looks.  The slot's start
             * is rounded up to the argument's own alignment either way -- an
             * indirect (wider-than-16) argument is a pointer in that slot,
             * and va_arg still aligns before reading it. */
            int al = copy_arg_align((unsigned char)f);
            if (al > 8 && (st % al)) st = (st + al - 1) & ~(al - 1);
            st += (cpy > 16) ? 8 : ((cpy + 7) & ~7);
        } else if ((f & CALL_ARG_STACK) || gp >= 8 || scalar || qv) {
            if (qv) {
                if (st & 15) st = (st + 15) & ~15;
                st += 16;
            } else {
                st += 8;
            }
        } else {
            gp++;
        }
    }
    int bytes = st;
    if (s->align16 == A64_MARK_SRET) bytes += 16;
    return bytes;
}

/* A scratch that is not one of the named registers (x0/x1/x16/x17 are
 * never allocated, so one of them is always free of homes). */
static int safe_tmp(int a, int b, int c, int d) {
    int cand[4] = { SCR0, SCR1, A64_X0, A64_X1 };
    for (int i = 0; i < 4; i++) {
        int t = cand[i];
        if (t != a && t != b && t != c && t != d) return t;
    }
    return SCR0;
}

static void place_from(C64 *c, IRValue v, int src) {
    if (v < 0) return;
    int hr = home_reg(c, v);
    if (hr < 0)
        frame_store(c, src, spill_off(c, v), 8, 0);
    else if (hr != src)
        a64_mov_reg(c->as, hr, src, 1);
}

/* Deliver two hardware registers into SSA homes without either write
 * destroying the other source. */
static void place_pair(C64 *c, IRValue lo, IRValue hi, int s0, int s1) {
    if (hi < 0) { place_from(c, lo, s0); return; }
    int h0 = home_reg(c, lo);
    int h1 = home_reg(c, hi);
    if (h0 == s1) {
        int tmp = safe_tmp(s0, s1, h0, h1);
        a64_mov_reg(c->as, tmp, s1, 1);
        place_from(c, lo, s0);
        place_from(c, hi, tmp);
    } else if (h1 == s0) {
        int tmp = safe_tmp(s0, s1, h0, h1);
        a64_mov_reg(c->as, tmp, s0, 1);
        place_from(c, hi, s1);
        place_from(c, lo, tmp);
    } else {
        place_from(c, hi, s1);
        place_from(c, lo, s0);
    }
}

/* Copy `sz` bytes from src to dest.  Both addresses are advanced.
 * x0/x1 are not allocatable, so one of them can hold the chunk. */
static void copy_bytes(C64 *c, int dest, int src, int sz) {
    A64Asm *a = c->as;
    int data = A64_X0;
    if (data == dest || data == src) data = A64_X1;
    if (data == dest || data == src) data = SCR0;
    int off = 0;
    while (off < sz) {
        int left = sz - off;
        int w = 1;
        if (left >= 8 && (off & 7) == 0) w = 8;
        else if (left >= 4 && (off & 3) == 0) w = 4;
        else if (left >= 2 && (off & 1) == 0) w = 2;
        emit_load(c, data, src, w, 1);
        emit_store(c, data, dest, w);
        off += w;
        if (off < sz) {
            a64_add_imm12(a, src, src, (unsigned)w, 0, 1, 0);
            a64_add_imm12(a, dest, dest, (unsigned)w, 0, 1, 0);
        }
    }
}

/* Darwin va_list is one pointer.  va_start stores the address of the
 * first anonymous stack slot; va_arg reads 8-byte slots and advances. */
static void emit_va(C64 *c, const IRInst *s) {
    A64Asm *a = c->as;
    if (strcmp(s->call_name, "va_end") == 0) return;

    int ap_home = home_reg(c, s->call_args[0]);
    int ap = ap_home >= 0 ? ap_home : SCR0;
    if (ap_home < 0)
        frame_load(c, ap, spill_off(c, s->call_args[0]), 8, 1, 0);

    if (strcmp(s->call_name, "va_start") == 0) {
        int cur = (ap == SCR1) ? A64_X0 : SCR1;
        emit_fp_addr(c, cur, c->vararg_off);
        a64_str64(a, cur, ap, 0);
        return;
    }

    int cur = (ap == SCR1) ? A64_X0 : SCR1;
    a64_ldr64(a, cur, ap, 0);
    int sz = s->imm > 0 ? (int)s->imm : 0;
    int indirect = sz > 16 || s->force_stack;
    int nxt = A64_X0;
    if (nxt == cur || nxt == ap) nxt = A64_X1;
    if (nxt == cur || nxt == ap) nxt = SCR0;
    /* Round this argument's slot up the way the caller rounded it -- on the
     * *offset* from the start of the variadic area, not on the address.  The
     * two only agree if both use the same quantity, and the address cannot
     * serve: the stack is guaranteed 16-aligned, so rounding a 32-aligned
     * argument's address up skipped a slot the caller had left alone and the
     * callee faulted reading it. */
    int al = (s->align16 >= 32) ? 32 : (s->align16 == 16 ? 16 : 0);
    if (al > 8) {
        emit_fp_addr(c, SCR0, c->vararg_off);
        a64_sub_reg(a, SCR1, cur, SCR0, A64_LSL, 0, 1, 0);
        a64_add_imm12(a, SCR1, SCR1, (unsigned)(al - 1), 0, 1, 0);
        a64_and_imm(a, SCR1, SCR1, (uint64_t)(~(uint64_t)(al - 1)), 1);
        a64_add_reg(a, cur, SCR0, SCR1, A64_LSL, 0, 1, 0);
    }
    /* Every anonymous slot is a multiple of 8 bytes, which is what the caller
     * reserves; rounding here keeps a small aggregate from sliding the
     * arguments after it out of step. */
    int adv = (sz == 0 || indirect) ? 8 : ((sz + 7) & ~7);
    a64_add_imm12(a, nxt, cur, (unsigned)adv, 0, 1, 0);
    a64_str64(a, nxt, ap, 0);

    if (sz == 0) {
        int w = s->width ? (int)s->width : 8;
        if (w != 1 && w != 2 && w != 4 && w != 8) w = 8;
        if (scalar_fp_val(c, s->dst)) {
            int h = fp_home(c, s->dst);
            int d = h >= 0 ? h : FSCR;
            if (w == 8) a64_ldr_d(a, d, cur, 0);
            else a64_ldr_s(a, d, cur, 0);
            if (h < 0) commit_fp(c, s->dst, d);
            return;
        }
        int d = dst_reg(c, s->dst);
        int tmp = d;
        if (tmp == cur || tmp == ap)
            tmp = safe_tmp(cur, ap, -1, -1);
        emit_load(c, tmp, cur, w, s->is_unsigned);
        if (tmp != d) a64_mov_reg(a, d, tmp, w == 8);
        commit(c, s->dst, d);
        return;
    }

    if (indirect)
        a64_ldr64(a, cur, cur, 0);
    int dest_home = s->call_nargs >= 2 ? home_reg(c, s->call_args[1]) : -1;
    int base;
    if (dest_home >= 0 && dest_home != cur && dest_home != nxt)
        base = dest_home;
    else {
        base = safe_tmp(cur, ap, nxt, dest_home);
        if (dest_home >= 0)
            a64_mov_reg(a, base, dest_home, 1);
        else if (s->call_nargs >= 2)
            frame_load(c, base, spill_off(c, s->call_args[1]), 8, 1, 0);
    }
    /* nxt is free once *ap has been stored, and it is not `base`. */
    a64_mov_reg(a, nxt, base, 1);
    copy_bytes(c, nxt, cur, sz);
    place_from(c, s->dst, base);
}

/* Names after the IR strips the __builtin_ prefix.  A trailing f/l is the
 * width suffix; long double is IEEE double on Darwin, so 'l' uses the
 * 64-bit instruction.  A user function of the same name still wins. */
static int fp_named(const char *n, const char *root) {
    size_t L = strlen(root);
    if (strncmp(n, root, L) != 0) return 0;
    char s = n[L];
    return s == '\0' || ((s == 'f' || s == 'l') && n[L + 1] == '\0');
}

static int fp_builtin_kind(const char *n, int *nargs) {
    if (!n) return 0;
    struct { const char *root; int kind; int narg; } tab[] = {
        {"copysign", 1, 2}, {"nearbyint", 2, 1}, {"floor", 3, 1},
        {"trunc", 4, 1}, {"round", 5, 1}, {"ceil", 6, 1},
        {"fabs", 7, 1}, {"sqrt", 8, 1}, {"fmin", 9, 2},
        {"fmax", 10, 2}, {"fma", 11, 3}, {"rint", 2, 1},
        {"powi", 12, 2}, {"nextafter", 13, 2},
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++) {
        if (fp_named(n, tab[i].root)) {
            *nargs = tab[i].narg;
            return tab[i].kind;
        }
    }
    return 0;
}

static void fp_unop(A64Asm *a, uint32_t base_d, int rd, int rn, int isd) {
    uint32_t base = isd ? base_d : (base_d & ~0x00400000u);
    a64_word(a, base | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
}

static void fp_binop(A64Asm *a, uint32_t base_d, int rd, int rn, int rm, int isd) {
    uint32_t base = isd ? base_d : (base_d & ~0x00400000u);
    a64_word(a, base | ((uint32_t)(rm & 31) << 16)
                    | ((uint32_t)(rn & 31) << 5)
                    | (uint32_t)(rd & 31));
}

static int fp_result(C64 *c, IRValue v, int fallback) {
    int h = fp_home(c, v);
    return h >= 0 ? h : fallback;
}

static void emit_fp_builtin(C64 *c, const IRInst *s, int kind) {
    A64Asm *a = c->as;
    if (s->dst < 0) return;
    int isd = vw(c, s->call_args[0]) == 8;
    if (kind == 1) {
        /* copysign: clear the sign bit, then OR in the sign of arg2. */
        int mag = load_fp(c, s->call_args[0], -1);
        int sgn = load_fp(c, s->call_args[1], mag);
        a64_fmov_gp(a, A64_X0, mag, 0, isd);
        a64_fmov_gp(a, A64_X1, sgn, 0, isd);
        unsigned sign = isd ? 63u : 31u;
        a64_lsl_imm(a, A64_X0, A64_X0, 1, isd);
        a64_lsr_imm(a, A64_X0, A64_X0, 1, isd);
        a64_lsr_imm(a, A64_X1, A64_X1, sign, isd);
        a64_lsl_imm(a, A64_X1, A64_X1, sign, isd);
        a64_or_reg(a, A64_X0, A64_X0, A64_X1, isd);
        int dst = fp_result(c, s->dst, (mag == FSCR || sgn == FSCR) ? A64_V30 : FSCR);
        a64_fmov_gp(a, dst, A64_X0, 1, isd);
        commit_fp(c, s->dst, dst);
        return;
    }
    if (kind == 11) {
        /* Park all three in GP first: a spilled third operand would
         * otherwise reuse the only two vector scratches. */
        int r = load_fp(c, s->call_args[0], -1);
        a64_fmov_gp(a, A64_X0, r, 0, isd);
        r = load_fp(c, s->call_args[1], -1);
        a64_fmov_gp(a, A64_X1, r, 0, isd);
        r = load_fp(c, s->call_args[2], -1);
        a64_fmov_gp(a, SCR0, r, 0, isd);
        a64_fmov_gp(a, A64_V0, A64_X0, 1, isd);
        a64_fmov_gp(a, A64_V1, A64_X1, 1, isd);
        a64_fmov_gp(a, A64_V2, SCR0, 1, isd);
        int dst = fp_result(c, s->dst, A64_V3);
        /* FMADD Dd, Dn, Dm, Da = arg0 * arg1 + arg2. */
        uint32_t base = isd ? 0x1F400000u : 0x1F000000u;
        a64_word(a, base | ((uint32_t)A64_V1 << 16) | ((uint32_t)A64_V2 << 10)
                       | ((uint32_t)A64_V0 << 5) | (uint32_t)(dst & 31));
        commit_fp(c, s->dst, dst);
        return;
    }
    if (kind == 9 || kind == 10) {
        int a0 = load_fp(c, s->call_args[0], -1);
        int a1 = load_fp(c, s->call_args[1], a0);
        int dst = fp_result(c, s->dst, a0);
        /* FMINNM/FMAXNM: a quiet NaN loses to the numeric operand.
         * Plain FMIN/FMAX return the NaN on this CPU. */
        fp_binop(a, kind == 9 ? 0x1E607800u : 0x1E606800u, dst, a0, a1, isd);
        commit_fp(c, s->dst, dst);
        return;
    }
    if (kind == 12) {
        /* powi(x, n).  n == 0 yields 1, including powi(NaN, 0).  A negative
         * exponent inverts at the end.  INT_MIN negates to itself in 32 bits
         * and the logical shift still walks that magnitude. */
        int nr = load_op(c, s->call_args[1], -1);
        if (nr != A64_X0) a64_mov_reg(a, A64_X0, nr, 0);
        int src = load_fp(c, s->call_args[0], -1);
        a64_fmov_reg(a, A64_V1, src, isd);
        unsigned one_hw = isd ? 0x3FF0u : 0x3F80u;
        a64_movz(a, A64_X2, one_hw, isd ? 3 : 1, isd);
        a64_fmov_gp(a, A64_V0, A64_X2, 1, isd);
        int Lpos = a64_new_label(a);
        int Lloop = a64_new_label(a);
        int Lsq = a64_new_label(a);
        int Linv = a64_new_label(a);
        int Ldone = a64_new_label(a);
        a64_movz(a, A64_X1, 0, 0, 0);
        a64_cmp_imm12(a, A64_X0, 0, 0, 0);
        a64_bcond(a, A64_GE, Lpos);
        a64_movz(a, A64_X1, 1, 0, 0);
        a64_neg(a, A64_X0, A64_X0, 0);
        a64_bind(a, Lpos);
        a64_bind(a, Lloop);
        a64_cbz(a, A64_X0, Linv, 0);
        a64_and_imm(a, A64_X3, A64_X0, 1, 0);
        a64_cbz(a, A64_X3, Lsq, 0);
        a64_fmul(a, A64_V0, A64_V0, A64_V1, isd);
        a64_bind(a, Lsq);
        a64_fmul(a, A64_V1, A64_V1, A64_V1, isd);
        a64_lsr_imm(a, A64_X0, A64_X0, 1, 0);
        a64_b(a, Lloop);
        a64_bind(a, Linv);
        a64_cbz(a, A64_X1, Ldone, 0);
        a64_fmov_gp(a, A64_V2, A64_X2, 1, isd);
        a64_fdiv(a, A64_V0, A64_V2, A64_V0, isd);
        a64_bind(a, Ldone);
        int dst = fp_result(c, s->dst, A64_V0);
        if (dst != A64_V0) a64_fmov_reg(a, dst, A64_V0, isd);
        commit_fp(c, s->dst, dst);
        return;
    }
    if (kind == 13) {
        /* nextafter(x, y): the adjacent encoding of x toward y.
         * ±0 steps to the tiniest subnormal with y's sign.  A NaN
         * in either argument produces a NaN.  Equal values return y,
         * so nextafter(+0, -0) keeps the sign. */
        int xa = load_fp(c, s->call_args[0], -1);
        a64_fmov_gp(a, A64_X0, xa, 0, isd);
        int ya = load_fp(c, s->call_args[1], xa);
        a64_fmov_gp(a, A64_X1, ya, 0, isd);
        a64_fmov_gp(a, A64_V0, A64_X0, 1, isd);
        a64_fmov_gp(a, A64_V1, A64_X1, 1, isd);
        unsigned exp_sh = isd ? 52u : 23u;
        unsigned exp_max = isd ? 0x7ffu : 0xffu;
        unsigned frac_sh = isd ? 12u : 9u;
        unsigned sign = isd ? 63u : 31u;
        int Lnan = a64_new_label(a);
        int Lchk_y = a64_new_label(a);
        int Lstep = a64_new_label(a);
        int Lnz = a64_new_label(a);
        int Ldec = a64_new_label(a);
        int Lbits = a64_new_label(a);
        int Ly = a64_new_label(a);
        int Ldone = a64_new_label(a);
        a64_lsr_imm(a, A64_X2, A64_X0, exp_sh, isd);
        a64_and_imm(a, A64_X2, A64_X2, exp_max, isd);
        a64_cmp_imm12(a, A64_X2, exp_max, 0, isd);
        a64_bcond(a, A64_NE, Lchk_y);
        a64_lsl_imm(a, A64_X3, A64_X0, frac_sh, isd);
        a64_cbnz(a, A64_X3, Lnan, isd);
        a64_bind(a, Lchk_y);
        a64_lsr_imm(a, A64_X2, A64_X1, exp_sh, isd);
        a64_and_imm(a, A64_X2, A64_X2, exp_max, isd);
        a64_cmp_imm12(a, A64_X2, exp_max, 0, isd);
        a64_bcond(a, A64_NE, Lstep);
        a64_lsl_imm(a, A64_X3, A64_X1, frac_sh, isd);
        a64_cbz(a, A64_X3, Lstep, isd);
        a64_bind(a, Lnan);
        a64_fadd(a, A64_V0, A64_V0, A64_V1, isd);
        a64_b(a, Ldone);
        a64_bind(a, Lstep);
        a64_fcmp(a, A64_V0, A64_V1, isd);
        a64_bcond(a, A64_EQ, Ly);
        a64_lsl_imm(a, A64_X2, A64_X0, 1, isd);
        a64_lsr_imm(a, A64_X2, A64_X2, 1, isd);
        a64_lsl_imm(a, A64_X3, A64_X1, 1, isd);
        a64_lsr_imm(a, A64_X3, A64_X3, 1, isd);
        a64_cbnz(a, A64_X2, Lnz, isd);
        a64_lsr_imm(a, A64_X0, A64_X1, sign, isd);
        a64_lsl_imm(a, A64_X0, A64_X0, sign, isd);
        a64_add_imm12(a, A64_X0, A64_X0, 1, 0, isd, 0);
        a64_b(a, Lbits);
        a64_bind(a, Lnz);
        a64_eor_reg(a, SCR0, A64_X0, A64_X1, isd);
        a64_lsr_imm(a, SCR0, SCR0, sign, isd);
        a64_cbnz(a, SCR0, Ldec, isd);
        a64_cmp_reg(a, A64_X2, A64_X3, isd);
        a64_bcond(a, A64_HI, Ldec);
        a64_add_imm12(a, A64_X0, A64_X0, 1, 0, isd, 0);
        a64_b(a, Lbits);
        a64_bind(a, Ldec);
        a64_sub_imm12(a, A64_X0, A64_X0, 1, 0, isd, 0);
        a64_bind(a, Lbits);
        a64_fmov_gp(a, A64_V0, A64_X0, 1, isd);
        a64_b(a, Ldone);
        a64_bind(a, Ly);
        a64_fmov_reg(a, A64_V0, A64_V1, isd);
        a64_bind(a, Ldone);
        int dst = fp_result(c, s->dst, A64_V0);
        if (dst != A64_V0) a64_fmov_reg(a, dst, A64_V0, isd);
        commit_fp(c, s->dst, dst);
        return;
    }
    int src = load_fp(c, s->call_args[0], -1);
    int dst = fp_result(c, s->dst, src == FSCR ? A64_V30 : FSCR);
    uint32_t base = 0x1E60C000u; /* fabs */
    if (kind == 8) base = 0x1E61C000u;       /* sqrt */
    else if (kind == 6) base = 0x1E64C000u;  /* ceil  = frintp */
    else if (kind == 3) base = 0x1E654000u;  /* floor = frintm */
    else if (kind == 4) base = 0x1E65C000u;  /* trunc = frintz */
    else if (kind == 5) base = 0x1E664000u;  /* round = frinta */
    else if (kind == 2) base = 0x1E67C000u;  /* rint / nearbyint = frinti */
    fp_unop(a, base, dst, src, isd);
    commit_fp(c, s->dst, dst);
}

/* __builtin_trap arrives as a call named __builtin_trap; __builtin_abort and
 * libc abort() both arrive as `abort` and are handled by emit_exit_builtin,
 * which raises SIGABRT.  brk #1 is what clang emits and the kernel reports
 * SIGTRAP.  debugtrap is not noreturn, so the instructions after it stay in
 * the function.  A user-defined function wins. */
static int emit_trap_builtin(C64 *c, const char *name) {
    if (!name || (strcmp(name, "__builtin_trap") != 0
                  && strcmp(name, "unreachable") != 0
                  && strcmp(name, "debugtrap") != 0))
        return 0;
    int defined = 0;
    if (find_function(c->ir, name, &defined) == 0) return 0;
    a64_word(c->as, 0xD4200020u);
    return 1;
}

/* __builtin_fpclassify(nan, inf, normal, subnormal, zero, x).
 * The category codes are integers; x is float or double. */
static void emit_fpclassify_builtin(C64 *c, const IRInst *s) {
    A64Asm *a = c->as;
    int isd = vw(c, s->call_args[5]) == 8;
    int fv = load_fp(c, s->call_args[5], -1);
    /* x16/x17 only. The category codes already sit in allocatable
     * registers, and overwriting one of them drops the result. */
    a64_fmov_gp(a, SCR0, fv, 0, isd);
    if (isd) {
        a64_lsr_imm(a, SCR1, SCR0, 52, 1);
        a64_and_imm(a, SCR1, SCR1, 0x7ff, 1);
        a64_lsl_imm(a, SCR0, SCR0, 12, 1);
        a64_lsr_imm(a, SCR0, SCR0, 12, 1);
    } else {
        a64_lsr_imm(a, SCR1, SCR0, 23, 0);
        a64_and_imm(a, SCR1, SCR1, 0xff, 0);
        a64_lsl_imm(a, SCR0, SCR0, 9, 0);
        a64_lsr_imm(a, SCR0, SCR0, 9, 0);
    }
    int Lspec = a64_new_label(a);
    int Linf = a64_new_label(a);
    int Lsub = a64_new_label(a);
    int Lzero = a64_new_label(a);
    int Lnorm = a64_new_label(a);
    int Ldone = a64_new_label(a);
    a64_cmp_imm12(a, SCR1, isd ? 0x7ffu : 0xffu, 0, isd ? 1 : 0);
    a64_bcond(a, A64_EQ, Lspec);
    a64_cbz(a, SCR1, Lsub, isd ? 1 : 0);
    a64_b(a, Lnorm);
    a64_bind(a, Lspec);
    a64_cbz(a, SCR0, Linf, isd ? 1 : 0);
    int dst = dst_reg(c, s->dst);
    int r = load_op(c, s->call_args[0], dst);
    if (r != dst) a64_mov_reg(a, dst, r, 0);
    a64_b(a, Ldone);
    a64_bind(a, Linf);
    r = load_op(c, s->call_args[1], dst);
    if (r != dst) a64_mov_reg(a, dst, r, 0);
    a64_b(a, Ldone);
    a64_bind(a, Lsub);
    a64_cbz(a, SCR0, Lzero, isd ? 1 : 0);
    r = load_op(c, s->call_args[3], dst);
    if (r != dst) a64_mov_reg(a, dst, r, 0);
    a64_b(a, Ldone);
    a64_bind(a, Lzero);
    r = load_op(c, s->call_args[4], dst);
    if (r != dst) a64_mov_reg(a, dst, r, 0);
    a64_b(a, Ldone);
    a64_bind(a, Lnorm);
    r = load_op(c, s->call_args[2], dst);
    if (r != dst) a64_mov_reg(a, dst, r, 0);
    a64_bind(a, Ldone);
    commit(c, s->dst, dst);
}

/* GNU __builtin_apply(fn, args, size): call `fn` with the argument block
 * `args` captured by __builtin_apply_args, leaving the result registers in a
 * block whose address this expression produces.  __builtin_return then
 * returns those from the enclosing function.
 *
 * This is not a non-local jump.  Like x86's emit_builtin_apply it is an
 * ordinary indirect call with sp moved down for the outgoing stack
 * arguments; sp is put back before returning, so the call site sees a plain
 * call whose result is a pointer. */
static void emit_builtin_apply(C64 *c, const IRInst *s) {
    A64Asm *a = c->as;
    int res = c->apply_result_off;
    if (s->call_nargs < 3 || res == 0) return;

    /* f, args and size may themselves live in x0-x7, which the register
     * reload below overwrites -- stash all three first. */
    int f = load_op(c, s->call_args[0], -1);
    emit_fp_addr(c, SCR1, res + APPLY_SCRATCH_OFF + 0);
    a64_str64(a, f, SCR1, 0);
    int blk = load_op(c, s->call_args[1], f);
    emit_fp_addr(c, SCR1, res + APPLY_SCRATCH_OFF + 8);
    a64_str64(a, blk, SCR1, 0);
    (void)load_op(c, s->call_args[2], blk);

    /* sp, so it can be restored once the call is done. */
    mov_sp_like(a, SCR0, A64_SP);
    emit_fp_addr(c, SCR1, res + APPLY_SCRATCH_OFF + 16);
    a64_str64(a, SCR0, SCR1, 0);

    /* Outgoing stack arguments.  `size` is a runtime value and arm64 has no
     * rep movsb, so this always moves APPLY_STACK_MAX bytes rather than
     * exactly `size`; the callee reads only what its signature declares. */
    a64_sub_imm12(a, A64_SP, A64_SP, APPLY_STACK_MAX, 0, 1, 0);
    emit_fp_addr(c, SCR0, res + APPLY_SCRATCH_OFF + 8);
    a64_ldr64(a, SCR0, SCR0, 0);          /* SCR0 = argument block */
    mov_sp_like(a, SCR1, A64_SP);
    for (int k = 0; k < APPLY_STACK_MAX; k += 8) {
        a64_ldr64(a, A64_X0, SCR0, k);
        a64_str64(a, A64_X0, SCR1, k);
    }

    /* Reload the argument registers from the block.  SCR0/SCR1 are outside
     * x0-x7 and v0-v7, so nothing here disturbs the pointer being walked. */
    for (int i = 0; i < 8; i++) {
        a64_add_imm12(a, SCR1, SCR0, (unsigned)(APPLY_GP_OFF + i * 8), 0, 1, 0);
        a64_ldr64(a, A64_X0 + i, SCR1, 0);
    }
    for (int i = 0; i < 8; i++) {
        a64_add_imm12(a, SCR1, SCR0, (unsigned)(APPLY_V_OFF + i * 16), 0, 1, 0);
        ldr_q(a, A64_V0 + i, SCR1, 0);
    }

    emit_fp_addr(c, SCR0, res + APPLY_SCRATCH_OFF + 0);
    a64_ldr64(a, SCR0, SCR0, 0);
    a64_blr(a, SCR0);

    /* Park the result registers, then put sp back. */
    for (int i = 0; i < 8; i++) {
        emit_fp_addr(c, SCR1, res + i * 8);
        a64_str64(a, A64_X0 + i, SCR1, 0);
    }
    for (int i = 0; i < 8; i++) {
        emit_fp_addr(c, SCR1, res + APPLY_V_OFF + i * 16);
        str_q(a, A64_V0 + i, SCR1, 0);
    }
    emit_fp_addr(c, SCR1, res + APPLY_SCRATCH_OFF + 16);
    a64_ldr64(a, SCR1, SCR1, 0);
    mov_sp_like(a, A64_SP, SCR1);

    int d = dst_reg(c, s->dst);
    emit_fp_addr(c, d, res);
    commit(c, s->dst, d);
}

/* GNU __builtin_return(block): return from the enclosing function with the
 * value the applied function left in `block`. */
static void emit_builtin_return(C64 *c, const IRInst *s) {
    A64Asm *a = c->as;
    if (s->call_nargs < 1 || c->epilog_label < 0) return;
    int p = load_op(c, s->call_args[0], -1);
    /* An integer result came back in x0/x1, a float one in v0/v1.  Both are
     * restored; whichever this function was declared to return is the one its
     * own caller reads. */
    a64_ldr64(a, A64_X0, p, 0);
    a64_ldr64(a, A64_X1, p, 8);
    ldr_q(a, A64_V0, p, APPLY_V_OFF);
    ldr_q(a, A64_V1, p, APPLY_V_OFF + 16);
    a64_b(a, c->epilog_label);
}

static void emit_call(C64 *c, const IRInst *s) {
    A64Asm *a = c->as;
    int n = s->call_nargs;

    /* The raw-syscall intrinsic has its own x16/x0..x5 convention;
     * dispatch BEFORE the generic argument shuffle below, which would
     * move the number/arguments into x0..x7 and clobber source homes
     * that the syscall lowering needs to re-read. */
    if (s->call_name && strcmp(s->call_name, "__syscall") == 0) {
        emit_syscall(c, s);
        return;
    }
    if (s->call_name && emit_pac_builtin(c, s)) {
        return;
    }
    if (s->call_name && strcmp(s->call_name, "__builtin_apply_args") == 0) {
        /* The block was filled by the prologue; hand back its address. */
        int d = dst_reg(c, s->dst);
        emit_fp_addr(c, d, c->apply_args_off);
        commit(c, s->dst, d);
        return;
    }
    if (s->call_name && strcmp(s->call_name, "__builtin_apply") == 0) {
        emit_builtin_apply(c, s);
        return;
    }
    if (s->call_name && strcmp(s->call_name, "__builtin_return") == 0) {
        emit_builtin_return(c, s);
        return;
    }
    if (s->call_name && strcmp(s->call_name, "__fakecc_swp") == 0) {
        /* SWP Rs, Rt, [Rn].  Rs is the new value, Rt receives the old one.
         * kind in imm: bit0 acquire, bit1 release. */
        if (s->call_nargs < 2) return;
        int w = s->width;
        if (w != 1 && w != 2 && w != 4 && w != 8) {
            die_at(s->loc.file ? s->loc.file : c->fn->loc.file,
                   s->loc.line, s->loc.col,
                   "arm64 backend: atomic exchange width must be 1, 2, 4, or 8");
            return;
        }
        if ((w == 4 || w == 8) && s->call_args[1] >= 0
            && scalar_fp_val(c, s->call_args[1])) {
            /* SWP has no scalar-fp form.  Move the bits through GPRs. */
            int isd = w == 8;
            int fv = load_fp(c, s->call_args[1], -1);
            int bits = SCR0;
            a64_fmov_gp(a, bits, fv, 0, isd);
            int p = load_ptrv(c, s->call_args[0], bits);
            if (p == bits) {
                int t = safe_tmp(bits, -1, -1, -1);
                a64_mov_reg(a, t, bits, 1);
                bits = t;
                p = load_ptrv(c, s->call_args[0], bits);
            }
            int old = safe_tmp(p, bits, -1, -1);
            uint32_t base = isd ? 0xF8208000u : 0xB8208000u;
            int kind = (int)s->imm;
            if (kind < 0 || kind > 3) kind = 3;
            if (kind & 1) base |= 1u << 23;
            if (kind & 2) base |= 1u << 22;
            a64_word(a, base | ((uint32_t)(bits & 31) << 16)
                           | ((uint32_t)(p & 31) << 5)
                           | (uint32_t)(old & 31));
            if (s->dst >= 0) {
                int h = fp_home(c, s->dst);
                int d = h >= 0 ? h : FSCR;
                a64_fmov_gp(a, d, old, 1, isd);
                if (h < 0) commit_fp(c, s->dst, d);
            }
            return;
        }
        int p = load_ptrv(c, s->call_args[0], -1);
        int v = load_op(c, s->call_args[1], p);
        int d = s->dst >= 0 ? dst_reg(c, s->dst) : SCR0;
        if (p == d || p == v) {
            int t = safe_tmp(d, v, p == d ? -1 : p, -1);
            a64_mov_reg(a, t, p, 1);
            p = t;
        }
        if (v == d || v == p) {
            int t = safe_tmp(d, p, -1, -1);
            a64_mov_reg(a, t, v, 1);
            v = t;
        }
        uint32_t base = w == 1 ? 0x38208000u : w == 2 ? 0x78208000u
                      : w == 4 ? 0xB8208000u : 0xF8208000u;
        int kind = (int)s->imm;
        if (kind < 0 || kind > 3) kind = 3;
        if (kind & 1) base |= 1u << 23;
        if (kind & 2) base |= 1u << 22;
        a64_word(a, base | ((uint32_t)(v & 31) << 16)
                       | ((uint32_t)(p & 31) << 5)
                       | (uint32_t)(d & 31));
        if (!s->is_unsigned && w < 8)
            a64_sxt(a, d, d, (unsigned)w, 1);
        if (s->dst >= 0) commit(c, s->dst, d);
        return;
    }
    if (s->call_name && strcmp(s->call_name, "__fakecc_fadd") == 0) {
        /* No LDADD for scalar float.  Load, add, CAS, and retry with the
         * value CAS observed.  The compare is on the IEEE bits. */
        if (s->call_nargs < 2 || s->dst < 0) return;
        int w = s->width;
        if (w != 4 && w != 8) {
            die_at(s->loc.file ? s->loc.file : c->fn->loc.file,
                   s->loc.line, s->loc.col,
                   "arm64 backend: float atomic add width must be 4 or 8");
            return;
        }
        int isd = w == 8;
        int kind = (int)s->imm & 3;
        int sub = ((int)s->imm & 4) != 0;
        int ret_new = ((int)s->imm & 8) != 0;
        int fv = load_fp(c, s->call_args[1], -1);
        if (fv != A64_V30) a64_fmov_reg(a, A64_V30, fv, isd);
        int p0 = load_ptrv(c, s->call_args[0], -1);
        a64_mov_reg(a, A64_X0, p0, 1);
        int p = A64_X0;
        int old = SCR0;
        if (kind == 1 || kind == 3) {
            uint32_t ldb = kind == 1
                ? (isd ? 0xF8BFC000u : 0xB8BFC000u)
                : (isd ? 0xC8DFFC00u : 0x88DFFC00u);
            a64_word(a, ldb | ((uint32_t)(p & 31) << 5) | (uint32_t)(old & 31));
        } else {
            emit_load(c, old, p, w, 1);
        }
        int Lretry = a64_new_label(a);
        int Ldone = a64_new_label(a);
        a64_bind(a, Lretry);
        a64_fmov_gp(a, FSCR, old, 1, isd);
        if (sub) a64_fsub(a, FSCR, FSCR, A64_V30, isd);
        else a64_fadd(a, FSCR, FSCR, A64_V30, isd);
        int neu = SCR1;
        a64_fmov_gp(a, neu, FSCR, 0, isd);
        int rs = A64_X1;
        a64_mov_reg(a, rs, old, 1);
        uint32_t base = isd ? 0xC8A07C00u : 0x88A07C00u;
        if (kind & 1) base |= 1u << 22;
        if (kind & 2) base |= 1u << 15;
        a64_word(a, base | ((uint32_t)(rs & 31) << 16)
                       | ((uint32_t)(p & 31) << 5)
                       | (uint32_t)(neu & 31));
        a64_cmp_reg(a, rs, old, isd);
        a64_bcond(a, A64_EQ, Ldone);
        a64_mov_reg(a, old, rs, 1);
        a64_b(a, Lretry);
        a64_bind(a, Ldone);
        int bits = ret_new ? neu : old;
        int h = fp_home(c, s->dst);
        int d = h >= 0 ? h : FSCR;
        a64_fmov_gp(a, d, bits, 1, isd);
        if (h < 0) commit_fp(c, s->dst, d);
        return;
    }
    if (s->call_name && (strcmp(s->call_name, "__fakecc_ldadd") == 0 ||
                         strcmp(s->call_name, "__fakecc_ldclr") == 0 ||
                         strcmp(s->call_name, "__fakecc_ldeor") == 0 ||
                         strcmp(s->call_name, "__fakecc_ldset") == 0)) {
        /* LDADD/LDCLR/LDEOR/LDSET.  LDCLR clears the bits of Rs, so the
         * mask is inverted here; the IR still passes the original mask. */
        if (s->call_nargs < 2) return;
        int w = s->width;
        if (w != 1 && w != 2 && w != 4 && w != 8) {
            die_at(s->loc.file ? s->loc.file : c->fn->loc.file,
                   s->loc.line, s->loc.col,
                   "arm64 backend: atomic add width must be 1, 2, 4, or 8");
            return;
        }
        int p = load_ptrv(c, s->call_args[0], -1);
        int v = load_op(c, s->call_args[1], p);
        int d = s->dst >= 0 ? dst_reg(c, s->dst) : SCR0;
        if (p == d || p == v) {
            int t = safe_tmp(d, v, p == d ? -1 : p, -1);
            a64_mov_reg(a, t, p, 1);
            p = t;
        }
        if (v == d || v == p) {
            int t = safe_tmp(d, p, -1, -1);
            a64_mov_reg(a, t, v, 1);
            v = t;
        }
        int opc = strcmp(s->call_name, "__fakecc_ldclr") == 0 ? 1
                : strcmp(s->call_name, "__fakecc_ldeor") == 0 ? 2
                : strcmp(s->call_name, "__fakecc_ldset") == 0 ? 3 : 0;
        if (opc == 1) {
            int inv = safe_tmp(p, v, d, -1);
            a64_mvn(a, inv, v, w == 8);
            v = inv;
        }
        uint32_t base = w == 1 ? 0x38200000u : w == 2 ? 0x78200000u
                      : w == 4 ? 0xB8200000u : 0xF8200000u;
        base |= (uint32_t)opc << 12;
        int kind = (int)s->imm;
        if (kind < 0 || kind > 3) kind = 3;
        if (kind & 1) base |= 1u << 23;
        if (kind & 2) base |= 1u << 22;
        a64_word(a, base | ((uint32_t)(v & 31) << 16)
                       | ((uint32_t)(p & 31) << 5)
                       | (uint32_t)(d & 31));
        if (!s->is_unsigned && w < 8)
            a64_sxt(a, d, d, (unsigned)w, 1);
        if (s->dst >= 0) commit(c, s->dst, d);
        return;
    }
    if (s->call_name && strcmp(s->call_name, "__fakecc_cas") == 0) {
        /* CAS Rs, Rt, [Rn].  Rs is the expected value and is replaced by
         * the value loaded from memory.  Rt is the desired value. */
        if (s->call_nargs < 3) return;
        int w = s->width;
        if (w != 1 && w != 2 && w != 4 && w != 8) {
            die_at(s->loc.file ? s->loc.file : c->fn->loc.file,
                   s->loc.line, s->loc.col,
                   "arm64 backend: atomic compare-exchange width must be 1, 2, 4, or 8");
            return;
        }
        int fp_des = (w == 4 || w == 8) && s->call_args[2] >= 0
                     && scalar_fp_val(c, s->call_args[2]);
        int des = -1;
        if (fp_des) {
            /* CAS has no scalar-fp form.  Desired bits go through a GPR
             * before the address registers are filled. */
            int isd = w == 8;
            int fv = load_fp(c, s->call_args[2], -1);
            des = SCR0;
            a64_fmov_gp(a, des, fv, 0, isd);
        }
        int p = load_ptrv(c, s->call_args[0], fp_des ? des : -1);
        int ep = load_ptrv(c, s->call_args[1], p);
        if (ep == p) {
            int t = safe_tmp(p, des, -1, -1);
            a64_mov_reg(a, t, ep, 1);
            ep = t;
        }
        if (!fp_des)
            des = load_op(c, s->call_args[2], p);
        if (des == p || des == ep) {
            int t = safe_tmp(p, ep, -1, -1);
            a64_mov_reg(a, t, des, 1);
            des = t;
        }
        /* Unsigned load: CAS zero-extends the loaded byte/half/word. */
        int exp = safe_tmp(p, ep, des, -1);
        emit_load(c, exp, ep, w, 1);
        int rs = safe_tmp(p, ep, des, exp);
        a64_mov_reg(a, rs, exp, 1);
        uint32_t base = w == 1 ? 0x08A07C00u : w == 2 ? 0x48A07C00u
                      : w == 4 ? 0x88A07C00u : 0xC8A07C00u;
        int kind = (int)s->imm;
        if (kind < 0 || kind > 3) kind = 3;
        if (kind & 1) base |= 1u << 22;
        if (kind & 2) base |= 1u << 15;
        a64_word(a, base | ((uint32_t)(rs & 31) << 16)
                       | ((uint32_t)(p & 31) << 5)
                       | (uint32_t)(des & 31));
        a64_cmp_reg(a, rs, exp, w == 8);
        int home = s->dst >= 0 ? dst_reg(c, s->dst) : SCR0;
        int flag = home;
        if (flag == rs || flag == ep || flag == p || flag == des || flag == exp)
            flag = safe_tmp(rs, ep, p, des);
        a64_cset(a, flag, A64_EQ, 0);
        int Ldone = a64_new_label(a);
        a64_bcond(a, A64_EQ, Ldone);
        emit_store(c, rs, ep, w);
        a64_bind(a, Ldone);
        if (s->dst >= 0) place_from(c, s->dst, flag);
        return;
    }
    if (s->call_name && strcmp(s->call_name, "__fakecc_cas_old") == 0) {
        /* CAS Rs, Rt, [Rn] where Rs is the expected value itself.  The
         * result is the previous memory value, left in Rs by the instruction. */
        if (s->call_nargs < 3) return;
        int w = s->width;
        if (w != 1 && w != 2 && w != 4 && w != 8) {
            die_at(s->loc.file ? s->loc.file : c->fn->loc.file,
                   s->loc.line, s->loc.col,
                   "arm64 backend: atomic compare-exchange width must be 1, 2, 4, or 8");
            return;
        }
        int p = load_ptrv(c, s->call_args[0], -1);
        int exp = load_op(c, s->call_args[1], p);
        if (exp == p) {
            int t = safe_tmp(p, -1, -1, -1);
            a64_mov_reg(a, t, exp, 1);
            exp = t;
        }
        int des = load_op(c, s->call_args[2], p);
        if (des == p || des == exp) {
            int t = safe_tmp(p, exp, -1, -1);
            a64_mov_reg(a, t, des, 1);
            des = t;
        }
        int rs = safe_tmp(p, exp, des, -1);
        a64_mov_reg(a, rs, exp, 1);
        uint32_t base = w == 1 ? 0x08A07C00u : w == 2 ? 0x48A07C00u
                      : w == 4 ? 0x88A07C00u : 0xC8A07C00u;
        int kind = (int)s->imm;
        if (kind < 0 || kind > 3) kind = 3;
        if (kind & 1) base |= 1u << 22;
        if (kind & 2) base |= 1u << 15;
        a64_word(a, base | ((uint32_t)(rs & 31) << 16)
                       | ((uint32_t)(p & 31) << 5)
                       | (uint32_t)(des & 31));
        if (s->dst >= 0) place_from(c, s->dst, rs);
        return;
    }
    if (is_va_builtin(s->call_name)) {
        emit_va(c, s);
        return;
    }
    if (bitg_kind(s->call_name)) {
        int defined = 0;
        if (find_function(c->ir, s->call_name, &defined) != 0) {
            emit_bitg_builtin(c, s);
            return;
        }
    }
    if (sat_kind(s->call_name)) {
        int defined = 0;
        if (find_function(c->ir, s->call_name, &defined) != 0) {
            emit_sat_builtin(c, s);
            return;
        }
    }
    if (s->call_name && strcmp(s->call_name, "readcyclecounter") == 0) {
        int defined = 0;
        if (find_function(c->ir, s->call_name, &defined) != 0) {
            /* CNTVCT_EL0 is readable here. CTR_EL0 is not, so this is
             * a different system register from the cache-line one. */
            int dst = s->dst >= 0 ? dst_reg(c, s->dst) : A64_X0;
            a64_word(c->as, 0xD53BE040u | (uint32_t)(dst & 31));
            if (s->dst >= 0) commit(c, s->dst, dst);
            return;
        }
    }
    if (s->call_name && (strcmp(s->call_name, "arm_dmb") == 0 ||
                         strcmp(s->call_name, "arm_dsb") == 0 ||
                         strcmp(s->call_name, "arm_isb") == 0 ||
                         strcmp(s->call_name, "__fakecc_dmb") == 0)) {
        int defined = 0;
        if (find_function(c->ir, s->call_name, &defined) != 0) {
            const IRInst *d = s->call_nargs >= 1 ? def_inst(c, s->call_args[0]) : NULL;
            if (!d || d->op != IR_CONST || d->imm < 0 || d->imm > 15) {
                die_at(s->loc.file ? s->loc.file : c->fn->loc.file,
                       s->loc.line, s->loc.col,
                       "arm64 backend: %s operand must be a constant 0..15",
                       s->call_name);
                return;
            }
            uint32_t base = strcmp(s->call_name, "arm_dsb") == 0 ? 0xD503309Fu
                          : strcmp(s->call_name, "arm_isb") == 0 ? 0xD50330DFu
                          : 0xD50330BFu;
            a64_word(c->as, base | ((uint32_t)d->imm << 8));
            return;
        }
    }
    if (is_bit_builtin(s->call_name)) {
        emit_bit_builtin(c, s);
        return;
    }
    {
        int rw = rotate_width(s->call_name);
        int bw = bitrev_width(s->call_name);
        if ((rw && s->call_nargs >= 2) || (bw && s->call_nargs >= 1)) {
            int defined = 0;
            if (find_function(c->ir, s->call_name, &defined) != 0) {
                if (bw) emit_bitrev_builtin(c, s, bw);
                else emit_rotate_builtin(c, s, rw);
                return;
            }
        }
    }
    if (stdc_op(s->call_name)) {
        int defined = 0;
        if (find_function(c->ir, s->call_name, &defined) != 0) {
            if (emit_stdc_builtin(c, s)) return;
        }
    }
    if (s->call_name && strcmp(s->call_name, "fpclassify") == 0
        && s->call_nargs >= 6) {
        int defined = 0;
        if (find_function(c->ir, s->call_name, &defined) != 0) {
            emit_fpclassify_builtin(c, s);
            return;
        }
    }
    {
        int fp_narg = 0;
        int fp_kind = fp_builtin_kind(s->call_name, &fp_narg);
        if (fp_kind && s->call_nargs >= fp_narg) {
            int defined = 0;
            if (find_function(c->ir, s->call_name, &defined) != 0) {
                emit_fp_builtin(c, s, fp_kind);
                return;
            }
        }
    }
    if (emit_trap_builtin(c, s->call_name)) return;

    int sret = s->align16 == A64_MARK_SRET;
    int start = sret ? 1 : 0;
    int gp_at[8], gp_n = 0, gp_used = 0, fp_used = 0, st_used = 0;
    int hfa_at[8], hfa_v[8], hfa_n = 0;
    int fp_at[8], fp_vn[8], fp_n = 0;
    int stk_at[IR_CALL_MAX_ARGS];
    int stk_off[IR_CALL_MAX_ARGS];
    int stk_n = 0;
    int stk_bytes = 0;
    for (int i = start; i < n; i++) {
        unsigned f = s->call_arg_on_stack ? s->call_arg_on_stack[i] : 0;
        int scalar = scalar_fp_val(c, s->call_args[i]);
        int qv = vec16_val(c, s->call_args[i]);
        int cpyb = call_arg_copy_bytes(s, i);
        if (cpyb) {
            /* An object that reaches memory behind a pointer.  Wider than 16
             * bytes it is passed *indirectly* -- AAPCS64 C.12 -- so the slot
             * holds the pointer and the ordinary stack store below writes it.
             * Up to 16 bytes it is copied inline before the GP shuffle (see
             * the loop after the stack-argument stores). */
            int al = copy_arg_align((unsigned char)f);
            if (al > 8 && (stk_bytes % al))
                stk_bytes = (stk_bytes + al - 1) & ~(al - 1);
            if (cpyb > 16) cpyb = 0;    /* indirect from here on */
            stk_at[stk_n] = i;
            stk_off[stk_n] = stk_bytes;
            stk_bytes += cpyb ? ((cpyb + 7) & ~7) : 8;
            stk_n++;
            st_used++;
            continue;
        }
        if ((scalar || qv) && !(f & CALL_ARG_STACK) && fp_used < 8) {
            fp_at[fp_n] = i;
            fp_vn[fp_n] = fp_used++;
            fp_n++;
        } else if ((f & CALL_ARG_HFA) && !(f & CALL_ARG_STACK) && fp_used < 8) {
            hfa_at[hfa_n] = i;
            hfa_v[hfa_n] = fp_used++;
            hfa_n++;
        } else if ((f & CALL_ARG_STACK) || gp_used >= 8 || scalar || qv) {
            if (qv && (stk_bytes & 15))
                stk_bytes = (stk_bytes + 15) & ~15;
            stk_at[stk_n] = i;
            stk_off[stk_n] = stk_bytes;
            stk_bytes += qv ? 16 : 8;
            stk_n++;
            st_used++;
        } else {
            gp_at[gp_n++] = i;
            gp_used++;
        }
    }
    (void)st_used;

    /* AAPCS64 puts stack arguments at [sp, ...) and grows the callee's frame
     * downwards from there, so nothing the call touches may sit above sp; an
     * alloca() left its block exactly there.  Lowering sp by the size of the
     * area moves both clear of the block, and offsets within the area are
     * unchanged so the stores below need no edit.  It has to happen before
     * those stores, which address [sp+0 ..). */
    int swap_sp = c->dyn_sp_off != -1 && c->call_area > 0;
    if (swap_sp)
        a64_sub_imm12(a, A64_SP, A64_SP, (unsigned)c->call_area, 0, 1, 0);

    /* Stack slots first: they read homes that the GP shuffle will
     * overwrite.  The area sits at [sp+0 ..).  A 16-byte vector occupies
     * one aligned Q slot once v0–v7 are full. */
    for (int i = 0; i < stk_n; i++) {
        if (call_arg_is_inline_copy(s, stk_at[i])) continue;   /* copied later */
        IRValue av = s->call_args[stk_at[i]];
        int off = stk_off[i];
        if (vec16_val(c, av)) {
            int r = load_q(c, av, -1);
            str_q(a, r, A64_SP, off);
        } else if (scalar_fp_val(c, av)) {
            int r = load_fp(c, av, -1);
            if (vw(c, av) == 8) a64_str_d(a, r, A64_SP, off);
            else a64_str_s(a, r, A64_SP, off);
        } else {
            int r = load_op(c, av, -1);
            a64_str64(a, r, A64_SP, off);
        }
    }
    /* MEMORY-blob arguments, copied here -- before the GP shuffle below.
     * That shuffle is what consumes the incoming x0-x7, so a pointer living
     * in one of them is gone by the time the copy runs, and the sources are
     * exactly the values the shuffle wants to read.  Only x16/x17 are spare
     * at this point, so the destination is addressed off sp and the source
     * off the pointer instead of going through copy_bytes(), which wants a
     * third register for the chunk.  LDUR/STUR are unscaled, so neither
     * side has to be aligned. */
    for (int i = 0; i < stk_n; i++) {
        if (!call_arg_is_inline_copy(s, stk_at[i])) continue;
        int nb = call_arg_copy_bytes(s, stk_at[i]);
        int base = stk_off[i];
        /* Unsigned-offset forms: their Rn=31 means SP, while the unscaled
         * LDUR/STUR used by the rest of the frame helpers reads 31 as XZR
         * (`strb w0, [sp], #0` would assemble as `[xzr]` and fault). */
        if (base + nb > 4095)
            c64_die(c, s, "aggregate stack blob past the reachable outgoing area");
        int src = load_op(c, s->call_args[stk_at[i]], SCR0);
        int k = 0;
        for (; k + 8 <= nb; k += 8) {
            a64_ldr64(a, SCR0, src, k);
            a64_str64(a, SCR0, A64_SP, base + k);
        }
        for (; k < nb; k++) {
            a64_ldr8(a, SCR0, src, k);
            a64_str8(a, SCR0, A64_SP, base + k);
        }
    }

    /* Scalar float/double arguments.  A home in v1 moving to v0 must
     * not run before the value that still lives in v0. */
    {
        int fsrc[8], fdst[8], fisd[8], fdone[8], fhome[8];
        IRValue fav[8];
        for (int i = 0; i < fp_n; i++) {
            fav[i] = s->call_args[fp_at[i]];
            fsrc[i] = fp_home(c, fav[i]);
            fhome[i] = fsrc[i] >= 0;
            fdst[i] = fp_vn[i];
            fisd[i] = vec16_val(c, fav[i]) ? 2 : (vw(c, fav[i]) == 8);
            fdone[i] = fhome[i] && fsrc[i] == fdst[i];
        }
        int nleft = fp_n;
        for (int i = 0; i < fp_n; i++) nleft -= fdone[i];
        while (nleft > 0) {
            int picked = -1;
            for (int i = 0; i < fp_n; i++) {
                if (fdone[i]) continue;
                /* j blocks i while it still has to read the register i wants
                 * to overwrite.  A value parked in FSCR, or a spill/const
                 * not yet materialized, occupies no V register and so never
                 * blocks anyone -- which also leaves a parked node last. */
                int blocked = 0;
                for (int j = 0; j < fp_n; j++) {
                    if (fdone[j] || j == i || !fhome[j]) continue;
                    if (fsrc[j] == fdst[i]) { blocked = 1; break; }
                }
                if (!blocked) { picked = i; break; }
            }
            if (picked < 0) {
                /* The remaining home moves form a cycle.  Park one source in
                 * FSCR and mark it homeless; its partner becomes movable and
                 * the parked node reads FSCR only after that partner ran. */
                for (int i = 0; i < fp_n; i++)
                    if (!fdone[i] && fhome[i]) {
                        if (fisd[i] == 2) fmov_q(a, FSCR, fsrc[i]);
                        else a64_fmov_reg(a, FSCR, fsrc[i], fisd[i]);
                        fsrc[i] = FSCR;
                        fhome[i] = 0;
                        break;
                    }
                continue;
            }
            int r;
            if (fhome[picked])
                r = fsrc[picked];
            else if (fsrc[picked] == FSCR)
                r = FSCR;   /* parked there above to break a register cycle */
            else
                r = fisd[picked] == 2 ? load_q(c, fav[picked], fdst[picked])
                                      : load_fp(c, fav[picked], fdst[picked]);
            if (r != fdst[picked]) {
                if (fisd[picked] == 2) fmov_q(a, fdst[picked], r);
                else a64_fmov_reg(a, fdst[picked], r, fisd[picked]);
            }
            fdone[picked] = 1;
            nleft--;
        }
    }
    /* HFA eightbytes: integer bits → dN/sN, before GP moves. */
    for (int i = 0; i < hfa_n; i++) {
        IRValue av = s->call_args[hfa_at[i]];
        int r = load_op(c, av, -1);
        a64_fmov_gp(a, hfa_v[i], r, 1, vw(c, av) == 8);
    }
    /* Indirect result pointer, stashed above the outgoing args. */
    if (sret) {
        int r = load_op(c, s->call_args[0], -1);
        a64_str64(a, r, A64_SP, c->call_area - 16);
    }

    /* Lift an indirect target into x17 (IP1), which no allocation owns. */
    int tgt = -1;
    if (s->call_callee >= 0) {
        int r = load_ptrv(c, s->call_callee, -1);
        a64_mov_reg(a, SCR1, r, 1);
        tgt = SCR1;
    }

    /* Register-argument moves with cycle breaking.
     *   kind 0: source lives in register src[i]
     *   kind 1: source is a spill slot / CONST (materialized into xi)
     * Slot i's destination is x{dreg}, which is not necessarily x{i}
     * once HFA and stack-forced args have opened holes. */
    int src[8], kind[8], done[8], dreg[8];
    for (int i = 0; i < gp_n; i++) {
        done[i] = 0;
        dreg[i] = A64_X0 + i;
        IRValue av = s->call_args[gp_at[i]];
        const IRInst *d = def_inst(c, av);
        int r = (d && d->op == IR_CONST) ? -1 : home_reg(c, av);
        if (r >= 0) { kind[i] = 0; src[i] = r; }
        else { kind[i] = 1; src[i] = -1; }
    }

    int remaining = gp_n;
    while (remaining > 0) {
        int picked = -1;
        for (int i = 0; i < gp_n; i++) {
            if (done[i]) continue;
            int blocked = 0;
            for (int j = 0; j < gp_n; j++)
                if (!done[j] && j != i && kind[j] == 0 &&
                    src[j] == dreg[i]) { blocked = 1; break; }
            if (!blocked) { picked = i; break; }
        }
        if (picked < 0) {
            for (int i = 0; i < gp_n; i++)
                if (!done[i] && kind[i] == 0) {
                    a64_mov_reg(a, SCR0, src[i], 1);
                    src[i] = SCR0;
                    break;
                }
            continue;
        }
        int i = picked;
        IRValue av = s->call_args[gp_at[i]];
        int is64 = vw(c, av) == 8;
        if (kind[i] == 0) {
            if (src[i] != dreg[i])
                a64_mov_reg(a, dreg[i], src[i], is64);
        } else {
            const IRInst *d = def_inst(c, av);
            if (d && d->op == IR_CONST) {
                emit_mov_imm_w(a, dreg[i], d->imm, is64);
            } else {
                int so = spill_off(c, av);
                frame_load(c, dreg[i], so, is64 ? 8 : 4, is64 ? 1 : 0, 0);
            }
        }
        done[i] = 1;
        remaining--;
    }

    if (sret)
        a64_ldr64(a, A64_X8, A64_SP, c->call_area - 16);

    if (tgt >= 0) {
        a64_blr(a, tgt);
    } else if (s->call_name && strcmp(s->call_name, "__fakecc_udivmodti4") == 0) {
        if (c->udiv_label < 0)
            c->udiv_label = a64_new_label(a);
        a64_bl(a, c->udiv_label);
    } else if (!(s->call_name && (emit_alloc_builtin(c, s->call_name)
                                 || emit_mem_builtin(c, s->call_name)
                                 || emit_exit_builtin(c, s->call_name)
                                 || emit_scan_builtin(c, s->call_name)
                                 || emit_find_builtin(c, s->call_name)
                                 || emit_span_builtin(c, s->call_name)
                                 || emit_copy_builtin(c, s->call_name)
                                 || emit_dup_builtin(c, s->call_name)
                                 || emit_pipe_builtin(c, s->call_name)
                                 || emit_io_builtin(c, s->call_name)
                                 || emit_getcwd(c, s->call_name)
                                 || emit_mmap(c, s->call_name)
                                 || emit_getenv(c, s->call_name)
                                 || emit_bound_builtin(c, s->call_name)
                                 || emit_cache_builtin(c, s->call_name)))) {
        int fi = 0;
        if (!s->call_name) {
            die_at(s->loc.file ? s->loc.file : c->fn->loc.file,
                   s->loc.line, s->loc.col,
                   "arm64 backend: call with no target");
        } else if (find_function(c->ir, s->call_name, &fi) != 0) {
            /* Not in this TU.  Under -c and in a multi-TU link a sibling may
             * define it, so route the call through a stub the linker
             * retargets; only a lone single-TU link has nowhere to look. */
            if (!emit_object_mode() && !emit_multi_tu())
                die_at(s->loc.file ? s->loc.file : c->fn->loc.file,
                       s->loc.line, s->loc.col,
                       "arm64 backend: call to undefined function '%s'",
                       s->call_name);
            if (emit_object_mode() && c64_is_weak_ref(c->ir, s->call_name)) {
                if (c->nxcall == c->capxcall) {
                    c->capxcall = c->capxcall ? c->capxcall * 2 : 8;
                    c->xcall = xrealloc(c->xcall, c->capxcall * sizeof *c->xcall);
                }
                c->xcall[c->nxcall].at = (uint32_t)a->code.len;
                c->xcall[c->nxcall].name = s->call_name;
                c->nxcall++;
                a64_word(a, 0x94000000u); /* bl, ARM64_RELOC_BRANCH26 */
            } else
                a64_bl(a, c64_stub_label(c, s->call_name));
        } else
            a64_bl(a, c->fn_label[fi]);
    }

    /* Every branch above ended in a call, so this is the one place the
     * allocating SP has to come back.  Builtins that expand inline return
     * before here, having moved nothing. */
    if (swap_sp) {
        emit_fp_addr(c, SCR1, c->dyn_sp_off);
        a64_ldr64(a, SCR0, SCR1, 0);
        mov_sp_like(a, A64_SP, SCR0);
    }

    if (s->align16 == A64_MARK_HFA) {
        /* An HFA result is bits in v0-v3.  Each eightbyte is moved on its
         * own rather than as a pair: the destination homes are GP registers
         * and the sources are V, so they cannot collide and one scratch is
         * enough for all four. */
        const IRValue hv[4] = { s->dst, s->b, s->c, s->d };
        for (int i = 0; i < 4; i++) {
            if (hv[i] < 0) continue;
            /* float HFA members occupy only the low 32 bits of the V
             * register; a 64-bit move would read stale upper bits. */
            a64_fmov_gp(a, SCR0, A64_V0 + i, 0, vw(c, hv[i]) == 8);
            place_from(c, hv[i], SCR0);
        }
    } else if (s->dst >= 0 && vec16_val(c, s->dst)) {
        int h = fp_home(c, s->dst);
        if (h < 0) commit_q(c, s->dst, A64_V0);
        else fmov_q(c->as, h, A64_V0);
    } else if (s->dst >= 0 && scalar_fp_val(c, s->dst)) {
        int h = fp_home(c, s->dst);
        int isd = vw(c, s->dst) == 8 || s->width == 8;
        if (h < 0) commit_fp(c, s->dst, A64_V0);
        else if (h != A64_V0) a64_fmov_reg(a, h, A64_V0, isd);
    } else if (s->dst >= 0 && s->b >= 0) {
        place_pair(c, s->dst, s->b, A64_X0, A64_X1);
    } else if (s->dst >= 0) {
        int d = dst_reg(c, s->dst);
        if (d != A64_X0)
            a64_mov_reg(a, d, A64_X0, s->width == 8 || vw(c, s->dst) == 8);
        commit(c, s->dst, d);
    }
}

/* Raw Darwin syscall: number in x16, arguments in x0..x5, svc #0x80.
 * The kernel sets the carry flag and leaves a positive errno in x0 on
 * failure.  x9 (caller-saved, already forbidden across the call, and
 * the scratch Apple's own stubs use) records that carry; a set carry
 * negates x0 so the result is the Linux-shaped -errno. */
/* Linux x86-64 syscall number -> Darwin arm64.  Returns `num` unchanged when
 * there is no counterpart, so an unmapped call reaches the kernel and fails
 * there rather than doing something arbitrary.
 *
 * Only entries verified against the Darwin arm64 syscall table are listed.
 * An entry that is merely plausible is worse than a missing one: a wrong
 * number silently performs a different operation, which is how a printf
 * ended up calling exit.  When in doubt, leave it unmapped -- the call
 * fails loudly at the kernel instead. */
static int64_t c64_darwin_syscall(int64_t num) {
    switch (num) {
    case 0:  return 3;   /* read */
    case 1:  return 4;   /* write */
    case 2:  return 5;   /* open */
    case 3:  return 6;   /* close */
    case 8:  return 199; /* lseek */
    case 9:  return 197; /* mmap */
    case 11: return 73;  /* munmap */
    case 39: return 20;  /* getpid */
    case 60: return 1;   /* exit */
    case 87: return 10;  /* unlink */
    case 90: return 15;  /* chmod (Linux dup2 is 33, not 90) */
    case 102: return 24; /* getuid */
    case 186: return 224;/* gettid */
    case 231: return 1;  /* exit_group -> exit: Darwin has no group */
    default: return num; /* futex(202), clone(56): no equivalent exists */
    }
}

/* Pointer-authentication intrinsics: __paciasp() / __pacia(rt) /
 * __autda(rt, rn) / __retab().  Darwin's thread machinery authenticates the
 * function pointer it hands to bsdthread_register, and libsystem_pthread
 * authenticates on every return, so a runtime that talks to the kernel
 * directly needs the same pair.  The instructions are HINT-space, so on a
 * process that did not opt in they do nothing -- which is exactly what a
 * runtime that ignores the signature needs. */
static int emit_pac_builtin(C64 *c, const IRInst *s) {
    A64Asm *a = c->as;
    if (!s->call_name) return 0;
    if (strcmp(s->call_name, "__paciasp") == 0) {
        a64_paciasp(a);
        return 1;
    }
    if (strcmp(s->call_name, "__retab") == 0) {
        a64_retab(a);
        return 1;
    }
    if (strcmp(s->call_name, "__pacia") == 0) {
        if (s->call_nargs < 1) c64_die(c, s, "__pacia without argument");
        int r = load_op(c, s->call_args[0], -1);
        a64_pacia(a, r);
        if (s->dst >= 0) {
            int d = dst_reg(c, s->dst);
            if (d != r) a64_mov_reg(a, d, r, 1);
            commit(c, s->dst, d);
        }
        return 1;
    }
    if (strcmp(s->call_name, "__autda") == 0) {
        if (s->call_nargs < 2) c64_die(c, s, "__autda needs two arguments");
        int rt = load_op(c, s->call_args[0], -1);
        int rn = load_op(c, s->call_args[1], -1);
        a64_autda(a, rt, rn);
        if (s->dst >= 0) {
            int d = dst_reg(c, s->dst);
            if (d != rt) a64_mov_reg(a, d, rt, 1);
            commit(c, s->dst, d);
        }
        return 1;
    }
    return 0;
}

static void emit_syscall(C64 *c, const IRInst *s) {
    A64Asm *a = c->as;
    int n = s->call_nargs;
    if (n < 1) c64_die(c, s, "__syscall without number");
    int nreg = n - 1;
    if (nreg > 6) nreg = 6;

    /* Cycle-safe moves of args[1..] into x0..x5.  x17 is the only
     * scratch (x16 is reserved for the number); mirrors emit_call's
     * permutation algorithm. */
    int src[6], kind[6], done[6];
    for (int i = 0; i < nreg; i++) {
        IRValue av = s->call_args[1 + i];
        const IRInst *d = def_inst(c, av);
        int r = (d && d->op == IR_CONST) ? -1 : home_reg(c, av);
        if (r >= 0) { kind[i] = 0; src[i] = r; }
        else { kind[i] = 1; src[i] = -1; }
        done[i] = 0;
    }
    int remaining = nreg;
    while (remaining > 0) {
        int picked = -1;
        for (int i = 0; i < nreg; i++) {
            if (done[i]) continue;
            int blocked = 0;
            for (int j = 0; j < nreg; j++)
                if (!done[j] && j != i && kind[j] == 0 &&
                    src[j] == A64_X0 + i) { blocked = 1; break; }
            if (!blocked) { picked = i; break; }
        }
        if (picked < 0) {
            for (int i = 0; i < nreg; i++)
                if (!done[i] && kind[i] == 0) {
                    a64_mov_reg(c->as, SCR1, src[i], 1);
                    src[i] = SCR1;
                    break;
                }
            continue;
        }
        int i = picked;
        IRValue av = s->call_args[1 + i];
        int is64 = vw(c, av) == 8;
        if (kind[i] == 0) {
            if (src[i] != A64_X0 + i)
                a64_mov_reg(c->as, A64_X0 + i, src[i], is64);
        } else {
            const IRInst *d = def_inst(c, av);
            if (d && d->op == IR_CONST) {
                emit_mov_imm_w(c->as, A64_X0 + i, d->imm, is64);
            } else {
                int so = spill_off(c, av);
                frame_load(c, A64_X0 + i, so, is64 ? 8 : 4, is64 ? 1 : 0, 0);
            }
        }
        done[i] = 1;
        remaining--;
    }

    /* Number last so the argument moves can use x16 as scratch freely. */
    IRValue nv = s->call_args[0];
    const IRInst *nd = def_inst(c, nv);
    if (nd && nd->op == IR_CONST) {
        /* The builtin runtime is written against Linux x86-64 syscall
         * numbers, but this backend talks to Darwin, where the same number
         * means something else entirely -- number 1 is exit, not write, so
         * an untranslated printf terminates the process instead of printing.
         * The sources predate the arm64 port and are shared with x86, so
         * translate here rather than in the runtime.
         *
         * Only a literal number is translated.  A computed one is left
         * alone: guessing there could silently change behaviour, and every
         * call site in the runtime passes a constant.  Numbers with no
         * Darwin counterpart (futex, clone) stay put and fail at the
         * kernel, which is the honest outcome for a platform that cannot
         * honour them. */
        int64_t num = nd->imm;
        int64_t mapped = c64_darwin_syscall(num);
        emit_mov_imm_w(a, A64_X16, mapped, 1);
    } else {
        int sr = load_op(c, nv, SCR0);
        a64_mov_reg(a, A64_X16, sr, 1);
    }
    a64_svc(a, 0x80);
    {
        int ok = a64_new_label(a);
        a64_cset(a, A64_X9, A64_CS, 1);
        a64_cbz(a, A64_X9, ok, 1);
        /* A raw syscall reports failure as the negated errno, which is what
         * lets a caller tell the codes apart.  The libc-level names in
         * emit_io_builtin collapse this to -1 instead, because POSIX has
         * them report -1 with the reason only in errno. */
        a64_neg(a, A64_X0, A64_X0, 1);
        a64_bind(a, ok);
    }

    if (s->dst >= 0) {
        int d = dst_reg(c, s->dst);
        if (d != A64_X0)
            a64_mov_reg(a, d, A64_X0, s->width == 8);
        commit(c, s->dst, d);
    }
}

/* ── Binary operations ────────────────────────────────────────────── */

static int cmp_cond(const IRInst *s) {
    switch (s->op) {
    case IR_EQ: return A64_EQ;
    case IR_NE: return A64_NE;
    case IR_LT: return s->is_unsigned ? A64_CC : A64_LT;
    case IR_LE: return s->is_unsigned ? A64_LS : A64_LE;
    case IR_GT: return s->is_unsigned ? A64_HI : A64_GT;
    case IR_GE: return s->is_unsigned ? A64_CS : A64_GE;
    default:    return A64_EQ;
    }
}

static int c64_eval_offset(C64 *c, int v, int64_t *out, int depth) {
    if (depth > 6 || v < 0 || !c->def || v >= c->fn->next_value_id) return 0;
    int di = c->def[v];
    if (di < 0 || (size_t)di >= c->fn->insts.len) return 0;
    const IRInst *s = &c->fn->insts.data[di];
    if (s->op == IR_CONST) { *out = s->imm; return 1; }
    if (s->op == IR_SEXT || s->op == IR_ZEXT || s->op == IR_TRUNC) {
        int64_t inner;
        if (!c64_eval_offset(c, s->a, &inner, depth + 1)) return 0;
        if (s->op == IR_SEXT) {
            int sw = (int)s->imm;
            if (sw == 1) inner = (int8_t)inner;
            else if (sw == 2) inner = (int16_t)inner;
            else if (sw == 4) inner = (int32_t)inner;
        } else if (s->width == 1) inner &= 0xff;
        else if (s->width == 2) inner &= 0xffff;
        else if (s->width == 4) inner &= 0xffffffffLL;
        *out = inner;
        return 1;
    }
    if (s->op == IR_MUL || s->op == IR_ADD) {
        int64_t x, y;
        if (!c64_eval_offset(c, s->a, &x, depth + 1)) return 0;
        if (!c64_eval_offset(c, s->b, &y, depth + 1)) return 0;
        *out = (s->op == IR_MUL) ? x * y : x + y;
        return 1;
    }
    return 0;
}

static int c64_value_uses(C64 *c, int v) {
    int n = 0;
    for (size_t i = 0; i < c->fn->insts.len; i++) {
        const IRInst *s = &c->fn->insts.data[i];
        if (s->a == v || s->b == v) n++;
        if (s->op != IR_CALL) continue;
        if (s->call_callee == v) n++;
        for (int k = 0; k < s->call_nargs; k++)
            if (s->call_args && s->call_args[k] == v) n++;
    }
    return n;
}

/* A one-use ADRP+ADD plus a constant byte offset becomes ARM64_RELOC_ADDEND
 * so the linker, not a later add, applies an offset that may cross a page. */
static int fold_page_addend(C64 *c, const IRInst *s) {
    if ((s->op != IR_ADD && s->op != IR_SUB) || s->width != 8 || s->is_float)
        return 0;
    int base, neg = s->op == IR_SUB;
    int64_t off = 0;
    if (neg) {
        base = s->a;
        if (!c64_eval_offset(c, s->b, &off, 0)) return 0;
        off = -off;
    } else {
        int64_t left = 0, right = 0;
        int lc = c64_eval_offset(c, s->a, &left, 0);
        int rc = c64_eval_offset(c, s->b, &right, 0);
        if (rc && !lc) { base = s->a; off = right; }
        else if (lc && !rc) { base = s->b; off = left; }
        else return 0;
    }
    if (off == 0 || off > 0x7FFFFF || off < -0x800000) return 0;
    int gi = -1;
    /* Only this function's gfix entries are candidates: value ids repeat
     * across functions, so a global scan could fold onto another function's
     * ADRP+ADD pair and corrupt its addend. */
    for (size_t i = c->fn_gfix_start; i < c->ngfix; i++)
        if (c->gfix[i].value == base) gi = (int)i;
    if (gi < 0) return 0;
    if (c->gfix[gi].tlvp) return 0;
    if (c64_value_uses(c, base) != 1) return 0;
    c->gfix[gi].addend += (int)off;
    int ra = load_op(c, base, -1);
    int d = dst_reg(c, s->dst);
    if (d != ra) a64_mov_reg(c->as, d, ra, 1);
    commit(c, s->dst, d);
    return 1;
}

static void emit_binop(C64 *c, const IRInst *s) {
    if (fold_page_addend(c, s)) return;
    A64Asm *a = c->as;
    int d = dst_reg(c, s->dst);
    int is64 = s->width == 8;

    if (s->op == IR_EQ || s->op == IR_NE || s->op == IR_LT ||
        s->op == IR_LE || s->op == IR_GT || s->op == IR_GE) {
        int ra = load_op(c, s->a, -1);
        int rb = load_op(c, s->b, ra);
        /* After CMP the operand registers are dead, so CSET may target the
         * home even if it aliases ra/rb. */
        a64_cmp_reg(a, ra, rb, vw(c, s->a) == 8);
        a64_cset(a, d, cmp_cond(s), 0);
        commit(c, s->dst, d);
        return;
    }

    int ra = load_op(c, s->a, -1);
    int rb = load_op(c, s->b, ra);
    switch (s->op) {
    case IR_ADD:
        a64_add_reg(a, d, ra, rb, A64_LSL, 0, is64, 0);
        break;
    case IR_MUL:
        a64_mul(a, d, ra, rb, is64);
        break;
    case IR_BAND:
        a64_and_reg(a, d, ra, rb, is64);
        break;
    case IR_BOR:
        a64_or_reg(a, d, ra, rb, is64);
        break;
    case IR_BXOR:
        a64_eor_reg(a, d, ra, rb, is64);
        break;
    case IR_SUB:
        a64_sub_reg(a, d, ra, rb, A64_LSL, 0, is64, 0);
        break;
    case IR_DIV:
        if (s->is_unsigned) a64_udiv(a, d, ra, rb, is64);
        else a64_sdiv(a, d, ra, rb, is64);
        break;
    case IR_MOD: {
        /* Quotient needs a register that is neither operand: both may
         * already occupy x16/x17 when spilled.  x0 is not allocatable. */
        int q = SCR1;
        if (q == ra || q == rb) q = A64_X0;
        if (s->is_unsigned) a64_udiv(a, q, ra, rb, is64);
        else a64_sdiv(a, q, ra, rb, is64);
        a64_msub(a, d, q, rb, ra, is64);  /* d = ra - q*rb */
        break;
    }
    case IR_SHL:
        a64_lslv(a, d, ra, rb, is64);
        break;
    case IR_SHR:
        if (s->is_unsigned) a64_lsrv(a, d, ra, rb, is64);
        else a64_asrv(a, d, ra, rb, is64);
        break;
    case IR_ROL: {
        /* rol(ra, rb) = ror(ra, -rb); the hardware masks the count.
         * Don't negate into ra when the spilled pair occupies both scratches. */
        int nreg = (SCR1 != ra) ? SCR1 : A64_X0;
        a64_neg(a, nreg, rb, is64);
        a64_rorv(a, d, ra, nreg, is64);
        break;
    }
    default:
        c64_die(c, s, "binary op");
        return;
    }
    commit(c, s->dst, d);
}

/* ── Per-function lowering ────────────────────────────────────────── */

static int vlabels_get(C64 *c, int id) {
    if (c->vlabels[id] < 0)
        c->vlabels[id] = a64_new_label(c->as);
    return c->vlabels[id];
}

/* A dead parameter still receives a register color (it has a def, so the
 * allocator does not treat it as unused).  That color can alias a live
 * parameter.  Copying the dead value into the shared register at entry
 * clobbers the live one.  Skip parameters nothing reads. */
static int param_is_used(const IRFunction *fn, IRValue v) {
    if (v < 0) return 0;
    for (size_t i = 0; i < fn->insts.len; i++) {
        const IRInst *s = &fn->insts.data[i];
        if (s->op == IR_PARAM) continue;
        if (s->a == v || s->b == v || s->call_callee == v) return 1;
        if (s->op == IR_CALL && s->call_args) {
            for (int k = 0; k < s->call_nargs; k++)
                if (s->call_args[k] == v) return 1;
        }
    }
    return 0;
}

/* imm12 stops at 4095.  Step by 4080 so SP stays 16-byte aligned;
 * the epilogue reloads SP from FP, so the split does not have to be one
 * instruction. */
static void sub_sp_bytes(A64Asm *a, unsigned bytes) {
    while (bytes > 4095) {
        a64_sub_imm12(a, A64_SP, A64_SP, 4080, 0, 1, 0);
        bytes -= 4080;
    }
    if (bytes)
        a64_sub_imm12(a, A64_SP, A64_SP, bytes, 0, 1, 0);
}

static void stp_q(A64Asm *a, int rt, int rt2, int rn, int byte_off) {
    int imm = byte_off / 16;
    a64_word(a, 0xAD000000u | ((imm & 0x7f) << 15)
             | ((rt2 & 31) << 10) | ((rn & 31) << 5) | (rt & 31));
}
static void ldp_q(A64Asm *a, int rt, int rt2, int rn, int byte_off) {
    int imm = byte_off / 16;
    a64_word(a, 0xAD400000u | ((imm & 0x7f) << 15)
             | ((rt2 & 31) << 10) | ((rn & 31) << 5) | (rt & 31));
}
static void str_q(A64Asm *a, int rt, int rn, int byte_off) {
    int imm = byte_off / 16;
    a64_word(a, 0x3D800000u | ((imm & 0xfff) << 10)
             | ((rn & 31) << 5) | (rt & 31));
}
/* MOV Vd.16B, Vn.16B — full Q, not a scalar FMOV. */
static void fmov_q(A64Asm *a, int rd, int rn) {
    if (rd == rn) return;
    a64_word(a, 0x4EA01C00u | ((unsigned)(rn & 31) << 16)
             | ((unsigned)(rn & 31) << 5) | (unsigned)(rd & 31));
}
static void ldr_q(A64Asm *a, int rt, int rn, int byte_off) {
    int imm = byte_off / 16;
    a64_word(a, 0x3DC00000u | ((imm & 0xfff) << 10)
             | ((rn & 31) << 5) | (rt & 31));
}

/* Vectors are addressed through a pointer to their storage, so a narrow one
 * is just a narrower load.  128-bit and narrower live in the same register;
 * only the width of the transfer differs. */
static void vec_load(A64Asm *a, int rt, int rn, int width) {
    if (width == 16) ldr_q(a, rt, rn, 0);
    else if (width == 8) a64_ldr_d(a, rt, rn, 0);
    else a64_ldr_s(a, rt, rn, 0);
}
static void vec_store(A64Asm *a, int rt, int rn, int width) {
    if (width == 16) str_q(a, rt, rn, 0);
    else if (width == 8) a64_str_d(a, rt, rn, 0);
    else a64_str_s(a, rt, rn, 0);
}

/* imm5 for UMOV/INS: index shifted above the size marker bit. */
static int elem_imm5(int esz, int index) {
    int shift = esz == 1 ? 1 : esz == 2 ? 2 : esz == 4 ? 3 : 4;
    int marker = esz == 1 ? 1 : esz == 2 ? 2 : esz == 4 ? 4 : 8;
    return (index << shift) | marker;
}

static void umov_elem(A64Asm *a, int rd, int vn, int esz, int index) {
    int imm5 = elem_imm5(esz, index);
    uint32_t base = (esz == 8) ? 0x4E003C00u : 0x0E003C00u;
    a64_word(a, base | ((uint32_t)imm5 << 16) | ((uint32_t)(vn & 31) << 5)
                  | (uint32_t)(rd & 31));
}

static void ins_elem(A64Asm *a, int vd, int rn, int esz, int index) {
    int imm5 = elem_imm5(esz, index);
    a64_word(a, 0x4E001C00u | ((uint32_t)imm5 << 16) | ((uint32_t)(rn & 31) << 5)
                            | (uint32_t)(vd & 31));
}

/* dst = a op b, all three are pointers to 16-byte vectors.
 * imm is the element size, width is the vector size, is_float selects
 * NEON scalar-FP rather than integer.  v30/v31 are the only scratches. */
static void emit_vec(C64 *c, const IRInst *s) {
    A64Asm *a = c->as;
    int esz = (int)s->imm;
    int w = s->width;
    if ((w != 16 && w != 8 && w != 4) || w < esz
        || (esz != 1 && esz != 2 && esz != 4 && esz != 8)) {
        c64_die(c, s, "unsupported vector width");
        return;
    }
    int pa = load_ptrv(c, s->a, -1);
    vec_load(a, A64_V30, pa, w);
    int pb = load_ptrv(c, s->b, -1);
    vec_load(a, A64_V31, pb, w);

    int sz = esz == 1 ? 0 : esz == 2 ? 1 : esz == 4 ? 2 : 3;
    uint32_t base = 0;
    int lane = 0;
    if (s->is_float) {
        /* No packed float form exists below 128 bits -- the narrowest is a
         * single D -- so two or more float lanes go element by element. */
        if (esz != 4 && esz != 8) {
            c64_die(c, s, "vector float element");
            return;
        }
        if (w < 16) lane = 1;
        uint32_t bit = (esz == 8) ? (1u << 22) : 0;
        switch (s->op) {
        case IR_VADD: base = 0x4E20D400u | bit; break;
        case IR_VSUB: base = 0x4EA0D400u | bit; break;
        case IR_VMUL: base = 0x6E20DC00u | bit; break;
        case IR_VDIV: base = 0x6E20FC00u | bit; break;
        case IR_VBAND: base = 0x4E201C00u; break;
        case IR_VBOR:  base = 0x4EA01C00u; break;
        case IR_VBXOR: base = 0x6E201C00u; break;
        default: c64_die(c, s, "vector op"); return;
        }
    } else {
        switch (s->op) {
        case IR_VADD: base = 0x4E208400u | ((uint32_t)sz << 22); break;
        case IR_VSUB: base = 0x6E208400u | ((uint32_t)sz << 22); break;
        case IR_VBAND: base = 0x4E201C00u; break;
        case IR_VBOR:  base = 0x4EA01C00u; break;
        case IR_VBXOR: base = 0x6E201C00u; break;
        case IR_VMUL:
            /* AArch64 has MUL down to 8-bit lanes (mul v0.8b = 0x0E229C20)
             * but none at 64 bits, which falls back to scalar MUL; the low
             * esz bits match either signedness. */
            if (esz == 8) {
                lane = 1;
                break;
            }
            base = 0x4E209C00u | ((uint32_t)sz << 22);
            break;
        default:
            c64_die(c, s, "vector integer divide");
            return;
        }
    }
    /* Q is bit 30 (0x40000000); the 64- and 32-bit forms of these opcodes
     * are the 128-bit word with that bit clear -- verified against clang:
     * and.16b 0x4E221C20 / and.8b 0x0E221C20, eor.16b 0x6E221C20 /
     * eor.8b 0x2E221C20. */
    if (w != 16) base &= ~0x40000000u;
    if (lane) {
        int n = w / esz;
        for (int i = 0; i < n; i++) {
            umov_elem(a, SCR0, A64_V30, esz, i);
            umov_elem(a, SCR1, A64_V31, esz, i);
            a64_mul(a, SCR0, SCR0, SCR1, esz == 8);
            ins_elem(a, A64_V30, SCR0, esz, i);
        }
    } else {
        a64_word(a, base | ((uint32_t)A64_V31 << 16) | ((uint32_t)A64_V30 << 5)
                      | (uint32_t)A64_V30);
    }
    int pd = load_ptrv(c, s->dst, -1);
    vec_store(a, A64_V30, pd, w);
}

/* Debug aid: FAKECC_DUMP_IR=1 prints one line per IR instruction, prefixed
 * with the function it belongs to.  Several passes run over the same
 * function, so the same IR appears more than once -- filter on the header
 * line when reading the output. */
static int c64_ir_dump_on(void) {
    static int cached = -1;
    if (cached < 0) cached = getenv("FAKECC_DUMP_IR") != NULL;
    return cached;
}

static void emit_function(C64 *c, int fi) {
    const IRFunction *fn = c->fn;
    A64Asm *a = c->as;
    a64_bind(a, c->fn_label[fi]);
    c->fn_gfix_start = c->ngfix;
    if (c64_ir_dump_on())
        fprintf(stderr, "== fn %s (fi=%d)\n", fn->name ? fn->name : "?", fi);

    const RAResult *ra = (const RAResult *)fn->ra;
    c->ra = ra;
    c->ra_fp = (const RAResult *)fn->ra_xmm;
    int nv = fn->next_value_id;

    /* ---- Frame plan ---- */
    c->def = xmalloc((size_t)nv * sizeof(int));
    for (int i = 0; i < nv; i++) c->def[i] = -1;
    c->alloca_off = xmalloc((size_t)nv * sizeof(int));
    memset(c->alloca_off, 0, (size_t)nv * sizeof(int));
    c->alloca_aln = xmalloc((size_t)nv * sizeof(int));
    memset(c->alloca_aln, 0, (size_t)nv * sizeof(int));
    c->spill_off = NULL;

    /* Pinned allocas (fp-relative, mirroring the x86 backend ordering).
     *
     * AAPCS64 only promises a 16-byte aligned SP on entry, so a slot asking
     * for 32 or more cannot be reached by a static fp-relative offset and has
     * to be masked at run time (see emit_fp_addr_aligned).  Every such mask
     * must use the *same* alignment: masking a 64-byte slot down to 64 and a
     * 32-byte slot down to 32 shifts them by different amounts, and with fp at
     * 32 mod 64 the two then overlap by 32 bytes.  So when any slot is
     * over-aligned, every pinned offset is rounded up to the frame's maximum
     * alignment, which makes all the masked addresses translate by one shared
     * delta and keeps their relative positions intact.  A 64-byte aligned
     * slot satisfies a 32-byte request, so over-aligning the others is
     * harmless. */
    int pinned = 0;
    int max_aln = 16;
    for (size_t j = 0; j < fn->insts.len; j++) {
        const IRInst *s = &fn->insts.data[j];
        if (s->dst >= 0) c->def[s->dst] = (int)j;
        if (s->op == IR_ALLOCA && s->alloca_bytes > 0 && s->dst >= 0) {
            int aln = (int)s->imm >= 16 ? (int)s->imm : 8;
            if (aln < 8) aln = 8;
            if (aln > max_aln) max_aln = aln;
        }
    }
    for (size_t j = 0; j < fn->insts.len; j++) {
        const IRInst *s = &fn->insts.data[j];
        if (s->op == IR_ALLOCA && s->alloca_bytes > 0 && s->dst >= 0) {
            int bytes = s->alloca_bytes;
            if (bytes % 8) bytes += 8 - (bytes % 8);
            int aln = (int)s->imm >= 16 ? (int)s->imm : 8;
            if (aln < 8) aln = 8;
            int masked = aln > 16;
            if (masked) aln = max_aln;    /* one shared mask for the frame */
            pinned += bytes;
            if (pinned % aln) pinned += aln - (pinned % aln);
            c->alloca_off[s->dst] = -pinned;
            c->alloca_aln[s->dst] = aln;
            /* A masked slot resolves up to max_aln-1 bytes below the offset
             * recorded here, so leave that much room before the next slot
             * starts.  Without this gap the masked slot lands on top of
             * whatever the plan placed below it. */
            if (masked) pinned += max_aln - 1;
        }
    }

    /* GP spill slots, placed below the pinned area.  SIMD spills
     * follow, 16 bytes each and 16-aligned. */
    int spill_area = ra ? ra->stack_size : 0;
    if (ra && ra->num_spill_slots > 0) {
        c->spill_off = xmalloc((size_t)ra->num_spill_slots * sizeof(int));
        for (int s2 = 0; s2 < ra->num_spill_slots; s2++)
            c->spill_off[s2] = -(pinned + (s2 + 1) * 8);
    }
    int fp_bytes = 0;
    c->fp_spill_off = NULL;
    if (c->ra_fp && c->ra_fp->num_spill_slots > 0) {
        int base = pinned + spill_area;
        if (base % 16) base += 16 - (base % 16);
        int nfp = c->ra_fp->num_spill_slots;
        c->fp_spill_off = xmalloc((size_t)nfp * sizeof(int));
        for (int s2 = 0; s2 < nfp; s2++)
            c->fp_spill_off[s2] = -(base + (s2 + 1) * 16);
        fp_bytes = (base - pinned - spill_area) + nfp * 16;
    }

    /* Outgoing stack-argument area (max over calls in this function). */
    int call_area = 0;
    for (size_t j = 0; j < fn->insts.len; j++) {
        const IRInst *s = &fn->insts.data[j];
        if (s->op == IR_CALL) {
            int bytes = outgoing_stack_bytes(c, s);
            if (bytes > call_area) call_area = bytes;
        }
    }
    if (call_area % 16) call_area += 16 - (call_area % 16);
    c->call_area = call_area;

    /* __builtin_apply_args / __builtin_apply blocks live below everything
     * else in the frame; their fp-relative offsets are what the builtins
     * address them by. */
    c->apply_args_off = 0;
    c->apply_result_off = 0;
    /* Slot for the SP a dynamic alloca leaves behind, placed above call_area
     * so the outgoing-argument writes cannot reach it. */
    c->dyn_sp_off = -1;
    for (size_t j = 0; j < fn->insts.len; j++)
        if (fn->insts.data[j].op == IR_DYN_ALLOCA) { c->dyn_sp_off = 1; break; }

    int off_acc = pinned + spill_area + fp_bytes;
    if (c->dyn_sp_off > 0) {
        /* Below everything else, and the slot's own space counted before its
         * offset is taken.  Reusing off_acc itself would put the slot on top
         * of the spill area: base already ends there, so -base points at the
         * first spill byte rather than past it.  The frame's pinned slots are
         * handed out from the same counter, so the overlap is silent.
         * 0 is also unusable: emit_fp_addr reads it as "the address is FP". */
        off_acc = ((off_acc + 15) & ~15) + 16;
        c->dyn_sp_off = -off_acc;
    }
    off_acc += call_area;
    if (fn->needs_apply_args) {
        off_acc += APPLY_BLOCK_SIZE;
        c->apply_args_off = -off_acc;
    }
    if (fn->needs_apply) {
        off_acc += APPLY_RES_SCRATCH;
        c->apply_result_off = -off_acc;
    }

    int locals = off_acc;
    if (locals % 16) locals += 16 - (locals % 16);
    c->frame_locals = locals;

    /* Callee-saved registers actually used (native codes x19..x28;
     * ra->reg[] holds hardware numbers, not palette colors). */
    int cs[10], ncs = 0;
    if (ra)
        for (int v = 0; v < nv; v++)
            if (ra->reg[v] >= A64_X19 && ra->reg[v] <= A64_X28) {
                int hw = ra->reg[v];
                int present = 0;
                for (int k = 0; k < ncs; k++) if (cs[k] == hw) present = 1;
                if (!present) cs[ncs++] = hw;
            }
    /* ascending order for deterministic pairs */
    for (int i = 0; i < ncs; i++)
        for (int j = i + 1; j < ncs; j++)
            if (cs[j] < cs[i]) { int t = cs[i]; cs[i] = cs[j]; cs[j] = t; }
    int cs_pairs = (ncs + 1) / 2;
    int cs_area = cs_pairs * 16;
    /* v8..v15 are callee-saved (the full 128 bits). */
    int vcs[8], nvcs = 0;
    if (c->ra_fp)
        for (int v = 0; v < nv; v++) {
            int hw = c->ra_fp->reg[v];
            if (hw < A64_V8 || hw > A64_V15) continue;
            int present = 0;
            for (int k = 0; k < nvcs; k++) if (vcs[k] == hw) present = 1;
            if (!present) vcs[nvcs++] = hw;
        }
    for (int i = 0; i < nvcs; i++)
        for (int j = i + 1; j < nvcs; j++)
            if (vcs[j] < vcs[i]) { int t = vcs[i]; vcs[i] = vcs[j]; vcs[j] = t; }
    int vcs_pairs = (nvcs + 1) / 2;
    int vcs_area = vcs_pairs * 32;
    if (nvcs & 1) vcs_area -= 16; /* a lone Q is 16 bytes, not a 32-byte pair */
    int save_total = 16 + cs_area + vcs_area;
    c->save_total = save_total;

    /* IR label table. */
    c->vlabels = xmalloc((size_t)fn->next_label_id * sizeof(int));
    for (int i = 0; i < fn->next_label_id; i++) c->vlabels[i] = -1;

    /* ---- Prologue ---- */
    a64_stp64(a, A64_FP, A64_LR, A64_SP, -save_total, A64_PAIR_PRE);
    mov_sp_like(a, A64_FP, A64_SP);
    for (int p = 0; p < ncs / 2; p++)
        a64_stp64(a, cs[2*p], cs[2*p+1], A64_FP, 16 + 16*p, A64_PAIR_OFFSET);
    if (ncs & 1)
        a64_str64(a, cs[ncs-1], A64_FP, 16 + 16*cs_pairs - 8);
    {
        int qbase = 16 + cs_area;
        for (int p = 0; p < nvcs / 2; p++)
            stp_q(a, vcs[2 * p], vcs[2 * p + 1], A64_FP, qbase + 32 * p);
        if (nvcs & 1)
            str_q(a, vcs[nvcs - 1], A64_FP, qbase + 32 * (nvcs / 2));
    }
    if (locals)
        sub_sp_bytes(a, (unsigned)locals);

    /* Seed the slot with the SP the prologue just produced: a call can
     * precede the first IR_DYN_ALLOCA (the alloca may sit in a branch that
     * has not been taken) and the restore would otherwise load whatever
     * happened to be in the frame there. */
    if (c->dyn_sp_off != -1) {
        emit_fp_addr(c, SCR0, c->dyn_sp_off);
        mov_sp_like(a, SCR1, A64_SP);
        a64_str64(a, SCR1, SCR0, 0);
    }

    /* __builtin_apply_args hands the caller the *incoming* registers, so
     * they have to be captured here -- before the parameter homing below
     * moves them into their homes. */
    if (fn->needs_apply_args) {
        for (int i = 0; i < 8; i++) {
            emit_fp_addr(c, SCR1, c->apply_args_off + APPLY_GP_OFF + i * 8);
            a64_str64(a, A64_X0 + i, SCR1, 0);
        }
        for (int i = 0; i < 8; i++) {
            emit_fp_addr(c, SCR1, c->apply_args_off + APPLY_V_OFF + i * 16);
            str_q(a, A64_V0 + i, SCR1, 0);
        }
        /* Stack arguments live in the caller's frame just above our saved
         * one, so the block's front is filled from there.  The chunk
         * register has to be one of the two scratches: x0-x7 still hold the
         * incoming arguments, and the parameter homing below reads them.
         * Borrowing x0 here made `bar(5)` apply foo(garbage).  Recomputing
         * the source per chunk keeps it to two registers. */
        emit_fp_addr(c, SCR0, c->apply_args_off + 0);
        for (int k = 0; k < APPLY_STACK_MAX; k += 8) {
            emit_fp_addr(c, SCR1, save_total + k);
            a64_ldr64(a, SCR1, SCR1, 0);
            a64_str64(a, SCR1, SCR0, k);
        }
    }

    /* ---- Incoming integer parameters (x0..x7, then caller stack) ---- */
    int nparams = 0;
    while (nparams < (int)fn->insts.len &&
           fn->insts.data[nparams].op == IR_PARAM)
        nparams++;

    /* Cycle-safe placement: a parameter arriving in x0..x7 may be homed in
     * x2..x7, so a naive move can clobber a not-yet-read incoming reg. */
    typedef struct { int src_reg; int src_mem;   /* -1, or fp offset */
                    int src_vec;                /* >=0: incoming V register */
                    int dst_reg; int dst_mem;   /* -1, or fp spill offset */
                    int is64; int done;
                    /* MEMORY blob: the incoming value is the *address* of
                     * the argument, not its contents. */
                    int is_blob; } PMove;
    PMove *mv = xmalloc((size_t)(nparams > 0 ? nparams : 1) * sizeof(PMove));
    typedef struct { int src; int dst; int spill; int isd; int stack; int done; } FIn;
    FIn fin[16];
    int nfin = 0;
    int gp_idx = 0, fp_idx = 0, stack_arg_idx = 0;
    for (int p = 0; p < nparams; p++) {
        const IRInst *s = &fn->insts.data[p];
        mv[p].src_vec = -1;
        mv[p].src_reg = -1;
        mv[p].src_mem = -1;
        mv[p].is_blob = 0;
        if (s->force_stack && s->alloca_bytes > 0) {
            /* MEMORY blob: ir.c gave the parameter an address-valued type
             * and pointed it at the incoming slot, exactly as x86 does with
             * an LEA of [rbp+off].  Nothing is copied -- the callee reads the
             * caller's outgoing area in place.  Honour the alignment ir.c
             * recorded, then charge the object's size to the slot budget. */
            int al = (s->align16 == 64) ? 64 : (s->align16 == 32) ? 32 : 16;
            int byte = 8 * stack_arg_idx;
            if (byte % al) byte += al - (byte % al);
            mv[p].src_mem = save_total + byte;
            stack_arg_idx = (byte + s->alloca_bytes + 7) / 8;
            mv[p].is_blob = 1;
            if (!param_is_used(fn, s->dst)) {
                mv[p].done = 1;
                continue;
            }
            int bhr = home_reg(c, s->dst);
            mv[p].dst_reg = bhr >= 0 ? bhr : -1;
            mv[p].dst_mem = bhr >= 0 ? -1 : spill_off(c, s->dst);
            mv[p].is64 = 1;
            continue;
        }
        int is_sret = fn->sret_value >= 0 && s->dst == fn->sret_value;
        if (vec16_val(c, s->dst)) {
            int from_stack = s->force_stack || fp_idx >= 8;
            int srcv;
            if (from_stack) {
                int byte = 8 * stack_arg_idx;
                if (byte & 15) {
                    stack_arg_idx++;
                    byte += 8;
                }
                srcv = save_total + byte;
                stack_arg_idx += 2;
            } else {
                srcv = fp_idx++;
            }
            mv[p].done = 1;
            mv[p].src_reg = -1;
            mv[p].src_vec = -1;
            mv[p].dst_reg = -1;
            mv[p].src_mem = -1;
            if (param_is_used(fn, s->dst)) {
                int dh = fp_home(c, s->dst);
                fin[nfin].src = srcv;
                fin[nfin].dst = dh;
                fin[nfin].spill = dh < 0 ? fp_spill(c, s->dst) : 0;
                fin[nfin].isd = 2;
                fin[nfin].stack = from_stack;
                fin[nfin].done = 0;
                nfin++;
            }
            continue;
        }
        if (scalar_fp_val(c, s->dst)) {
            int isd = s->width == 8;
            int from_stack = s->force_stack || fp_idx >= 8;
            int srcv = from_stack ? save_total + 8 * stack_arg_idx++ : fp_idx++;
            mv[p].done = 1;
            mv[p].src_reg = -1;
            mv[p].src_vec = -1;
            mv[p].dst_reg = -1;
            mv[p].src_mem = -1;
            if (param_is_used(fn, s->dst)) {
                int dh = fp_home(c, s->dst);
                fin[nfin].src = srcv;
                fin[nfin].dst = dh;
                fin[nfin].spill = dh < 0 ? fp_spill(c, s->dst) : 0;
                fin[nfin].isd = isd;
                fin[nfin].stack = from_stack;
                fin[nfin].done = 0;
                nfin++;
            }
            continue;
        }
        if (s->is_float && !s->force_stack) {
            /* HFA eightbyte: bits arrive in the next V register. */
            mv[p].src_vec = fp_idx++;
        } else if (is_sret) {
            /* Darwin indirect result pointer.  Does not consume x0. */
            mv[p].src_reg = A64_X8;
        } else if (s->force_stack || gp_idx >= 8) {
            /* Above the saved frame (Darwin ABI: no pushed return addr). */
            mv[p].src_mem = save_total + 8 * stack_arg_idx++;
        } else {
            mv[p].src_reg = A64_X0 + gp_idx++;
        }
        int hr = home_reg(c, s->dst);
        if (hr >= 0) { mv[p].dst_reg = hr; mv[p].dst_mem = -1; }
        else { mv[p].dst_reg = -1; mv[p].dst_mem = spill_off(c, s->dst); }
        mv[p].is64 = s->width == 8;
        if (!param_is_used(fn, s->dst))
            mv[p].done = 1;
        else
            mv[p].done = (mv[p].src_reg >= 0 && mv[p].dst_reg == mv[p].src_reg);
    }
    int remaining = 0;
    for (int p = 0; p < nparams; p++) remaining += !mv[p].done;
#define PM_EMIT(p) do {                                                      \
        PMove *_m = &mv[(p)];                                                \
        if (_m->is_blob) {                                                   \
            int _addr = SCR1;                                                \
            a64_add_imm12(a, _addr, A64_FP, (unsigned)_m->src_mem, 0, 1, 0);  \
            if (_m->dst_reg >= 0) {                                          \
                if (_m->dst_reg != _addr)                                    \
                    a64_mov_reg(a, _m->dst_reg, _addr, 1);                   \
            } else {                                                         \
                frame_store(c, _addr, _m->dst_mem, 8, 0);                       \
            }                                                                \
        } else if (_m->src_vec >= 0) {                                       \
            int _tmp = _m->dst_reg >= 0 ? _m->dst_reg : SCR1;                \
            a64_fmov_gp(a, _tmp, _m->src_vec, 0, _m->is64);                  \
            if (_m->dst_reg < 0)                                             \
                frame_store(c, _tmp, _m->dst_mem, 8, 0);                        \
        } else if (_m->dst_reg >= 0) {                                       \
            if (_m->src_reg >= 0) {                                          \
                if (_m->src_reg != _m->dst_reg)                              \
                    a64_mov_reg(a, _m->dst_reg, _m->src_reg, _m->is64);      \
            } else {                                                         \
                frame_load(c, SCR1, _m->src_mem, 8, 1, 0);                      \
                a64_mov_reg(a, _m->dst_reg, SCR1, _m->is64);                 \
            }                                                                \
        } else {                                                             \
            if (_m->src_reg >= 0)                                            \
                a64_mov_reg(a, SCR1, _m->src_reg, _m->is64);                 \
            else                                                             \
                frame_load(c, SCR1, _m->src_mem, 8, 1, 0);                      \
            frame_store(c, SCR1, _m->dst_mem, 8, 0);                            \
        }                                                                    \
        _m->done = 1; remaining--;                                           \
    } while (0)

    /* Incoming V-reg parameters can be homed in v0..v7, so place them
     * before the GP shuffle and break cycles through v31. */
    {
        int nleft = nfin;
        while (nleft > 0) {
            int picked = -1;
            for (int i = 0; i < nfin; i++) {
                if (fin[i].done) continue;
                int blocked = 0;
                if (fin[i].dst >= 0)
                    for (int j = 0; j < nfin; j++) {
                        if (fin[j].done || j == i || fin[j].stack) continue;
                        if (fin[j].src == fin[i].dst) { blocked = 1; break; }
                    }
                /* An HFA eightbyte still parked in an incoming V register is
                 * not in fin at all -- the GP shuffle below consumes it, and
                 * that runs only after this loop -- so the check above cannot
                 * see it.  Writing into a register that still holds one
                 * destroys the value before it is read: a scalar double homed
                 * into v0 overwrote the real part of a __complex__ double
                 * arriving in v0/v1, and `cond ? a : b` then returned b as
                 * the real part. */
                if (!blocked && fin[i].dst >= 0)
                    for (int q = 0; q < nparams; q++)
                        if (!mv[q].done && mv[q].src_vec == fin[i].dst) {
                            blocked = 1;
                            break;
                        }
                if (!blocked) { picked = i; break; }
            }
            if (picked < 0) {
                /* Every pending fin write would land on a V register an HFA
                 * eightbyte still needs.  Consume those eightbytes first: a
                 * move with src_vec >= 0 only reads a V register and writes a
                 * GP one, which no pending float move touches.  Skip one
                 * whose destination GP register is still another pending
                 * move's incoming source -- that is a real cycle and the
                 * shuffle below breaks it properly. */
                int released = 0;
                for (int q = 0; q < nparams; q++) {
                    if (mv[q].done || mv[q].src_vec < 0) continue;
                    int cycle = 0;
                    if (mv[q].dst_reg >= 0)
                        for (int r = 0; r < nparams; r++)
                            if (!mv[r].done && r != q
                                && mv[r].src_reg == mv[q].dst_reg) {
                                cycle = 1;
                                break;
                            }
                    if (cycle) continue;
                    PM_EMIT(q);
                    released = 1;
                    break;
                }
                if (released) continue;
                for (int i = 0; i < nfin; i++)
                    if (!fin[i].done && !fin[i].stack) {
                        if (fin[i].isd == 2) fmov_q(a, FSCR, fin[i].src);
                        else a64_fmov_reg(a, FSCR, fin[i].src, fin[i].isd);
                        fin[i].src = FSCR;
                        break;
                    }
                continue;
            }
            FIn *m = &fin[picked];
            int tmp = m->dst >= 0 ? m->dst : A64_V30;
            if (m->stack) {
                emit_fp_addr(c, SCR0, m->src);
                if (m->isd == 2) ldr_q(a, tmp, SCR0, 0);
                else if (m->isd) a64_ldr_d(a, tmp, SCR0, 0);
                else a64_ldr_s(a, tmp, SCR0, 0);
            } else if (m->isd == 2) {
                fmov_q(a, tmp, m->src);
            } else if (tmp != m->src) {
                a64_fmov_reg(a, tmp, m->src, m->isd);
            }
            if (m->dst < 0) {
                if (m->isd == 2) {
                    emit_fp_addr(c, SCR1, m->spill);
                    str_q(a, tmp, SCR1, 0);
                } else {
                    fp_frame(c, tmp, m->spill, m->isd, 1, 0);
                }
            }
            m->done = 1;
            nleft--;
        }
    }
    while (remaining > 0) {
        int picked = -1;
        for (int p = 0; p < nparams; p++) {
            if (mv[p].done) continue;
            int blocked = 0;
            for (int q = 0; q < nparams; q++) {
                if (mv[q].done || q == p) continue;
                /* Emitting p overwrites Wp; blocked while some pending q
                 * still needs the incoming value held in that register. */
                if (mv[p].dst_reg >= 0 && mv[q].src_reg >= 0 &&
                    mv[q].src_reg != mv[q].dst_reg &&
                    mv[p].dst_reg == mv[q].src_reg) { blocked = 1; break; }
            }
            if (!blocked) { picked = p; break; }
        }
        if (picked >= 0) { PM_EMIT(picked); continue; }

        /* No safe move: the pending reg->reg items form one or more pure
         * permutation cycles over x0..x7.  Resolve one cycle: save its
         * first source in x16, then walk destinations backwards, closing
         * with the saved value. */
        int i0 = -1;
        for (int p = 0; p < nparams; p++)
            if (!mv[p].done && mv[p].src_reg >= 0 && mv[p].dst_reg >= 0) {
                i0 = p; break;
            }
        if (i0 < 0) {
            c64_die(c, &fn->insts.data[0], "parameter shuffle");
            break;
        }
        /* Cycles may mix 32/64-bit params; move whole registers so a
         * 64-bit value cannot lose its upper half to a w-form move. */
        mv[i0].is64 = 1;
        a64_mov_reg(a, SCR0, mv[i0].src_reg, 1);
        int cur = mv[i0].src_reg;
        for (;;) {
            int q = -1;
            for (int p = 0; p < nparams; p++)
                if (!mv[p].done && mv[p].dst_reg >= 0 &&
                    mv[p].dst_reg != mv[p].src_reg &&
                    mv[p].dst_reg == cur) { q = p; break; }
            if (q < 0) {
                c64_die(c, &fn->insts.data[0], "parameter cycle");
                remaining = 0;
                break;
            }
            if (q == i0) {
                a64_mov_reg(a, mv[i0].dst_reg, SCR0, 1);
                mv[i0].done = 1; remaining--;
                break;
            }
            cur = mv[q].src_reg;
            mv[q].is64 = 1;
            PM_EMIT(q);
        }
    }
    free(mv);
#undef PM_EMIT
    c->vararg_off = save_total + 8 * stack_arg_idx;

    /* ---- Body ---- */
    int epilog = a64_new_label(a);
    c->epilog_label = epilog;
    for (size_t j = nparams; j < fn->insts.len; j++) {
        const IRInst *s = &fn->insts.data[j];
        if (c64_ir_dump_on())
            fprintf(stderr, "  IR[%zu] op=%d a=%d b=%d dst=%d imm=%d w=%d "
                            "name=%s\n",
                    j, (int)s->op, (int)s->a, (int)s->b, (int)s->dst,
                    (int)s->imm, (int)s->width,
                    s->call_name ? s->call_name : "-");
        switch (s->op) {
        case IR_CONST: {
            if (scalar_fp_val(c, s->dst) || s->is_float) {
                if (s->width != 4 && s->width != 8)
                    c64_die(c, s, "long double");
                int isd = s->width == 8;
                int h = fp_home(c, s->dst);
                int tmp = h >= 0 ? h : FSCR;
                emit_mov_imm_w(a, SCR0, s->float_imm, isd);
                a64_fmov_gp(a, tmp, SCR0, 1, isd);
                if (h < 0) commit_fp(c, s->dst, tmp);
                break;
            }
            int d = dst_reg(c, s->dst);
            emit_mov_imm_w(a, d, s->imm, s->width == 8);
            commit(c, s->dst, d);
            break;
        }
        case IR_COPY: {
            if (vec16_val(c, s->dst) || vec16_val(c, s->a)) {
                int r = load_q(c, s->a, -1);
                commit_q(c, s->dst, r);
                break;
            }
            if (scalar_fp_val(c, s->dst) || scalar_fp_val(c, s->a)) {
                int r = load_fp(c, s->a, -1);
                commit_fp(c, s->dst, r);
                break;
            }
            int ra = load_op(c, s->a, -1);
            int d = dst_reg(c, s->dst);
            if (d != ra) a64_mov_reg(a, d, ra, s->width == 8);
            commit(c, s->dst, d);
            break;
        }
        case IR_TRUNC: {
            if (vec16_val(c, s->dst) || vec16_val(c, s->a)) {
                int r = load_q(c, s->a, -1);
                commit_q(c, s->dst, r);
                break;
            }
            if (scalar_fp_val(c, s->dst) || scalar_fp_val(c, s->a)) {
                int r = load_fp(c, s->a, -1);
                commit_fp(c, s->dst, r);
                break;
            }
            int ra = load_op(c, s->a, -1);
            int d = dst_reg(c, s->dst);
            int fromw = vw(c, s->a);
            /* A register keeps its high bits, so narrowing must re-establish
             * the value-of-width-W invariant, matching the x86 backend's
             * mask_to_width: a signed cast sign-extends (SXTB/SXTH), an
             * unsigned one zero-extends (UXTB/UXTH).  Treating TRUNC as a bare
             * move left the discarded bits behind and the compare opcodes use
             * the register as-is -- (unsigned char)(x & 256) then read 256.
             * The 4-byte target is a plain W move, which clears the top word. */
            if (s->width < 4 && fromw > s->width) {
                if (s->is_unsigned)
                    a64_uxt(a, d, ra, (unsigned)s->width, 0);
                else
                    a64_sxt(a, d, ra, (unsigned)s->width, 0);
            }
            else if (d != ra)
                a64_mov_reg(a, d, ra, s->width == 8);
            commit(c, s->dst, d);
            break;
        }
        case IR_SEXT: {
            int ra = load_op(c, s->a, -1);
            int d = dst_reg(c, s->dst);
            int fromw = vw(c, s->a);
            if (fromw >= (int)s->width)
                a64_mov_reg(a, d, ra, s->width == 8);
            else
                a64_sxt(a, d, ra, (unsigned)fromw, s->width == 8);
            commit(c, s->dst, d);
            break;
        }
        case IR_ZEXT: {
            int ra = load_op(c, s->a, -1);
            int d = dst_reg(c, s->dst);
            int fromw = vw(c, s->a);
            if (fromw >= (int)s->width)
                a64_mov_reg(a, d, ra, s->width == 8);
            else
                a64_uxt(a, d, ra, (unsigned)fromw, s->width == 8);
            commit(c, s->dst, d);
            break;
        }
        case IR_NEG: {
            if (scalar_fp_val(c, s->a) || scalar_fp_val(c, s->dst)) {
                int r = load_fp(c, s->a, -1);
                int h = fp_home(c, s->dst);
                int d = h >= 0 ? h : FSCR;
                int isd = vw(c, s->a) == 8;
                a64_word(a, (isd ? 0x1E614000u : 0x1E214000u)
                          | ((r & 31) << 5) | (d & 31));
                if (h < 0) commit_fp(c, s->dst, d);
                break;
            }
            int ra = load_op(c, s->a, -1);
            int d = dst_reg(c, s->dst);
            a64_neg(a, d, ra, s->width == 8);
            commit(c, s->dst, d);
            break;
        }
        case IR_BNOT: {
            int ra = load_op(c, s->a, -1);
            int d = dst_reg(c, s->dst);
            a64_mvn(a, d, ra, s->width == 8);
            commit(c, s->dst, d);
            break;
        }
        case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
        case IR_BAND: case IR_BOR: case IR_BXOR: case IR_SHL: case IR_SHR:
        case IR_ROL:
        case IR_EQ: case IR_NE: case IR_LT: case IR_LE: case IR_GT:
        case IR_GE:
            emit_binop(c, s);
            break;

        case IR_VADD: case IR_VSUB: case IR_VMUL: case IR_VDIV:
        case IR_VBAND: case IR_VBOR: case IR_VBXOR:
            emit_vec(c, s);
            break;

        case IR_ALLOCA:
            break;  /* slot assigned in the frame plan */
        case IR_ADDR: {
            /* &alloca(alloca id in a) */
            int d = dst_reg(c, s->dst);
            int off = (s->a >= 0 && s->a < nv) ? c->alloca_off[s->a] : 0;
            emit_fp_addr_aligned(c, d, off, alloca_align_of(c, s->a, nv));
            commit(c, s->dst, d);
            break;
        }
        case IR_STORE_PTR: {
            /* imm 3: release or seq_cst store.  x86 ignores imm and keeps
             * a plain store, which is what that backend already emitted. */
            if (s->imm == 3 && !vec16_val(c, s->b) && scalar_fp_val(c, s->b)) {
                /* stlr has no scalar-fp form.  Move the bits through a GPR. */
                int isd = vw(c, s->b) == 8;
                int fv = load_fp(c, s->b, -1);
                int bits = SCR0;
                a64_fmov_gp(a, bits, fv, 0, isd);
                int p = load_ptrv(c, s->a, bits);
                if (p == bits) {
                    a64_mov_reg(a, SCR1, bits, 1);
                    bits = SCR1;
                    p = load_ptrv(c, s->a, bits);
                }
                uint32_t base = isd ? 0xC89FFC00u : 0x889FFC00u;
                a64_word(a, base | ((uint32_t)(p & 31) << 5) | (uint32_t)(bits & 31));
                break;
            }
            if (s->imm == 3 && !vec16_val(c, s->b) && !scalar_fp_val(c, s->b)) {
                int p = load_ptrv(c, s->a, -1);
                int v = load_op(c, s->b, p);
                uint32_t base = s->width <= 1 ? 0x089FFC00u
                              : s->width == 2 ? 0x489FFC00u
                              : s->width == 4 ? 0x889FFC00u
                              : 0xC89FFC00u;
                a64_word(a, base | ((uint32_t)(p & 31) << 5) | (uint32_t)(v & 31));
                break;
            }
            if (vec16_val(c, s->b)) {
                int v = load_q(c, s->b, -1);
                int p = load_ptrv(c, s->a, SCR0);
                str_q(a, v, p, 0);
                break;
            }
            if (scalar_fp_val(c, s->b)) {
                /* Float materialization uses x16; load the address after it. */
                int v = load_fp(c, s->b, -1);
                int p = load_ptrv(c, s->a, SCR0);
                if (vw(c, s->b) == 8) a64_str_d(a, v, p, 0);
                else a64_str_s(a, v, p, 0);
                break;
            }
            int p = load_ptrv(c, s->a, -1);
            int v = load_op(c, s->b, p);
            emit_store(c, v, p, s->width);
            break;
        }
        case IR_LOAD_PTR: {
            /* imm 1: acquire/consume (ldapr).  imm 2: seq_cst (ldar). */
            if ((s->imm == 1 || s->imm == 2) && scalar_fp_val(c, s->dst)) {
                /* ldapr/ldar write a GPR.  Move the bits into the float dest. */
                int isd = vw(c, s->dst) == 8;
                int p = load_ptrv(c, s->a, -1);
                int bits = (p == SCR0) ? SCR1 : SCR0;
                int w = isd ? 8 : 4;
                uint32_t base = s->imm == 2
                    ? (w == 4 ? 0x88DFFC00u : 0xC8DFFC00u)
                    : (w == 4 ? 0xB8BFC000u : 0xF8BFC000u);
                a64_word(a, base | ((uint32_t)(p & 31) << 5) | (uint32_t)(bits & 31));
                int h = fp_home(c, s->dst);
                int d = h >= 0 ? h : FSCR;
                a64_fmov_gp(a, d, bits, 1, isd);
                if (h < 0) commit_fp(c, s->dst, d);
                break;
            }
            if ((s->imm == 1 || s->imm == 2) && !vec16_val(c, s->dst)
                && !scalar_fp_val(c, s->dst)) {
                int p = load_ptrv(c, s->a, -1);
                int d = dst_reg(c, s->dst);
                int w = s->width;
                uint32_t base = s->imm == 2
                    ? (w <= 1 ? 0x08DFFC00u : w == 2 ? 0x48DFFC00u
                       : w == 4 ? 0x88DFFC00u : 0xC8DFFC00u)
                    : (w <= 1 ? 0x38BFC000u : w == 2 ? 0x78BFC000u
                       : w == 4 ? 0xB8BFC000u : 0xF8BFC000u);
                a64_word(a, base | ((uint32_t)(p & 31) << 5) | (uint32_t)(d & 31));
                if (!s->is_unsigned && w < 8)
                    a64_sxt(a, d, d, (unsigned)w, 1);
                commit(c, s->dst, d);
                break;
            }
            if (vec16_val(c, s->dst)) {
                int p = load_ptrv(c, s->a, -1);
                int h = fp_home(c, s->dst);
                int d = h >= 0 ? h : FSCR;
                ldr_q(a, d, p, 0);
                if (h < 0) commit_q(c, s->dst, d);
                break;
            }
            if (scalar_fp_val(c, s->dst)) {
                int p = load_ptrv(c, s->a, -1);
                int h = fp_home(c, s->dst);
                int d = h >= 0 ? h : FSCR;
                if (vw(c, s->dst) == 8) a64_ldr_d(a, d, p, 0);
                else a64_ldr_s(a, d, p, 0);
                if (h < 0) commit_fp(c, s->dst, d);
                break;
            }
            int p = load_ptrv(c, s->a, -1);
            int d = dst_reg(c, s->dst);
            emit_load(c, d, p, s->width, s->is_unsigned);
            commit(c, s->dst, d);
            break;
        }

        case IR_LABEL:
            a64_bind(a, vlabels_get(c, (int)s->imm));
            break;
        case IR_BR:
            a64_b(a, vlabels_get(c, (int)s->imm));
            break;
        case IR_CBR: {
            int r = load_op(c, s->a, -1);
            a64_cbz(a, r, vlabels_get(c, (int)s->b), 1);
            a64_b(a, vlabels_get(c, (int)s->imm));
            break;
        }
        case IR_JMP_PTR: {
            int r = load_ptrv(c, s->a, -1);
            a64_br_reg(a, r);
            break;
        }
        case IR_LADDR: {
            int d = dst_reg(c, s->dst);
            int lab = vlabels_get(c, (int)s->imm);
            /* A multi-TU module is merged by the linker, so the label's
             * address is not known here -- the same reason pointer
             * initializers need a relocation (f10186a3).  Baking the
             * module-local offset in produced a computed goto that branched
             * into the Mach-O header once another module was linked in. */
            if (emit_object_mode() || emit_multi_tu()) {
                const char *nm = note_label_sym(c, (int)s->imm, lab);
                note_page_reloc(c, (uint32_t)a->code.len, -1, nm, s->dst);
                a64_word(a, 0x90000000u | (uint32_t)(d & 31));
                a64_word(a, 0x91000000u | ((uint32_t)(d & 31) << 5)
                                        | (uint32_t)(d & 31));
            } else {
                a64_adrp_add_label(a, d, lab);
            }
            commit(c, s->dst, d);
            break;
        }

        case IR_CALL:
            emit_call(c, s);
            break;

        case IR_RETURN: {
            if (s->a >= 0 && vec16_val(c, s->a)) {
                int r = load_q(c, s->a, -1);
                fmov_q(a, A64_V0, r);
                a64_b(a, epilog);
                break;
            }
            if (s->a >= 0 && scalar_fp_val(c, s->a)) {
                int r = load_fp(c, s->a, -1);
                int isd = vw(c, s->a) == 8;
                if (r != A64_V0) a64_fmov_reg(a, A64_V0, r, isd);
                a64_b(a, epilog);
                break;
            }
            if (s->align16 == A64_MARK_HFA && s->a >= 0) {
                int r0 = load_op(c, s->a, -1);
                int r1 = s->b >= 0 ? load_op(c, s->b, r0) : -1;
                int r2 = s->c >= 0 ? load_op(c, s->c, r1) : -1;
                int r3 = s->d >= 0 ? load_op(c, s->d, r2) : -1;
                a64_fmov_gp(a, A64_V0, r0, 1, vw(c, s->a) == 8);
                if (r1 >= 0)
                    a64_fmov_gp(a, A64_V1, r1, 1, vw(c, s->b) == 8);
                if (r2 >= 0)
                    a64_fmov_gp(a, A64_V2, r2, 1, vw(c, s->c) == 8);
                if (r3 >= 0)
                    a64_fmov_gp(a, A64_V3, r3, 1, vw(c, s->d) == 8);
            } else if (s->a >= 0 && s->b >= 0) {
                int r0 = load_op(c, s->a, -1);
                int r1 = load_op(c, s->b, r0);
                int wide = vw(c, s->a) == 8;
                if (r0 == A64_X1) {
                    a64_mov_reg(a, SCR0, r0, wide);
                    if (r1 != A64_X1) a64_mov_reg(a, A64_X1, r1, vw(c, s->b) == 8);
                    a64_mov_reg(a, A64_X0, SCR0, wide);
                } else {
                    if (r1 != A64_X1) a64_mov_reg(a, A64_X1, r1, vw(c, s->b) == 8);
                    if (r0 != A64_X0) a64_mov_reg(a, A64_X0, r0, wide);
                }
            } else if (s->a >= 0) {
                int r = load_op(c, s->a, -1);
                int wide = vw(c, s->a) == 8 || fn->ret_width >= 8;
                if (r != A64_X0)
                    a64_mov_reg(a, A64_X0, r, wide);
            }
            a64_b(a, epilog);
            break;
        }

        case IR_FRAME_ADDR: {
            /* [fp] is the caller's frame pointer, same chain as rbp. */
            int d = dst_reg(c, s->dst);
            a64_mov_reg(a, d, A64_FP, 1);
            int level = (int)s->imm;
            if (level < 0) level = 0;
            for (int i = 0; i < level; i++)
                a64_ldr64(a, d, d, 0);
            commit(c, s->dst, d);
            break;
        }
        case IR_RETURN_ADDR: {
            int d = dst_reg(c, s->dst);
            a64_mov_reg(a, d, A64_FP, 1);
            int level = (int)s->imm;
            if (level < 0) level = 0;
            for (int i = 0; i < level; i++)
                a64_ldr64(a, d, d, 0);
            a64_ldr64(a, d, d, 8);
            commit(c, s->dst, d);
            break;
        }
        case IR_STACK_SAVE: {
            int d = dst_reg(c, s->dst);
            mov_sp_like(a, d, A64_SP);
            commit(c, s->dst, d);
            break;
        }
        case IR_STACK_RESTORE: {
            int r = load_op(c, s->a, -1);
            mov_sp_like(a, A64_SP, r);
            break;
        }
        case IR_LONGJMP: {
            /* buf[0] = fp, buf[1] = resume pc, buf[2] = sp, matching the
             * x86 lowering.  Read all three before writing fp or sp.
             * x0/x1 are not allocatable; a spilled pointer arrives in x16. */
            int buf = load_ptrv(c, s->a, -1);
            int pc = A64_X0;
            int spv = A64_X1;
            if (buf == pc) pc = SCR0;
            if (buf == spv) spv = SCR1;
            a64_ldr64(a, pc, buf, 8);
            a64_ldr64(a, spv, buf, 16);
            a64_ldr64(a, A64_FP, buf, 0);
            mov_sp_like(a, A64_SP, spv);
            a64_br_reg(a, pc);
            break;
        }
        case IR_DYN_ALLOCA: {
            /* -1 is the "no slot" sentinel; a real offset is always negative. */
            int dyn_slot = c->dyn_sp_off != -1 ? c->dyn_sp_off : 0;
            int r = load_op(c, s->a, -1);
            if (s->b >= 0) {
                /* alloca_with_align: alignment arrives in bits.  The ABI
                 * still requires SP to stay 16-byte aligned, so anything
                 * finer than that is raised.  For a power of two,
                 * ~(align-1) is just -align. */
                if (r == SCR1) {
                    a64_mov_reg(a, SCR0, r, 1);
                    r = SCR0;
                }
                int al = load_op(c, s->b, r);
                if (al == r) {
                    a64_mov_reg(a, SCR1, al, 1);
                    al = SCR1;
                }
                a64_lsr_imm(a, SCR1, al, 3, 1);
                int Lbig = a64_new_label(a);
                a64_cmp_imm12(a, SCR1, 16, 0, 1);
                a64_bcond(a, A64_CS, Lbig);
                a64_movz(a, SCR1, 16, 0, 1);
                a64_bind(a, Lbig);
                a64_neg(a, SCR1, SCR1, 1);
                mov_sp_like(a, A64_X0, A64_SP);
                a64_sub_reg(a, A64_X0, A64_X0, r, A64_LSL, 0, 1, 0);
                a64_and_reg(a, A64_X0, A64_X0, SCR1, 1);
                mov_sp_like(a, A64_SP, A64_X0);
                int d = dst_reg(c, s->dst);
                if (d != A64_X0) a64_mov_reg(a, d, A64_X0, 1);
                if (dyn_slot) {
                    emit_fp_addr(c, SCR0, dyn_slot);
                    mov_sp_like(a, SCR1, A64_SP);
                    a64_str64(a, SCR1, SCR0, 0);
                }
                commit(c, s->dst, d);
                break;
            }
            /* round size up to 16, then bump sp via a GP staging reg
             * (SUB shifted-register cannot address SP). */
            a64_add_imm12(a, SCR1, r, 15, 0, 1, 0);
            if (a64_and_imm(a, SCR1, SCR1, ~(uint64_t)15, 1) != 0)
                a64_mov_reg(a, SCR1, r, 1);
            mov_sp_like(a, SCR0, A64_SP);
            a64_sub_reg(a, SCR0, SCR0, SCR1, A64_LSL, 0, 1, 0);
            mov_sp_like(a, A64_SP, SCR0);
            int d = dst_reg(c, s->dst);
            a64_mov_reg(a, d, SCR0, 1);
            if (dyn_slot) {
                emit_fp_addr(c, SCR1, dyn_slot);
                mov_sp_like(a, SCR0, A64_SP);
                a64_str64(a, SCR0, SCR1, 0);
            }
            commit(c, s->dst, d);
            break;
        }

        case IR_DBG_VALUE:
            break;

        case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV: {
            int isd = s->width == 8;
            int ra = load_fp(c, s->a, -1);
            int rb = load_fp(c, s->b, ra);
            int h = fp_home(c, s->dst);
            int d = h >= 0 ? h : FSCR;
            if (s->op == IR_FADD) a64_fadd(a, d, ra, rb, isd);
            else if (s->op == IR_FSUB) a64_fsub(a, d, ra, rb, isd);
            else if (s->op == IR_FMUL) a64_fmul(a, d, ra, rb, isd);
            else a64_fdiv(a, d, ra, rb, isd);
            if (h < 0) commit_fp(c, s->dst, d);
            break;
        }
        case IR_FCMP: {
            int isd = vw(c, s->a) == 8;
            int ra = load_fp(c, s->a, -1);
            int rb = load_fp(c, s->b, ra);
            a64_fcmp(a, ra, rb, isd);
            int d = dst_reg(c, s->dst);
            /* FCMP sets NZCV.  Ordered LE is EQ|LT; the single LE
             * condition is true for NaN, which C does not want. */
            if (s->is_unsigned == 1) {
                int tmp = (d == SCR0) ? SCR1 : SCR0;
                a64_cset(a, d, A64_EQ, 0);
                a64_cset(a, tmp, A64_MI, 0);
                a64_or_reg(a, d, d, tmp, 0);
            } else {
                int cond = A64_MI;
                switch (s->is_unsigned) {
                case 0: cond = A64_MI; break;
                case 2: cond = A64_GT; break;
                case 3: cond = A64_GE; break;
                case 4: cond = A64_EQ; break;
                default: cond = A64_NE; break;
                }
                a64_cset(a, d, cond, 0);
            }
            commit(c, s->dst, d);
            break;
        }
        case IR_SITOFP: {
            int src_w = s->imm ? (int)s->imm : 4;
            int src_u = s->is_unsigned;
            int isd = s->width == 8;
            int gp = load_op(c, s->a, -1);
            if (src_w < 4) {
                int tmp = (gp == SCR0) ? SCR1 : SCR0;
                if (src_u) a64_uxt(a, tmp, gp, (unsigned)src_w, 0);
                else a64_sxt(a, tmp, gp, (unsigned)src_w, 0);
                gp = tmp;
                src_w = 4;
            }
            int from64 = src_w >= 8;
            uint32_t base;
            if (isd && from64) base = src_u ? 0x9E630000u : 0x9E620000u;
            else if (isd) base = src_u ? 0x1E630000u : 0x1E620000u;
            else if (from64) base = src_u ? 0x9E230000u : 0x9E220000u;
            else base = src_u ? 0x1E230000u : 0x1E220000u;
            int h = fp_home(c, s->dst);
            int d = h >= 0 ? h : FSCR;
            a64_word(a, base | ((gp & 31) << 5) | (d & 31));
            if (h < 0) commit_fp(c, s->dst, d);
            break;
        }
        case IR_FPTOSI: {
            int isd = vw(c, s->a) == 8;
            int fs = load_fp(c, s->a, -1);
            int to64 = s->width >= 8;
            int un = s->is_unsigned;
            uint32_t base;
            if (isd && to64) base = un ? 0x9E790000u : 0x9E780000u;
            else if (isd) base = un ? 0x1E790000u : 0x1E780000u;
            else if (to64) base = un ? 0x9E390000u : 0x9E380000u;
            else base = un ? 0x1E390000u : 0x1E380000u;
            int d = dst_reg(c, s->dst);
            a64_word(a, base | ((fs & 31) << 5) | (d & 31));
            if (s->width < 4) {
                /* Keep the low bytes.  The convert wrote a W or X. */
                if (s->width == 1) a64_uxt(a, d, d, 1, 0);
                else if (s->width == 2) a64_uxt(a, d, d, 2, 0);
            }
            commit(c, s->dst, d);
            break;
        }
        case IR_FPEXT: case IR_FPTRUNC: {
            int src_d = vw(c, s->a) == 8;
            int dst_d = s->width == 8;
            int r = load_fp(c, s->a, -1);
            int h = fp_home(c, s->dst);
            int d = h >= 0 ? h : FSCR;
            if (src_d == dst_d) {
                if (d != r) a64_fmov_reg(a, d, r, dst_d);
            } else {
                uint32_t base = dst_d ? 0x1E22C000u : 0x1E624000u;
                a64_word(a, base | ((r & 31) << 5) | (d & 31));
            }
            if (h < 0) commit_fp(c, s->dst, d);
            break;
        }
        case IR_LOAD: {
            /* At -O0 a is a pinned-alloca slot (ternary/short-circuit
             * temps).  The unpinned SSA form (-O1) is a plain copy. */
            if (scalar_fp_val(c, s->dst)) {
                int off = (s->a >= 0 && s->a < nv) ? c->alloca_off[s->a] : 0;
                int saln = alloca_align_of(c, s->a, nv);
                int h = fp_home(c, s->dst);
                int d = h >= 0 ? h : FSCR;
                if (off != 0) {
                    fp_frame(c, d, off, vw(c, s->dst) == 8, 0, saln);
                } else {
                    int r = load_fp(c, s->a, d == FSCR ? -1 : d);
                    if (d != r) a64_fmov_reg(a, d, r, vw(c, s->dst) == 8);
                }
                if (h < 0) commit_fp(c, s->dst, d);
                break;
            }
            int off = (s->a >= 0 && s->a < nv) ? c->alloca_off[s->a] : 0;
            int d = dst_reg(c, s->dst);
            if (off != 0) {
                frame_load(c, d, off, s->width, s->is_unsigned,
                           alloca_align_of(c, s->a, nv));
            } else {
                int ra = load_op(c, s->a, -1);
                if (d != ra) a64_mov_reg(a, d, ra, s->width == 8);
            }
            commit(c, s->dst, d);
            break;
        }
        case IR_STORE: {
            if (scalar_fp_val(c, s->b)) {
                int off = (s->a >= 0 && s->a < nv) ? c->alloca_off[s->a] : 0;
                int saln = alloca_align_of(c, s->a, nv);
                int v = load_fp(c, s->b, -1);
                if (off != 0) fp_frame(c, v, off, vw(c, s->b) == 8, 1, saln);
                else commit_fp(c, s->a, v);
                break;
            }
            int off = (s->a >= 0 && s->a < nv) ? c->alloca_off[s->a] : 0;
            int saln = alloca_align_of(c, s->a, nv);
            int v = load_op(c, s->b, -1);
            if (off != 0) {
                frame_store(c, v, off, s->width, saln);
            } else {
                /* -O1 SSA form: make value a's home hold b. */
                int d = dst_reg(c, s->a);
                if (d != v) a64_mov_reg(a, d, v, s->width == 8);
                commit(c, s->a, d);
            }
            break;
        }
        case IR_GADDR: {
            int gi = find_global_idx(c->ir, s->call_name);
            /* The symbol may live in a sibling TU (the builtin runtime
             * linked alongside the program is the common case), so the same
             * rule as calls applies: only a lone single-TU link has nowhere
             * else to look, and there a page reloc could not be resolved. */
            if (gi < 0 && !emit_object_mode() && !emit_multi_tu()) {
                c64_die(c, s, "external global variable");
                break;
            }
            int d = dst_reg(c, s->dst);
            /* adrp d, page ; add d, d, #pageoff — both words patched in
             * codegen64 once __const/__data/__bss placement is final. */
            note_page_reloc(c, (uint32_t)a->code.len, gi,
                            gi < 0 ? s->call_name : NULL, s->dst);
            /* A symbol defined in another image is reached through the GOT.
             * ADRP cannot span the 4 GiB gap down to a missing weak address,
             * so those stay a direct page reloc and the linker writes zero. */
            int use_got = gi < 0 && emit_object_mode()
                       && !c64_is_weak_ref(c->ir, s->call_name);
            if (use_got) c->gfix[c->ngfix - 1].got = 1;
            a64_word(a, 0x90000000u | (uint32_t)(d & 31));
            if (use_got)
                a64_word(a, 0xF9400000u | ((uint32_t)(d & 31) << 5)
                                        | (uint32_t)(d & 31));
            else
                a64_word(a, 0x91000000u | ((uint32_t)(d & 31) << 5)
                                        | (uint32_t)(d & 31));
            commit(c, s->dst, d);
            break;
        }
        case IR_GADDR_TLS: {
            int gi = find_global_idx(c->ir, s->call_name);
            /* A sibling TU may own it, same as any other external. */
            if (gi < 0 && !emit_object_mode() && !emit_multi_tu()) {
                c64_die(c, s, "external thread-local variable");
                break;
            }
            int d = dst_reg(c, s->dst);
            note_page_reloc(c, (uint32_t)a->code.len, gi,
                            gi < 0 ? s->call_name : NULL, s->dst);
            c->gfix[c->ngfix - 1].tlvp = 1;
            /* adrp x0, desc; ldr x0, [x0, #off]; ldr x16, [x0]; blr x16 */
            a64_word(a, 0x90000000u | (uint32_t)A64_X0);
            a64_word(a, 0xF9400000u | ((uint32_t)A64_X0 << 5) | (uint32_t)A64_X0);
            a64_ldr64(a, A64_X16, A64_X0, 0);
            a64_blr(a, A64_X16);
            if (d != A64_X0) a64_mov_reg(a, d, A64_X0, 1);
            commit(c, s->dst, d);
            break;
        }
        case IR_FADDR: {
            int fi = 0;
            int missing = find_function(c->ir, s->call_name, &fi) != 0;
            /* Taking a sibling TU's address is legal in a multi-TU link. */
            if (missing && !emit_object_mode() && !emit_multi_tu()) {
                c64_die(c, s, "external function address");
                break;
            }
            int d = dst_reg(c, s->dst);
            /* Object text is concatenated after an entry stub, and a
             * multi-TU module is merged by the linker, so in both cases a
             * baked ADRP misses the real page.  Record a reloc instead --
             * the same reason IR_LADDR and pointer initializers need one. */
            if (emit_object_mode() || emit_multi_tu()) {
                note_page_reloc(c, (uint32_t)a->code.len, -1, s->call_name, s->dst);
                int use_got = missing && !c64_is_weak_ref(c->ir, s->call_name);
                if (use_got) c->gfix[c->ngfix - 1].got = 1;
                a64_word(a, 0x90000000u | (uint32_t)(d & 31));
                if (use_got)
                    a64_word(a, 0xF9400000u | ((uint32_t)(d & 31) << 5)
                                            | (uint32_t)(d & 31));
                else
                    a64_word(a, 0x91000000u | ((uint32_t)(d & 31) << 5)
                                            | (uint32_t)(d & 31));
            } else {
                a64_adrp_add_label(a, d, c->fn_label[fi]);
            }
            commit(c, s->dst, d);
            break;
        }
        default:
            if (s->is_float) c64_die(c, s, "floating point");
            c64_die(c, s, "IR op");
            break;
        }
    }

    /* ---- Epilogue ---- */
    a64_bind(a, epilog);
    mov_sp_like(a, A64_SP, A64_FP);
    for (int p = 0; p < ncs / 2; p++)
        a64_ldp64(a, cs[2*p], cs[2*p+1], A64_FP, 16 + 16*p, A64_PAIR_OFFSET);
    if (ncs & 1)
        a64_ldr64(a, cs[ncs-1], A64_FP, 16 + 16*cs_pairs - 8);
    {
        int qbase = 16 + cs_area;
        for (int p = 0; p < nvcs / 2; p++)
            ldp_q(a, vcs[2 * p], vcs[2 * p + 1], A64_FP, qbase + 32 * p);
        if (nvcs & 1)
            ldr_q(a, vcs[nvcs - 1], A64_FP, qbase + 32 * (nvcs / 2));
    }
    a64_ldp64(a, A64_FP, A64_LR, A64_SP, save_total, A64_PAIR_POST);
    a64_ret(a, A64_LR);

    free(c->def);
    free(c->alloca_off);
    free(c->alloca_aln);
    free(c->spill_off);
    free(c->fp_spill_off);
    free(c->vlabels);
    c->ra = NULL;
}

/* Unsigned 128/128 restoring division.  Same contract as
 * runtime/int128.c __fakecc_udivmodti4: x0..x3 are the two 128-bit
 * values (lo, hi), x4..x7 are pointers to q_lo, q_hi, r_lo, r_hi.
 * A zero divisor yields quotient 0 and remainder = numerator. */
static void emit_udivmodti4(A64Asm *a, int label) {
    a64_bind(a, label);
    int Lzero = a64_new_label(a);
    int Ltop  = a64_new_label(a);
    int Lge   = a64_new_label(a);
    int Lbit  = a64_new_label(a);
    int Lsub  = a64_new_label(a);
    int Lqhi  = a64_new_label(a);
    int Lnext = a64_new_label(a);
    int Lstore = a64_new_label(a);

    a64_or_reg(a, A64_X14, A64_X2, A64_X3, 1);
    a64_cbz(a, A64_X14, Lzero, 1);

    a64_movz(a, A64_X9,  0, 0, 1);          /* q_lo */
    a64_movz(a, A64_X10, 0, 0, 1);          /* q_hi */
    a64_movz(a, A64_X11, 0, 0, 1);          /* r_lo */
    a64_movz(a, A64_X12, 0, 0, 1);          /* r_hi */
    a64_movz(a, A64_X13, 127, 0, 1);        /* bit index */

    a64_bind(a, Ltop);
    a64_lsr_imm(a, SCR0, A64_X12, 63, 1);   /* carry out of r_hi */
    a64_lsr_imm(a, A64_X14, A64_X11, 63, 1);
    a64_lsl_imm(a, A64_X12, A64_X12, 1, 1);
    a64_or_reg(a, A64_X12, A64_X12, A64_X14, 1);
    a64_lsl_imm(a, A64_X11, A64_X11, 1, 1);

    a64_cmp_imm12(a, A64_X13, 64, 0, 1);
    a64_bcond(a, A64_GE, Lge);
    a64_lsrv(a, A64_X14, A64_X0, A64_X13, 1);
    a64_b(a, Lbit);
    a64_bind(a, Lge);
    a64_sub_imm12(a, A64_X15, A64_X13, 64, 0, 1, 0);
    a64_lsrv(a, A64_X14, A64_X1, A64_X15, 1);
    a64_bind(a, Lbit);
    a64_movz(a, A64_X15, 1, 0, 1);
    a64_and_reg(a, A64_X14, A64_X14, A64_X15, 1);
    a64_or_reg(a, A64_X11, A64_X11, A64_X14, 1);

    a64_cbnz(a, SCR0, Lsub, 1);
    a64_cmp_reg(a, A64_X12, A64_X3, 1);
    a64_bcond(a, A64_HI, Lsub);
    a64_bcond(a, A64_CC, Lnext);
    a64_cmp_reg(a, A64_X11, A64_X2, 1);
    a64_bcond(a, A64_CC, Lnext);

    a64_bind(a, Lsub);
    a64_cmp_reg(a, A64_X11, A64_X2, 1);
    a64_cset(a, A64_X14, A64_CC, 1);        /* borrow */
    a64_sub_reg(a, A64_X11, A64_X11, A64_X2, A64_LSL, 0, 1, 0);
    a64_sub_reg(a, A64_X12, A64_X12, A64_X3, A64_LSL, 0, 1, 0);
    a64_sub_reg(a, A64_X12, A64_X12, A64_X14, A64_LSL, 0, 1, 0);
    a64_cmp_imm12(a, A64_X13, 64, 0, 1);
    a64_bcond(a, A64_GE, Lqhi);
    a64_movz(a, A64_X15, 1, 0, 1);
    a64_lslv(a, A64_X15, A64_X15, A64_X13, 1);
    a64_or_reg(a, A64_X9, A64_X9, A64_X15, 1);
    a64_b(a, Lnext);
    a64_bind(a, Lqhi);
    a64_sub_imm12(a, A64_X15, A64_X13, 64, 0, 1, 0);
    a64_movz(a, A64_X14, 1, 0, 1);
    a64_lslv(a, A64_X14, A64_X14, A64_X15, 1);
    a64_or_reg(a, A64_X10, A64_X10, A64_X14, 1);

    a64_bind(a, Lnext);
    a64_sub_imm12(a, A64_X13, A64_X13, 1, 0, 1, 0);
    a64_cmp_imm12(a, A64_X13, 0, 0, 1);
    a64_bcond(a, A64_GE, Ltop);
    a64_b(a, Lstore);

    a64_bind(a, Lzero);
    a64_movz(a, A64_X9,  0, 0, 1);
    a64_movz(a, A64_X10, 0, 0, 1);
    a64_mov_reg(a, A64_X11, A64_X0, 1);
    a64_mov_reg(a, A64_X12, A64_X1, 1);

    a64_bind(a, Lstore);
    a64_str64(a, A64_X9,  A64_X4, 0);
    a64_str64(a, A64_X10, A64_X5, 0);
    a64_str64(a, A64_X11, A64_X6, 0);
    a64_str64(a, A64_X12, A64_X7, 0);
    a64_ret(a, A64_LR);
}

/* ── Module entry point ──────────────────────────────────────────── */

void codegen64(const IRModule *ir, EmitModule *out, int want_debug) {
(void)want_debug;  /* DWARF emission lands in T20. */

    int main_id = -1;
    int have_main = find_function(ir, "main", &main_id) == 0;
    /* A TU without main is only an error when nothing else can supply it:
     * under -c the object carries no entry at all, and in a multi-TU link a
     * sibling TU may define it. */
    if (!have_main && !emit_object_mode() && !emit_multi_tu())
        die_at("<arm64>", 0, 0, "no 'main' function found");
    /* The LC_MAIN stub belongs to the one TU that defines main; in a multi-TU
     * link the rest contribute functions and data only. */
    /* Who writes the program entry point.  A lone freestanding TU has to
     * emit its own LC_MAIN stub.  Under -c the object must not contain one
     * (the linker writes it into the executable), and in a multi-TU link the
     * linker emits exactly one for the merged image -- a per-TU stub would
     * duplicate it and displace every symbol after it. */
    int want_entry = have_main && !emit_object_mode() && !emit_multi_tu();

    A64Asm a;
    a64_init(&a);

    C64 c;
    memset(&c, 0, sizeof c);
    c.as = &a;
    c.ir = ir;
    c.udiv_label = -1;
    c.tlv_thunk = -1;
    c.environ_off = -1;

    c.fn_label = xmalloc(ir->functions.len * sizeof(int));
    for (size_t i = 0; i < ir->functions.len; i++)
        c.fn_label[i] = a64_new_label(&a);

    /* Lay globals out into __const / __data / __bss (same placement rules
     * as the x86 path).  Their image addresses depend on the final text
     * length, so ADRP+ADD references are patched after resolve. */
    if (ir->globals.len) {
        c.gsect = xmalloc(ir->globals.len * sizeof(int));
        c.goff = xmalloc(ir->globals.len * sizeof(size_t));
        c.tlv_off = xmalloc(ir->globals.len * sizeof(int));
        for (size_t gi = 0; gi < ir->globals.len; gi++) c.tlv_off[gi] = -1;
    }
    for (size_t gi = 0; gi < ir->globals.len; gi++) {
        const IRGlobal *g = &ir->globals.data[gi];
        size_t al = g->align > 0 ? (size_t)g->align : 8;
        /* A readonly global that contains a pointer must live in __DATA:
         * dyld chained fixups are applied to writable pages, and
         * __TEXT,__const is mapped read-only/execute.  A C const object
         * with no fixup is the same kind of bytes as a string literal. */
        if ((g->is_readonly || (g->is_const_obj && g->init_bytes))
            && !g->num_fixups && !g->is_tls) {
            while (out->rodata.len % al) { char z = 0; buffer_append(&out->rodata, &z, 1); }
            c.gsect[gi] = G_RO;
            c.goff[gi] = out->rodata.len;
            if (g->init_bytes) {
                buffer_append(&out->rodata, g->init_bytes, g->size);
            } else {
                for (int k = 0; k < g->size; k++) { char z = 0; buffer_append(&out->rodata, &z, 1); }
            }
            if (al > out->rodata_align) out->rodata_align = al;
        } else if (g->init_bytes || g->num_fixups) {
            while (out->data.len % al) { char z = 0; buffer_append(&out->data, &z, 1); }
            c.gsect[gi] = G_DATA;
            c.goff[gi] = out->data.len;
            if (g->init_bytes) {
                buffer_append(&out->data, g->init_bytes, g->size);
            } else {
                for (int k = 0; k < g->size; k++) { char z = 0; buffer_append(&out->data, &z, 1); }
            }
            if (al > out->data_align) out->data_align = al;
            for (int fi = 0; fi < g->num_fixups; fi++) {
                if (c.npfix == c.cappfix) {
                    c.cappfix = c.cappfix ? c.cappfix * 2 : 16;
                    c.pfix = xrealloc(c.pfix, c.cappfix * sizeof *c.pfix);
                }
                c.pfix[c.npfix].gidx = (int)gi;
                c.pfix[c.npfix].slot_off = g->fixups[fi].offset;
                c.pfix[c.npfix].sym = g->fixups[fi].sym;
                c.pfix[c.npfix].sub = g->fixups[fi].sub;
                c.pfix[c.npfix].addend = g->fixups[fi].addend;
                c.npfix++;
            }
        } else if (emit_object_mode() && !g->is_static) {
            /* Tentative definition.  A later real definition in another
             * file must be able to win, so the object carries a common
             * symbol instead of its own BSS bytes. */
            c.gsect[gi] = G_COMMON;
            c.goff[gi] = al;
        } else {
            while (out->bss_size % al) out->bss_size++;
            c.gsect[gi] = G_BSS;
            c.goff[gi] = out->bss_size;
            out->bss_size += g->size;
            if (al > out->bss_align) out->bss_align = al;
        }
    }
    if (!emit_object_mode() && c.tlv_off) {
        int any_tls = 0;
        for (size_t gi = 0; gi < ir->globals.len; gi++) {
            if (!ir->globals.data[gi].is_tls) continue;
            any_tls = 1;
            while (out->data.len % 8) {
                char z = 0;
                buffer_append(&out->data, &z, 1);
            }
            c.tlv_off[gi] = (int)out->data.len;
            char desc[24];
            memset(desc, 0, sizeof desc);
            buffer_append(&out->data, desc, sizeof desc);
        }
        if (any_tls) {
            c.tlv_thunk = a64_new_label(&a);
            if (out->data_align < 8) out->data_align = 8;
        }
    }
    /* The environ slot is written by the entry stub and read back by the
     * getenv builtin, so it is only allocated for the TU that emits it. */
    if (want_entry) {
        while (out->bss_size % 8) out->bss_size++;
        c.environ_off = (int)out->bss_size;
        out->bss_size += 8;
        if (out->bss_align < 8) out->bss_align = 8;
    }

    /* Constructor/destructor order (matches the ELF _start walk in
     * link.c): constructors ascending by priority, source order as the
     * stable tiebreak; destructors are the ascending list walked
     * backwards.  Single-module stage: calls are direct BLs to local
     * function labels.  T14/T15 replaces this with a merged pointer
     * table once multi-module linking lands. */
    int *ctors = xmalloc(ir->functions.len * sizeof(int));
    int *dtors = xmalloc(ir->functions.len * sizeof(int));
    int nctors = 0, ndtors = 0;
    for (size_t i = 0; i < ir->functions.len; i++) {
        const IRFunction *fn = &ir->functions.data[i];
        if (fn->is_constructor) {
            int prio = fn->ctor_prio ? fn->ctor_prio : INIT_PRIO_DEFAULT;
            int pos = nctors++;
            while (pos > 0) {
                const IRFunction *prev = &ir->functions.data[ctors[pos - 1]];
                int pp = prev->ctor_prio ? prev->ctor_prio : INIT_PRIO_DEFAULT;
                if (pp <= prio) break;
                ctors[pos] = ctors[pos - 1];
                pos--;
            }
            ctors[pos] = (int)i;
        }
        if (fn->is_destructor) {
            int prio = fn->dtor_prio ? fn->dtor_prio : INIT_PRIO_DEFAULT;
            int pos = ndtors++;
            while (pos > 0) {
                const IRFunction *prev = &ir->functions.data[dtors[pos - 1]];
                int pp = prev->dtor_prio ? prev->dtor_prio : INIT_PRIO_DEFAULT;
                if (pp <= prio) break;
                dtors[pos] = dtors[pos - 1];
                pos--;
            }
            dtors[pos] = (int)i;
        }
    }

    /* LC_MAIN entry stub.  Object files (-c) are just the functions.
     * dyld hands us x0=argc, x1=argv, x2=envp; keep
     * them in callee-saved registers across the constructor calls, call
     * main, run destructors in reverse, then Darwin exit(main's value).
     * The stub owns x19..x22 outright (process entry has no caller whose
     * values matter) but saves them anyway to keep the frame ABI-clean. */
    if (want_entry) {
    a64_stp64(&a, A64_FP, A64_LR, A64_SP, -48, A64_PAIR_PRE);
    mov_sp_like(&a, A64_FP, A64_SP);   /* ADD, not ORR (x31==SP vs XZR) */
    a64_stp64(&a, A64_X19, A64_X20, A64_FP, 16, A64_PAIR_OFFSET);
    a64_stp64(&a, A64_X21, A64_X22, A64_FP, 32, A64_PAIR_OFFSET);
    a64_mov_reg(&a, A64_X19, A64_X0, 1);
    a64_mov_reg(&a, A64_X20, A64_X1, 1);
    a64_mov_reg(&a, A64_X21, A64_X2, 1);
    emit_environ_addr(&c, A64_X16);
    a64_str64(&a, A64_X21, A64_X16, 0);
    /* Hand envp to the runtime, if it provides the hook, so its getenv can
     * scan the process environment without /proc. */
    int envset_id = -1;
    if (find_function(ir, "__fakecc_set_environ", &envset_id) == 0) {
        a64_mov_reg(&a, A64_X0, A64_X21, 1);
        a64_bl(&a, c.fn_label[envset_id]);
    }
    for (int i = 0; i < nctors; i++)
        a64_bl(&a, c.fn_label[ctors[i]]);
    a64_mov_reg(&a, A64_X0, A64_X19, 1);
    a64_mov_reg(&a, A64_X1, A64_X20, 1);
    a64_mov_reg(&a, A64_X2, A64_X21, 1);
    a64_bl(&a, c.fn_label[main_id]);
    a64_mov_reg(&a, A64_X22, A64_X0, 1);          /* save main's result */
    for (int i = ndtors - 1; i >= 0; i--)
        a64_bl(&a, c.fn_label[dtors[i]]);
    a64_mov_reg(&a, A64_X0, A64_X22, 1);
    /* The C runtime's exit() flushes stdio before leaving; a bare
     * freestanding TU without it falls back to the raw exit syscall. */
    int exit_id = -1;
    if (find_function(ir, "exit", &exit_id) == 0)
        a64_bl(&a, c.fn_label[exit_id]);
    else {
        a64_movz(&a, A64_X16, 1, 0, 1);          /* exit */
        a64_svc(&a, 0x80);
    }
    /* Unreachable, but keep a valid epilogue for disassembly/tools. */
    a64_ldp64(&a, A64_X21, A64_X22, A64_FP, 32, A64_PAIR_OFFSET);
    a64_ldp64(&a, A64_X19, A64_X20, A64_FP, 16, A64_PAIR_OFFSET);
    a64_ldp64(&a, A64_FP, A64_LR, A64_SP, 48, A64_PAIR_POST);
    a64_ret(&a, A64_LR);
    }
    free(ctors);
    free(dtors);

    for (size_t i = 0; i < ir->functions.len; i++) {
        c.fn = &ir->functions.data[i];
        emit_function(&c, (int)i);
    }
    if (c.udiv_label >= 0)
        emit_udivmodti4(&a, c.udiv_label);
    /* Two ways to reach a sibling TU's function, and they are not
     * interchangeable.  Under -c the module is a real object that dyld
     * relocates through the GOT, which is what an indirect branch
     * expresses; a direct BL has no place to record its displacement and
     * self-branches.  When the linker merges in-memory modules it patches
     * text relocations itself, so a BRANCH26 to the final address is both
     * correct and cheaper.  Pick by which path this module will take. */
    for (size_t i = 0; i < c.nstub; i++) {
        a64_bind(&a, c.stub[i].lab);
        if (emit_object_mode()) {
            note_page_reloc(&c, (uint32_t)a.code.len, -1, c.stub[i].name, -1);
            c.gfix[c.ngfix - 1].got = 1;
            a64_word(&a, 0x90000000u | (uint32_t)A64_X16);
            a64_word(&a, 0xF9400000u | ((uint32_t)A64_X16 << 5)
                                 | (uint32_t)A64_X16);
            a64_br_reg(&a, A64_X16);
        } else {
            /* A single BL, not an ADRP+ADD pair, so it cannot ride the
             * gfix -> reloc conversion below (that would emit PAGE21 at
             * `at` and PAGEOFF12 at `at+4`, one instruction past the stub).
             * Mark it so the conversion emits just the branch. */
            note_page_reloc(&c, (uint32_t)a.code.len, -3 /* stub marker */,
                            c.stub[i].name, -1);
            a64_word(&a, 0x14000000u); /* b, imm26 patched by the linker */
        }
    }
    if (c.tlv_thunk >= 0) {
        a64_bind(&a, c.tlv_thunk);
        a64_ldr64(&a, A64_X0, A64_X0, 16);
        a64_ret(&a, A64_LR);
    }

    /* The code buffer lands after the Mach-O header pad; ADRP page fixups
     * must compute against the runtime file/VA offset. */
    /* Object and multi-TU modules are relocated later, so any address baked
     * into them would be relative to a layout that does not exist yet. */
    a64_set_base(&a, (emit_object_mode() || emit_multi_tu()) ? 0
                                                        : macho_text_offset());
    if (a64_resolve(&a) != 0) {
        a64_free(&a);
        free(c.fn_label);
        die_at("<arm64>", 0, 0, "arm64 label resolution failed");
    }

    out->text.data = a.code.data;
    out->text.len = a.code.len;
    out->text.cap = a.code.cap;
    out->text_align = 4;
    memset(&a.code, 0, sizeof(a.code));

    /* Patch ADRP+ADD pairs and record dyld rebases for pointer
     * initializers now that text length (and so section placement) is
     * known.  Neither an object file nor a multi-TU link can bake in the
     * executable layout: the first goes through dyld, the second has the
     * linker concatenating sections, so what looks like a section-relative
     * offset here stops being one.  Both must record relocations instead. */
    if (!emit_object_mode() && !emit_multi_tu() && (c.ngfix || c.npfix)) {
        uint64_t ro_off, data_off, bss_off;
        macho_section_offsets(out, out->text.len, &ro_off, &data_off, &bss_off);
        if (c.tlv_thunk >= 0 && a.labels[c.tlv_thunk].bound && c.tlv_off) {
            uint64_t thunk_va = (uint64_t)macho_text_offset()
                              + a.labels[c.tlv_thunk].pos;
            for (size_t gi = 0; gi < ir->globals.len; gi++) {
                if (c.tlv_off[gi] < 0) continue;
                uint64_t desc = data_off + (uint64_t)c.tlv_off[gi];
                uint64_t base = c.gsect[gi] == G_RO ? ro_off
                              : c.gsect[gi] == G_DATA ? data_off : bss_off;
                emit_module_add_rebase(out, desc, thunk_va);
                emit_module_add_rebase(out, desc + 16, base + c.goff[gi]);
            }
        }
        for (size_t i = 0; i < c.ngfix; i++) {
            int gi = c.gfix[i].gidx;
            if (c.gfix[i].tlvp) {
                if (gi < 0 || !c.tlv_off || c.tlv_off[gi] < 0) {
                    die_at("<arm64>", 0, 0,
                           "arm64 backend: thread-local variable has no descriptor");
                    continue;
                }
                uint64_t desc = data_off + (uint64_t)c.tlv_off[gi];
                uint64_t pc = (uint64_t)macho_text_offset() + c.gfix[i].at;
                int64_t pages = (int64_t)((desc & ~(uint64_t)0xFFF)
                                          - (pc & ~(uint64_t)0xFFF)) >> 12;
                uint32_t w0, w1;
                memcpy(&w0, out->text.data + c.gfix[i].at, 4);
                memcpy(&w1, out->text.data + c.gfix[i].at + 4, 4);
                w0 |= (uint32_t)((pages & 3) << 29)
                    | (uint32_t)(((pages >> 2) & 0x7FFFF) << 5);
                /* TLVP page-offset is an address, so the load becomes an add. */
                w1 = 0x91000000u | ((uint32_t)(desc & 0xFFFu) << 10)
                   | (w1 & 0x3FFu);
                memcpy(out->text.data + c.gfix[i].at, &w0, 4);
                memcpy(out->text.data + c.gfix[i].at + 4, &w1, 4);
                continue;
            }
            uint64_t tgt;
            if (gi < 0) {
                tgt = bss_off + (uint64_t)c.environ_off
                    + (uint64_t)c.gfix[i].addend;
            } else {
                uint64_t base = c.gsect[gi] == G_RO ? ro_off
                              : c.gsect[gi] == G_DATA ? data_off : bss_off;
                tgt = base + c.goff[gi] + (uint64_t)c.gfix[i].addend;
            }
            uint64_t pc = (uint64_t)macho_text_offset() + c.gfix[i].at;
            int64_t pages = (int64_t)((tgt & ~(uint64_t)0xFFF)
                                      - (pc & ~(uint64_t)0xFFF)) >> 12;
            uint32_t w0, w1;
            memcpy(&w0, out->text.data + c.gfix[i].at, 4);
            memcpy(&w1, out->text.data + c.gfix[i].at + 4, 4);
            w0 |= (uint32_t)((pages & 3) << 29)
                | (uint32_t)(((pages >> 2) & 0x7FFFF) << 5);
            w1 |= (uint32_t)(tgt & 0xFFF) << 10;
            memcpy(out->text.data + c.gfix[i].at, &w0, 4);
            memcpy(out->text.data + c.gfix[i].at + 4, &w1, 4);
        }
        for (size_t i = 0; i < c.npfix; i++) {
            int gi = c.pfix[i].gidx;
            const IRGlobal *g = &ir->globals.data[gi];
            uint64_t gbase = c.gsect[gi] == G_RO ? ro_off
                           : c.gsect[gi] == G_DATA ? data_off : bss_off;
            uint64_t slot = gbase + c.goff[gi] + (uint64_t)c.pfix[i].slot_off;
            if (c.pfix[i].sub) {
                int pgi = find_global_idx(ir, c.pfix[i].sym);
                int ngi = find_global_idx(ir, c.pfix[i].sub);
                if (pgi < 0 || ngi < 0) {
                    die_at(g->loc.file ? g->loc.file : "<arm64>", g->loc.line, 0,
                           "arm64 backend: symbol difference is not local");
                    continue;
                }
                uint64_t pb = c.gsect[pgi] == G_RO ? ro_off
                            : c.gsect[pgi] == G_DATA ? data_off : bss_off;
                uint64_t nb = c.gsect[ngi] == G_RO ? ro_off
                            : c.gsect[ngi] == G_DATA ? data_off : bss_off;
                int64_t diff = (int64_t)(pb + c.goff[pgi])
                             - (int64_t)(nb + c.goff[ngi])
                             + c.pfix[i].addend;
                size_t raw = c.goff[gi] + (size_t)c.pfix[i].slot_off;
                if (raw + 8 <= out->data.len)
                    memcpy(out->data.data + raw, &diff, 8);
                continue;
            }
            int tgi = find_global_idx(ir, c.pfix[i].sym);
            int64_t tgt;
            if (tgi >= 0) {
                uint64_t tb = c.gsect[tgi] == G_RO ? ro_off
                            : c.gsect[tgi] == G_DATA ? data_off : bss_off;
                tgt = (int64_t)(tb + c.goff[tgi]) + c.pfix[i].addend;
            } else {
                int fi = 0;
                if (find_function(ir, c.pfix[i].sym, &fi) != 0 ||
                    !a.labels[c.fn_label[fi]].bound) {
                    die_at(g->loc.file ? g->loc.file : "<arm64>",
                           g->loc.line, 0,
                           "arm64 backend: external symbol '%s' in "
                           "global initializer", c.pfix[i].sym);
                }
                tgt = (int64_t)macho_text_offset()
                    + (int64_t)a.labels[c.fn_label[fi]].pos
                    + c.pfix[i].addend;
            }
            if (tgt < 0 || gbase == 0)
                die_at(g->loc.file ? g->loc.file : "<arm64>", g->loc.line, 0,
                       "arm64 backend: bad pointer initializer");
            emit_module_add_rebase(out, slot, (uint64_t)tgt);
        }
    }
    /* Functions become linkable symbols whenever anything may consume them
     * later: an object file, or a sibling TU in a multi-TU link whose branch
     * has to be retargeted.  A lone freestanding TU needs none of this. */
    if (emit_object_mode() || emit_multi_tu()) {
        for (size_t i = 0; i < ir->functions.len; i++) {
            if (!a.labels[c.fn_label[i]].bound) continue;
            const IRFunction *fn = &ir->functions.data[i];
            size_t start = a.labels[c.fn_label[i]].pos;
            size_t end = out->text.len;
            if (i + 1 < ir->functions.len &&
                a.labels[c.fn_label[i + 1]].bound)
                end = a.labels[c.fn_label[i + 1]].pos;
            else if (c.udiv_label >= 0 && a.labels[c.udiv_label].bound)
                end = a.labels[c.udiv_label].pos;
            int fsi = emit_module_add_symbol(out, fn->name,
                                   fn->is_static ? 0 : fn->is_weak ? 2
                                       : fn->is_hidden ? 3 : 1,
                                   2 /* STT_FUNC */,
                                   (uint16_t)SECT_TEXT, start, end - start);
            if (fn->is_used && fsi >= 0)
                out->syms[fsi].macho_desc |= 0x0020;
            if (fn->is_constructor || fn->is_destructor) {
                int fsym = emit_module_find_symbol(out, fn->name);
                Buffer *arr = fn->is_constructor ? &out->init_array
                                                 : &out->fini_array;
                int **prio = fn->is_constructor ? &out->init_prio
                                                : &out->fini_prio;
                size_t slot = arr->len;
                uint64_t z = 0;
                buffer_append(arr, (const char *)&z, 8);
                emit_module_add_data_reloc(out, slot, 0 /* UNSIGNED */, fsym, 0);
                out->data_relocs[out->num_data_relocs - 1].shndx =
                    fn->is_constructor ? (uint16_t)SECT_INIT_ARRAY
                                       : (uint16_t)SECT_FINI_ARRAY;
                size_t nslot = arr->len / 8;
                *prio = realloc(*prio, nslot * sizeof(int));
                if (!*prio) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
                (*prio)[nslot - 1] = fn->is_constructor
                    ? (fn->ctor_prio ? fn->ctor_prio : INIT_PRIO_DEFAULT)
                    : (fn->dtor_prio ? fn->dtor_prio : INIT_PRIO_DEFAULT);
            }
        }
        int *gsym = NULL;
        if (ir->globals.len)
            gsym = xmalloc(ir->globals.len * sizeof(int));
        for (size_t gi = 0; gi < ir->globals.len; gi++) {
            const IRGlobal *g = &ir->globals.data[gi];
            if (gsym) gsym[gi] = -1;
            if (!g->name || !c.gsect) continue;
            uint16_t sh = c.gsect[gi] == G_COMMON ? (uint16_t)SHN_COMMON
                        : c.gsect[gi] == G_RO ? (uint16_t)SECT_RODATA
                        : c.gsect[gi] == G_DATA ? (uint16_t)SECT_DATA
                        : (uint16_t)SECT_BSS;
            /* Common: value is the alignment.  A real symbol: value is
             * the offset within its section. */
            gsym[gi] = emit_module_add_symbol(out, g->name,
                                              g->is_static ? 0 : g->is_weak ? 2
                                                  : g->is_hidden ? 3 : 1,
                                              1 /* STT_OBJECT */, sh,
                                              c.goff[gi], (size_t)g->size);
            if (g->is_used && gsym[gi] >= 0)
                out->syms[gsym[gi]].macho_desc |= 0x0020;
        }
        for (size_t i = 0; i < c.nlrel; i++) {
            if (emit_module_find_symbol(out, c.lrel[i].name) >= 0) continue;
            int lab = c.lrel[i].a64lab;
            if (lab < 0 || (size_t)lab >= a.nlabels || !a.labels[lab].bound)
                continue;
            emit_module_add_symbol(out, c.lrel[i].name, 0 /* local */,
                                   2 /* STT_FUNC */, (uint16_t)SECT_TEXT,
                                   a.labels[lab].pos, 0);
        }
        /* ADRP + ADD against the global symbol.  The linker fills the
         * page and page-offset immediates (ARM64_RELOC_PAGE21 / PAGEOFF12). */
        for (size_t i = 0; i < c.ngfix; i++) {
            int gi = c.gfix[i].gidx;
            /* gidx -2 marks the entry stub's environ slot and -1 a by-name
             * external; neither is a module global, so there is nothing to
             * relocate against.  The direct-write path bakes the environ
             * address in earlier, and it has no TU siblings then. */
            if (gi < 0 && !c.gfix[i].name) continue;
            if (gi == -3) {          /* stub call site: one BRANCH26 */
                int si = c64_undef_sym(out, ir, c.gfix[i].name);
                if (si < 0) continue;
                /* `at` is the code length just before the BL was emitted,
                 * which is already the instruction's own offset. */
                emit_module_add_reloc(out, c.gfix[i].at, 11 /* BRANCH26_TAIL */,
                                      si, 0);
                continue;
            }
            int si = (gsym && gi >= 0) ? gsym[gi] : -1;
            if (si < 0 && c.gfix[i].name)
                si = c64_undef_sym(out, ir, c.gfix[i].name);
            if (si < 0) {
                die_at("<arm64>", 0, 0,
                       "arm64 object file: global has no symbol");
                continue;
            }
            if (c.gfix[i].tlvp) {
                emit_module_add_reloc(out, c.gfix[i].at, 8 /* TLVP_LOAD_PAGE21 */,
                                      si, 0);
                emit_module_add_reloc(out, c.gfix[i].at + 4,
                                      9 /* TLVP_LOAD_PAGEOFF12 */, si, 0);
                continue;
            }
            if (c.gfix[i].got) {
                if (c.gfix[i].addend)
                    emit_module_add_reloc(out, c.gfix[i].at, 10 /* ADDEND */,
                                          0, c.gfix[i].addend);
                emit_module_add_reloc(out, c.gfix[i].at, 5 /* GOT_LOAD_PAGE21 */,
                                      si, 0);
                if (c.gfix[i].addend)
                    emit_module_add_reloc(out, c.gfix[i].at + 4, 10 /* ADDEND */,
                                          0, c.gfix[i].addend);
                emit_module_add_reloc(out, c.gfix[i].at + 4,
                                      6 /* GOT_LOAD_PAGEOFF12 */, si, 0);
                continue;
            }
            /* fold_page_addend folded "&g + n" into the fixup rather than
             * emitting an add, so the offset has to travel on both halves of
             * the pair.  Carrying it as a separate type-10 ADDEND record left
             * the linker computing the page from the bare symbol, and the
             * address came out a page low -- visible as a[i] reading a[0]. */
            emit_module_add_reloc(out, c.gfix[i].at, 3 /* PAGE21 */, si,
                                  c.gfix[i].addend);
            emit_module_add_reloc(out, c.gfix[i].at + 4, 4 /* PAGEOFF12 */,
                                  si, c.gfix[i].addend);
        }
        /* Pointer slots in __data: ARM64_RELOC_UNSIGNED, addend in the
         * eight bytes at the slot. */
        for (size_t i = 0; i < c.npfix; i++) {
            int si = c64_undef_sym(out, ir, c.pfix[i].sym);
            int gi = c.pfix[i].gidx;
            if (!c.gsect || gi < 0 || c.gsect[gi] != G_DATA) {
                die_at("<arm64>", 0, 0,
                       "arm64 object file: pointer initializer is not in __data");
                continue;
            }
            size_t off = c.goff[gi] + (size_t)c.pfix[i].slot_off;
            if (off + 8 <= out->data.len) {
                int64_t add = c.pfix[i].addend;
                memcpy(out->data.data + off, &add, 8);
            }
            if (c.pfix[i].sub) {
                int subi = c64_undef_sym(out, ir, c.pfix[i].sub);
                emit_module_add_data_reloc(out, off, 1 /* SUBTRACTOR */, subi, 0);
            }
            emit_module_add_data_reloc(out, off, 0 /* UNSIGNED */, si,
                                       c.pfix[i].addend);
        }
        for (size_t i = 0; i < c.nxcall; i++) {
            int si = c64_undef_sym(out, ir, c.xcall[i].name);
            emit_module_add_reloc(out, c.xcall[i].at, 2 /* BRANCH26 */, si, 0);
        }
        for (size_t ai = 0; ai < ir->aliases.len; ai++) {
            const IRAlias *al = &ir->aliases.data[ai];
            int tsym = emit_module_find_symbol(out, al->target);
            if (tsym < 0) continue;
            EmitSymbol target_sym = out->syms[tsym];
            uint8_t bind = al->is_static ? 0 : al->is_weak ? 2 : 1;
            int existing = emit_module_find_symbol(out, al->name);
            if (existing >= 0 && out->syms[existing].shndx == SECT_UNDEF) {
                out->syms[existing].binding = bind;
                out->syms[existing].type = target_sym.type;
                out->syms[existing].shndx = target_sym.shndx;
                out->syms[existing].value = target_sym.value;
                out->syms[existing].size = target_sym.size;
            } else if (existing < 0) {
                emit_module_add_symbol(out, al->name, bind, target_sym.type,
                                       target_sym.shndx, target_sym.value,
                                       target_sym.size);
            }
        }
        free(gsym);
    }
    free(c.gfix);
    free(c.pfix);
    free(c.xcall);
    free(c.stub);
    for (size_t i = 0; i < c.nlrel; i++) free(c.lrel[i].name);
    free(c.lrel);
    free(c.gsect);
    free(c.goff);
    free(c.tlv_off);
    a64_free(&a);
    free(c.fn_label);
}
