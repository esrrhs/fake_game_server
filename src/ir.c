#include "fakecc/ir.h"
#include "fakecc/ast.h"
#include "fakecc/common.h"
#include "fakecc/dfp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* File-scope handles used by lower_expr when it needs to allocate a new
 * anonymous rodata global for a string literal.  Set at the start of
 * ir_generate; cleared at the end.
 *
 * Every reader lives in this file and sees these definitions directly.  Do
 * not re-declare them `extern` inside a function body: fakecc currently
 * lowers a block-scope `extern` as a fresh zero-initialized local, so such
 * a redeclaration silently shadows the global in the self-hosted build. */
IRModule *g_ir_module = NULL;
int g_str_counter = 0;
int g_flt_counter = 0;
const StructRegistry *g_ir_structs = NULL;   /* set by ir_generate */
const TranslationUnit *g_ir_tu = NULL;       /* set by ir_generate */
static int g_ir_pin_locals = 0;              /* -O0: keep scalars in memory */

static int align_up(int x, int align) {
    if (align <= 0) return x;
    return (x + align - 1) & ~(align - 1);
}

static const FunctionDecl *g_ir_cur_fd = NULL; /* set by ir_generate */

/* Forward declarations for LabelMap and g_ir_label_map used by lower_expr
 * and lower_stmt (defined later in this file). */
typedef struct {
    char **names;
    int   *ids;
    size_t len;
    size_t cap;
} LabelMap;
static int labelmap_find(const LabelMap *lm, const char *name);
static LabelMap *g_ir_label_map = NULL;
static int *g_ir_label_vla_id = NULL;   /* per label: innermost live VLA id, or -1 */
static int g_ir_label_vla_id_count = 0;
static int *g_ir_vla_sp_slots = NULL;   /* per VLA: alloca holding SP after alloc */
static int g_ir_vla_count = 0;
static int g_ir_entry_sp_slot = -1;
static int g_ir_vla_seq = 0;
static int g_ir_computed_goto_vla_id = -2; /* unused for restore; labels restore SP */
static int g_vla_walk_stack[256];
static int g_vla_walk_sp = 0;
static int g_vla_walk_serial = 0;
static int g_vla_scan_addrs_only = 0;
int g_instrument_functions = 0;
int g_sanitize_address = 0;
int g_no_builtin = 0;
#define IR_MAX_NO_BUILTIN 64
static char *g_no_builtin_names[IR_MAX_NO_BUILTIN];
static int g_n_no_builtin_names;

void ir_disable_builtin(const char *name) {
    if (!name || !*name) return;
    if (g_n_no_builtin_names >= IR_MAX_NO_BUILTIN) return;
    g_no_builtin_names[g_n_no_builtin_names++] = xstrdup(name);
}

int ir_builtin_disabled(const char *name) {
    if (!name) return 0;
    if (strncmp(name, "__builtin_", 10) == 0) return 0;
    for (int i = 0; i < g_n_no_builtin_names; i++) {
        if (strcmp(g_no_builtin_names[i], name) == 0) return 1;
    }
    return g_no_builtin;
}

/* Return the live struct registry during lowering, NULL outside it.
 * type_size() uses this to refresh stale cached struct widths. */
const StructRegistry *get_ir_structs(void) {
    return g_ir_tu ? &g_ir_tu->structs : NULL;
}

const TranslationUnit *get_ir_tu(void) {
    return g_ir_tu;
}

static const char *lookup_asm_alias(const char *name) {
    if (fakecc_had_error()) return NULL;
    if (!g_ir_tu || !name) return NULL;
    for (size_t i = 0; i < g_ir_tu->functions.len; i++) {
        if (strcmp(g_ir_tu->functions.data[i].name, name) == 0 &&
            g_ir_tu->functions.data[i].alias_target)
            return g_ir_tu->functions.data[i].alias_target;
    }
    for (size_t i = 0; i < g_ir_tu->globals.len; i++) {
        const Stmt *s = &g_ir_tu->globals.data[i];
        if (s->kind == ST_DECL && s->u.decl.name && s->u.decl.alias_target &&
            strcmp(s->u.decl.name, name) == 0)
            return s->u.decl.alias_target;
    }
    return NULL;
}


/* Loop-stack: (continue_label, break_label).  Pushed on entry to every
 * loop-lowering block; popped on exit.  ST_BREAK / ST_CONTINUE consult the
 * top entry.  Depth guarded by sema — this stack never underflows. */
typedef struct {
    int cont_label;
    int break_label;
} LoopFrame;
static LoopFrame g_loops[32];
static int g_loop_depth = 0;
static void push_loop(int cont, int brk) {
    g_loops[g_loop_depth].cont_label = cont;
    g_loops[g_loop_depth].break_label = brk;
    g_loop_depth++;
}
static void pop_loop(void) { g_loop_depth--; }

/* GCC __builtin_va_arg_pack / __builtin_va_arg_pack_len: valid uses exist only
 * inside always-inlined wrappers.  While lowering such a wrapper we keep the
 * call-site anonymous arguments here so nested calls can splice them in. */
typedef struct {
    Expr **extra;
    size_t extra_len;
    int join_label;
    IRValue result_slot;
    Type ret_type;
    int is_void;
} VaArgPackFrame;
static VaArgPackFrame g_vapack_stack[8];
static int g_vapack_depth = 0;

static void emit_inst_w(IRFunction *fn, IROpcode op, IRValue dst, IRValue a, IRValue b,
                        int64_t imm, int width, int is_unsigned, SourceLoc loc);

/* ------------------------------------------------------------------ */
/* IRModule lifetime                                                   */
/* ------------------------------------------------------------------ */

void ir_module_init(IRModule *m) {
    m->functions.data = NULL;
    m->functions.len = 0;
    m->functions.cap = 0;
    m->globals.data = NULL;
    m->globals.len = 0;
    m->globals.cap = 0;
    m->aliases.data = NULL;
    m->aliases.len = 0;
    m->aliases.cap = 0;
    m->weak_refs = NULL;
    m->n_weak_refs = 0;
    m->cap_weak_refs = 0;
}

void ir_module_free(IRModule *m) {
    for (size_t i = 0; i < m->functions.len; i++) {
        free(m->functions.data[i].name);
        for (size_t j = 0; j < m->functions.data[i].insts.len; j++) {
            free(m->functions.data[i].insts.data[j].call_name);
            free(m->functions.data[i].insts.data[j].call_args);
            free(m->functions.data[i].insts.data[j].call_arg_on_stack);
            free(m->functions.data[i].insts.data[j].call_arg_nbytes);
        }
        free(m->functions.data[i].insts.data);
        free(m->functions.data[i].value_width);
        free(m->functions.data[i].value_is_unsigned);
        free(m->functions.data[i].value_is_float);
        for (size_t d = 0; d < m->functions.data[i].num_dbg_vars; d++) {
            IRDebugVar *dv = &m->functions.data[i].dbg_vars[d];
            free(dv->name);
            free(dv->struct_tag);
            for (int mi = 0; mi < dv->num_members; mi++)
                free(dv->members[mi].name);
            free(dv->members);
        }
        free(m->functions.data[i].dbg_vars);
        /* Free register allocation results if present. */
        extern void ra_result_free(void *ra);
        if (m->functions.data[i].ra)
            ra_result_free(m->functions.data[i].ra);
        if (m->functions.data[i].ra_xmm)
            ra_result_free(m->functions.data[i].ra_xmm);
    }
    free(m->functions.data);
    for (size_t i = 0; i < m->globals.len; i++) {
        free(m->globals.data[i].name);
        free(m->globals.data[i].init_bytes);
        for (int f = 0; f < m->globals.data[i].num_fixups; f++) {
            free(m->globals.data[i].fixups[f].sym);
            free(m->globals.data[i].fixups[f].sub);
        }
        free(m->globals.data[i].fixups);
    }
    free(m->globals.data);
    for (size_t i = 0; i < m->aliases.len; i++) {
        free(m->aliases.data[i].name);
        free(m->aliases.data[i].target);
    }
    free(m->aliases.data);
    for (size_t i = 0; i < m->n_weak_refs; i++) free(m->weak_refs[i]);
    free(m->weak_refs);
    m->weak_refs = NULL;
    m->n_weak_refs = 0;
    m->cap_weak_refs = 0;
    m->functions.data = NULL;
    m->functions.len = 0;
    m->functions.cap = 0;
    m->globals.data = NULL;
    m->globals.len = 0;
    m->globals.cap = 0;
    m->aliases.data = NULL;
    m->aliases.len = 0;
    m->aliases.cap = 0;
}

void ir_module_push_alias(IRModule *m, const char *name, const char *target,
                          int is_static, SourceLoc loc) {
    if (m->aliases.len >= m->aliases.cap) {
        size_t nc = m->aliases.cap ? m->aliases.cap * 2 : 4;
        m->aliases.data = realloc(m->aliases.data, nc * sizeof(IRAlias));
        if (!m->aliases.data) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        m->aliases.cap = nc;
    }
    IRAlias *al = &m->aliases.data[m->aliases.len++];
    al->name = xstrdup(name);
    al->target = xstrdup(target);
    al->is_static = is_static;
    al->is_weak = 0;
    al->loc = loc;
}

static void ir_add_weak_ref(IRModule *m, const char *name) {
    if (!name) return;
    for (size_t i = 0; i < m->n_weak_refs; i++)
        if (strcmp(m->weak_refs[i], name) == 0) return;
    if (m->n_weak_refs >= m->cap_weak_refs) {
        size_t nc = m->cap_weak_refs ? m->cap_weak_refs * 2 : 4;
        m->weak_refs = realloc(m->weak_refs, nc * sizeof(char *));
        if (!m->weak_refs) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        m->cap_weak_refs = nc;
    }
    m->weak_refs[m->n_weak_refs++] = xstrdup(name);
}

static IRGlobal *ir_module_push_global(IRModule *m, const char *name,
                                       int size, char *init_bytes,
                                       int is_readonly, int is_static,
                                       int is_tls, SourceLoc loc) {
    if (m->globals.len >= m->globals.cap) {
        size_t nc = m->globals.cap ? m->globals.cap * 2 : 4;
        m->globals.data = realloc(m->globals.data, nc * sizeof(IRGlobal));
        if (!m->globals.data) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        m->globals.cap = nc;
    }
    IRGlobal *g = &m->globals.data[m->globals.len++];
    g->name = xstrdup(name);
    g->size = size;
    g->init_bytes = init_bytes;   /* takes ownership */
    g->is_readonly = is_readonly;
    g->is_const_obj = 0;
    g->is_static = is_static;
    g->is_weak = 0;
    g->is_hidden = 0;
    g->is_used = 0;
    g->is_tls = is_tls;
    g->align = 8;
    g->loc = loc;
    g->fixups = NULL;
    g->num_fixups = 0;
    g->cap_fixups = 0;
    return g;
}

/* `const int`, `const int a[]`, and deeper arrays.  A pointer to const
 * is not a const object: the pointer itself can be reseated. */
static int type_is_const_obj(Type t) {
    while (t.kind == TY_ARRAY && t.elem_type)
        t = *t.elem_type;
    return t.is_const;
}

/* Record a pointer-slot fixup on global `g`: the slot at byte offset `offset`
 * within g->init_bytes must be patched with the link-time address of symbol
 * `sym` (an array/struct global decaying to a pointer). */
static void add_global_fixup(IRGlobal *g, int offset, const char *sym, int addend) {
    if (g->num_fixups >= g->cap_fixups) {
        size_t nc = g->cap_fixups ? g->cap_fixups * 2 : 4;
        g->fixups = realloc(g->fixups, nc * sizeof(GlobalFixup));
        if (!g->fixups) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        g->cap_fixups = (int)nc;
    }
    g->fixups[g->num_fixups].offset = offset;
    g->fixups[g->num_fixups].sym = xstrdup(sym);
    g->fixups[g->num_fixups].sub = NULL;
    g->fixups[g->num_fixups].addend = addend;
    g->num_fixups++;
}

/* ------------------------------------------------------------------ */
/* IRInstArray helpers                                                 */
/* ------------------------------------------------------------------ */

static void ir_inst_array_push(IRInstArray *a, IRInst inst) {
    if (a->len >= a->cap) {
        size_t new_cap = a->cap ? a->cap * 2 : 8;
        a->data = realloc(a->data, new_cap * sizeof(IRInst));
        if (!a->data) {
            fprintf(stderr, "fakecc: out of memory\n");
            exit(1);
        }
        a->cap = new_cap;
    }
    a->data[a->len++] = inst;
}


/* Allocate heap arg arrays for an IR_CALL.  Safe for nargs==0 (leaves NULL). */
static void ir_call_reserve_args(IRInst *inst, int nargs) {
    inst->call_nargs = nargs;
    inst->call_args = NULL;
    inst->call_arg_on_stack = NULL;
    inst->call_arg_nbytes = NULL;
    if (nargs <= 0) return;
    if (nargs > IR_CALL_MAX_ARGS) {
        fprintf(stderr, "fakecc: internal error: call with %d args (max %d)\n",
                nargs, IR_CALL_MAX_ARGS);
        exit(1);
    }
    inst->call_args = calloc((size_t)nargs, sizeof(IRValue));
    inst->call_arg_on_stack = calloc((size_t)nargs, sizeof(unsigned char));
    inst->call_arg_nbytes = calloc((size_t)nargs, sizeof(int));
    if (!inst->call_args || !inst->call_arg_on_stack || !inst->call_arg_nbytes) {
        fprintf(stderr, "fakecc: OOM\n");
        exit(1);
    }
}

/* ------------------------------------------------------------------ */
/* IRFunctionArray helpers                                             */
/* ------------------------------------------------------------------ */

static void ir_func_array_push(IRFunctionArray *a, IRFunction fn) {
    fn.ra = NULL;  /* No register allocation yet. */
    if (a->len >= a->cap) {
        size_t new_cap = a->cap ? a->cap * 2 : 4;
        a->data = realloc(a->data, new_cap * sizeof(IRFunction));
        if (!a->data) {
            fprintf(stderr, "fakecc: out of memory\n");
            exit(1);
        }
        a->cap = new_cap;
    }
    a->data[a->len++] = fn;
}

/* ------------------------------------------------------------------ */
/* AST → IR lowering                                                   */
/* ------------------------------------------------------------------ */

/* Allocate a fresh SSA value id and return it. */
static IRValue new_value(IRFunction *fn) {
    return fn->next_value_id++;
}

/* Record width/signedness for value `v`. */
static void set_value_type(IRFunction *fn, IRValue v, int width, int is_unsigned) {
    if (v < 0) return;
    if (v >= fn->value_meta_cap) {
        int old_cap = fn->value_meta_cap;
        int new_cap = old_cap ? old_cap * 2 : 16;
        while (new_cap <= v) new_cap *= 2;
        fn->value_width = realloc(fn->value_width, new_cap * sizeof(int));
        fn->value_is_unsigned = realloc(fn->value_is_unsigned, new_cap * sizeof(int));
        fn->value_is_float = realloc(fn->value_is_float, new_cap * sizeof(int));
        if (!fn->value_width || !fn->value_is_unsigned || !fn->value_is_float) {
            fprintf(stderr, "fakecc: OOM\n"); exit(1);
        }
        for (int i = old_cap; i < new_cap; i++) {
            fn->value_width[i] = 4;
            fn->value_is_unsigned[i] = 0;
            fn->value_is_float[i] = 0;
        }
        fn->value_meta_cap = new_cap;
    }
    fn->value_width[v] = width;
    fn->value_is_unsigned[v] = is_unsigned;
}

/* A real DMB.  x86 codegen ignores this call, which is what that
 * backend already did for these builtins.  crm is the 4-bit option. */
static void emit_fakecc_dmb(IRFunction *fn, int crm, SourceLoc loc) {
    IRValue immv = new_value(fn);
    emit_inst_w(fn, IR_CONST, immv, -1, -1, crm, 4, 1, loc);
    set_value_type(fn, immv, 4, 1);
    IRInst inst;
    memset(&inst, 0, sizeof(inst));
    inst.c = -1;
    inst.d = -1;
    inst.op = IR_CALL;
    inst.dst = -1;
    inst.a = -1;
    inst.b = -1;
    inst.loc = loc;
    inst.call_name = xstrdup("__fakecc_dmb");
    inst.call_callee = -1;
    ir_call_reserve_args(&inst, 1);
    inst.call_args[0] = immv;
    ir_inst_array_push(&fn->insts, inst);
}

/* Atomic exchange.  kind 0 relaxed, 1 acquire, 2 release, 3 acq_rel.
 * x86 lowers this to a plain swap, which is what that backend already
 * emitted.  arm64 emits SWP/SWPA/SWPL/SWPAL. */
static IRValue emit_fakecc_swp(IRFunction *fn, IRValue addr, IRValue val,
                               int kind, int width, int is_unsigned, SourceLoc loc) {
    IRValue dst = new_value(fn);
    IRInst inst;
    memset(&inst, 0, sizeof(inst));
    inst.c = -1;
    inst.d = -1;
    inst.op = IR_CALL;
    inst.dst = dst;
    inst.a = -1;
    inst.b = -1;
    inst.imm = kind;
    inst.loc = loc;
    inst.width = width;
    inst.is_unsigned = is_unsigned;
    inst.call_name = xstrdup("__fakecc_swp");
    inst.call_callee = -1;
    ir_call_reserve_args(&inst, 2);
    inst.call_args[0] = addr;
    inst.call_args[1] = val;
    ir_inst_array_push(&fn->insts, inst);
    set_value_type(fn, dst, width, is_unsigned);
    return dst;
}

/* Atomic add of `val` into *addr.  Returns the previous value.
 * kind matches SWP: 0 relaxed, 1 acquire, 2 release, 3 acq_rel. */
static IRValue emit_fakecc_lse(IRFunction *fn, const char *name, IRValue addr, IRValue val,
                               int kind, int width, int is_unsigned, SourceLoc loc) {
    IRValue dst = new_value(fn);
    IRInst inst;
    memset(&inst, 0, sizeof(inst));
    inst.c = -1;
    inst.d = -1;
    inst.op = IR_CALL;
    inst.dst = dst;
    inst.a = -1;
    inst.b = -1;
    inst.imm = kind;
    inst.loc = loc;
    inst.width = width;
    inst.is_unsigned = is_unsigned;
    inst.call_name = xstrdup(name);
    inst.call_callee = -1;
    ir_call_reserve_args(&inst, 2);
    inst.call_args[0] = addr;
    inst.call_args[1] = val;
    ir_inst_array_push(&fn->insts, inst);
    set_value_type(fn, dst, width, is_unsigned);
    return dst;
}

/* Float add/sub has no LSE op.  The backend retries with CAS.
 * imm: kind in bits 0..1, bit 2 subtracts, bit 3 returns the new value. */
static IRValue emit_fakecc_fadd(IRFunction *fn, IRValue addr, IRValue delta,
                               int kind, int sub, int fetch_old, int width,
                               SourceLoc loc) {
    IRValue dst = new_value(fn);
    IRInst inst;
    memset(&inst, 0, sizeof(inst));
    inst.c = -1;
    inst.d = -1;
    inst.op = IR_CALL;
    inst.dst = dst;
    inst.a = -1;
    inst.b = -1;
    inst.imm = (kind & 3) | (sub ? 4 : 0) | (fetch_old ? 0 : 8);
    inst.loc = loc;
    inst.width = width;
    inst.is_unsigned = 0;
    inst.call_name = xstrdup("__fakecc_fadd");
    inst.call_callee = -1;
    ir_call_reserve_args(&inst, 2);
    inst.call_args[0] = addr;
    inst.call_args[1] = delta;
    ir_inst_array_push(&fn->insts, inst);
    set_value_type(fn, dst, width, 0);
    return dst;
}

/* old + delta, then bring a narrow sum back to the type width.  A 32-bit
 * ADD of a byte leaves the carry in the next byte, and a later 64-bit
 * compare would treat that as a different value. */
static IRValue emit_bin_w(IRFunction *fn, IROpcode op, IRValue a, IRValue b,
                          int width, int is_unsigned, SourceLoc loc);
static IRValue atomic_add_result(IRFunction *fn, IROpcode op, IRValue old, IRValue delta,
                                 int sz, int is_u, SourceLoc loc) {
    IRValue sum = emit_bin_w(fn, op, old, delta, sz, is_u, loc);
    if (sz >= 8 || (is_u && sz == 4)) return sum;
    IRValue n = new_value(fn);
    emit_inst_w(fn, is_u ? IR_ZEXT : IR_SEXT, n, sum, -1, 0, 8, is_u, loc);
    set_value_type(fn, n, sz, is_u);
    return n;
}

/* Compare-exchange.  kind 0 relaxed, 1 acquire, 2 release, 3 acq_rel.
 * Returns 1 on success.  On failure the observed value is written through
 * exp_ptr.  x86 does the same with a plain compare. */
static IRValue emit_fakecc_cas(IRFunction *fn, IRValue addr, IRValue exp_ptr,
                               IRValue desired, int kind, int width, SourceLoc loc) {
    IRValue dst = new_value(fn);
    IRInst inst;
    memset(&inst, 0, sizeof(inst));
    inst.c = -1;
    inst.d = -1;
    inst.op = IR_CALL;
    inst.dst = dst;
    inst.a = -1;
    inst.b = -1;
    inst.imm = kind;
    inst.loc = loc;
    inst.width = width;
    inst.is_unsigned = 1;
    inst.call_name = xstrdup("__fakecc_cas");
    inst.call_callee = -1;
    ir_call_reserve_args(&inst, 3);
    inst.call_args[0] = addr;
    inst.call_args[1] = exp_ptr;
    inst.call_args[2] = desired;
    ir_inst_array_push(&fn->insts, inst);
    set_value_type(fn, dst, 4, 0);
    return dst;
}

/* Same CAS, but the expected argument is a value and the result is the
 * previous memory contents.  Nothing is written back through a pointer. */
static IRValue emit_fakecc_cas_old(IRFunction *fn, IRValue addr, IRValue expected,
                                  IRValue desired, int kind, int width,
                                  int is_unsigned, SourceLoc loc) {
    IRValue dst = new_value(fn);
    IRInst inst;
    memset(&inst, 0, sizeof(inst));
    inst.c = -1;
    inst.d = -1;
    inst.op = IR_CALL;
    inst.dst = dst;
    inst.a = -1;
    inst.b = -1;
    inst.imm = kind;
    inst.loc = loc;
    inst.width = width;
    inst.is_unsigned = is_unsigned;
    inst.call_name = xstrdup("__fakecc_cas_old");
    inst.call_callee = -1;
    ir_call_reserve_args(&inst, 3);
    inst.call_args[0] = addr;
    inst.call_args[1] = expected;
    inst.call_args[2] = desired;
    ir_inst_array_push(&fn->insts, inst);
    set_value_type(fn, dst, width, is_unsigned);
    return dst;
}

static int get_value_width(const IRFunction *fn, IRValue v) {
    if (v < 0 || v >= fn->value_meta_cap) return 4;
    return fn->value_width[v];
}

static int get_value_is_unsigned(const IRFunction *fn, IRValue v) {
    if (v < 0 || v >= fn->value_meta_cap) return 0;
    return fn->value_is_unsigned[v];
}

/* ---- Address-taken analysis: does `name` have `&name` anywhere in expr? ---- */
static int expr_takes_addr_of(const Expr *e, const char *name);
static int stmt_takes_addr_of(const Stmt *s, const char *name);

static int expr_takes_addr_of(const Expr *e, const char *name) {
    if (!e) return 0;
    switch (e->kind) {
    case EX_INT_LIT: return 0;
    case EX_FLOAT_LIT: return 0;
    case EX_STR: return 0;
    case EX_BINOP:
        return expr_takes_addr_of(e->u.bin.l, name)
            || expr_takes_addr_of(e->u.bin.r, name);
    case EX_UNARY: return expr_takes_addr_of(e->u.un.operand, name);
    case EX_VAR: return 0;
    case EX_ASSIGN:
        return expr_takes_addr_of(e->u.assign.lvalue, name)
            || expr_takes_addr_of(e->u.assign.rvalue, name);
    case EX_CALL: {
        if (expr_takes_addr_of(e->u.call.callee, name)) return 1;
        for (size_t i = 0; i < e->u.call.args.len; i++)
            if (expr_takes_addr_of(e->u.call.args.data[i], name)) return 1;
        return 0;
    }
    case EX_ADDR:
        if (e->u.addr.operand->kind == EX_VAR &&
            strcmp(e->u.addr.operand->u.var.name, name) == 0) return 1;
        return expr_takes_addr_of(e->u.addr.operand, name);
    case EX_DEREF: return expr_takes_addr_of(e->u.deref.operand, name);
    case EX_INDEX:
        return expr_takes_addr_of(e->u.idx.array, name)
            || expr_takes_addr_of(e->u.idx.index, name);
    case EX_MEMBER:
        return expr_takes_addr_of(e->u.member.obj, name);
    case EX_CAST: return expr_takes_addr_of(e->u.cast.operand, name);
    case EX_SIZEOF_TYPE: return 0;
    case EX_SIZEOF_EXPR: return expr_takes_addr_of(e->u.sizeof_e.operand, name);
    case EX_TERNARY:
        return expr_takes_addr_of(e->u.tern.cond, name)
            || expr_takes_addr_of(e->u.tern.then, name)
            || expr_takes_addr_of(e->u.tern.else_, name);
    case EX_INC_DEC:
        return expr_takes_addr_of(e->u.incdec.operand, name);
    case EX_COMPOUND_ASSIGN:
        return expr_takes_addr_of(e->u.comp.lvalue, name)
            || expr_takes_addr_of(e->u.comp.rvalue, name);
    case EX_COMMA:
        return expr_takes_addr_of(e->u.comma.lhs, name)
            || expr_takes_addr_of(e->u.comma.rhs, name);
    case EX_INIT_LIST:
        for (int i = 0; i < e->u.init_list.num_elements; i++)
            if (expr_takes_addr_of(e->u.init_list.elements[i], name)) return 1;
        return 0;
    case EX_COMPOUND_LITERAL:
        return expr_takes_addr_of(e->u.compound.init, name);
    case EX_ALIGNOF_TYPE:
        return 0; /* _Alignof(T) references no variable */
    case EX_ALIGNOF_EXPR:
        return expr_takes_addr_of(e->u.alignof_e.operand, name);
    case EX_STMT_EXPR:
        if (e->u.stmt_expr.stmts) {
            for (size_t i = 0; i < e->u.stmt_expr.stmts->len; i++)
                if (stmt_takes_addr_of(&e->u.stmt_expr.stmts->data[i], name)) return 1;
        }
        return 0;
    case EX_LABEL_ADDR:
        return 0; /* &&label references no variable */
    }
    return 0;
}

static int expr_has_label_addr(const Expr *e) {
    if (!e) return 0;
    if (e->kind == EX_LABEL_ADDR) return 1;
    if (e->kind == EX_INIT_LIST) {
        for (int i = 0; i < e->u.init_list.num_elements; i++)
            if (expr_has_label_addr(e->u.init_list.elements[i])) return 1;
    }
    if (e->kind == EX_CAST) return expr_has_label_addr(e->u.cast.operand);
    /* Label-address arithmetic (`&&l2 - &&l1`) and negation (`-&&l1`) must also
     * be detected so static locals with such initializers are dynamically
     * initialized on the stack rather than packed as compile-time constants. */
    if (e->kind == EX_BINOP)
        return expr_has_label_addr(e->u.bin.l) || expr_has_label_addr(e->u.bin.r);
    if (e->kind == EX_UNARY)
        return expr_has_label_addr(e->u.un.operand);
    return 0;
}

static int stmt_takes_addr_of(const Stmt *s, const char *name) {
    if (!s) return 0;
    switch (s->kind) {
    case ST_DECL:   return s->u.decl.init && expr_takes_addr_of(s->u.decl.init, name);
    case ST_EXPR:   return expr_takes_addr_of(s->u.expr, name);
    case ST_RETURN: return expr_takes_addr_of(s->u.value, name);
    case ST_IF:
        if (expr_takes_addr_of(s->u.if_s.cond, name)) return 1;
        if (stmt_takes_addr_of(s->u.if_s.then_s, name)) return 1;
        return s->u.if_s.else_s && stmt_takes_addr_of(s->u.if_s.else_s, name);
    case ST_WHILE:
        return expr_takes_addr_of(s->u.while_s.cond, name)
            || stmt_takes_addr_of(s->u.while_s.body, name);
    case ST_DO_WHILE:
        return expr_takes_addr_of(s->u.do_s.cond, name)
            || stmt_takes_addr_of(s->u.do_s.body, name);
    case ST_GOTO:
        return s->u.goto_s.target_expr && expr_takes_addr_of(s->u.goto_s.target_expr, name);
    case ST_LABEL:
        return stmt_takes_addr_of(s->u.label_s.stmt, name);
    case ST_SWITCH:
        if (expr_takes_addr_of(s->u.switch_s.cond, name)) return 1;
        if (s->u.switch_s.body && stmt_takes_addr_of(s->u.switch_s.body, name)) return 1;
        for (int i = 0; i < s->u.switch_s.num_cases; i++)
            for (size_t j = 0; j < s->u.switch_s.cases[i].stmts.len; j++)
                if (stmt_takes_addr_of(&s->u.switch_s.cases[i].stmts.data[j], name))
                    return 1;
        return 0;
    case ST_FOR:
        if (s->u.for_s.init && stmt_takes_addr_of(s->u.for_s.init, name)) return 1;
        if (s->u.for_s.cond && expr_takes_addr_of(s->u.for_s.cond, name)) return 1;
        if (s->u.for_s.step && expr_takes_addr_of(s->u.for_s.step, name)) return 1;
        return stmt_takes_addr_of(s->u.for_s.body, name);
    case ST_BREAK:
    case ST_CONTINUE:
        return 0;
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.len; i++)
            if (stmt_takes_addr_of(&s->u.block.data[i], name)) return 1;
        return 0;
    }
    return 0;
}

static int64_t bitfield_mask64(int bit_width) {
    if (bit_width <= 0) return 0;
    if (bit_width >= 64) return -1;
    /* unsigned: 1LL << 63 would overflow the signed type. */
    return (int64_t)((1ULL << bit_width) - 1ULL);
}

static IRValue emit_bswap_val(IRFunction *fn, IRValue v, int uw, SourceLoc loc) {
    if (uw <= 1) return v;
    if (uw == 2) {
        IRValue s8 = new_value(fn);
        emit_inst_w(fn, IR_CONST, s8, -1, -1, 8, 8, 1, loc);
        IRValue hi = new_value(fn);
        emit_inst_w(fn, IR_SHR, hi, v, s8, 0, 2, 1, loc);
        IRValue lo = new_value(fn);
        emit_inst_w(fn, IR_SHL, lo, v, s8, 0, 2, 1, loc);
        IRValue res = new_value(fn);
        emit_inst_w(fn, IR_BOR, res, hi, lo, 0, 2, 1, loc);
        return res;
    }
    if (uw == 4) {
        IRValue s24 = new_value(fn);
        emit_inst_w(fn, IR_CONST, s24, -1, -1, 24, 8, 1, loc);
        IRValue s8 = new_value(fn);
        emit_inst_w(fn, IR_CONST, s8, -1, -1, 8, 8, 1, loc);
        IRValue m_mid = new_value(fn);
        emit_inst_w(fn, IR_CONST, m_mid, -1, -1, 0x00ff0000ULL, 4, 1, loc);
        IRValue m_lo = new_value(fn);
        emit_inst_w(fn, IR_CONST, m_lo, -1, -1, 0x0000ff00ULL, 4, 1, loc);

        IRValue b0 = new_value(fn);
        emit_inst_w(fn, IR_SHR, b0, v, s24, 0, 4, 1, loc);
        IRValue b3 = new_value(fn);
        emit_inst_w(fn, IR_SHL, b3, v, s24, 0, 4, 1, loc);

        IRValue v_shr8 = new_value(fn);
        emit_inst_w(fn, IR_SHR, v_shr8, v, s8, 0, 4, 1, loc);
        IRValue b1 = new_value(fn);
        emit_inst_w(fn, IR_BAND, b1, v_shr8, m_lo, 0, 4, 1, loc);

        IRValue v_shl8 = new_value(fn);
        emit_inst_w(fn, IR_SHL, v_shl8, v, s8, 0, 4, 1, loc);
        IRValue b2 = new_value(fn);
        emit_inst_w(fn, IR_BAND, b2, v_shl8, m_mid, 0, 4, 1, loc);

        IRValue or1 = new_value(fn);
        emit_inst_w(fn, IR_BOR, or1, b0, b3, 0, 4, 1, loc);
        IRValue or2 = new_value(fn);
        emit_inst_w(fn, IR_BOR, or2, or1, b1, 0, 4, 1, loc);
        IRValue res = new_value(fn);
        emit_inst_w(fn, IR_BOR, res, or2, b2, 0, 4, 1, loc);
        return res;
    }
    if (uw == 8) {
        /* Pairwise byte/half/word swaps: 01234567 -> 10325476 -> 32107654 -> 76543210. */
        IRValue s8 = new_value(fn);
        emit_inst_w(fn, IR_CONST, s8, -1, -1, 8, 8, 1, loc);
        IRValue s16 = new_value(fn);
        emit_inst_w(fn, IR_CONST, s16, -1, -1, 16, 8, 1, loc);
        IRValue s32 = new_value(fn);
        emit_inst_w(fn, IR_CONST, s32, -1, -1, 32, 8, 1, loc);
        IRValue m_odd = new_value(fn);
        emit_inst_w(fn, IR_CONST, m_odd, -1, -1, (int64_t)0x00ff00ff00ff00ffULL, 8, 1, loc);
        IRValue m_ev = new_value(fn);
        emit_inst_w(fn, IR_CONST, m_ev, -1, -1, (int64_t)0xff00ff00ff00ff00ULL, 8, 1, loc);
        IRValue a = new_value(fn);
        emit_inst_w(fn, IR_SHR, a, v, s8, 0, 8, 1, loc);
        IRValue a2 = new_value(fn);
        emit_inst_w(fn, IR_BAND, a2, a, m_odd, 0, 8, 1, loc);
        IRValue b = new_value(fn);
        emit_inst_w(fn, IR_SHL, b, v, s8, 0, 8, 1, loc);
        IRValue b2 = new_value(fn);
        emit_inst_w(fn, IR_BAND, b2, b, m_ev, 0, 8, 1, loc);
        IRValue t = new_value(fn);
        emit_inst_w(fn, IR_BOR, t, a2, b2, 0, 8, 1, loc);
        IRValue m_o16 = new_value(fn);
        emit_inst_w(fn, IR_CONST, m_o16, -1, -1, (int64_t)0x0000ffff0000ffffULL, 8, 1, loc);
        IRValue m_e16 = new_value(fn);
        emit_inst_w(fn, IR_CONST, m_e16, -1, -1, (int64_t)0xffff0000ffff0000ULL, 8, 1, loc);
        a = new_value(fn);
        emit_inst_w(fn, IR_SHR, a, t, s16, 0, 8, 1, loc);
        a2 = new_value(fn);
        emit_inst_w(fn, IR_BAND, a2, a, m_o16, 0, 8, 1, loc);
        b = new_value(fn);
        emit_inst_w(fn, IR_SHL, b, t, s16, 0, 8, 1, loc);
        b2 = new_value(fn);
        emit_inst_w(fn, IR_BAND, b2, b, m_e16, 0, 8, 1, loc);
        t = new_value(fn);
        emit_inst_w(fn, IR_BOR, t, a2, b2, 0, 8, 1, loc);
        a = new_value(fn);
        emit_inst_w(fn, IR_SHR, a, t, s32, 0, 8, 1, loc);
        b = new_value(fn);
        emit_inst_w(fn, IR_SHL, b, t, s32, 0, 8, 1, loc);
        IRValue res = new_value(fn);
        emit_inst_w(fn, IR_BOR, res, a, b, 0, 8, 1, loc);
        return res;
    }
    return v;
}

/* Look up bitfield info for `e` (an EX_MEMBER).  If the accessed member is a
 * bitfield, set *bit_width/bit_offset to its width (bits) and position within
 * the storage unit, and return 1.  Otherwise return 0.  The storage unit is the
 * naturally-aligned integer (1/2/4 bytes) that holds the bitfield run. */
static int member_bitfield(const Expr *e, int *bit_width, int *bit_offset,
                           int *unit_width, int *is_be, int *is_unsigned) {
    if (e->kind != EX_MEMBER) return 0;
    if (e->u.member.obj->type.kind != TY_STRUCT) return 0;
    const char *tag = e->u.member.obj->type.tag;
    if (!tag) return 0;
    const StructDef *sd = struct_registry_find_c(g_ir_structs, tag);
    if (!sd) return 0;
    long long off = 0;
    const StructMember *m = struct_lookup_member(g_ir_structs, sd,
                                                 e->u.member.name, &off);
    if (!m || m->bit_width <= 0) return 0;
    *bit_width = m->bit_width;
    *bit_offset = m->bit_offset;
    *unit_width = type_size(m->type);
    if (*unit_width > 8) *unit_width = 8;
    if (*unit_width < 1) *unit_width = 1;
    /* Packed fields can start at a non-zero bit in their first byte and
     * spill past T.  Widen the primary load to 8 bytes; if the field also
     * spills past 64 bits, emit_bf_load reads extra bytes past that. */
    if (*bit_offset + *bit_width > *unit_width * 8)
        *unit_width = 8;
    if (is_be) *is_be = sd->is_big_endian;
    if (is_unsigned) *is_unsigned = m->type.is_unsigned;
    return 1;
}

static int member_struct_be(const Expr *e) {
    if (e->kind != EX_MEMBER) return 0;
    if (e->u.member.obj->type.kind != TY_STRUCT) return 0;
    const char *tag = e->u.member.obj->type.tag;
    if (!tag) return 0;
    const StructDef *sd = struct_registry_find_c(g_ir_structs, tag);
    return sd && sd->is_big_endian;
}

/* Returns 1 if the EX_MEMBER refers to a struct field whose declared type
 * is TY_ARRAY.  This is used to detect array-to-pointer decay that sema
 * performed in-place by overwriting the node's type with TY_PTR: in that
 * case lower_expr must return the member's address (decay) rather than
 * loading the pointer value stored there. */
static int member_field_is_array(const Expr *e) {
    if (e->kind != EX_MEMBER) return 0;
    if (e->u.member.obj->type.kind != TY_STRUCT) return 0;
    const char *tag = e->u.member.obj->type.tag;
    if (!tag) return 0;
    const StructDef *sd = struct_registry_find_c(g_ir_structs, tag);
    if (!sd) return 0;
    const StructMember *m = struct_lookup_member(g_ir_structs, sd,
                                                 e->u.member.name, NULL);
    return m && m->type.kind == TY_ARRAY;
}

static int stmt_has_computed_goto(const Stmt *s) {
    if (!s) return 0;
    switch (s->kind) {
    case ST_DECL: return s->u.decl.init && expr_has_label_addr(s->u.decl.init);
    case ST_EXPR: return expr_has_label_addr(s->u.expr);
    case ST_RETURN: return expr_has_label_addr(s->u.value);
    case ST_GOTO: return s->u.goto_s.target_expr != NULL;
    case ST_IF:
        return (s->u.if_s.cond && expr_has_label_addr(s->u.if_s.cond))
            || stmt_has_computed_goto(s->u.if_s.then_s)
            || (s->u.if_s.else_s && stmt_has_computed_goto(s->u.if_s.else_s));
    case ST_WHILE:
        return (s->u.while_s.cond && expr_has_label_addr(s->u.while_s.cond))
            || stmt_has_computed_goto(s->u.while_s.body);
    case ST_DO_WHILE:
        return (s->u.do_s.cond && expr_has_label_addr(s->u.do_s.cond))
            || stmt_has_computed_goto(s->u.do_s.body);
    case ST_FOR:
        return (s->u.for_s.init && stmt_has_computed_goto(s->u.for_s.init))
            || (s->u.for_s.cond && expr_has_label_addr(s->u.for_s.cond))
            || (s->u.for_s.step && expr_has_label_addr(s->u.for_s.step))
            || stmt_has_computed_goto(s->u.for_s.body);
    case ST_LABEL: return stmt_has_computed_goto(s->u.label_s.stmt);
    case ST_SWITCH:
        if (s->u.switch_s.body && stmt_has_computed_goto(s->u.switch_s.body)) return 1;
        for (int i = 0; i < s->u.switch_s.num_cases; i++)
            for (size_t j = 0; j < s->u.switch_s.cases[i].stmts.len; j++)
                if (stmt_has_computed_goto(&s->u.switch_s.cases[i].stmts.data[j])) return 1;
        return 0;
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.len; i++)
            if (stmt_has_computed_goto(&s->u.block.data[i])) return 1;
        return 0;
    default: return 0;
    }
}

static int fd_has_computed_goto(const FunctionDecl *fd) {
    if (!fd) return 0;
    for (size_t i = 0; i < fd->body.len; i++)
        if (stmt_has_computed_goto(&fd->body.data[i])) return 1;
    return 0;
}

static int stmt_has_vla(const Stmt *s) {
    if (!s) return 0;
    switch (s->kind) {
    case ST_DECL:
        return type_is_vla(s->u.decl.type);
    case ST_IF:
        return stmt_has_vla(s->u.if_s.then_s)
            || (s->u.if_s.else_s && stmt_has_vla(s->u.if_s.else_s));
    case ST_WHILE:
        return stmt_has_vla(s->u.while_s.body);
    case ST_DO_WHILE:
        return stmt_has_vla(s->u.do_s.body);
    case ST_FOR:
        return (s->u.for_s.init && stmt_has_vla(s->u.for_s.init))
            || stmt_has_vla(s->u.for_s.body);
    case ST_LABEL:
        return stmt_has_vla(s->u.label_s.stmt);
    case ST_SWITCH:
        if (s->u.switch_s.body && stmt_has_vla(s->u.switch_s.body)) return 1;
        for (int i = 0; i < s->u.switch_s.num_cases; i++)
            for (size_t j = 0; j < s->u.switch_s.cases[i].stmts.len; j++)
                if (stmt_has_vla(&s->u.switch_s.cases[i].stmts.data[j])) return 1;
        return 0;
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.len; i++)
            if (stmt_has_vla(&s->u.block.data[i])) return 1;
        return 0;
    default: return 0;
    }
}

static int fd_has_vla(const FunctionDecl *fd) {
    if (!fd) return 0;
    for (size_t i = 0; i < fd->body.len; i++)
        if (stmt_has_vla(&fd->body.data[i])) return 1;
    return 0;
}

static int expr_has_setjmp(const Expr *e) {
    if (!e) return 0;
    if (e->kind == EX_CALL && e->u.call.callee && e->u.call.callee->kind == EX_VAR &&
        strcmp(e->u.call.callee->u.var.name, "__builtin_setjmp") == 0) return 1;
    switch (e->kind) {
    case EX_BINOP: return expr_has_setjmp(e->u.bin.l) || expr_has_setjmp(e->u.bin.r);
    case EX_UNARY: return expr_has_setjmp(e->u.un.operand);
    case EX_ASSIGN: return expr_has_setjmp(e->u.assign.lvalue) || expr_has_setjmp(e->u.assign.rvalue);
    case EX_CAST: return expr_has_setjmp(e->u.cast.operand);
    case EX_COMMA: return expr_has_setjmp(e->u.comma.lhs) || expr_has_setjmp(e->u.comma.rhs);
    case EX_TERNARY:
        return expr_has_setjmp(e->u.tern.cond) || expr_has_setjmp(e->u.tern.then) || expr_has_setjmp(e->u.tern.else_);
    case EX_CALL:
        if (expr_has_setjmp(e->u.call.callee)) return 1;
        for (size_t i = 0; i < e->u.call.args.len; i++)
            if (expr_has_setjmp(e->u.call.args.data[i])) return 1;
        return 0;
    default: return 0;
    }
}

static int stmt_has_setjmp(const Stmt *s) {
    if (!s) return 0;
    switch (s->kind) {
    case ST_DECL: return s->u.decl.init && expr_has_setjmp(s->u.decl.init);
    case ST_EXPR: return expr_has_setjmp(s->u.expr);
    case ST_RETURN: return expr_has_setjmp(s->u.value);
    case ST_IF:
        return expr_has_setjmp(s->u.if_s.cond)
            || stmt_has_setjmp(s->u.if_s.then_s)
            || (s->u.if_s.else_s && stmt_has_setjmp(s->u.if_s.else_s));
    case ST_WHILE:
        return expr_has_setjmp(s->u.while_s.cond) || stmt_has_setjmp(s->u.while_s.body);
    case ST_DO_WHILE:
        return expr_has_setjmp(s->u.do_s.cond) || stmt_has_setjmp(s->u.do_s.body);
    case ST_FOR:
        return (s->u.for_s.init && stmt_has_setjmp(s->u.for_s.init))
            || expr_has_setjmp(s->u.for_s.cond)
            || expr_has_setjmp(s->u.for_s.step)
            || stmt_has_setjmp(s->u.for_s.body);
    case ST_LABEL: return stmt_has_setjmp(s->u.label_s.stmt);
    case ST_SWITCH:
        if (expr_has_setjmp(s->u.switch_s.cond)) return 1;
        if (s->u.switch_s.body && stmt_has_setjmp(s->u.switch_s.body)) return 1;
        for (int i = 0; i < s->u.switch_s.num_cases; i++)
            for (size_t j = 0; j < s->u.switch_s.cases[i].stmts.len; j++)
                if (stmt_has_setjmp(&s->u.switch_s.cases[i].stmts.data[j])) return 1;
        return 0;
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.len; i++)
            if (stmt_has_setjmp(&s->u.block.data[i])) return 1;
        return 0;
    default: return 0;
    }
}

static int fd_has_setjmp(const FunctionDecl *fd) {
    if (!fd) return 0;
    for (size_t i = 0; i < fd->body.len; i++)
        if (stmt_has_setjmp(&fd->body.data[i])) return 1;
    return 0;
}

/* Is `name` pinned in the given function body? True iff array-typed
 * (its decl is TY_ARRAY, or param would be TY_PTR — the latter is fine) or
 * `&name` appears anywhere in the function, or the function uses computed gotos. */
static int is_pinned_in_body(const FunctionDecl *fd, const char *name, Type ty) {
    if (ty.kind == TY_ARRAY) return 1;
    if (ty.kind == TY_STRUCT) return 1;
    if (ty.is_vector) return 1;
    if (fd_has_computed_goto(fd)) return 1;
    if (fd_has_setjmp(fd)) return 1;
    for (size_t i = 0; i < fd->body.len; i++)
        if (stmt_takes_addr_of(&fd->body.data[i], name)) return 1;
    return 0;
}

/* Record float-ness for value `v`.
 * 0 = integer/pointer, 1 = IEEE float/double (or x87 long double with
 * width 16), 2 = _Decimal*, 4 = 16-byte SSE vector (one XMM, not ld). */
static void set_value_float(IRFunction *fn, IRValue v, int is_float) {
    if (v < 0) return;
    if (v >= fn->value_meta_cap) {
        /* Ensure meta arrays are allocated (set_value_type would do this, but
         * a pure-float value may never pass through set_value_type). */
        set_value_type(fn, v, 4, 0);
    }
    if (fn->value_is_float)
        fn->value_is_float[v] = is_float;
}
static int get_value_is_float(const IRFunction *fn, IRValue v) {
    if (v < 0 || v >= fn->value_meta_cap || !fn->value_is_float) return 0;
    return fn->value_is_float[v];
}

static void emit_asan_check(IRFunction *fn, IRValue addr, int width, int is_write, SourceLoc loc) {
    if (!g_sanitize_address || addr < 0) return;
    if (g_ir_cur_fd && g_ir_cur_fd->name) {
        const char *fn_name = g_ir_cur_fd->name;
        if (strncmp(fn_name, "__asan_", 7) == 0 ||
            strncmp(fn_name, "asan_", 5) == 0 ||
            strcmp(fn_name, "malloc") == 0 ||
            strcmp(fn_name, "free") == 0 ||
            strcmp(fn_name, "realloc") == 0 ||
            strcmp(fn_name, "calloc") == 0 ||
            strcmp(fn_name, "heap_grow") == 0 ||
            strcmp(fn_name, "map_anon") == 0) {
            return;
        }
    }
    char hook_name[32];
    if (width == 1 || width == 2 || width == 4 || width == 8) {
        snprintf(hook_name, sizeof(hook_name), "__asan_%s%d", is_write ? "store" : "load", width);
        IRInst inst;
        memset(&inst, 0, sizeof(inst));
        inst.c = -1;
        inst.d = -1;
        inst.op = IR_CALL;
        inst.dst = -1;
        inst.a = -1;
        inst.b = -1;
        inst.imm = 0;
        inst.loc = loc;
        inst.call_name = xstrdup(hook_name);
        inst.call_callee = -1;
        ir_call_reserve_args(&inst, 1);
        inst.call_args[0] = addr;
        ir_inst_array_push(&fn->insts, inst);
    } else if (width > 0) {
        snprintf(hook_name, sizeof(hook_name), "__asan_%sN", is_write ? "store" : "load");
        IRValue sz_val = new_value(fn);
        emit_inst_w(fn, IR_CONST, sz_val, -1, -1, (int64_t)width, 8, 1, loc);
        IRInst inst;
        memset(&inst, 0, sizeof(inst));
        inst.c = -1;
        inst.d = -1;
        inst.op = IR_CALL;
        inst.dst = -1;
        inst.a = -1;
        inst.b = -1;
        inst.imm = 0;
        inst.loc = loc;
        inst.call_name = xstrdup(hook_name);
        inst.call_callee = -1;
        ir_call_reserve_args(&inst, 2);
        inst.call_args[0] = addr;
        inst.call_args[1] = sz_val;
        ir_inst_array_push(&fn->insts, inst);
    }
}

/* Push an instruction with the given fields (width + signedness). */
static void emit_inst_w(IRFunction *fn, IROpcode op, IRValue dst, IRValue a, IRValue b,
                        int64_t imm, int width, int is_unsigned, SourceLoc loc) {
    if (g_sanitize_address) {
        if (op == IR_LOAD_PTR && a >= 0) {
            emit_asan_check(fn, a, width ? width : 4, 0, loc);
        } else if (op == IR_STORE_PTR && a >= 0) {
            emit_asan_check(fn, a, width ? width : 4, 1, loc);
        }
    }
    IRInst inst;
    memset(&inst, 0, sizeof(inst));
    inst.c = -1;
    inst.d = -1;
    inst.op = op;
    inst.dst = dst;
    inst.a = a;
    inst.b = b;
    inst.imm = imm;
    inst.loc = loc;
    inst.call_name = NULL;
    inst.call_args = NULL;
    inst.call_arg_on_stack = NULL;
    inst.call_arg_nbytes = NULL;
    inst.call_nargs = 0;
    inst.call_callee = -1;
    inst.width = width;
    inst.is_unsigned = is_unsigned;
    inst.alloca_bytes = 0;
    inst.float_imm = 0;
    inst.is_float = 0;
    inst.force_stack = 0;
    inst.align16 = 0;
    inst.x87_pair = 0;
    inst.is_volatile = 0;
    ir_inst_array_push(&fn->insts, inst);
    if (dst >= 0) set_value_type(fn, dst, width ? width : 4, is_unsigned);
}

static void mark_last_volatile(IRFunction *fn, int vol) {
    if (vol && fn->insts.len)
        fn->insts.data[fn->insts.len - 1].is_volatile = 1;
}

static void emit_profile_call(IRFunction *fn, const char *hook_name, const FunctionDecl *fd, SourceLoc loc) {
    IRValue fn_addr = new_value(fn);
    emit_inst_w(fn, IR_FADDR, fn_addr, -1, -1, 0, 8, 1, loc);
    fn->insts.data[fn->insts.len - 1].call_name = xstrdup(fd->name);

    IRValue ret_addr = new_value(fn);
    emit_inst_w(fn, IR_RETURN_ADDR, ret_addr, -1, -1, 0, 8, 1, loc);

    IRInst inst;
    memset(&inst, 0, sizeof(inst));
    inst.c = -1;
    inst.d = -1;
    inst.op = IR_CALL;
    inst.dst = -1;
    inst.a = -1;
    inst.b = -1;
    inst.imm = 0;
    inst.loc = loc;
    inst.call_name = xstrdup(hook_name);
    inst.call_callee = -1;
    ir_call_reserve_args(&inst, 2);
    inst.call_args[0] = fn_addr;
    inst.call_args[1] = ret_addr;
    ir_inst_array_push(&fn->insts, inst);
}

/* Push a float-typed instruction. */
static void emit_inst_f(IRFunction *fn, IROpcode op, IRValue dst, IRValue a, IRValue b,
                        int width, SourceLoc loc) {
    emit_inst_w(fn, op, dst, a, b, 0, width, 0, loc);
    if (dst >= 0) {
        set_value_float(fn, dst, 1);
        fn->insts.data[fn->insts.len - 1].is_float = 1;
    }
}

/* Emit a float constant (bit pattern in `bits`). */
/* Materialize a long double constant: its 80 bits do not fit in an immediate,
 * so the bytes go into an anonymous rodata global and the IR_CONST names it. */
static IRValue emit_ld_const(IRFunction *fn, long double val, SourceLoc loc) {
    char name[32];
    snprintf(name, sizeof name, "__fld.%d", g_flt_counter++);
    char *init = malloc(10);
    memcpy(init, &val, 10);
    IRGlobal *lg = ir_module_push_global(g_ir_module, name, 10, init, 1, 1, 0, loc);
    lg->align = 16;
    IRValue v = new_value(fn);
    emit_inst_w(fn, IR_CONST, v, -1, -1, 0, 16, 0, loc);
    fn->insts.data[fn->insts.len - 1].is_float = 1;
    fn->insts.data[fn->insts.len - 1].call_name = xstrdup(name);
    set_value_float(fn, v, 1);
    return v;
}

static IRValue emit_float_const(IRFunction *fn, int width, int64_t bits, SourceLoc loc) {
    if (width == 16) {
        double d;
        memcpy(&d, &bits, sizeof d);
        return emit_ld_const(fn, (long double)d, loc);
    }
    IRValue v = new_value(fn);
    emit_inst_w(fn, IR_CONST, v, -1, -1, 0, width, 0, loc);
    IRInst *inst = &fn->insts.data[fn->insts.len - 1];
    inst->is_float = 1;
    inst->float_imm = bits;
    set_value_float(fn, v, 1);
    return v;
}

/* Signaling NaN.  The quiet bit stays clear.  An empty tag or a zero
 * payload uses the default signaling NaN for that width.  Darwin has no
 * nans() in libm, so the bits are built here instead of calling out. */
static unsigned long long nans_payload(const char *tag) {
    char *end = NULL;
    unsigned long long v;
    if (!tag || !tag[0]) return 0;
    v = strtoull(tag, &end, 0);
    if (end == tag) return 0;
    return v;
}

static int nans_builtin_bits(const Expr *e, int width,
                             unsigned long long *lo, unsigned long long *hi) {
    if (!e || e->kind != EX_CALL || !e->u.call.callee
        || e->u.call.callee->kind != EX_VAR)
        return 0;
    const char *name = e->u.call.callee->u.var.name;
    int is_f = strcmp(name, "__builtin_nansf") == 0;
    int is_l = strcmp(name, "__builtin_nansl") == 0;
    int is_d = strcmp(name, "__builtin_nans") == 0;
    if (!is_f && !is_l && !is_d) return 0;
    const char *tag = "";
    unsigned long long p;
    if (e->u.call.args.len > 0 && e->u.call.args.data[0]
        && e->u.call.args.data[0]->kind == EX_STR
        && e->u.call.args.data[0]->u.str.bytes)
        tag = e->u.call.args.data[0]->u.str.bytes;
    p = nans_payload(tag);
    *lo = 0;
    *hi = 0;
    if (is_f || width == 4) {
        unsigned mant = (unsigned)p & 0x3fffffu;
        if (mant == 0) mant = 0x200000u;
        *lo = 0x7f800000u | mant;
        return 1;
    }
    if (width >= 16) {
        unsigned long long mant = p & ((1ULL << 62) - 1);
        if (mant == 0) mant = 1ULL << 61;
        *lo = mant | (1ULL << 63);
        *hi = 0x7fff;
        return 1;
    }
    p &= (1ULL << 51) - 1;
    if (p == 0) p = 1ULL << 50;
    *lo = (0x7ffULL << 52) | p;
    return 1;
}

/* Forward decls — bool_normalize (below) calls coerce / emit_bin_w, which are
 * defined later in this file. */
static IRValue coerce(IRFunction *fn, IRValue v, int src_w, int src_u,
                      int dst_w, int dst_u, SourceLoc loc);
static IRValue emit_bin_w(IRFunction *fn, IROpcode op, IRValue a, IRValue b,
                          int width, int is_unsigned, SourceLoc loc);
static int get_value_is_decimal(const IRFunction *fn, IRValue v);
static IRValue emit_dfp_zero(IRFunction *fn, int width, SourceLoc loc);
static IRValue emit_dfp_cmp(IRFunction *fn, int width, BinOp op,
                            IRValue a, IRValue b, SourceLoc loc);

/* Convert a masked bitfield payload into a promoted integer: sign-extend
 * if the field is signed so `int x:3` holding -1 reads as -1, not 7. */
static IRValue bitfield_to_promoted(IRFunction *fn, IRValue bits, int uw,
                                    int bit_width, int is_unsigned, int is_bool,
                                    SourceLoc loc) {
    IRValue v = bits;
    int u = 1;
    if (!is_unsigned && !is_bool && bit_width < uw * 8) {
        int shift = uw * 8 - bit_width;
        IRValue s = new_value(fn);
        emit_inst_w(fn, IR_CONST, s, -1, -1, shift, 8, 1, loc);
        IRValue up = new_value(fn);
        emit_inst_w(fn, IR_SHL, up, v, s, 0, uw, 0, loc);
        IRValue down = new_value(fn);
        emit_inst_w(fn, IR_SHR, down, up, s, 0, uw, 0, loc);
        v = down;
        u = 0;
    }
    int dw = uw > 4 ? uw : 4;
    return coerce(fn, v, uw, u, dw, u, loc);
}

static IRValue emit_fcmp(IRFunction *fn, IRValue a, IRValue b, int w, int enc, SourceLoc loc) {
    IRValue r = new_value(fn);
    emit_inst_f(fn, IR_FCMP, r, a, b, w, loc);
    fn->insts.data[fn->insts.len - 1].is_unsigned = enc;
    set_value_float(fn, r, 0);
    set_value_type(fn, r, 4, 0);
    return r;
}

/* Normalize any scalar value to a 0/1 int (width 4) for a _Bool destination.
 * C _Bool semantics: 0 if the value compares equal to 0, else 1.  The
 * comparison is done at the source width/domain so that e.g. (_Bool)0x100
 * is 1, not 0 (an early truncate to width 1 would lose the high bits). */
static IRValue bool_normalize(IRFunction *fn, IRValue v, int w, int u,
                              int is_float, SourceLoc loc) {
    if (is_float == 2 || get_value_is_decimal(fn, v)) {
        int dw = w == 4 ? 4 : (w == 16 ? 16 : 8);
        IRValue z = emit_dfp_zero(fn, dw, loc);
        return emit_dfp_cmp(fn, dw, BOP_NE, v, z, loc);
    }
    if (is_float) {
        IRValue zero = emit_float_const(fn, w, 0, loc);
        return emit_fcmp(fn, v, zero, w, 5 /* NE */, loc);
    }
    /* Compare at the source width so (_Bool)(1LL<<32) and a pointer whose
     * low 32 bits are zero still become 1.  Narrow values are promoted. */
    int cw = (w < 4) ? 4 : w;
    if (w > 0 && w != cw)
        v = coerce(fn, v, w, u, cw, u, loc);
    IRValue zero = new_value(fn);
    emit_inst_w(fn, IR_CONST, zero, -1, -1, 0, cw, 0, loc);
    IRValue r = emit_bin_w(fn, IR_NE, v, zero, cw, 0, loc);
    if (cw != 4)
        r = coerce(fn, r, cw, 0, 4, 0, loc);
    return r;
}

/* CBR is an integer test.  A floating condition is compared against +0.0 so
 * -0.0 is false (C scalar zero).  Callers keep the original value for GNU `?:`. */
static IRValue cbr_from_scalar(IRFunction *fn, IRValue v, SourceLoc loc) {
    if (get_value_is_decimal(fn, v))
        return bool_normalize(fn, v, get_value_width(fn, v), 0, 2, loc);
    if (get_value_is_float(fn, v))
        return bool_normalize(fn, v, get_value_width(fn, v), 0, 1, loc);
    return v;
}

/* Push an instruction (width/signedness default 4/signed). */
static void emit_inst(IRFunction *fn, IROpcode op, IRValue dst, IRValue a, IRValue b,
                      int64_t imm, SourceLoc loc) {
    emit_inst_w(fn, op, dst, a, b, imm, 4, 0, loc);
}

/* Push a binary/2-source instruction with the given opcode. */
static IRValue emit_bin_w(IRFunction *fn, IROpcode op, IRValue a, IRValue b,
                          int width, int is_unsigned, SourceLoc loc) {
    IRValue v = new_value(fn);
    emit_inst_w(fn, op, v, a, b, 0, width, is_unsigned, loc);
    return v;
}

/* Emit an IR_ALLOCA with a total-byte size (for pinned allocas: arrays or
 * address-taken variables). */
static IRValue emit_alloca(IRFunction *fn, int total_bytes, int width,
                           int is_unsigned, SourceLoc loc) {
    IRValue v = new_value(fn);
    emit_inst_w(fn, IR_ALLOCA, v, -1, -1, 0, width, is_unsigned, loc);
    fn->insts.data[fn->insts.len - 1].alloca_bytes = total_bytes;
    return v;
}

/* Pin a stack slot with SysV natural alignment.  Alignments ≥ 16 are
 * recorded in IR_ALLOCA.imm so codegen can place/realign the slot
 * (incoming rbp is only 16-aligned). */
static IRValue emit_alloca_aligned(IRFunction *fn, int total, int al, SourceLoc loc) {
    if (total < 1) total = 1;
    if (al < 1) al = 1;
    int slack = (al > 16) ? al : 0;
    IRValue v = emit_alloca(fn, total + slack, 8, 1, loc);
    if (al >= 16)
        fn->insts.data[fn->insts.len - 1].imm = al;
    return v;
}

static IRValue emit_alloca_ty(IRFunction *fn, Type ty, SourceLoc loc) {
    return emit_alloca_aligned(fn, type_size(ty), (int)type_align(ty), loc);
}

static int sysv_sse_vec_bytes(int nreg, int size, const SysVRegClass *cls) {
    if (nreg == 1 && cls && cls[0] == SYSV_CLS_SSE
        && (size == 8 || size == 16 || size == 32 || size == 64))
        return size;
    return 0;
}

static unsigned char sysv_stack_arg_flags(int on_stack, Type ty) {
    unsigned char f = on_stack ? CALL_ARG_STACK : 0;
    int al = type_stack_align(ty);
    if (al >= 16) f |= CALL_ARG_ALIGN16;
    if (al >= 32) f |= CALL_ARG_ALIGN32;
    if (al >= 64) f |= CALL_ARG_ALIGN64;
    return f;
}

/* Emit `dst = ptr + delta` where delta is a compile-time byte constant.
 * Used to form addresses of struct members / copy chunks. */
static IRValue emit_add_const(IRFunction *fn, IRValue ptr, int delta,
                              SourceLoc loc) {
    if (delta == 0) return ptr;
    IRValue c = new_value(fn);
    emit_inst_w(fn, IR_CONST, c, -1, -1, delta, 8, 1, loc);
    return emit_bin_w(fn, IR_ADD, ptr, c, 8, 1, loc);
}

/* Packed bitfields may start at a non-zero bit and spill past 64 bits
 * (e.g. `unsigned char a:1; unsigned long long b:64` in a packed struct).
 * The storage is split into an 8-byte low load and a 1..8-byte high load. */
typedef struct {
    IRValue lo;
    IRValue hi;  /* -1 if the field fits in 8 bytes */
    int uw;      /* bytes in lo */
    int hi_w;    /* bytes in hi; 0 if none */
} BFUnit;

static int bitfield_span_bytes(int bit_offset, int bit_width) {
    int bits = bit_offset + bit_width;
    int n = (bits + 7) / 8;
    if (n < 1) n = 1;
    if (n > 16) n = 16;
    return n;
}

static IRValue emit_iconst_w(IRFunction *fn, int64_t val, int w, int u, SourceLoc loc) {
    IRValue c = new_value(fn);
    emit_inst_w(fn, IR_CONST, c, -1, -1, val, w, u, loc);
    return c;
}

static BFUnit emit_bf_load(IRFunction *fn, IRValue addr, int bit_offset, int bit_width,
                          int unit_width, int is_be, SourceLoc loc) {
    BFUnit u;
    int span = bitfield_span_bytes(bit_offset, bit_width);
    u.hi = -1;
    u.hi_w = 0;
    if (span <= 8) {
        u.uw = unit_width ? unit_width : 4;
        if (u.uw < 1) u.uw = 1;
        if (u.uw > 8) u.uw = 8;
        u.lo = new_value(fn);
        emit_inst_w(fn, IR_LOAD_PTR, u.lo, addr, -1, 0, u.uw, 1, loc);
        if (is_be) u.lo = emit_bswap_val(fn, u.lo, u.uw, loc);
        return u;
    }
    u.uw = 8;
    u.hi_w = span - 8;
    u.lo = new_value(fn);
    emit_inst_w(fn, IR_LOAD_PTR, u.lo, addr, -1, 0, 8, 1, loc);
    {
        IRValue hi_addr = emit_add_const(fn, addr, 8, loc);
        u.hi = new_value(fn);
        emit_inst_w(fn, IR_LOAD_PTR, u.hi, hi_addr, -1, 0, u.hi_w, 1, loc);
    }
    return u;
}

static void emit_bf_store(IRFunction *fn, IRValue addr, BFUnit u, int is_be, SourceLoc loc) {
    IRValue lo = u.lo;
    if (u.hi_w == 0 && is_be)
        lo = emit_bswap_val(fn, lo, u.uw, loc);
    emit_inst_w(fn, IR_STORE_PTR, -1, addr, lo, 0, u.uw, 1, loc);
    if (u.hi_w > 0) {
        IRValue hi_addr = emit_add_const(fn, addr, 8, loc);
        emit_inst_w(fn, IR_STORE_PTR, -1, hi_addr, u.hi, 0, u.hi_w, 1, loc);
    }
}

static int bf_work_width(const BFUnit *u) {
    return u->hi_w ? 8 : u->uw;
}

static IRValue emit_bf_extract(IRFunction *fn, BFUnit u, int bit_offset, int bit_width,
                              SourceLoc loc) {
    int ww = bf_work_width(&u);
    int64_t mask = bitfield_mask64(bit_width);
    IRValue v;
    if (u.hi_w == 0) {
        v = u.lo;
        if (bit_offset > 0) {
            IRValue s = emit_iconst_w(fn, bit_offset, 8, 1, loc);
            IRValue shifted = new_value(fn);
            emit_inst_w(fn, IR_SHR, shifted, v, s, 0, ww, 1, loc);
            v = shifted;
        }
    } else {
        IRValue hi8 = (u.hi_w == 8) ? u.hi : coerce(fn, u.hi, u.hi_w, 1, 8, 1, loc);
        IRValue lo_part = u.lo;
        if (bit_offset > 0) {
            IRValue s = emit_iconst_w(fn, bit_offset, 8, 1, loc);
            lo_part = new_value(fn);
            emit_inst_w(fn, IR_SHR, lo_part, u.lo, s, 0, 8, 1, loc);
        }
        IRValue hs = emit_iconst_w(fn, 64 - bit_offset, 8, 1, loc);
        IRValue hi_part = new_value(fn);
        emit_inst_w(fn, IR_SHL, hi_part, hi8, hs, 0, 8, 1, loc);
        v = new_value(fn);
        emit_inst_w(fn, IR_BOR, v, lo_part, hi_part, 0, 8, 1, loc);
    }
    IRValue m = emit_iconst_w(fn, mask, ww, 1, loc);
    IRValue masked = new_value(fn);
    emit_inst_w(fn, IR_BAND, masked, v, m, 0, ww, 1, loc);
    return masked;
}

static BFUnit emit_bf_insert(IRFunction *fn, BFUnit u, IRValue val, int bit_offset,
                            int bit_width, SourceLoc loc) {
    int ww = bf_work_width(&u);
    int64_t mask = bitfield_mask64(bit_width);
    IRValue vm = coerce(fn, val, get_value_width(fn, val), 1, ww, 1, loc);
    IRValue m = emit_iconst_w(fn, mask, ww, 1, loc);
    IRValue masked = new_value(fn);
    emit_inst_w(fn, IR_BAND, masked, vm, m, 0, ww, 1, loc);
    if (u.hi_w == 0) {
        IRValue so = emit_iconst_w(fn, bit_offset, 8, 1, loc);
        IRValue ms = new_value(fn);
        emit_inst_w(fn, IR_SHL, ms, m, so, 0, ww, 1, loc);
        IRValue nm = new_value(fn);
        emit_inst_w(fn, IR_BNOT, nm, ms, -1, 0, ww, 1, loc);
        IRValue cleared = new_value(fn);
        emit_inst_w(fn, IR_BAND, cleared, u.lo, nm, 0, ww, 1, loc);
        IRValue vs = new_value(fn);
        emit_inst_w(fn, IR_SHL, vs, masked, so, 0, ww, 1, loc);
        u.lo = new_value(fn);
        emit_inst_w(fn, IR_BOR, u.lo, cleared, vs, 0, ww, 1, loc);
        return u;
    }
    /* Spanning: low `bit_offset` bits of lo stay; the rest of lo plus
     * `hi_bits` of hi take the field. */
    uint64_t lo_keep = (bit_offset <= 0) ? 0ULL
                    : (bit_offset >= 64 ? ~(uint64_t)0 : ((1ULL << bit_offset) - 1));
    IRValue lo_kept = new_value(fn);
    emit_inst_w(fn, IR_BAND, lo_kept, u.lo, emit_iconst_w(fn, (int64_t)lo_keep, 8, 1, loc),
                0, 8, 1, loc);
    IRValue lo_ins = new_value(fn);
    emit_inst_w(fn, IR_SHL, lo_ins, masked, emit_iconst_w(fn, bit_offset, 8, 1, loc),
                0, 8, 1, loc);
    u.lo = new_value(fn);
    emit_inst_w(fn, IR_BOR, u.lo, lo_kept, lo_ins, 0, 8, 1, loc);
    int lo_bits = 64 - bit_offset;
    int hi_bits = bit_offset + bit_width - 64;
    int64_t hi_mask = bitfield_mask64(hi_bits);
    IRValue hi8 = (u.hi_w == 8) ? u.hi : coerce(fn, u.hi, u.hi_w, 1, 8, 1, loc);
    IRValue hm = emit_iconst_w(fn, hi_mask, 8, 1, loc);
    IRValue nhm = new_value(fn);
    emit_inst_w(fn, IR_BNOT, nhm, hm, -1, 0, 8, 1, loc);
    IRValue hi_kept = new_value(fn);
    emit_inst_w(fn, IR_BAND, hi_kept, hi8, nhm, 0, 8, 1, loc);
    IRValue hi_ins = new_value(fn);
    emit_inst_w(fn, IR_SHR, hi_ins, masked, emit_iconst_w(fn, lo_bits, 8, 1, loc),
                0, 8, 1, loc);
    IRValue hi_bitsv = new_value(fn);
    emit_inst_w(fn, IR_BAND, hi_bitsv, hi_ins, hm, 0, 8, 1, loc);
    IRValue hi_new = new_value(fn);
    emit_inst_w(fn, IR_BOR, hi_new, hi_kept, hi_bitsv, 0, 8, 1, loc);
    u.hi = (u.hi_w == 8) ? hi_new : coerce(fn, hi_new, 8, 1, u.hi_w, 1, loc);
    return u;
}

/* Host-side insert of a bitfield into a packed initializer blob. */
static void pack_bitfield_host(char *bytes, int nbytes, int byte_off,
                               int bit_offset, int bit_width, uint64_t val_masked,
                               int is_be, int uw) {
    int span = bitfield_span_bytes(bit_offset, bit_width);
    if (byte_off < 0 || byte_off >= nbytes) return;
    if (byte_off + span > nbytes) span = nbytes - byte_off;
    if (span <= 8) {
        int n = uw;
        if (n < 1) n = 1;
        if (n > span) n = span;
        if (n > 8) n = 8;
        uint64_t mask = (uint64_t)bitfield_mask64(bit_width);
        if (is_be) {
            uint64_t unit = 0;
            int b;
            for (b = 0; b < n; b++)
                unit = (unit << 8) | (unsigned char)bytes[byte_off + b];
            unit &= ~(mask << bit_offset);
            unit |= (val_masked << bit_offset);
            for (b = 0; b < n; b++)
                bytes[byte_off + b] = (char)((unit >> ((n - 1 - b) * 8)) & 0xff);
        } else {
            uint64_t unit = 0;
            memcpy(&unit, bytes + byte_off, (size_t)n);
            unit &= ~(mask << bit_offset);
            unit |= (val_masked << bit_offset);
            memcpy(bytes + byte_off, &unit, (size_t)n);
        }
        return;
    }
    uint64_t lo = 0, hi = 0;
    memcpy(&lo, bytes + byte_off, 8);
    memcpy(&hi, bytes + byte_off + 8, (size_t)(span - 8));
    uint64_t lo_keep = (bit_offset <= 0) ? 0ULL
                     : (bit_offset >= 64 ? ~(uint64_t)0 : ((1ULL << bit_offset) - 1));
    lo = (lo & lo_keep) | (val_masked << bit_offset);
    {
        int lo_bits = 64 - bit_offset;
        int hi_bits = bit_offset + bit_width - 64;
        uint64_t hi_mask = (uint64_t)bitfield_mask64(hi_bits);
        hi = (hi & ~hi_mask) | ((val_masked >> lo_bits) & hi_mask);
    }
    memcpy(bytes + byte_off, &lo, 8);
    memcpy(bytes + byte_off + 8, &hi, (size_t)(span - 8));
}

/* Copy `size` bytes from *src into *dst, one natural-width chunk at a time
 * (8/4/2/1 bytes).  Implements struct value copy: a struct is represented as
 * a pointer to its bytes, so every by-value boundary (param bind, return,
 * assignment) must move the bytes, not the pointer.  Chosen width is always
 * a power of two no larger than the remaining bytes and no larger than 8, so
 * it matches a single LOAD_PTR/STORE_PTR on the codegen side. */
/* Byte count above which emit_struct_copy delegates to memcpy instead of
 * unrolling per-chunk load/store.  Unrolling a 64KB struct generates ~8k
 * IR instructions and times out the optimizer; a single memcpy call is
 * O(1) IR.  The cutoff balances code size against memcpy call overhead. */
#define STRUCT_COPY_MEMCPY_THRESHOLD 64

static void emit_struct_copy(IRFunction *fn, IRValue dst, IRValue src,
                              int size, SourceLoc loc) {
    if (size > STRUCT_COPY_MEMCPY_THRESHOLD) {
        /* Large copy: call memcpy(dst, src, size).  The caller is responsible
         * for declaring memcpy (it is not a builtin).  Emit a direct named
         * call so codegen patches it like any other external call. */
        IRValue sz_val = new_value(fn);
        emit_inst_w(fn, IR_CONST, sz_val, -1, -1, (int64_t)size, 8, 1, loc);
        IRInst inst;
        memset(&inst, 0, sizeof(inst));
        inst.c = -1;
        inst.d = -1;
        inst.op = IR_CALL;
        inst.dst = -1;
        inst.a = -1;
        inst.b = -1;
        inst.imm = 0;
        inst.loc = loc;
        inst.call_name = xstrdup("memcpy");
        inst.call_callee = -1;
        ir_call_reserve_args(&inst, 3);
        inst.call_args[0] = dst;
        inst.call_args[1] = src;
        inst.call_args[2] = sz_val;
        ir_inst_array_push(&fn->insts, inst);
        return;
    }
    int off = 0;
    while (size > 0) {
        int w = (size >= 8) ? 8 : (size >= 4) ? 4 : (size >= 2) ? 2 : 1;
        IRValue saddr = emit_add_const(fn, src, off, loc);
        IRValue daddr = emit_add_const(fn, dst, off, loc);
        IRValue v = new_value(fn);
        emit_inst_w(fn, IR_LOAD_PTR, v, saddr, -1, 0, w, 1, loc);
        emit_inst_w(fn, IR_STORE_PTR, -1, daddr, v, 0, w, 1, loc);
        off += w;
        size -= w;
    }
}

/* Load the SysV register eightbytes of an aggregate at `addr` into SSA
 * values.  `n`/`cls` come from sysv_classify_agg (or MEMORY expansion).
 * `cls` may be NULL (all INTEGER).  `out` must have room for `n` values. */
static void load_agg_regs(IRFunction *fn, IRValue addr, int size, int n,
                          const SysVRegClass *cls, IRValue *out,
                          SourceLoc loc) {
    /* 8/16/32-byte SSE vector: one XMM/YMM holds the whole object.
     * Float-kind 4 distinguishes this from x87 long double (kind 1, width 16). */
    {
        int vw = sysv_sse_vec_bytes(n, size, cls);
        if (vw) {
            IRValue v = new_value(fn);
            emit_inst_w(fn, IR_LOAD_PTR, v, addr, -1, 0, vw, 1, loc);
            fn->insts.data[fn->insts.len - 1].is_float = 1;
            set_value_type(fn, v, vw, 0);
            set_value_float(fn, v, 4);
            out[0] = v;
            return;
        }
    }
    for (int i = 0; i < n; i++) {
        int off = i * 8;
        int remain = size - off;
        if (remain > 8) remain = 8;
        if (remain <= 0) remain = 1;
        int remain_n = remain;
        int off_b = 0;
        IRValue v = -1;
        int is_sse = (cls && cls[i] == SYSV_CLS_SSE);
        while (remain_n > 0) {
            int chunk = remain_n >= 8 ? 8 : remain_n >= 4 ? 4 : remain_n >= 2 ? 2 : 1;
            IRValue a = emit_add_const(fn, addr, off + off_b, loc);
            IRValue piece = new_value(fn);
            emit_inst_w(fn, IR_LOAD_PTR, piece, a, -1, 0, chunk, 1, loc);
            if (!is_sse && chunk < 8) {
                IRValue wide = new_value(fn);
                emit_inst_w(fn, IR_ZEXT, wide, piece, -1, chunk, 8, 1, loc);
                piece = wide;
            }
            if (off_b > 0 && !is_sse) {
                IRValue sh = new_value(fn);
                emit_inst_w(fn, IR_CONST, sh, -1, -1, (long long)off_b * 8, 8, 1, loc);
                IRValue shifted = new_value(fn);
                emit_inst_w(fn, IR_SHL, shifted, piece, sh, 0, 8, 1, loc);
                IRValue merged = new_value(fn);
                emit_inst_w(fn, IR_BOR, merged, v, shifted, 0, 8, 1, loc);
                v = merged;
            } else {
                v = piece;
            }
            off_b += chunk;
            remain_n -= chunk;
        }
        if (is_sse) {
            set_value_type(fn, v, remain <= 4 ? 4 : 8, 0);
            set_value_float(fn, v, 1);
        } else {
            set_value_type(fn, v, 8, 1);
        }
        out[i] = v;
    }
}

/* Store register eightbytes into an aggregate at `addr`. */
static void store_agg_regs(IRFunction *fn, IRValue addr, int size, int n,
                           const SysVRegClass *cls, const IRValue *vals,
                           SourceLoc loc) {
    {
        int vw = sysv_sse_vec_bytes(n, size, cls);
        if (!vw && n == 1 && get_value_is_float(fn, vals[0])) {
            int gw = get_value_width(fn, vals[0]);
            if (gw == 8 || gw == 16 || gw == 32 || gw == 64) vw = gw;
        }
        if (vw) {
            emit_inst_w(fn, IR_STORE_PTR, -1, addr, vals[0], 0, vw, 1, loc);
            fn->insts.data[fn->insts.len - 1].is_float = 1;
            return;
        }
    }
    (void)cls;
    for (int i = 0; i < n; i++) {
        int off = i * 8;
        int remain = size - off;
        if (remain <= 0) break;
        if (remain > 8) remain = 8;
        int remain_n = remain;
        int off_b = 0;
        IRValue srcv = vals[i];
        while (remain_n > 0) {
            int w = remain_n >= 8 ? 8 : remain_n >= 4 ? 4 : remain_n >= 2 ? 2 : 1;
            IRValue a = emit_add_const(fn, addr, off + off_b, loc);
            IRValue v = srcv;
            if (off_b > 0 && !get_value_is_float(fn, srcv)) {
                IRValue sh = new_value(fn);
                emit_inst_w(fn, IR_CONST, sh, -1, -1, (long long)off_b * 8, 8, 1, loc);
                IRValue shifted = new_value(fn);
                emit_inst_w(fn, IR_SHR, shifted, srcv, sh, 0, 8, 1, loc);
                v = shifted;
            }
            int vw = get_value_width(fn, v);
            if (vw != w && !get_value_is_float(fn, v)) {
                if (vw > w) {
                    IRValue t = new_value(fn);
                    emit_inst_w(fn, IR_TRUNC, t, v, -1, vw, w, 1, loc);
                    v = t;
                } else {
                    IRValue t = new_value(fn);
                    emit_inst_w(fn, IR_ZEXT, t, v, -1, vw, w, 1, loc);
                    v = t;
                }
            }
            emit_inst_w(fn, IR_STORE_PTR, -1, a, v, 0, w, 1, loc);
            off_b += w;
            remain_n -= w;
        }
    }
}

/* Darwin aggregate classes differ from SysV in two ways that the rest of
 * this file expresses by rewriting the SysV eightbyte classes:
 *   - a non-HFA SSE eightbyte (mixed int/float) travels in a GP register
 *     holding the raw bits, not a V register;
 *   - an HFA keeps its SSE classes so the backend uses v0..v7, but the
 *     SSA values stay integer bit patterns (see CALL_ARG_HFA);
 *   - a 16-byte vector stays one SSE class: Darwin passes it in a single
 *     Q register (q0..q7), not as an 8-byte GP.
 * An HFA that does not fit in two eightbytes (3–4 doubles) is not
 * representable in the current IR and is rejected. */
/* Sema marks a builtin that has no recorded prototype (memset, memcpy,
 * …) as variadic with zero parameters so the arity check accepts the
 * call.  That is usually not a Darwin varargs function: the arguments use
 * the normal registers.  A real `f(...)` has a user name and does use the
 * stack convention.
 *
 * The exception is the stdio formatted-output family (__builtin_printf,
 * __builtin_fprintf, __builtin_sprintf_chk, the *_unlocked variants, …):
 * those genuinely are variadic and their anonymous arguments ride the
 * Darwin vararg stack slots.  Match the printf/scanf family by name and
 * keep every other builtin on registers — a fixed-arity builtin such as
 * __builtin_exit(0) otherwise sends its one argument to the stack while
 * the callee reads w0.  (The fortify __*_chk folds mark their own stack
 * arguments explicitly and are unaffected.) */
static int prototype_less_builtin(const char *name) {
    if (!name) return 0;
    const char *n = name;
    if (strncmp(n, "__builtin_", 10) == 0) n += 10;
    /* Real variadic formatted-I/O: honour the Darwin anonymous-stack ABI. */
    if (strstr(n, "printf") != NULL || strstr(n, "scanf") != NULL)
        return 0;
    if (strncmp(name, "__builtin_", 10) == 0)
        return 1;
    return strcmp(name, "memset") == 0 || strcmp(name, "memcpy") == 0
        || strcmp(name, "memmove") == 0 || strcmp(name, "mempcpy") == 0
        || strcmp(name, "alloca") == 0;
}

/* Darwin passes every anonymous argument on the stack, in 8-byte
 * slots.  Named parameters keep the normal register convention. */
static int darwin_unnamed_va(int variadic, int index, int nparams, int fake) {
    if (fake) return 0;
    return !abi_sret_uses_gp() && variadic && index >= nparams;
}

/* A variadic float constant is rewritten to the integer bits clang stores
 * in the 8-byte stack slot.  A computed value stays in the V-reg file and
 * is stored with STR S/D at the call. */
static IRValue va_float_bits(IRFunction *fn, IRValue v, SourceLoc loc) {
    if (!get_value_is_float(fn, v)) return v;
    IRValue cur = v;
    for (int hop = 0; hop < 8; hop++) {
        const IRInst *def = NULL;
        for (size_t i = 0; i < fn->insts.len; i++) {
            const IRInst *d = &fn->insts.data[i];
            if (d->dst == cur) def = d;
        }
        if (def && def->op == IR_CONST) {
            IRValue bits = new_value(fn);
            emit_inst_w(fn, IR_CONST, bits, -1, -1, def->float_imm, 8, 1, loc);
            return bits;
        }
        if (def && (def->op == IR_COPY || def->op == IR_TRUNC) && def->a >= 0) {
            cur = def->a;
            if (!get_value_is_float(fn, cur)) return cur;
            continue;
        }
        break;
    }
    return v;
}

/* `cls_cap` is how many SysVRegClass slots the caller owns.  AAPCS64 gives
 * an HFA one V register per element, so a three- or four-element HFA (24 /
 * 32 bytes) is register-passed and register-returned in v0-v3: both the call
 * and the return paths hand in capacity 4.  A wider HFA (>4 elements) is
 * MEMORY. */
static void abi_adjust_cls(Type t, SysVRegClass *cls, int *nreg, int cls_cap,
                           SourceLoc loc) {
    if (abi_sret_uses_gp()) return;
    if (abi_is_hfa(t)) {
        if (*nreg == 0) {
            int nel = abi_hfa_elem_count(t);
            if (nel > 2 && nel <= cls_cap) {
                for (int i = 0; i < nel; i++) cls[i] = SYSV_CLS_SSE;
                *nreg = nel;
                return;
            }
            die_at(loc.file ? loc.file : "<arm64>", loc.line, loc.col,
                   "arm64 HFA wider than two eightbytes is not supported yet");
        }
        return;
    }
    /* vector_size(16) is one Q register on Darwin.  Wider vectors have no
     * YMM/ZMM, so they travel as a pointer (argument in a GP, result in x8). */
    if (t.is_vector && type_size(t) == 16) return;
    if (t.is_vector && type_size(t) > 16) {
        *nreg = 0;
        return;
    }
    /* A 16-byte aggregate SysV folded to a single eightbyte, either because
     * it holds a vector (SSE + SSEUP collapse to one XMM) or because the
     * second eightbyte is padding.  arm64 has no XMM pair to merge into: a
     * composite that is not an HFA occupies one eightbyte per double-word,
     * so `struct { V }` has to be two of them.  Leaving it at one passed
     * only x0, and the upper half came back zero --
     * `struct S { V x; }; id(s).x[1]` read 0 instead of 4. */
    /* SysV folds a wide SSE aggregate -- one YMM or ZMM -- into a single
     * eightbyte (SSE + SSEUP*n).  arm64 has neither, so an aggregate past 16
     * bytes is MEMORY.  Left as one INTEGER eightbyte it silently kept only
     * the first eight bytes: `struct Box { V x; }` with a 32-byte V came back
     * from a call with three quarters of the object missing.  A bare vector
     * wider than 16 is handled just above; this is the same rule applied to a
     * vector wrapped in a struct, which is not itself is_vector.
     * abi_indirect_agg() already says these travel by pointer. */
    if (t.kind == TY_STRUCT && *nreg == 1 && type_size(t) > 16) {
        *nreg = 0;
        return;
    }
    if (t.kind == TY_STRUCT && type_size(t) == 16 && *nreg == 1) {
        cls[0] = SYSV_CLS_INTEGER;
        cls[1] = SYSV_CLS_INTEGER;
        *nreg = 2;
        return;
    }
    for (int i = 0; i < *nreg && i < 2; i++)
        if (cls[i] == SYSV_CLS_SSE) cls[i] = SYSV_CLS_INTEGER;
}

static int  new_label(IRFunction *fn);
static void emit_label(IRFunction *fn, int label, SourceLoc loc);
static void emit_br(IRFunction *fn, int label, SourceLoc loc);
static void emit_cbr(IRFunction *fn, IRValue cond, int t_label, int f_label,
                     SourceLoc loc);

static IRValue emit_float_abs(IRFunction *fn, IRValue v, int elem_sz, SourceLoc loc) {
    IRValue zero = emit_float_const(fn, elem_sz, 0, loc);
    IRValue lt0 = emit_fcmp(fn, v, zero, elem_sz, 0 /* LT */, loc);
    IRValue neg_v = emit_bin_w(fn, IR_FSUB, zero, v, elem_sz, 0, loc);
    set_value_float(fn, neg_v, 1);
    
    IRValue slot = new_value(fn);
    emit_inst_w(fn, IR_ALLOCA, slot, -1, -1, 0, elem_sz, 0, loc);
    set_value_float(fn, slot, 1);
    
    int Lt = new_label(fn), Lf = new_label(fn), Ld = new_label(fn);
    emit_cbr(fn, lt0, Lt, Lf, loc);
    emit_label(fn, Lt, loc);
    emit_inst_w(fn, IR_STORE, -1, slot, neg_v, 0, elem_sz, 0, loc);
    fn->insts.data[fn->insts.len - 1].is_float = 1;
    emit_br(fn, Ld, loc);
    emit_label(fn, Lf, loc);
    emit_inst_w(fn, IR_STORE, -1, slot, v, 0, elem_sz, 0, loc);
    fn->insts.data[fn->insts.len - 1].is_float = 1;
    emit_br(fn, Ld, loc);
    emit_label(fn, Ld, loc);
    
    IRValue res = new_value(fn);
    emit_inst_w(fn, IR_LOAD, res, slot, -1, 0, elem_sz, 0, loc);
    fn->insts.data[fn->insts.len - 1].is_float = 1;
    set_value_float(fn, res, 1);
    return res;
}

/* Scalar __int128 is a 16-byte INTEGER pair (lo, hi) in memory, matching
 * SysV AMD64.  SSA values for these expressions are addresses, not 128-bit
 * registers — arithmetic is expanded into 64-bit ops (add+carry, schoolbook
 * mul, restoring div via runtime). */
static int type_is_i128(Type t) {
    return t.kind == TY_INT && t.width == 16 && !t.is_vector;
}
static int type_is_d128(Type t) {
    return t.kind == TY_FLOAT && t.is_decimal && t.width == 16 && !t.is_vector;
}
static int type_is_pair16(Type t) {
    return type_is_i128(t) || type_is_d128(t);
}
static int get_value_is_decimal(const IRFunction *fn, IRValue v) {
    return get_value_is_float(fn, v) == 2;
}
static void set_value_decimal(IRFunction *fn, IRValue v) {
    set_value_float(fn, v, 2);
}

static IRValue i64imm(IRFunction *fn, int64_t x, SourceLoc loc);
static IRValue i128_alloc(IRFunction *fn, SourceLoc loc);
static void i128_load2(IRFunction *fn, IRValue addr, IRValue *lo, IRValue *hi,
                      SourceLoc loc);
static void i128_store2(IRFunction *fn, IRValue addr, IRValue lo, IRValue hi,
                       SourceLoc loc);
static IRValue convert_numeric(IRFunction *fn, IRValue v,
                               int src_w, int dst_w, int dst_u,
                               int to_float, SourceLoc loc);
static IRValue coerce(IRFunction *fn, IRValue v, int src_w, int src_u,
                      int dst_w, int dst_u, SourceLoc loc);

static IRValue emit_runtime_call(IRFunction *fn, const char *name,
                                 IRValue *args, int nargs, int dst_w,
                                 int dst_float_kind, SourceLoc loc) {
    IRValue dst = new_value(fn);
    IRInst inst;
    memset(&inst, 0, sizeof(inst));
    inst.c = -1;
    inst.d = -1;
    inst.op = IR_CALL;
    inst.dst = dst;
    inst.a = -1;
    inst.b = -1;
    inst.width = dst_w ? dst_w : 4;
    inst.is_float = dst_float_kind ? 1 : 0;
    inst.loc = loc;
    inst.call_name = xstrdup(name);
    inst.call_callee = -1;
    ir_call_reserve_args(&inst, nargs);
    for (int i = 0; i < nargs; i++)
        inst.call_args[i] = args[i];
    ir_inst_array_push(&fn->insts, inst);
    set_value_type(fn, dst, dst_w ? dst_w : 4, 1);
    if (dst_float_kind == 2) set_value_decimal(fn, dst);
    else if (dst_float_kind) set_value_float(fn, dst, 1);
    return dst;
}

static void emit_runtime_call_void(IRFunction *fn, const char *name,
                                   IRValue *args, int nargs, SourceLoc loc) {
    IRInst inst;
    memset(&inst, 0, sizeof(inst));
    inst.c = -1;
    inst.d = -1;
    inst.op = IR_CALL;
    inst.dst = -1;
    inst.a = -1;
    inst.b = -1;
    inst.loc = loc;
    inst.call_name = xstrdup(name);
    inst.call_callee = -1;
    ir_call_reserve_args(&inst, nargs);
    for (int i = 0; i < nargs; i++)
        inst.call_args[i] = args[i];
    ir_inst_array_push(&fn->insts, inst);
}

static IRValue emit_bits_from_dfp(IRFunction *fn, IRValue f, int w, SourceLoc loc) {
    int bw = w == 4 ? 4 : 8;
    IRValue slot = emit_alloca(fn, bw, bw, 1, loc);
    IRValue addr = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, loc);
    emit_inst_w(fn, IR_STORE_PTR, -1, addr, f, 0, bw, 1, loc);
    fn->insts.data[fn->insts.len - 1].is_float = 1;
    IRValue bits = new_value(fn);
    emit_inst_w(fn, IR_LOAD_PTR, bits, addr, -1, 0, bw, 1, loc);
    return bits;
}

static IRValue emit_dfp_from_bits(IRFunction *fn, IRValue bits, int w, SourceLoc loc) {
    int bw = w == 4 ? 4 : 8;
    IRValue slot = emit_alloca(fn, bw, bw, 1, loc);
    IRValue addr = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, loc);
    emit_inst_w(fn, IR_STORE_PTR, -1, addr, bits, 0, bw, 1, loc);
    IRValue f = new_value(fn);
    emit_inst_w(fn, IR_LOAD_PTR, f, addr, -1, 0, bw, 1, loc);
    set_value_decimal(fn, f);
    return f;
}

static IRValue emit_d128_const(IRFunction *fn, unsigned long long lo,
                               unsigned long long hi, SourceLoc loc) {
    IRValue dst = i128_alloc(fn, loc);
    i128_store2(fn, dst, i64imm(fn, (int64_t)lo, loc),
                i64imm(fn, (int64_t)hi, loc), loc);
    return dst;
}

static IRValue emit_dfp_const(IRFunction *fn, int width,
                              unsigned long long lo, unsigned long long hi,
                              SourceLoc loc) {
    if (width == 16)
        return emit_d128_const(fn, lo, hi, loc);
    int bw = width == 4 ? 4 : 8;
    IRValue v = new_value(fn);
    emit_inst_w(fn, IR_CONST, v, -1, -1, 0, bw, 0, loc);
    IRInst *inst = &fn->insts.data[fn->insts.len - 1];
    inst->is_float = 1;
    inst->float_imm = (int64_t)lo;
    set_value_decimal(fn, v);
    return v;
}

static const char *dfp_arith_name(int width, int op) {
    /* op: 0 add 1 sub 2 mul 3 div */
    if (width == 4) {
        if (op == 1) return "__bid_subsd3";
        if (op == 2) return "__bid_mulsd3";
        if (op == 3) return "__bid_divsd3";
        return "__bid_addsd3";
    }
    if (op == 1) return "__bid_subdd3";
    if (op == 2) return "__bid_muldd3";
    if (op == 3) return "__bid_divdd3";
    return "__bid_adddd3";
}

static IRValue emit_dfp_arith(IRFunction *fn, int width, int op,
                              IRValue a, IRValue b, SourceLoc loc) {
    if (width == 16) {
        IRValue alo, ahi, blo, bhi;
        i128_load2(fn, a, &alo, &ahi, loc);
        i128_load2(fn, b, &blo, &bhi, loc);
        IRValue dst = i128_alloc(fn, loc);
        IRValue dlo_slot = emit_alloca(fn, 8, 8, 1, loc);
        IRValue dhi_slot = emit_alloca(fn, 8, 8, 1, loc);
        IRValue dlo = emit_bin_w(fn, IR_ADDR, dlo_slot, -1, 8, 1, loc);
        IRValue dhi = emit_bin_w(fn, IR_ADDR, dhi_slot, -1, 8, 1, loc);
        IRValue args[6];
        args[0] = dlo; args[1] = dhi;
        args[2] = alo; args[3] = ahi;
        args[4] = blo; args[5] = bhi;
        const char *nm = "__fakecc_addtd3";
        if (op == 1) nm = "__fakecc_subtd3";
        else if (op == 2) nm = "__fakecc_multd3";
        else if (op == 3) nm = "__fakecc_divtd3";
        emit_runtime_call_void(fn, nm, args, 6, loc);
        IRValue rlo = new_value(fn);
        emit_inst_w(fn, IR_LOAD_PTR, rlo, dlo, -1, 0, 8, 1, loc);
        IRValue rhi = new_value(fn);
        emit_inst_w(fn, IR_LOAD_PTR, rhi, dhi, -1, 0, 8, 1, loc);
        i128_store2(fn, dst, rlo, rhi, loc);
        return dst;
    }
    IRValue ba = emit_bits_from_dfp(fn, a, width, loc);
    IRValue bb = emit_bits_from_dfp(fn, b, width, loc);
    IRValue args[2];
    args[0] = ba;
    args[1] = bb;
    int bw = width == 4 ? 4 : 8;
    IRValue rb = emit_runtime_call(fn, dfp_arith_name(width, op), args, 2,
                                   bw, 0, loc);
    return emit_dfp_from_bits(fn, rb, width, loc);
}

static IRValue emit_dfp_cmp(IRFunction *fn, int width, BinOp op,
                            IRValue a, IRValue b, SourceLoc loc) {
    IRValue c;
    if (width == 16) {
        IRValue alo, ahi, blo, bhi;
        i128_load2(fn, a, &alo, &ahi, loc);
        i128_load2(fn, b, &blo, &bhi, loc);
        IRValue args[4];
        args[0] = alo; args[1] = ahi; args[2] = blo; args[3] = bhi;
        c = emit_runtime_call(fn, "__fakecc_cmptd2", args, 4, 4, 0, loc);
    } else {
        IRValue ba = emit_bits_from_dfp(fn, a, width, loc);
        IRValue bb = emit_bits_from_dfp(fn, b, width, loc);
        IRValue args[2];
        args[0] = ba;
        args[1] = bb;
        const char *nm = width == 4 ? "__bid_eqsd2" : "__bid_eqdd2";
        c = emit_runtime_call(fn, nm, args, 2, 4, 0, loc);
    }
    IRValue z = new_value(fn);
    emit_inst_w(fn, IR_CONST, z, -1, -1, 0, 4, 0, loc);
    IRValue m1 = new_value(fn);
    emit_inst_w(fn, IR_CONST, m1, -1, -1, -1, 4, 0, loc);
    IRValue p1 = new_value(fn);
    emit_inst_w(fn, IR_CONST, p1, -1, -1, 1, 4, 0, loc);
    if (op == BOP_EQ) return emit_bin_w(fn, IR_EQ, c, z, 4, 0, loc);
    if (op == BOP_NE) return emit_bin_w(fn, IR_NE, c, z, 4, 0, loc);
    if (op == BOP_LT) return emit_bin_w(fn, IR_EQ, c, m1, 4, 0, loc);
    if (op == BOP_GT) return emit_bin_w(fn, IR_EQ, c, p1, 4, 0, loc);
    if (op == BOP_LE) {
        IRValue eq0 = emit_bin_w(fn, IR_EQ, c, z, 4, 0, loc);
        IRValue eqm = emit_bin_w(fn, IR_EQ, c, m1, 4, 0, loc);
        return emit_bin_w(fn, IR_BOR, eq0, eqm, 4, 0, loc);
    }
    IRValue eq0 = emit_bin_w(fn, IR_EQ, c, z, 4, 0, loc);
    IRValue eqp = emit_bin_w(fn, IR_EQ, c, p1, 4, 0, loc);
    return emit_bin_w(fn, IR_BOR, eq0, eqp, 4, 0, loc);
}

static IRValue emit_dfp_zero(IRFunction *fn, int width, SourceLoc loc) {
    unsigned long long lo = 0, hi = 0;
    dfp_from_str("0", width, &lo, &hi);
    return emit_dfp_const(fn, width, lo, hi, loc);
}

static IRValue convert_to_decimal(IRFunction *fn, IRValue v, Type src, Type dst,
                                  SourceLoc loc) {
    int dw = dst.width ? dst.width : 8;
    if (src.is_decimal && src.width == dw)
        return v;
    if (src.is_decimal) {
        if (src.width == 16 && dw != 16) {
            IRValue lo, hi;
            i128_load2(fn, v, &lo, &hi, loc);
            IRValue args[2];
            args[0] = lo; args[1] = hi;
            const char *nm = dw == 4 ? "__bid_trunctdsd2" : "__bid_trunctddd2";
            int bw = dw == 4 ? 4 : 8;
            IRValue bits = emit_runtime_call(fn, nm, args, 2, bw, 0, loc);
            return emit_dfp_from_bits(fn, bits, dw, loc);
        }
        if (dw == 16) {
            IRValue bits = (src.width == 16) ? v : emit_bits_from_dfp(fn, v, src.width, loc);
            IRValue dstv = i128_alloc(fn, loc);
            IRValue dlo_s = emit_alloca(fn, 8, 8, 1, loc);
            IRValue dhi_s = emit_alloca(fn, 8, 8, 1, loc);
            IRValue dlo = emit_bin_w(fn, IR_ADDR, dlo_s, -1, 8, 1, loc);
            IRValue dhi = emit_bin_w(fn, IR_ADDR, dhi_s, -1, 8, 1, loc);
            if (src.width == 4) {
                IRValue args[3];
                args[0] = dlo; args[1] = dhi; args[2] = bits;
                emit_runtime_call_void(fn, "__fakecc_extendsdtd2", args, 3, loc);
            } else if (src.width == 8) {
                IRValue args[3];
                args[0] = dlo; args[1] = dhi; args[2] = bits;
                emit_runtime_call_void(fn, "__fakecc_extendddtd2", args, 3, loc);
            } else {
                return v;
            }
            IRValue rlo = new_value(fn);
            emit_inst_w(fn, IR_LOAD_PTR, rlo, dlo, -1, 0, 8, 1, loc);
            IRValue rhi = new_value(fn);
            emit_inst_w(fn, IR_LOAD_PTR, rhi, dhi, -1, 0, 8, 1, loc);
            i128_store2(fn, dstv, rlo, rhi, loc);
            return dstv;
        }
        if (src.width == 4 && dw == 8) {
            IRValue bits = emit_bits_from_dfp(fn, v, 4, loc);
            IRValue args[1]; args[0] = bits;
            IRValue rb = emit_runtime_call(fn, "__bid_extendsddd2", args, 1, 8, 0, loc);
            return emit_dfp_from_bits(fn, rb, 8, loc);
        }
        if (src.width == 8 && dw == 4) {
            IRValue bits = emit_bits_from_dfp(fn, v, 8, loc);
            IRValue args[1]; args[0] = bits;
            IRValue rb = emit_runtime_call(fn, "__bid_truncddsd2", args, 1, 4, 0, loc);
            return emit_dfp_from_bits(fn, rb, 4, loc);
        }
        return v;
    }
    if (src.kind == TY_INT) {
        int sw = get_value_width(fn, v);
        if (sw < 4) v = coerce(fn, v, sw, src.is_unsigned, 4, src.is_unsigned, loc);
        if (dw == 16) {
            IRValue dstv = i128_alloc(fn, loc);
            IRValue dlo_s = emit_alloca(fn, 8, 8, 1, loc);
            IRValue dhi_s = emit_alloca(fn, 8, 8, 1, loc);
            IRValue dlo = emit_bin_w(fn, IR_ADDR, dlo_s, -1, 8, 1, loc);
            IRValue dhi = emit_bin_w(fn, IR_ADDR, dhi_s, -1, 8, 1, loc);
            IRValue args[3];
            args[0] = dlo; args[1] = dhi; args[2] = v;
            emit_runtime_call_void(fn, "__fakecc_floatsitd", args, 3, loc);
            IRValue rlo = new_value(fn);
            emit_inst_w(fn, IR_LOAD_PTR, rlo, dlo, -1, 0, 8, 1, loc);
            IRValue rhi = new_value(fn);
            emit_inst_w(fn, IR_LOAD_PTR, rhi, dhi, -1, 0, 8, 1, loc);
            i128_store2(fn, dstv, rlo, rhi, loc);
            return dstv;
        }
        const char *nm = dw == 4 ? "__bid_floatsisd" : "__bid_floatsidd";
        if (sw >= 8) nm = "__bid_floatdisdd";
        IRValue args[1]; args[0] = v;
        int bw = dw == 4 ? 4 : 8;
        IRValue rb = emit_runtime_call(fn, nm, args, 1, bw, 0, loc);
        if (dw == 4 && sw >= 8) {
            /* floatdisdd returns d64; trunc to d32. */
            IRValue targs[1]; targs[0] = rb;
            rb = emit_runtime_call(fn, "__bid_truncddsd2", targs, 1, 4, 0, loc);
        }
        return emit_dfp_from_bits(fn, rb, dw, loc);
    }
    /* binary float → decimal */
    int sw = get_value_width(fn, v);
    if (sw == 16)
        v = convert_numeric(fn, v, 16, 8, 0, 1, loc);
    IRValue bits = emit_bits_from_dfp(fn, v, 8, loc);
    if (dw == 16) {
        /* via d64 then extend */
        IRValue args[1]; args[0] = bits;
        IRValue d64 = emit_runtime_call(fn, "__bid_extenddfdd", args, 1, 8, 0, loc);
        Type d64t = type_make_decimal(8);
        IRValue tmp = emit_dfp_from_bits(fn, d64, 8, loc);
        return convert_to_decimal(fn, tmp, d64t, dst, loc);
    }
    const char *nm = dw == 4 ? "__bid_truncdfsd" : "__bid_extenddfdd";
    IRValue args[1]; args[0] = bits;
    int bw = dw == 4 ? 4 : 8;
    IRValue rb = emit_runtime_call(fn, nm, args, 1, bw, 0, loc);
    return emit_dfp_from_bits(fn, rb, dw, loc);
}

static IRValue convert_from_decimal(IRFunction *fn, IRValue v, Type src, Type dst,
                                    SourceLoc loc) {
    int sw = src.width ? src.width : 8;
    if (dst.is_decimal)
        return convert_to_decimal(fn, v, src, dst, loc);
    if (dst.kind == TY_INT || dst.is_bool) {
        IRValue bits;
        if (sw == 16) {
            IRValue lo, hi;
            i128_load2(fn, v, &lo, &hi, loc);
            IRValue args[2]; args[0] = lo; args[1] = hi;
            bits = emit_runtime_call(fn, "__fakecc_fixtdsi", args, 2, 4, 0, loc);
        } else {
            bits = emit_bits_from_dfp(fn, v, sw, loc);
            const char *nm = sw == 4 ? "__bid_fixsdsi" : "__bid_fixddsi";
            if (dst.width == 8 && sw == 8) nm = "__bid_fixdddi";
            IRValue args[1]; args[0] = bits;
            int rw = (dst.width >= 8 && sw == 8) ? 8 : 4;
            bits = emit_runtime_call(fn, nm, args, 1, rw, 0, loc);
        }
        if (dst.is_bool) {
            IRValue z = new_value(fn);
            emit_inst_w(fn, IR_CONST, z, -1, -1, 0, 4, 0, loc);
            return emit_bin_w(fn, IR_NE, bits, z, 4, 0, loc);
        }
        int dw = dst.kind == TY_PTR ? 8 : (dst.width ? dst.width : 4);
        return coerce(fn, bits, get_value_width(fn, bits), 0, dw, dst.is_unsigned, loc);
    }
    /* decimal → binary float */
    IRValue bits;
    if (sw == 16) {
        IRValue lo, hi;
        i128_load2(fn, v, &lo, &hi, loc);
        IRValue args[2]; args[0] = lo; args[1] = hi;
        bits = emit_runtime_call(fn, "__bid_trunctddd2", args, 2, 8, 0, loc);
        bits = emit_runtime_call(fn, "__bid_truncdddf", &bits, 1, 8, 0, loc);
    } else {
        bits = emit_bits_from_dfp(fn, v, sw, loc);
        const char *nm = sw == 4 ? "__bid_extendsddf" : "__bid_truncdddf";
        IRValue args[1]; args[0] = bits;
        bits = emit_runtime_call(fn, nm, args, 1, 8, 0, loc);
    }
    IRValue f = emit_dfp_from_bits(fn, bits, 8, loc);
    /* bits were IEEE binary stored as decimal-tagged; retag as binary float. */
    set_value_float(fn, f, 1);
    int dw = dst.width ? dst.width : 8;
    if (dw == 8) return f;
    return convert_numeric(fn, f, 8, dw, 0, 1, loc);
}

static IRValue i64imm(IRFunction *fn, int64_t x, SourceLoc loc) {
    IRValue v = new_value(fn);
    emit_inst_w(fn, IR_CONST, v, -1, -1, x, 8, 1, loc);
    return v;
}

static IRValue i128_alloc(IRFunction *fn, SourceLoc loc) {
    IRValue slot = emit_alloca(fn, 16, 16, 1, loc);
    return emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, loc);
}

static void i128_load2(IRFunction *fn, IRValue addr, IRValue *lo, IRValue *hi,
                      SourceLoc loc) {
    *lo = new_value(fn);
    emit_inst_w(fn, IR_LOAD_PTR, *lo, addr, -1, 0, 8, 1, loc);
    IRValue hip = emit_add_const(fn, addr, 8, loc);
    *hi = new_value(fn);
    emit_inst_w(fn, IR_LOAD_PTR, *hi, hip, -1, 0, 8, 1, loc);
}

static void i128_store2(IRFunction *fn, IRValue addr, IRValue lo, IRValue hi,
                       SourceLoc loc) {
    emit_inst_w(fn, IR_STORE_PTR, -1, addr, lo, 0, 8, 1, loc);
    IRValue hip = emit_add_const(fn, addr, 8, loc);
    emit_inst_w(fn, IR_STORE_PTR, -1, hip, hi, 0, 8, 1, loc);
}

/* libgcc-style __int128 ↔ float: runtime IEEE conversion, not hi*2^64+lo. */
static IRValue emit_i128_to_float(IRFunction *fn, IRValue addr, int is_unsigned,
                                 int fw, SourceLoc loc) {
    IRValue lo, hi;
    i128_load2(fn, addr, &lo, &hi, loc);
    const char *name;
    if (fw == 4)
        name = is_unsigned ? "__fakecc_floatuntisf" : "__fakecc_floattisf";
    else if (fw == 16)
        name = is_unsigned ? "__fakecc_floatuntixf" : "__fakecc_floattixf";
    else
        name = is_unsigned ? "__fakecc_floatuntidf" : "__fakecc_floattidf";
    IRValue dst = new_value(fn);
    set_value_type(fn, dst, fw ? fw : 8, 0);
    set_value_float(fn, dst, 1);
    IRInst inst;
    memset(&inst, 0, sizeof(inst));
    inst.c = -1;
    inst.d = -1;
    inst.op = IR_CALL;
    inst.dst = dst;
    inst.a = -1;
    inst.b = -1;
    inst.width = fw ? fw : 8;
    inst.is_float = 1;
    inst.loc = loc;
    inst.call_name = xstrdup(name);
    inst.call_callee = -1;
    ir_call_reserve_args(&inst, 2);
    inst.call_args[0] = lo;
    inst.call_args[1] = hi;
    ir_inst_array_push(&fn->insts, inst);
    return dst;
}

static IRValue emit_float_to_i128(IRFunction *fn, IRValue x, int is_unsigned,
                                 SourceLoc loc) {
    int fw = get_value_width(fn, x);
    const char *name;
    if (fw == 4)
        name = is_unsigned ? "__fakecc_fixunssfti" : "__fakecc_fixsfti";
    else if (fw == 16)
        name = is_unsigned ? "__fakecc_fixunsxfti" : "__fakecc_fixxfti";
    else
        name = is_unsigned ? "__fakecc_fixunsdfti" : "__fakecc_fixdfti";
    IRValue los = emit_alloca(fn, 8, 8, 1, loc);
    IRValue his = emit_alloca(fn, 8, 8, 1, loc);
    IRValue lop = emit_bin_w(fn, IR_ADDR, los, -1, 8, 1, loc);
    IRValue hip = emit_bin_w(fn, IR_ADDR, his, -1, 8, 1, loc);
    IRInst inst;
    memset(&inst, 0, sizeof(inst));
    inst.c = -1;
    inst.d = -1;
    inst.op = IR_CALL;
    inst.dst = -1;
    inst.a = -1;
    inst.b = -1;
    inst.loc = loc;
    inst.call_name = xstrdup(name);
    inst.call_callee = -1;
    ir_call_reserve_args(&inst, 3);
    inst.call_args[0] = x;
    inst.call_args[1] = lop;
    inst.call_args[2] = hip;
    ir_inst_array_push(&fn->insts, inst);
    IRValue dst = i128_alloc(fn, loc);
    IRValue lo = new_value(fn);
    emit_inst_w(fn, IR_LOAD_PTR, lo, lop, -1, 0, 8, 1, loc);
    IRValue hi = new_value(fn);
    emit_inst_w(fn, IR_LOAD_PTR, hi, hip, -1, 0, 8, 1, loc);
    i128_store2(fn, dst, lo, hi, loc);
    return dst;
}

static IRValue i128_from_scalar(IRFunction *fn, IRValue v, int src_w, int src_u,
                               int dst_u, SourceLoc loc) {
    (void)dst_u;
    IRValue lo = coerce(fn, v, src_w, src_u, 8, src_u, loc);
    IRValue hi;
    if (src_u) {
        hi = i64imm(fn, 0, loc);
    } else {
        IRValue sh = i64imm(fn, 63, loc);
        hi = emit_bin_w(fn, IR_SHR, lo, sh, 8, 0, loc);
    }
    IRValue dst = i128_alloc(fn, loc);
    i128_store2(fn, dst, lo, hi, loc);
    return dst;
}

/* Lift an overflow-builtin operand to a 128-bit slot using *its own*
 * signedness (GCC: both operands are promoted to infinite precision). */
static IRValue overflow_lift_i128(IRFunction *fn, IRValue v, Type ty,
                                  SourceLoc loc) {
    if (type_is_i128(ty)) return v;
    int w = get_value_width(fn, v);
    if (w <= 0) w = ty.width ? ty.width : 4;
    return i128_from_scalar(fn, v, w, ty.is_unsigned, ty.is_unsigned, loc);
}

/* True (i32) if the 128-bit two's-complement value (lo, hi) is not in
 * range of dest type `tw` bytes / `tu` unsigned. */
static IRValue i128_not_in_dest(IRFunction *fn, IRValue lo, IRValue hi,
                                int tw, int tu, SourceLoc loc) {
    if (tw >= 16) {
        if (tu) {
            /* unsigned __int128: overflow iff the mathematical value is
             * negative (high half negative as signed). */
            IRValue z = i64imm(fn, 0, loc);
            IRValue is_neg = emit_bin_w(fn, IR_LT, hi, z, 8, 0, loc);
            return coerce(fn, is_neg, 8, 0, 4, 0, loc);
        }
        IRValue c0 = new_value(fn);
        emit_inst_w(fn, IR_CONST, c0, -1, -1, 0, 4, 0, loc);
        return c0;
    }
    IRValue narrow = coerce(fn, lo, 8, tu, tw, tu, loc);
    IRValue ext_lo = coerce(fn, narrow, tw, tu, 8, tu, loc);
    IRValue lo_bad = emit_bin_w(fn, IR_NE, lo, ext_lo, 8, 1, loc);
    IRValue exp_hi;
    if (tu) {
        exp_hi = i64imm(fn, 0, loc);
    } else {
        IRValue c63 = i64imm(fn, 63, loc);
        exp_hi = emit_bin_w(fn, IR_SHR, ext_lo, c63, 8, 0, loc);
    }
    IRValue hi_bad = emit_bin_w(fn, IR_NE, hi, exp_hi, 8, 1, loc);
    IRValue ov = emit_bin_w(fn, IR_BOR, lo_bad, hi_bad, 8, 1, loc);
    return coerce(fn, ov, 8, 1, 4, 0, loc);
}

static IRValue i128_as_addr(IRFunction *fn, IRValue v, Type ty, int dst_u,
                           SourceLoc loc) {
    if (type_is_i128(ty)) return v;
    int w = get_value_width(fn, v);
    int u = get_value_is_unsigned(fn, v);
    if (w <= 0) w = 4;
    return i128_from_scalar(fn, v, w, u, dst_u, loc);
}

static void i128_add(IRFunction *fn, IRValue dst, IRValue a, IRValue b,
                    SourceLoc loc) {
    IRValue alo, ahi, blo, bhi;
    i128_load2(fn, a, &alo, &ahi, loc);
    i128_load2(fn, b, &blo, &bhi, loc);
    IRValue slo = emit_bin_w(fn, IR_ADD, alo, blo, 8, 1, loc);
    IRValue carry = emit_bin_w(fn, IR_LT, slo, alo, 8, 1, loc);
    carry = coerce(fn, carry, 4, 0, 8, 1, loc);
    IRValue shi = emit_bin_w(fn, IR_ADD, ahi, bhi, 8, 1, loc);
    shi = emit_bin_w(fn, IR_ADD, shi, carry, 8, 1, loc);
    i128_store2(fn, dst, slo, shi, loc);
}

static void i128_sub(IRFunction *fn, IRValue dst, IRValue a, IRValue b,
                    SourceLoc loc) {
    IRValue alo, ahi, blo, bhi;
    i128_load2(fn, a, &alo, &ahi, loc);
    i128_load2(fn, b, &blo, &bhi, loc);
    IRValue dlo = emit_bin_w(fn, IR_SUB, alo, blo, 8, 1, loc);
    IRValue borrow = emit_bin_w(fn, IR_LT, alo, blo, 8, 1, loc);
    borrow = coerce(fn, borrow, 4, 0, 8, 1, loc);
    IRValue dhi = emit_bin_w(fn, IR_SUB, ahi, bhi, 8, 1, loc);
    dhi = emit_bin_w(fn, IR_SUB, dhi, borrow, 8, 1, loc);
    i128_store2(fn, dst, dlo, dhi, loc);
}

static void i128_neg(IRFunction *fn, IRValue dst, IRValue a, SourceLoc loc) {
    IRValue z = i128_alloc(fn, loc);
    i128_store2(fn, z, i64imm(fn, 0, loc), i64imm(fn, 0, loc), loc);
    i128_sub(fn, dst, z, a, loc);
}

static void mul64_wide(IRFunction *fn, IRValue a, IRValue b,
                      IRValue *lo_out, IRValue *hi_out, SourceLoc loc) {
    IRValue mask = i64imm(fn, 0xffffffffLL, loc);
    IRValue sh32 = i64imm(fn, 32, loc);
    IRValue a0 = emit_bin_w(fn, IR_BAND, a, mask, 8, 1, loc);
    IRValue a1 = emit_bin_w(fn, IR_SHR, a, sh32, 8, 1, loc);
    IRValue b0 = emit_bin_w(fn, IR_BAND, b, mask, 8, 1, loc);
    IRValue b1 = emit_bin_w(fn, IR_SHR, b, sh32, 8, 1, loc);
    IRValue p0 = emit_bin_w(fn, IR_MUL, a0, b0, 8, 1, loc);
    IRValue p1 = emit_bin_w(fn, IR_MUL, a0, b1, 8, 1, loc);
    IRValue p2 = emit_bin_w(fn, IR_MUL, a1, b0, 8, 1, loc);
    IRValue p3 = emit_bin_w(fn, IR_MUL, a1, b1, 8, 1, loc);
    IRValue p0h = emit_bin_w(fn, IR_SHR, p0, sh32, 8, 1, loc);
    IRValue p1l = emit_bin_w(fn, IR_BAND, p1, mask, 8, 1, loc);
    IRValue p2l = emit_bin_w(fn, IR_BAND, p2, mask, 8, 1, loc);
    IRValue m = emit_bin_w(fn, IR_ADD, p0h, p1l, 8, 1, loc);
    m = emit_bin_w(fn, IR_ADD, m, p2l, 8, 1, loc);
    IRValue mh = emit_bin_w(fn, IR_SHR, m, sh32, 8, 1, loc);
    IRValue p1h = emit_bin_w(fn, IR_SHR, p1, sh32, 8, 1, loc);
    IRValue p2h = emit_bin_w(fn, IR_SHR, p2, sh32, 8, 1, loc);
    IRValue hi = emit_bin_w(fn, IR_ADD, p3, p1h, 8, 1, loc);
    hi = emit_bin_w(fn, IR_ADD, hi, p2h, 8, 1, loc);
    hi = emit_bin_w(fn, IR_ADD, hi, mh, 8, 1, loc);
    IRValue ml = emit_bin_w(fn, IR_BAND, m, mask, 8, 1, loc);
    IRValue mls = emit_bin_w(fn, IR_SHL, ml, sh32, 8, 1, loc);
    IRValue p0l = emit_bin_w(fn, IR_BAND, p0, mask, 8, 1, loc);
    *lo_out = emit_bin_w(fn, IR_BOR, mls, p0l, 8, 1, loc);
    *hi_out = hi;
}

static void i128_mul(IRFunction *fn, IRValue dst, IRValue a, IRValue b,
                    SourceLoc loc) {
    IRValue alo, ahi, blo, bhi;
    i128_load2(fn, a, &alo, &ahi, loc);
    i128_load2(fn, b, &blo, &bhi, loc);
    IRValue tlo, thi;
    mul64_wide(fn, alo, blo, &tlo, &thi, loc);
    IRValue p1 = emit_bin_w(fn, IR_MUL, ahi, blo, 8, 1, loc);
    IRValue p2 = emit_bin_w(fn, IR_MUL, alo, bhi, 8, 1, loc);
    IRValue hi = emit_bin_w(fn, IR_ADD, thi, p1, 8, 1, loc);
    hi = emit_bin_w(fn, IR_ADD, hi, p2, 8, 1, loc);
    i128_store2(fn, dst, tlo, hi, loc);
}

static void i128_bit(IRFunction *fn, IROpcode op, IRValue dst, IRValue a,
                    IRValue b, SourceLoc loc) {
    IRValue alo, ahi, blo, bhi;
    i128_load2(fn, a, &alo, &ahi, loc);
    i128_load2(fn, b, &blo, &bhi, loc);
    i128_store2(fn, dst,
                emit_bin_w(fn, op, alo, blo, 8, 1, loc),
                emit_bin_w(fn, op, ahi, bhi, 8, 1, loc), loc);
}

static void i128_bnot(IRFunction *fn, IRValue dst, IRValue a, SourceLoc loc) {
    IRValue lo, hi;
    i128_load2(fn, a, &lo, &hi, loc);
    IRValue nlo = new_value(fn);
    emit_inst_w(fn, IR_BNOT, nlo, lo, -1, 0, 8, 1, loc);
    IRValue nhi = new_value(fn);
    emit_inst_w(fn, IR_BNOT, nhi, hi, -1, 0, 8, 1, loc);
    i128_store2(fn, dst, nlo, nhi, loc);
}

static IRValue i128_nz(IRFunction *fn, IRValue a, SourceLoc loc) {
    IRValue lo, hi;
    i128_load2(fn, a, &lo, &hi, loc);
    IRValue z = i64imm(fn, 0, loc);
    IRValue nlo = emit_bin_w(fn, IR_NE, lo, z, 8, 1, loc);
    IRValue nhi = emit_bin_w(fn, IR_NE, hi, z, 8, 1, loc);
    return emit_bin_w(fn, IR_BOR, nlo, nhi, 4, 0, loc);
}

static void i128_shl(IRFunction *fn, IRValue dst, IRValue a, IRValue n8,
                    SourceLoc loc) {
    IRValue alo, ahi;
    i128_load2(fn, a, &alo, &ahi, loc);
    IRValue n = coerce(fn, n8, get_value_width(fn, n8),
                       get_value_is_unsigned(fn, n8), 8, 1, loc);
    int L_ge128 = new_label(fn), L_ge64 = new_label(fn), L_nz = new_label(fn);
    int L_done = new_label(fn), L_small = new_label(fn);
    IRValue c128 = i64imm(fn, 128, loc);
    IRValue c64 = i64imm(fn, 64, loc);
    IRValue c0 = i64imm(fn, 0, loc);
    IRValue ge128 = emit_bin_w(fn, IR_GE, n, c128, 8, 1, loc);
    emit_cbr(fn, ge128, L_ge128, L_ge64, loc);
    emit_label(fn, L_ge128, loc);
    i128_store2(fn, dst, c0, c0, loc);
    emit_br(fn, L_done, loc);
    emit_label(fn, L_ge64, loc);
    IRValue ge64 = emit_bin_w(fn, IR_GE, n, c64, 8, 1, loc);
    emit_cbr(fn, ge64, L_nz, L_small, loc);
    emit_label(fn, L_nz, loc);
    {
        IRValue amt = emit_bin_w(fn, IR_SUB, n, c64, 8, 1, loc);
        IRValue hi = emit_bin_w(fn, IR_SHL, alo, amt, 8, 1, loc);
        i128_store2(fn, dst, c0, hi, loc);
        emit_br(fn, L_done, loc);
    }
    emit_label(fn, L_small, loc);
    {
        IRValue is0 = emit_bin_w(fn, IR_EQ, n, c0, 8, 1, loc);
        int L_copy = new_label(fn), L_sh = new_label(fn);
        emit_cbr(fn, is0, L_copy, L_sh, loc);
        emit_label(fn, L_copy, loc);
        i128_store2(fn, dst, alo, ahi, loc);
        emit_br(fn, L_done, loc);
        emit_label(fn, L_sh, loc);
        IRValue lo = emit_bin_w(fn, IR_SHL, alo, n, 8, 1, loc);
        IRValue hi = emit_bin_w(fn, IR_SHL, ahi, n, 8, 1, loc);
        IRValue sub = emit_bin_w(fn, IR_SUB, c64, n, 8, 1, loc);
        IRValue spill = emit_bin_w(fn, IR_SHR, alo, sub, 8, 1, loc);
        hi = emit_bin_w(fn, IR_BOR, hi, spill, 8, 1, loc);
        i128_store2(fn, dst, lo, hi, loc);
        emit_br(fn, L_done, loc);
    }
    emit_label(fn, L_done, loc);
}

static void i128_shr(IRFunction *fn, IRValue dst, IRValue a, IRValue n8,
                    int arith, SourceLoc loc) {
    IRValue alo, ahi;
    i128_load2(fn, a, &alo, &ahi, loc);
    IRValue n = coerce(fn, n8, get_value_width(fn, n8),
                       get_value_is_unsigned(fn, n8), 8, 1, loc);
    int su = arith ? 0 : 1;
    IRValue fill = arith ? emit_bin_w(fn, IR_SHR, ahi, i64imm(fn, 63, loc), 8, 0, loc)
                         : i64imm(fn, 0, loc);
    int L_ge128 = new_label(fn), L_ge64 = new_label(fn), L_big = new_label(fn);
    int L_done = new_label(fn), L_small = new_label(fn);
    IRValue c128 = i64imm(fn, 128, loc);
    IRValue c64 = i64imm(fn, 64, loc);
    IRValue c0 = i64imm(fn, 0, loc);
    IRValue ge128 = emit_bin_w(fn, IR_GE, n, c128, 8, 1, loc);
    emit_cbr(fn, ge128, L_ge128, L_ge64, loc);
    emit_label(fn, L_ge128, loc);
    i128_store2(fn, dst, fill, fill, loc);
    emit_br(fn, L_done, loc);
    emit_label(fn, L_ge64, loc);
    IRValue ge64 = emit_bin_w(fn, IR_GE, n, c64, 8, 1, loc);
    emit_cbr(fn, ge64, L_big, L_small, loc);
    emit_label(fn, L_big, loc);
    {
        IRValue amt = emit_bin_w(fn, IR_SUB, n, c64, 8, 1, loc);
        IRValue lo = emit_bin_w(fn, IR_SHR, ahi, amt, 8, su, loc);
        i128_store2(fn, dst, lo, fill, loc);
        emit_br(fn, L_done, loc);
    }
    emit_label(fn, L_small, loc);
    {
        IRValue is0 = emit_bin_w(fn, IR_EQ, n, c0, 8, 1, loc);
        int L_copy = new_label(fn), L_sh = new_label(fn);
        emit_cbr(fn, is0, L_copy, L_sh, loc);
        emit_label(fn, L_copy, loc);
        i128_store2(fn, dst, alo, ahi, loc);
        emit_br(fn, L_done, loc);
        emit_label(fn, L_sh, loc);
        IRValue hi = emit_bin_w(fn, IR_SHR, ahi, n, 8, su, loc);
        IRValue lo = emit_bin_w(fn, IR_SHR, alo, n, 8, 1, loc);
        IRValue sub = emit_bin_w(fn, IR_SUB, c64, n, 8, 1, loc);
        IRValue spill = emit_bin_w(fn, IR_SHL, ahi, sub, 8, 1, loc);
        lo = emit_bin_w(fn, IR_BOR, lo, spill, 8, 1, loc);
        i128_store2(fn, dst, lo, hi, loc);
        emit_br(fn, L_done, loc);
    }
    emit_label(fn, L_done, loc);
}

static void i128_divmod(IRFunction *fn, IRValue qdst, IRValue rdst,
                       IRValue n, IRValue d, int is_unsigned, SourceLoc loc) {
    IRValue nsign = i64imm(fn, 0, loc);
    IRValue dsign = i64imm(fn, 0, loc);
    IRValue nabs = i128_alloc(fn, loc);
    IRValue dabs = i128_alloc(fn, loc);
    emit_struct_copy(fn, nabs, n, 16, loc);
    emit_struct_copy(fn, dabs, d, 16, loc);
    if (!is_unsigned) {
        IRValue nlo, nhi;
        i128_load2(fn, n, &nlo, &nhi, loc);
        nsign = emit_bin_w(fn, IR_LT, nhi, i64imm(fn, 0, loc), 8, 0, loc);
        int Ln = new_label(fn), Lafter = new_label(fn);
        emit_cbr(fn, nsign, Ln, Lafter, loc);
        emit_label(fn, Ln, loc);
        i128_neg(fn, nabs, n, loc);
        emit_br(fn, Lafter, loc);
        emit_label(fn, Lafter, loc);
        IRValue dlo, dhi;
        i128_load2(fn, d, &dlo, &dhi, loc);
        dsign = emit_bin_w(fn, IR_LT, dhi, i64imm(fn, 0, loc), 8, 0, loc);
        int Ld = new_label(fn), Lafterd = new_label(fn);
        emit_cbr(fn, dsign, Ld, Lafterd, loc);
        emit_label(fn, Ld, loc);
        i128_neg(fn, dabs, d, loc);
        emit_br(fn, Lafterd, loc);
        emit_label(fn, Lafterd, loc);
    }
    IRValue nlo, nhi, dlo, dhi;
    i128_load2(fn, nabs, &nlo, &nhi, loc);
    i128_load2(fn, dabs, &dlo, &dhi, loc);
    IRValue qlo_s = emit_alloca(fn, 8, 8, 1, loc);
    IRValue qhi_s = emit_alloca(fn, 8, 8, 1, loc);
    IRValue rlo_s = emit_alloca(fn, 8, 8, 1, loc);
    IRValue rhi_s = emit_alloca(fn, 8, 8, 1, loc);
    IRValue qlo_p = emit_bin_w(fn, IR_ADDR, qlo_s, -1, 8, 1, loc);
    IRValue qhi_p = emit_bin_w(fn, IR_ADDR, qhi_s, -1, 8, 1, loc);
    IRValue rlo_p = emit_bin_w(fn, IR_ADDR, rlo_s, -1, 8, 1, loc);
    IRValue rhi_p = emit_bin_w(fn, IR_ADDR, rhi_s, -1, 8, 1, loc);
    IRInst inst;
    memset(&inst, 0, sizeof(inst));
    inst.c = -1;
    inst.d = -1;
    inst.op = IR_CALL;
    inst.dst = -1;
    inst.a = -1;
    inst.b = -1;
    inst.loc = loc;
    inst.call_name = xstrdup("__fakecc_udivmodti4");
    inst.call_callee = -1;
    ir_call_reserve_args(&inst, 8);
    inst.call_args[0] = nlo;
    inst.call_args[1] = nhi;
    inst.call_args[2] = dlo;
    inst.call_args[3] = dhi;
    inst.call_args[4] = qlo_p;
    inst.call_args[5] = qhi_p;
    inst.call_args[6] = rlo_p;
    inst.call_args[7] = rhi_p;
    ir_inst_array_push(&fn->insts, inst);
    IRValue qlo = new_value(fn);
    emit_inst_w(fn, IR_LOAD_PTR, qlo, qlo_p, -1, 0, 8, 1, loc);
    IRValue qhi = new_value(fn);
    emit_inst_w(fn, IR_LOAD_PTR, qhi, qhi_p, -1, 0, 8, 1, loc);
    IRValue rlo = new_value(fn);
    emit_inst_w(fn, IR_LOAD_PTR, rlo, rlo_p, -1, 0, 8, 1, loc);
    IRValue rhi = new_value(fn);
    emit_inst_w(fn, IR_LOAD_PTR, rhi, rhi_p, -1, 0, 8, 1, loc);
    i128_store2(fn, qdst, qlo, qhi, loc);
    i128_store2(fn, rdst, rlo, rhi, loc);
    if (!is_unsigned) {
        IRValue qneg = emit_bin_w(fn, IR_NE, nsign, dsign, 4, 0, loc);
        int Lqn = new_label(fn), Laq = new_label(fn);
        emit_cbr(fn, qneg, Lqn, Laq, loc);
        emit_label(fn, Lqn, loc);
        i128_neg(fn, qdst, qdst, loc);
        emit_br(fn, Laq, loc);
        emit_label(fn, Laq, loc);
        int Lrn = new_label(fn), Lar = new_label(fn);
        emit_cbr(fn, nsign, Lrn, Lar, loc);
        emit_label(fn, Lrn, loc);
        i128_neg(fn, rdst, rdst, loc);
        emit_br(fn, Lar, loc);
        emit_label(fn, Lar, loc);
    }
}

static IRValue i128_cmp(IRFunction *fn, BinOp op, IRValue a, IRValue b,
                       int is_unsigned, SourceLoc loc) {
    IRValue alo, ahi, blo, bhi;
    i128_load2(fn, a, &alo, &ahi, loc);
    i128_load2(fn, b, &blo, &bhi, loc);
    if (op == BOP_EQ) {
        IRValue el = emit_bin_w(fn, IR_EQ, alo, blo, 8, 1, loc);
        IRValue eh = emit_bin_w(fn, IR_EQ, ahi, bhi, 8, 1, loc);
        return emit_bin_w(fn, IR_BAND, el, eh, 4, 0, loc);
    }
    if (op == BOP_NE) {
        IRValue el = emit_bin_w(fn, IR_NE, alo, blo, 8, 1, loc);
        IRValue eh = emit_bin_w(fn, IR_NE, ahi, bhi, 8, 1, loc);
        return emit_bin_w(fn, IR_BOR, el, eh, 4, 0, loc);
    }
    int hu = is_unsigned ? 1 : 0;
    IRValue hi_lt = emit_bin_w(fn, IR_LT, ahi, bhi, 8, hu, loc);
    IRValue hi_gt = emit_bin_w(fn, IR_GT, ahi, bhi, 8, hu, loc);
    IRValue hi_eq = emit_bin_w(fn, IR_EQ, ahi, bhi, 8, 1, loc);
    IRValue lo_lt = emit_bin_w(fn, IR_LT, alo, blo, 8, 1, loc);
    IRValue lo_le = emit_bin_w(fn, IR_LE, alo, blo, 8, 1, loc);
    IRValue lo_gt = emit_bin_w(fn, IR_GT, alo, blo, 8, 1, loc);
    IRValue lo_ge = emit_bin_w(fn, IR_GE, alo, blo, 8, 1, loc);
    IRValue lt = emit_bin_w(fn, IR_BOR, hi_lt,
                            emit_bin_w(fn, IR_BAND, hi_eq, lo_lt, 4, 0, loc),
                            4, 0, loc);
    IRValue le = emit_bin_w(fn, IR_BOR, hi_lt,
                            emit_bin_w(fn, IR_BAND, hi_eq, lo_le, 4, 0, loc),
                            4, 0, loc);
    IRValue gt = emit_bin_w(fn, IR_BOR, hi_gt,
                            emit_bin_w(fn, IR_BAND, hi_eq, lo_gt, 4, 0, loc),
                            4, 0, loc);
    IRValue ge = emit_bin_w(fn, IR_BOR, hi_gt,
                            emit_bin_w(fn, IR_BAND, hi_eq, lo_ge, 4, 0, loc),
                            4, 0, loc);
    if (op == BOP_LT) return lt;
    if (op == BOP_LE) return le;
    if (op == BOP_GT) return gt;
    return ge;
}

static IRValue lower_i128_binop(IRFunction *fn, IRValue la, IRValue ra,
                               Type lt, Type rt, BinOp bop, Type res_ty,
                               SourceLoc loc) {
    if (fakecc_had_error()) return FAKECC_ERR;
    int dst_u = type_is_i128(res_ty) ? res_ty.is_unsigned
              : (type_is_i128(lt) ? lt.is_unsigned : rt.is_unsigned);
    IRValue a = i128_as_addr(fn, la, lt, dst_u, loc);
    IRValue b = i128_as_addr(fn, ra, rt, dst_u, loc);
    if (bop >= BOP_EQ && bop <= BOP_GE)
        return i128_cmp(fn, bop, a, b, dst_u, loc);
    IRValue dst = i128_alloc(fn, loc);
    if (bop == BOP_ADD) i128_add(fn, dst, a, b, loc);
    else if (bop == BOP_SUB) i128_sub(fn, dst, a, b, loc);
    else if (bop == BOP_MUL) i128_mul(fn, dst, a, b, loc);
    else if (bop == BOP_DIV || bop == BOP_MOD) {
        IRValue q = i128_alloc(fn, loc);
        IRValue r = i128_alloc(fn, loc);
        i128_divmod(fn, q, r, a, b, dst_u, loc);
        emit_struct_copy(fn, dst, bop == BOP_DIV ? q : r, 16, loc);
    } else if (bop == BOP_BITAND) i128_bit(fn, IR_BAND, dst, a, b, loc);
    else if (bop == BOP_BITOR) i128_bit(fn, IR_BOR, dst, a, b, loc);
    else if (bop == BOP_BITXOR) i128_bit(fn, IR_BXOR, dst, a, b, loc);
    else if (bop == BOP_SHL || bop == BOP_SHR) {
        IRValue sh = ra;
        if (type_is_i128(rt)) {
            IRValue hi_ign;
            i128_load2(fn, ra, &sh, &hi_ign, loc);
        }
        if (bop == BOP_SHL) i128_shl(fn, dst, a, sh, loc);
        else i128_shr(fn, dst, a, sh, !dst_u, loc);
    }
    else i128_add(fn, dst, a, b, loc);
    return dst;
}

/* Emit `dst = &global-with-name`.  When `is_tls` is set, emits IR_GADDR_TLS
 * instead of IR_GADDR — the codegen path emits `lea %rxx, %fs:[rip+TPOFF64]`
 * so the linker resolves the address against the thread-local storage
 * template.  This is the Initial-Exec TLS model. */
static IRValue emit_gaddr(IRFunction *fn, const char *name, SourceLoc loc, int is_tls) {
    IRValue v = new_value(fn);
    IROpcode op = is_tls ? IR_GADDR_TLS : IR_GADDR;
    emit_inst_w(fn, op, v, -1, -1, 0, 8, 1, loc);
    fn->insts.data[fn->insts.len - 1].call_name = xstrdup(name);
    return v;
}

/* IR symbol table: variable name → slot (an IRValue, i.e. a stack slot). */
typedef struct {
    const char *name;
    const char *global_name; /* non-NULL only for static locals: the mangled
                              * global (fn.name) that backs this variable.
                              * emit_gaddr uses this instead of `name`. */
    IRValue slot;         /* alloca-value id (locals) or -1 (globals) */
    int pinned;
    int width;
    int is_unsigned;
    int is_global;        /* 1 = refers to a module-level global */
    int is_tls;           /* 1 = this global is __thread/_Thread_local;
                           * lower_lvalue_addr emits IR_GADDR_TLS instead of
                           * IR_GADDR so codegen routes to %fs:[TPOFF64] */
    int is_vla;           /* 1 = variable-length array with base ptr in slot */
    IRValue vla_nbytes_slot; /* alloca holding the frozen byte size of a VLA object; -1 if not */
    Type ty;
} IRSlot;

typedef struct {
    IRSlot *data;
    size_t len;
    size_t cap;
} IRSymTable;

static void irsymtable_init(IRSymTable *st) {
    st->data = NULL;
    st->len = 0;
    st->cap = 0;
}

static void irsymtable_free(IRSymTable *st) {
    free(st->data);
    st->data = NULL;
    st->len = 0;
    st->cap = 0;
}

static void irsymtable_push(IRSymTable *st, const char *name, IRValue slot,
                            int pinned, Type ty) {
    if (st->len >= st->cap) {
        size_t new_cap = st->cap ? st->cap * 2 : 8;
        st->data = realloc(st->data, new_cap * sizeof(IRSlot));
        if (!st->data) {
            fprintf(stderr, "fakecc: out of memory\n");
            exit(1);
        }
        st->cap = new_cap;
    }
    st->data[st->len].name = name;
    st->data[st->len].global_name = NULL;
    st->data[st->len].slot = slot;
    st->data[st->len].pinned = pinned;
    st->data[st->len].width = ty.kind == TY_ARRAY
        ? type_size(*ty.elem_type)
        : (ty.kind == TY_PTR ? 8 : (ty.width ? ty.width : 4));
    st->data[st->len].is_unsigned = ty.is_unsigned;
    st->data[st->len].is_global = 0;
    st->data[st->len].is_tls = 0;
    st->data[st->len].is_vla = 0;
    st->data[st->len].vla_nbytes_slot = -1;
    st->data[st->len].ty = ty;
    st->len++;
}

static void irsymtable_push_vla(IRSymTable *st, const char *name, IRValue slot, Type ty) {
    irsymtable_push(st, name, slot, 1, ty);
    st->data[st->len - 1].is_vla = 1;
}

static void ir_dbg_fill_struct(IRDebugVar *dv, Type ty) {
    dv->struct_tag = NULL;
    dv->struct_size = 0;
    dv->members = NULL;
    dv->num_members = 0;
    const char *tag = NULL;
    if (ty.kind == TY_STRUCT && ty.tag) tag = ty.tag;
    else if (ty.kind == TY_PTR && ty.pointee && ty.pointee->kind == TY_STRUCT
             && ty.pointee->tag)
        tag = ty.pointee->tag;
    if (!tag) return;
    dv->struct_tag = xstrdup(tag);
    const StructDef *sd = struct_registry_find_c(g_ir_structs, tag);
    if (!sd) return;
    dv->struct_size = sd->size > 0 ? sd->size : (ty.kind == TY_STRUCT ? type_size(ty) : 0);
    if (sd->num_members <= 0) return;
    dv->members = xmalloc((size_t)sd->num_members * sizeof(IRDebugMember));
    dv->num_members = sd->num_members;
    for (int i = 0; i < sd->num_members; i++) {
        const StructMember *sm = &sd->members[i];
        IRDebugMember *m = &dv->members[i];
        memset(m, 0, sizeof(*m));
        m->name = sm->name ? xstrdup(sm->name) : xstrdup("");
        m->offset = sm->offset;
        m->bit_width = sm->bit_width;
        m->bit_offset = sm->bit_offset;
        m->type_kind = (int)sm->type.kind;
        m->width = sm->type.kind == TY_PTR ? 8
                 : (sm->type.kind == TY_ARRAY ? type_size(sm->type)
                    : (sm->type.kind == TY_STRUCT ? type_size(sm->type)
                       : (sm->type.width ? sm->type.width : 4)));
        m->is_unsigned = sm->type.is_unsigned;
        m->is_bool = sm->type.is_bool;
    }
}

static void ir_add_dbg_var(IRFunction *fn, const char *name, SourceLoc loc,
                           IRDebugVarKind kind, Type ty, int alloca_ssa,
                           int param_idx) {
    if (fn->num_dbg_vars >= fn->cap_dbg_vars) {
        size_t nc = fn->cap_dbg_vars ? fn->cap_dbg_vars * 2 : 4;
        fn->dbg_vars = realloc(fn->dbg_vars, nc * sizeof(IRDebugVar));
        if (!fn->dbg_vars) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        fn->cap_dbg_vars = nc;
    }
    IRDebugVar *dv = &fn->dbg_vars[fn->num_dbg_vars++];
    memset(dv, 0, sizeof(*dv));
    dv->name = xstrdup(name);
    dv->loc = loc;
    dv->kind = kind;
    dv->type_kind = (int)ty.kind;
    dv->width = ty.kind == TY_PTR ? 8
              : (ty.kind == TY_ARRAY ? type_size(ty)
                 : (ty.width ? ty.width : 4));
    dv->is_unsigned = ty.is_unsigned;
    dv->is_bool = ty.is_bool;
    dv->array_len = ty.kind == TY_ARRAY ? ty.length : 0;
    dv->alloca_ssa = alloca_ssa;
    dv->param_idx = param_idx;
    ir_dbg_fill_struct(dv, ty);
}

static void irsymtable_push_global(IRSymTable *st, const char *name, Type ty, int is_tls) {
    irsymtable_push(st, name, -1, 1, ty);
    st->data[st->len - 1].is_global = 1;
    st->data[st->len - 1].is_tls = is_tls;
}

/* Push a static local: it is backed by a mangled global `global_name`
 * (fn.name " . " varname) but bound in the symbol table under its C name so
 * ordinary EX_VAR lookups find it.  The global path in lower_expr handles the
 * load/store via the mangled name. */
static void irsymtable_push_static_local(IRSymTable *st, const char *name,
                                         const char *global_name, Type ty) {
    irsymtable_push(st, name, -1, 1, ty);
    st->data[st->len - 1].is_global = 1;
    st->data[st->len - 1].global_name = xstrdup(global_name);
}

/* The global name a global/static-local slot refers to.  For ordinary globals
 * this is the C name; for static locals it is the mangled fn.varname. */
static const char *slot_global_name(const IRSlot *entry) {
    return entry->global_name ? entry->global_name : entry->name;
}

static IRSymTable g_ir_globals_st;

static const IRSlot *irsymtable_find(const IRSymTable *st, const char *name) {
    if (st) {
        for (size_t i = st->len; i > 0; i--)
            if (strcmp(st->data[i-1].name, name) == 0) return &st->data[i-1];
    }
    for (size_t i = g_ir_globals_st.len; i > 0; i--)
        if (strcmp(g_ir_globals_st.data[i-1].name, name) == 0) return &g_ir_globals_st.data[i-1];
    return NULL;
}

/* Forward decl. */
static IRValue lower_expr(IRFunction *fn, IRSymTable *st, const Expr *e);
static int fold_const_float(const Expr *e, long double *out, const IRModule *ir);
static IRValue lower_lvalue_addr(IRFunction *fn, IRSymTable *st, const Expr *e);
static IRValue lower_sizeof_type(IRFunction *fn, IRSymTable *st, Type t, SourceLoc loc);
static void lower_stmt(IRFunction *fn, IRSymTable *st, const Stmt *s,
                       const FunctionDecl *cur_fd);

static const StructMember *ir_find_struct_member(const StructDef *sd, const char *name) {
    if (!sd || !name) return NULL;
    for (int i = 0; i < sd->num_members; i++) {
        if (sd->members[i].name && strcmp(sd->members[i].name, name) == 0)
            return &sd->members[i];
    }
    return NULL;
}

/* GCC Fortify: __builtin_object_size and __builtin___*_chk folding.
 * Matches GCC's compile-time object-size / chk split (type 0/1 unknown →
 * (size_t)-1, type 2/3 unknown → 0; chk with size == -1 or proven n <= size
 * becomes the plain memcpy/memset/...).  Pointer arguments of
 * __builtin_object_size are not evaluated. */

typedef struct {
    int known;
    long long whole_size;
    long long whole_off;
    long long inner_size;
    long long inner_off;
} BosRef;

typedef struct {
    IRSymTable *st;
    const char *vis[24];
    int nvis;
    int bos_type;
} BosCtx;

static const Expr *bos_skip_cast(const Expr *e) {
    while (e && e->kind == EX_CAST)
        e = e->u.cast.operand;
    return e;
}

static int bos_ptr_elem_size(Type t) {
    long long s = 1;
    if (t.kind == TY_ARRAY && t.elem_type)
        s = type_size(*t.elem_type);
    else if (t.kind == TY_PTR && t.pointee)
        s = type_size(*t.pointee);
    if (s <= 0) s = 1;
    return (int)s;
}

static unsigned long long bos_unk(int bos_type) {
    return (bos_type & 2) ? 0ull : ~(unsigned long long)0;
}

static int bos_is_unk(unsigned long long v, int bos_type) {
    return (bos_type & 2) ? 0 : (v == ~(unsigned long long)0);
}

static unsigned long long bos_remaining(BosRef r, int bos_type) {
    if (!r.known) return bos_unk(bos_type);
    long long sz = (bos_type & 1) ? r.inner_size : r.whole_size;
    long long off = (bos_type & 1) ? r.inner_off : r.whole_off;
    if (off < 0) off = 0;
    if (sz <= off) return 0;
    return (unsigned long long)(sz - off);
}

static BosRef bos_ref_unk(void) {
    BosRef r;
    r.known = 0;
    r.whole_size = r.whole_off = r.inner_size = r.inner_off = 0;
    return r;
}

static BosRef bos_ref_make(long long sz, long long off) {
    BosRef r;
    r.known = 1;
    r.whole_size = sz;
    r.whole_off = off;
    r.inner_size = sz;
    r.inner_off = off;
    return r;
}

static int bos_is_param(const char *name) {
    if (!g_ir_cur_fd || !name) return 0;
    for (size_t i = 0; i < g_ir_cur_fd->params.len; i++) {
        if (g_ir_cur_fd->params.data[i].name &&
            strcmp(g_ir_cur_fd->params.data[i].name, name) == 0)
            return 1;
    }
    return 0;
}

static int bos_lvalue_is_name(const Expr *e, const char *name) {
    e = bos_skip_cast(e);
    return e && e->kind == EX_VAR && e->u.var.name && name &&
           strcmp(e->u.var.name, name) == 0;
}

static int bos_is_memcpy_family(const char *n) {
    if (!n) return 0;
    if (strcmp(n, "memcpy") == 0 || strcmp(n, "memmove") == 0 ||
        strcmp(n, "mempcpy") == 0 || strcmp(n, "memset") == 0)
        return 1;
    if (strcmp(n, "__builtin_memcpy") == 0 || strcmp(n, "__builtin_memmove") == 0 ||
        strcmp(n, "__builtin_mempcpy") == 0 || strcmp(n, "__builtin_memset") == 0)
        return 1;
    if (strcmp(n, "__memcpy_chk") == 0 || strcmp(n, "__memmove_chk") == 0 ||
        strcmp(n, "__mempcpy_chk") == 0 || strcmp(n, "__memset_chk") == 0)
        return 1;
    if (strcmp(n, "__builtin___memcpy_chk") == 0 ||
        strcmp(n, "__builtin___memmove_chk") == 0 ||
        strcmp(n, "__builtin___mempcpy_chk") == 0 ||
        strcmp(n, "__builtin___memset_chk") == 0)
        return 1;
    return 0;
}

static int bos_is_alloca(const char *n) {
    return n && (strcmp(n, "__builtin_alloca") == 0 || strcmp(n, "alloca") == 0);
}

static int bos_is_chk_builtin(const char *n) {
    return n && (strcmp(n, "__builtin___memcpy_chk") == 0 ||
                 strcmp(n, "__builtin___memmove_chk") == 0 ||
                 strcmp(n, "__builtin___mempcpy_chk") == 0 ||
                 strcmp(n, "__builtin___memset_chk") == 0 ||
                 strcmp(n, "__builtin___stpncpy_chk") == 0 ||
                 strcmp(n, "__builtin___strncpy_chk") == 0);
}


static int bos_is_snprintf_chk_builtin(const char *n) {
    return n && (strcmp(n, "__builtin___snprintf_chk") == 0 ||
                 strcmp(n, "__builtin___vsnprintf_chk") == 0);
}

static int bos_is_sprintf_chk_builtin(const char *n) {
    return n && (strcmp(n, "__builtin___sprintf_chk") == 0 ||
                 strcmp(n, "__builtin___vsprintf_chk") == 0);
}

static int bos_is_stpcpy_chk_builtin(const char *n) {
    return n && (strcmp(n, "__builtin___stpcpy_chk") == 0 ||
                 strcmp(n, "__builtin___strcpy_chk") == 0);
}

static int bos_is_strcat_chk_builtin(const char *n) {
    return n && strcmp(n, "__builtin___strcat_chk") == 0;
}

static int bos_is_strncat_chk_builtin(const char *n) {
    return n && strcmp(n, "__builtin___strncat_chk") == 0;
}

static const char *bos_chk_plain_name(const char *n) {
    if (strstr(n, "mempcpy")) return "mempcpy";
    if (strstr(n, "memcpy")) return "memcpy";
    if (strstr(n, "memmove")) return "memmove";
    if (strstr(n, "memset")) return "memset";
    if (strstr(n, "stpncpy")) return "stpncpy";
    if (strstr(n, "strncpy")) return "strncpy";
    return "memcpy";
}

static const char *bos_chk_rt_name(const char *n) {
    if (strstr(n, "mempcpy")) return "__mempcpy_chk";
    if (strstr(n, "memcpy")) return "__memcpy_chk";
    if (strstr(n, "memmove")) return "__memmove_chk";
    if (strstr(n, "memset")) return "__memset_chk";
    if (strstr(n, "stpncpy")) return "__stpncpy_chk";
    if (strstr(n, "strncpy")) return "__strncpy_chk";
    return "__memcpy_chk";
}

static void bos_collect_expr(const Expr *e, const char *name,
                             const Expr **out, int *n, int cap, int *saw_unk);
static void bos_collect_stmt(const Stmt *s, const char *name,
                             const Expr **out, int *n, int cap, int *saw_unk);

static void bos_collect_expr(const Expr *e, const char *name,
                             const Expr **out, int *n, int cap, int *saw_unk) {
    if (!e) return;
    if (e->kind == EX_ASSIGN && bos_lvalue_is_name(e->u.assign.lvalue, name)) {
        if (*n < cap) out[(*n)++] = e->u.assign.rvalue;
        else *saw_unk = 1;
        bos_collect_expr(e->u.assign.lvalue, name, out, n, cap, saw_unk);
        bos_collect_expr(e->u.assign.rvalue, name, out, n, cap, saw_unk);
        return;
    }
    if (e->kind == EX_COMPOUND_ASSIGN && bos_lvalue_is_name(e->u.comp.lvalue, name)) {
        *saw_unk = 1;
    }
    switch (e->kind) {
    case EX_BINOP:
        bos_collect_expr(e->u.bin.l, name, out, n, cap, saw_unk);
        bos_collect_expr(e->u.bin.r, name, out, n, cap, saw_unk);
        break;
    case EX_UNARY:
        bos_collect_expr(e->u.un.operand, name, out, n, cap, saw_unk);
        break;
    case EX_ASSIGN:
        bos_collect_expr(e->u.assign.lvalue, name, out, n, cap, saw_unk);
        bos_collect_expr(e->u.assign.rvalue, name, out, n, cap, saw_unk);
        break;
    case EX_CALL:
        bos_collect_expr(e->u.call.callee, name, out, n, cap, saw_unk);
        for (size_t i = 0; i < e->u.call.args.len; i++)
            bos_collect_expr(e->u.call.args.data[i], name, out, n, cap, saw_unk);
        break;
    case EX_ADDR:
        bos_collect_expr(e->u.addr.operand, name, out, n, cap, saw_unk);
        break;
    case EX_DEREF:
        bos_collect_expr(e->u.deref.operand, name, out, n, cap, saw_unk);
        break;
    case EX_INDEX:
        bos_collect_expr(e->u.idx.array, name, out, n, cap, saw_unk);
        bos_collect_expr(e->u.idx.index, name, out, n, cap, saw_unk);
        break;
    case EX_MEMBER:
        bos_collect_expr(e->u.member.obj, name, out, n, cap, saw_unk);
        break;
    case EX_CAST:
        bos_collect_expr(e->u.cast.operand, name, out, n, cap, saw_unk);
        break;
    case EX_TERNARY:
        bos_collect_expr(e->u.tern.cond, name, out, n, cap, saw_unk);
        bos_collect_expr(e->u.tern.then, name, out, n, cap, saw_unk);
        bos_collect_expr(e->u.tern.else_, name, out, n, cap, saw_unk);
        break;
    case EX_INC_DEC:
        bos_collect_expr(e->u.incdec.operand, name, out, n, cap, saw_unk);
        break;
    case EX_COMPOUND_ASSIGN:
        bos_collect_expr(e->u.comp.lvalue, name, out, n, cap, saw_unk);
        bos_collect_expr(e->u.comp.rvalue, name, out, n, cap, saw_unk);
        break;
    case EX_COMMA:
        bos_collect_expr(e->u.comma.lhs, name, out, n, cap, saw_unk);
        bos_collect_expr(e->u.comma.rhs, name, out, n, cap, saw_unk);
        break;
    case EX_STMT_EXPR:
        if (e->u.stmt_expr.stmts) {
            for (size_t i = 0; i < e->u.stmt_expr.stmts->len; i++)
                bos_collect_stmt(&e->u.stmt_expr.stmts->data[i], name, out, n, cap, saw_unk);
        }
        break;
    case EX_COMPOUND_LITERAL:
        bos_collect_expr(e->u.compound.init, name, out, n, cap, saw_unk);
        break;
    default:
        break;
    }
}

static void bos_collect_stmt(const Stmt *s, const char *name,
                             const Expr **out, int *n, int cap, int *saw_unk) {
    if (!s) return;
    switch (s->kind) {
    case ST_DECL:
        if (s->u.decl.name && name && strcmp(s->u.decl.name, name) == 0 && s->u.decl.init) {
            if (*n < cap) out[(*n)++] = s->u.decl.init;
            else *saw_unk = 1;
        }
        bos_collect_expr(s->u.decl.init, name, out, n, cap, saw_unk);
        break;
    case ST_EXPR:
        bos_collect_expr(s->u.expr, name, out, n, cap, saw_unk);
        break;
    case ST_RETURN:
        bos_collect_expr(s->u.value, name, out, n, cap, saw_unk);
        break;
    case ST_IF:
        bos_collect_expr(s->u.if_s.cond, name, out, n, cap, saw_unk);
        bos_collect_stmt(s->u.if_s.then_s, name, out, n, cap, saw_unk);
        bos_collect_stmt(s->u.if_s.else_s, name, out, n, cap, saw_unk);
        break;
    case ST_WHILE:
        bos_collect_expr(s->u.while_s.cond, name, out, n, cap, saw_unk);
        bos_collect_stmt(s->u.while_s.body, name, out, n, cap, saw_unk);
        break;
    case ST_DO_WHILE:
        bos_collect_expr(s->u.do_s.cond, name, out, n, cap, saw_unk);
        bos_collect_stmt(s->u.do_s.body, name, out, n, cap, saw_unk);
        break;
    case ST_FOR:
        bos_collect_stmt(s->u.for_s.init, name, out, n, cap, saw_unk);
        bos_collect_expr(s->u.for_s.cond, name, out, n, cap, saw_unk);
        bos_collect_expr(s->u.for_s.step, name, out, n, cap, saw_unk);
        bos_collect_stmt(s->u.for_s.body, name, out, n, cap, saw_unk);
        break;
    case ST_LABEL:
        bos_collect_stmt(s->u.label_s.stmt, name, out, n, cap, saw_unk);
        break;
    case ST_SWITCH:
        bos_collect_expr(s->u.switch_s.cond, name, out, n, cap, saw_unk);
        bos_collect_stmt(s->u.switch_s.body, name, out, n, cap, saw_unk);
        for (int i = 0; i < s->u.switch_s.num_cases; i++)
            for (size_t j = 0; j < s->u.switch_s.cases[i].stmts.len; j++)
                bos_collect_stmt(&s->u.switch_s.cases[i].stmts.data[j], name, out, n, cap, saw_unk);
        break;
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.len; i++)
            bos_collect_stmt(&s->u.block.data[i], name, out, n, cap, saw_unk);
        break;
    default:
        break;
    }
}


static int bos_stmt_declares(const Stmt *s, const char *name) {
    if (!s || !name) return 0;
    switch (s->kind) {
    case ST_DECL:
        if (s->u.decl.name && strcmp(s->u.decl.name, name) == 0)
            return 1;
        return 0;
    case ST_IF:
        return bos_stmt_declares(s->u.if_s.then_s, name)
            || bos_stmt_declares(s->u.if_s.else_s, name);
    case ST_WHILE:
        return bos_stmt_declares(s->u.while_s.body, name);
    case ST_DO_WHILE:
        return bos_stmt_declares(s->u.do_s.body, name);
    case ST_FOR:
        return bos_stmt_declares(s->u.for_s.init, name)
            || bos_stmt_declares(s->u.for_s.body, name);
    case ST_LABEL:
        return bos_stmt_declares(s->u.label_s.stmt, name);
    case ST_SWITCH: {
        if (bos_stmt_declares(s->u.switch_s.body, name))
            return 1;
        for (int i = 0; i < s->u.switch_s.num_cases; i++)
            for (size_t j = 0; j < s->u.switch_s.cases[i].stmts.len; j++)
                if (bos_stmt_declares(&s->u.switch_s.cases[i].stmts.data[j], name))
                    return 1;
        return 0;
    }
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.len; i++)
            if (bos_stmt_declares(&s->u.block.data[i], name))
                return 1;
        return 0;
    default:
        return 0;
    }
}

static int bos_cur_fn_declares(const char *name) {
    if (!g_ir_cur_fd || !name) return 0;
    for (size_t i = 0; i < g_ir_cur_fd->body.len; i++)
        if (bos_stmt_declares(&g_ir_cur_fd->body.data[i], name))
            return 1;
    return 0;
}

static int bos_collect_sources(const char *name, const Expr **out, int cap, int *saw_unk) {
    int n = 0;
    *saw_unk = 0;
    int is_local = bos_cur_fn_declares(name);
    if (g_ir_cur_fd) {
        for (size_t i = 0; i < g_ir_cur_fd->body.len; i++)
            bos_collect_stmt(&g_ir_cur_fd->body.data[i], name, out, &n, cap, saw_unk);
    }
    if (!is_local && g_ir_tu) {
        for (size_t i = 0; i < g_ir_tu->globals.len; i++) {
            const Stmt *gs = &g_ir_tu->globals.data[i];
            if (gs->kind == ST_DECL && gs->u.decl.name && name &&
                strcmp(gs->u.decl.name, name) == 0 && gs->u.decl.init) {
                if (n < cap) out[n++] = gs->u.decl.init;
                else *saw_unk = 1;
            }
        }
    }
    return n;
}

static unsigned long long bos_eval(BosCtx *ctx, const Expr *e);
static BosRef bos_objref(BosCtx *ctx, const Expr *e);

static int bos_vis_push(BosCtx *ctx, const char *name) {
    for (int i = 0; i < ctx->nvis; i++)
        if (ctx->vis[i] && name && strcmp(ctx->vis[i], name) == 0)
            return 0;
    if (ctx->nvis >= 24) return 0;
    ctx->vis[ctx->nvis++] = name;
    return 1;
}

static void bos_vis_pop(BosCtx *ctx) {
    if (ctx->nvis > 0) ctx->nvis--;
}

static BosRef bos_objref(BosCtx *ctx, const Expr *e) {
    e = bos_skip_cast(e);
    if (!e) return bos_ref_unk();
    if (e->kind == EX_COMMA)
        return bos_objref(ctx, e->u.comma.rhs);
    if (e->kind == EX_ADDR)
        return bos_objref(ctx, e->u.addr.operand);
    if (e->kind == EX_STR)
        return bos_ref_make((long long)e->u.str.len + 1, 0);
    if (e->kind == EX_COMPOUND_LITERAL) {
        long long sz = type_size(e->u.compound.target_type);
        if (sz <= 0) sz = type_size(e->type);
        if (sz <= 0) return bos_ref_unk();
        return bos_ref_make(sz, 0);
    }
    if (e->kind == EX_CALL && e->u.call.callee && e->u.call.callee->kind == EX_VAR) {
        const char *cn = e->u.call.callee->u.var.name;
        if (bos_is_alloca(cn) && e->u.call.args.len > 0) {
            long long sz = 0;
            if (!fold_const_int(e->u.call.args.data[0], &sz) || sz < 0)
                return bos_ref_unk();
            return bos_ref_make(sz, 0);
        }
        if (bos_is_memcpy_family(cn) && e->u.call.args.len > 0) {
            unsigned long long rem = bos_eval(ctx, e->u.call.args.data[0]);
            if (bos_is_unk(rem, ctx->bos_type)) return bos_ref_unk();
            return bos_ref_make((long long)rem, 0);
        }
        return bos_ref_unk();
    }
    if (e->kind == EX_MEMBER) {
        BosRef base = bos_objref(ctx, e->u.member.obj);
        if (!base.known) return bos_ref_unk();
        Type ot = e->u.member.obj->type;
        if (ot.kind == TY_PTR && ot.pointee) ot = *ot.pointee;
        if (ot.kind != TY_STRUCT || !ot.tag) return bos_ref_unk();
        const StructDef *sd = g_ir_structs ? struct_registry_find_c(g_ir_structs, ot.tag) : NULL;
        const StructMember *sm = ir_find_struct_member(sd, e->u.member.name);
        if (!sm) return bos_ref_unk();
        long long msz = type_size(sm->type);
        if (msz < 0) return bos_ref_unk();
        base.whole_off += sm->offset;
        base.inner_size = msz;
        base.inner_off = 0;
        return base;
    }
    if (e->kind == EX_INDEX) {
        BosRef base = bos_objref(ctx, e->u.idx.array);
        if (!base.known) return bos_ref_unk();
        long long idx = 0;
        if (!fold_const_int(e->u.idx.index, &idx)) return bos_ref_unk();
        int esz = bos_ptr_elem_size(e->u.idx.array->type);
        long long delta = idx * (long long)esz;
        base.whole_off += delta;
        base.inner_off += delta;
        return base;
    }
    if (e->kind == EX_BINOP && (e->u.bin.op == BOP_ADD || e->u.bin.op == BOP_SUB)) {
        const Expr *ptr = NULL, *idxe = NULL;
        Type pty;
        int lptr = type_is_ptr_or_array(e->u.bin.l->type);
        int rptr = type_is_ptr_or_array(e->u.bin.r->type);
        if (lptr && !rptr) { ptr = e->u.bin.l; idxe = e->u.bin.r; pty = e->u.bin.l->type; }
        else if (rptr && !lptr && e->u.bin.op == BOP_ADD) {
            ptr = e->u.bin.r; idxe = e->u.bin.l; pty = e->u.bin.r->type;
        } else
            return bos_ref_unk();
        BosRef base = bos_objref(ctx, ptr);
        if (!base.known) return bos_ref_unk();
        long long idx = 0;
        if (!fold_const_int(idxe, &idx)) return bos_ref_unk();
        int esz = bos_ptr_elem_size(pty);
        long long delta = idx * (long long)esz;
        if (e->u.bin.op == BOP_SUB) delta = -delta;
        base.whole_off += delta;
        base.inner_off += delta;
        return base;
    }
    if (e->kind == EX_VAR && e->u.var.name) {
        if (bos_is_param(e->u.var.name)) return bos_ref_unk();
        const IRSlot *slot = irsymtable_find(ctx->st, e->u.var.name);
        if (!slot) return bos_ref_unk();
        if (slot->ty.kind == TY_ARRAY || slot->ty.kind == TY_STRUCT) {
            long long sz = type_size(slot->ty);
            if (sz <= 0) return bos_ref_unk();
            return bos_ref_make(sz, 0);
        }
        if (slot->ty.kind == TY_PTR) {
            if (!bos_vis_push(ctx, e->u.var.name)) return bos_ref_unk();
            const Expr *srcs[32];
            int saw_unk = 0;
            int ns = bos_collect_sources(e->u.var.name, srcs, 32, &saw_unk);
            unsigned long long acc = 0;
            int any = 0;
            if (!saw_unk) {
                for (int i = 0; i < ns; i++) {
                    unsigned long long v = bos_eval(ctx, srcs[i]);
                    if (!any) { acc = v; any = 1; }
                    else if (bos_is_unk(acc, ctx->bos_type) || bos_is_unk(v, ctx->bos_type)) {
                        acc = bos_unk(ctx->bos_type);
                    } else if (ctx->bos_type & 2) {
                        if (v < acc) acc = v;
                    } else {
                        if (v > acc) acc = v;
                    }
                }
            }
            bos_vis_pop(ctx);
            if (saw_unk || !any || bos_is_unk(acc, ctx->bos_type))
                return bos_ref_unk();
            return bos_ref_make((long long)acc, 0);
        }
        return bos_ref_unk();
    }
    return bos_ref_unk();
}

static unsigned long long bos_eval(BosCtx *ctx, const Expr *e) {
    e = bos_skip_cast(e);
    if (!e) return bos_unk(ctx->bos_type);
    if (e->kind == EX_COMMA)
        return bos_eval(ctx, e->u.comma.rhs);
    if (e->kind == EX_TERNARY) {
        unsigned long long a = bos_eval(ctx, e->u.tern.then);
        unsigned long long b = bos_eval(ctx, e->u.tern.else_);
        if (bos_is_unk(a, ctx->bos_type) || bos_is_unk(b, ctx->bos_type))
            return bos_unk(ctx->bos_type);
        if (ctx->bos_type & 2)
            return a < b ? a : b;
        return a > b ? a : b;
    }
    if (e->kind == EX_CALL && e->u.call.callee && e->u.call.callee->kind == EX_VAR) {
        const char *cn = e->u.call.callee->u.var.name;
        if (bos_is_memcpy_family(cn) && e->u.call.args.len > 0)
            return bos_eval(ctx, e->u.call.args.data[0]);
    }
    BosRef r = bos_objref(ctx, e);
    return bos_remaining(r, ctx->bos_type);
}

static unsigned long long compute_builtin_object_size(IRSymTable *st, const Expr *ptr, int bos_type) {
    BosCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.st = st;
    ctx.bos_type = bos_type;
    return bos_eval(&ctx, ptr);
}

static int bos_int_max(IRSymTable *st, const Expr *e, unsigned long long *out) {
    e = bos_skip_cast(e);
    long long c = 0;
    if (fold_const_int(e, &c)) {
        *out = (unsigned long long)c;
        return 1;
    }
    if (!e || e->kind != EX_VAR || !e->u.var.name) return 0;
    /* File-scope objects can be written in other functions (or via asm), so
     * their initializer is not a proven runtime max.  Only locals. */
    const IRSlot *slot = NULL;
    if (st) {
        for (size_t i = st->len; i > 0; i--)
            if (strcmp(st->data[i-1].name, e->u.var.name) == 0) {
                slot = &st->data[i-1];
                break;
            }
    }
    if (!slot || slot->ty.is_volatile) return 0;
    const Expr *srcs[32];
    int saw_unk = 0;
    int ns = bos_collect_sources(e->u.var.name, srcs, 32, &saw_unk);
    if (saw_unk || ns == 0) return 0;
    unsigned long long mx = 0;
    int any = 0;
    for (int i = 0; i < ns; i++) {
        long long v = 0;
        if (!fold_const_int(srcs[i], &v)) return 0;
        unsigned long long u = (unsigned long long)v;
        if (!any || u > mx) mx = u;
        any = 1;
    }
    if (!any) return 0;
    *out = mx;
    return 1;
}

static IRValue bos_emit_named_call(IRFunction *fn, const char *name,
                                   IRValue *args, int nargs, SourceLoc loc) {
    IRValue dst = new_value(fn);
    IRInst inst;
    memset(&inst, 0, sizeof(inst));
    inst.c = -1;
    inst.d = -1;
    inst.op = IR_CALL;
    inst.dst = dst;
    inst.a = -1;
    inst.b = -1;
    inst.loc = loc;
    inst.call_name = xstrdup(name);
    inst.call_callee = -1;
    ir_call_reserve_args(&inst, nargs);
    for (int i = 0; i < nargs; i++)
        inst.call_args[i] = args[i];
    inst.width = 8;
    inst.is_unsigned = 1;
    ir_inst_array_push(&fn->insts, inst);
    set_value_type(fn, dst, 8, 1);
    return dst;
}

static IRValue lower_fortify_chk_call(IRFunction *fn, IRSymTable *st, const Expr *e) {
    if (fakecc_had_error()) return FAKECC_ERR;
    const char *cn = e->u.call.callee->u.var.name;
    const Expr *size_e = e->u.call.args.data[3];
    unsigned long long size_val = ~(unsigned long long)0;
    int size_const = 0;
    if (size_e && size_e->kind == EX_CALL && size_e->u.call.callee &&
        size_e->u.call.callee->kind == EX_VAR &&
        strcmp(size_e->u.call.callee->u.var.name, "__builtin_object_size") == 0 &&
        size_e->u.call.args.len >= 1) {
        long long bt = 0;
        if (size_e->u.call.args.len >= 2)
            fold_const_int(size_e->u.call.args.data[1], &bt);
        size_val = compute_builtin_object_size(st, size_e->u.call.args.data[0], (int)bt);
        size_const = 1;
    } else {
        long long sc = 0;
        if (fold_const_int(size_e, &sc)) {
            size_val = (unsigned long long)sc;
            size_const = 1;
        }
    }
    IRValue dst = lower_expr(fn, st, e->u.call.args.data[0]);
    IRValue a1 = lower_expr(fn, st, e->u.call.args.data[1]);
    IRValue a2 = lower_expr(fn, st, e->u.call.args.data[2]);
    int fold_plain = 0;
    if (size_const && size_val == ~(unsigned long long)0)
        fold_plain = 1;
    else if (size_const) {
        long long nconst = 0;
        unsigned long long nmax = 0;
        if (fold_const_int(e->u.call.args.data[2], &nconst)) {
            if ((unsigned long long)nconst <= size_val)
                fold_plain = 1;
        } else if (bos_int_max(st, e->u.call.args.data[2], &nmax) && nmax <= size_val) {
            fold_plain = 1;
        }
    }
    if (fold_plain) {
        IRValue args[3];
        args[0] = dst;
        args[1] = a1;
        args[2] = a2;
        return bos_emit_named_call(fn, bos_chk_plain_name(cn), args, 3, e->loc);
    }
    IRValue szv = new_value(fn);
    if (size_const) {
        emit_inst_w(fn, IR_CONST, szv, -1, -1, (int64_t)size_val, 8, 1, e->loc);
        set_value_type(fn, szv, 8, 1);
    } else {
        szv = lower_expr(fn, st, size_e);
    }
    IRValue args[4];
    args[0] = dst;
    args[1] = a1;
    args[2] = a2;
    args[3] = szv;
    return bos_emit_named_call(fn, bos_chk_rt_name(cn), args, 4, e->loc);
}

/* Lower a va_list forwarded to vsprintf/vsnprintf after a fortify fold.
 * On arm64 Darwin va_list is an 8-byte by-value aggregate that rides one
 * GP register; lower_expr yields its storage address, so load the cursor.
 * On SysV the tag is array-of-1 and decays to a pointer, which lower_expr
 * already provides. */
static IRValue bos_lower_va_list_arg(IRFunction *fn, IRSymTable *st,
                                     const Expr *arg) {
    IRValue v = lower_expr(fn, st, arg);
    if (!abi_sret_uses_gp() && arg->type.kind == TY_STRUCT && arg->type.tag
        && strcmp(arg->type.tag, "__va_list_tag") == 0) {
        IRValue m = new_value(fn);
        emit_inst_w(fn, IR_LOAD_PTR, m, v, -1, 0, 8, 1, arg->loc);
        return m;
    }
    return v;
}

/* va_first >= 0 marks args[va_first..] as anonymous variadic arguments:
 * under fakecc's Darwin convention those ride 8-byte outgoing stack slots
 * (never registers), exactly like the normal EX_CALL variadic path, so the
 * callee's va_start/va_arg see them. */
static IRValue bos_emit_named_call_w(IRFunction *fn, const char *name,
                                     IRValue *args, int nargs, SourceLoc loc,
                                     int width, int is_unsigned, int va_first) {
    IRValue dst = new_value(fn);
    IRInst inst;
    memset(&inst, 0, sizeof(inst));
    inst.c = -1;
    inst.d = -1;
    inst.op = IR_CALL;
    inst.dst = dst;
    inst.a = -1;
    inst.b = -1;
    inst.loc = loc;
    inst.call_name = xstrdup(name);
    inst.call_callee = -1;
    ir_call_reserve_args(&inst, nargs);
    for (int i = 0; i < nargs; i++) {
        IRValue av = args[i];
        if (va_first >= 0 && i >= va_first)
            av = va_float_bits(fn, av, loc);
        inst.call_args[i] = av;
        if (va_first >= 0 && i >= va_first)
            inst.call_arg_on_stack[i] = CALL_ARG_STACK;
    }
    inst.width = width;
    inst.is_unsigned = is_unsigned;
    ir_inst_array_push(&fn->insts, inst);
    set_value_type(fn, dst, width, is_unsigned);
    return dst;
}

static int bos_eval_size_arg(IRSymTable *st, const Expr *size_e,
                             unsigned long long *size_val) {
    *size_val = ~(unsigned long long)0;
    if (!size_e) return 0;
    if (size_e->kind == EX_CALL && size_e->u.call.callee &&
        size_e->u.call.callee->kind == EX_VAR &&
        strcmp(size_e->u.call.callee->u.var.name, "__builtin_object_size") == 0 &&
        size_e->u.call.args.len >= 1) {
        long long bt = 0;
        if (size_e->u.call.args.len >= 2)
            fold_const_int(size_e->u.call.args.data[1], &bt);
        *size_val = compute_builtin_object_size(st, size_e->u.call.args.data[0], (int)bt);
        return 1;
    }
    long long sc = 0;
    if (fold_const_int(size_e, &sc)) {
        *size_val = (unsigned long long)sc;
        return 1;
    }
    return 0;
}

/* __builtin___snprintf_chk(dst, n, flag, size, fmt, ...) and
 * __builtin___vsnprintf_chk(dst, n, flag, size, fmt, ap).
 * Fold to snprintf/vsnprintf when size is unknown (-1) or n is proven
 * <= size. Check vsnprintf before snprintf (substring). */
static IRValue lower_fortify_snprintf_chk_call(IRFunction *fn, IRSymTable *st,
                                               const Expr *e) {
    if (fakecc_had_error()) return FAKECC_ERR;
    const char *cn = e->u.call.callee->u.var.name;
    const Expr *size_e = e->u.call.args.data[3];
    unsigned long long size_val = ~(unsigned long long)0;
    int size_const = bos_eval_size_arg(st, size_e, &size_val);
    int fold_plain = 0;
    if (size_const && size_val == ~(unsigned long long)0)
        fold_plain = 1;
    else if (size_const) {
        long long nconst = 0;
        unsigned long long nmax = 0;
        if (fold_const_int(e->u.call.args.data[1], &nconst)) {
            if ((unsigned long long)nconst <= size_val)
                fold_plain = 1;
        } else if (bos_int_max(st, e->u.call.args.data[1], &nmax) && nmax <= size_val) {
            fold_plain = 1;
        }
    }
    int is_v = cn && strstr(cn, "vsnprintf") != 0;
    const char *plain = is_v ? "vsnprintf" : "snprintf";
    const char *chk = is_v ? "__vsnprintf_chk" : "__snprintf_chk";
    IRValue dst = lower_expr(fn, st, e->u.call.args.data[0]);
    IRValue nval = lower_expr(fn, st, e->u.call.args.data[1]);
    IRValue args[IR_CALL_MAX_ARGS];
    int nargs = 0;
    args[nargs++] = dst;
    args[nargs++] = nval;
    if (fold_plain) {
        for (int i = 4; i < (int)e->u.call.args.len; i++) {
            if (nargs >= IR_CALL_MAX_ARGS) {
                fprintf(stderr, "fakecc: too many snprintf_chk arguments\n");
                exit(1);
            }
            args[nargs++] = (is_v && i == 5)
                ? bos_lower_va_list_arg(fn, st, e->u.call.args.data[i])
                : lower_expr(fn, st, e->u.call.args.data[i]);
        }
        return bos_emit_named_call_w(fn, plain, args, nargs, e->loc, 4, 0,
                                     is_v ? -1 : 3);
    }
    args[nargs++] = lower_expr(fn, st, e->u.call.args.data[2]);
    IRValue szv = new_value(fn);
    if (size_const) {
        emit_inst_w(fn, IR_CONST, szv, -1, -1, (int64_t)size_val, 8, 1, e->loc);
        set_value_type(fn, szv, 8, 1);
    } else {
        szv = lower_expr(fn, st, size_e);
    }
    args[nargs++] = szv;
    for (int i = 4; i < (int)e->u.call.args.len; i++) {
        if (nargs >= IR_CALL_MAX_ARGS) {
            fprintf(stderr, "fakecc: too many snprintf_chk arguments\n");
            exit(1);
        }
        args[nargs++] = (is_v && i == 5)
            ? bos_lower_va_list_arg(fn, st, e->u.call.args.data[i])
            : lower_expr(fn, st, e->u.call.args.data[i]);
    }
    return bos_emit_named_call_w(fn, chk, args, nargs, e->loc, 4, 0,
                                 is_v ? -1 : 5);
}

/* Known C-string bytes (NUL not included in *nbytes) plus a byte offset. */
static int bos_cstr_info(const Expr *e, const char **bytes, int *nbytes, long long *off) {
    e = bos_skip_cast(e);
    if (!e) return 0;
    if (e->kind == EX_COMMA)
        return bos_cstr_info(e->u.comma.rhs, bytes, nbytes, off);
    if (e->kind == EX_STR) {
        *bytes = e->u.str.bytes ? e->u.str.bytes : "";
        *nbytes = e->u.str.len;
        *off = 0;
        return 1;
    }
    if (e->kind == EX_ADDR)
        return bos_cstr_info(e->u.addr.operand, bytes, nbytes, off);
    if (e->kind == EX_INDEX) {
        long long idx = 0;
        if (!fold_const_int(e->u.idx.index, &idx)) return 0;
        if (!bos_cstr_info(e->u.idx.array, bytes, nbytes, off)) return 0;
        int esz = bos_ptr_elem_size(e->u.idx.array->type);
        *off += idx * (long long)esz;
        return 1;
    }
    if (e->kind == EX_BINOP && (e->u.bin.op == BOP_ADD || e->u.bin.op == BOP_SUB)) {
        const Expr *ptr = NULL, *idxe = NULL;
        Type pty;
        int lptr = type_is_ptr_or_array(e->u.bin.l->type);
        int rptr = type_is_ptr_or_array(e->u.bin.r->type);
        if (lptr && !rptr) { ptr = e->u.bin.l; idxe = e->u.bin.r; pty = e->u.bin.l->type; }
        else if (rptr && !lptr && e->u.bin.op == BOP_ADD) {
            ptr = e->u.bin.r; idxe = e->u.bin.l; pty = e->u.bin.r->type;
        } else
            return 0;
        long long idx = 0;
        if (!fold_const_int(idxe, &idx)) return 0;
        if (!bos_cstr_info(ptr, bytes, nbytes, off)) return 0;
        int esz = bos_ptr_elem_size(pty);
        long long delta = idx * (long long)esz;
        if (e->u.bin.op == BOP_SUB) delta = -delta;
        *off += delta;
        return 1;
    }
    if (e->kind == EX_VAR && e->u.var.name) {
        if (e->type.is_volatile) return 0;
        /* Local pointer to a single known C string (e.g. const char *x2 = "").
         * File-scope pointers can be asm-clobbered, so those stay unknown. */
        if (e->type.kind == TY_PTR) {
            if (bos_is_param(e->u.var.name)) return 0;
            /* File-scope pointers can be asm-clobbered. A same-named local
             * (e.g. test1's s2 = "") is a different object. */
            if (!bos_cur_fn_declares(e->u.var.name))
                return 0;
            static int depth;
            if (depth >= 8) return 0;
            const Expr *srcs[32];
            int saw_unk = 0;
            int ns = bos_collect_sources(e->u.var.name, srcs, 32, &saw_unk);
            if (saw_unk || ns == 0) return 0;
            depth++;
            const char *b0 = NULL;
            int n0 = 0;
            long long o0 = 0;
            int ok = 1;
            for (int i = 0; i < ns; i++) {
                const char *b = NULL;
                int n = 0;
                long long o = 0;
                if (!bos_cstr_info(srcs[i], &b, &n, &o)) { ok = 0; break; }
                if (i == 0) { b0 = b; n0 = n; o0 = o; }
                else if (n != n0 || o != o0 || memcmp(b, b0, (size_t)n) != 0) {
                    ok = 0;
                    break;
                }
            }
            depth--;
            if (!ok) return 0;
            *bytes = b0 ? b0 : "";
            *nbytes = n0;
            *off = o0;
            return 1;
        }
        if (!g_ir_tu) return 0;
        for (size_t i = 0; i < g_ir_tu->globals.len; i++) {
            const Stmt *gs = &g_ir_tu->globals.data[i];
            if (gs->kind != ST_DECL || !gs->u.decl.name) continue;
            if (strcmp(gs->u.decl.name, e->u.var.name) != 0) continue;
            if (gs->u.decl.type.kind != TY_ARRAY) return 0;
            if (gs->u.decl.type.is_volatile) return 0;
            const Expr *init = bos_skip_cast(gs->u.decl.init);
            if (!init || init->kind != EX_STR) return 0;
            *bytes = init->u.str.bytes ? init->u.str.bytes : "";
            *nbytes = init->u.str.len;
            *off = 0;
            return 1;
        }
    }
    return 0;
}

static int bos_cstr_len(const Expr *e, unsigned long long *out) {
    const char *b = NULL;
    int n = 0;
    long long off = 0;
    if (!bos_cstr_info(e, &b, &n, &off)) return 0;
    if (off < 0 || off > n) return 0;
    *out = (unsigned long long)(n - off);
    return 1;
}

/* Exact or proven-max C-string length. Locals only for pointer variables
 * (file-scope pointers can be asm-clobbered / reassigned). */
static int bos_cstr_len_max(IRSymTable *st, const Expr *e, unsigned long long *out) {
    e = bos_skip_cast(e);
    if (bos_cstr_len(e, out)) return 1;
    if (!e || e->kind != EX_VAR || !e->u.var.name) return 0;
    if (bos_is_param(e->u.var.name)) return 0;
    const IRSlot *slot = NULL;
    if (st) {
        for (size_t i = st->len; i > 0; i--)
            if (strcmp(st->data[i-1].name, e->u.var.name) == 0) {
                slot = &st->data[i-1];
                break;
            }
    }
    if (!slot || slot->ty.is_volatile) return 0;
    const Expr *srcs[32];
    int saw_unk = 0;
    int ns = bos_collect_sources(e->u.var.name, srcs, 32, &saw_unk);
    if (saw_unk || ns == 0) return 0;
    unsigned long long mx = 0;
    int any = 0;
    for (int i = 0; i < ns; i++) {
        unsigned long long sl = 0;
        if (!bos_cstr_len(srcs[i], &sl)) return 0;
        if (!any || sl > mx) mx = sl;
        any = 1;
    }
    if (!any) return 0;
    *out = mx;
    return 1;
}

/* Exact sprintf output length (excluding NUL) when the format is a known
 * string using only literal text, %% , %s (known C string), and %c. */
static int bos_sprintf_out_len(const Expr *fmt, const Expr **va, int nva,
                               unsigned long long *out) {
    const char *s = NULL;
    int n = 0;
    long long off = 0;
    if (!bos_cstr_info(fmt, &s, &n, &off)) return 0;
    if (off < 0 || off > n) return 0;
    s += off;
    n -= (int)off;
    unsigned long long total = 0;
    int ai = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] != '%') {
            total++;
            continue;
        }
        if (i + 1 >= n) return 0;
        i++;
        if (s[i] == '%') {
            total++;
            continue;
        }
        if (s[i] == 's') {
            if (ai >= nva) return 0;
            unsigned long long sl = 0;
            if (!bos_cstr_len(va[ai], &sl)) return 0;
            total += sl;
            ai++;
            continue;
        }
        if (s[i] == 'c') {
            if (ai >= nva) return 0;
            total += 1;
            ai++;
            continue;
        }
        return 0;
    }
    *out = total;
    return 1;
}

/* __builtin___sprintf_chk(dst, flag, size, fmt, ...) and
 * __builtin___vsprintf_chk(dst, flag, size, fmt, ap).
 * Fold to sprintf/vsprintf when dest size is unknown (-1) or the
 * formatted output (plus NUL) is proven to fit. vsprintf args live in
 * va_list, so output length is only proven for conversion-free formats. */
static IRValue lower_fortify_sprintf_chk_call(IRFunction *fn, IRSymTable *st,
                                              const Expr *e) {
    if (fakecc_had_error()) return FAKECC_ERR;
    const char *cn = e->u.call.callee->u.var.name;
    int is_v = cn && strstr(cn, "vsprintf") != 0;
    const char *plain = is_v ? "vsprintf" : "sprintf";
    const char *chk = is_v ? "__vsprintf_chk" : "__sprintf_chk";
    const Expr *size_e = e->u.call.args.data[2];
    unsigned long long size_val = ~(unsigned long long)0;
    int size_const = bos_eval_size_arg(st, size_e, &size_val);
    int fold_plain = 0;
    if (size_const && size_val == ~(unsigned long long)0)
        fold_plain = 1;
    else if (size_const && e->u.call.args.len >= 4) {
        const Expr **va = NULL;
        int nva = 0;
        if (!is_v) {
            nva = (int)e->u.call.args.len - 4;
            if (nva > 0) va = (const Expr **)&e->u.call.args.data[4];
        }
        unsigned long long olen = 0;
        if (bos_sprintf_out_len(e->u.call.args.data[3], va, nva, &olen)
            && olen < size_val)
            fold_plain = 1;
    }
    IRValue dst = lower_expr(fn, st, e->u.call.args.data[0]);
    IRValue args[IR_CALL_MAX_ARGS];
    int nargs = 0;
    args[nargs++] = dst;
    if (fold_plain) {
        for (int i = 3; i < (int)e->u.call.args.len; i++) {
            if (nargs >= IR_CALL_MAX_ARGS) {
                fprintf(stderr, "fakecc: too many sprintf_chk arguments\n");
                exit(1);
            }
            args[nargs++] = (is_v && i == 4)
                ? bos_lower_va_list_arg(fn, st, e->u.call.args.data[i])
                : lower_expr(fn, st, e->u.call.args.data[i]);
        }
        return bos_emit_named_call_w(fn, plain, args, nargs, e->loc, 4, 0,
                                     is_v ? -1 : 2);
    }
    args[nargs++] = lower_expr(fn, st, e->u.call.args.data[1]);
    IRValue szv = new_value(fn);
    if (size_const) {
        emit_inst_w(fn, IR_CONST, szv, -1, -1, (int64_t)size_val, 8, 1, e->loc);
        set_value_type(fn, szv, 8, 1);
    } else {
        szv = lower_expr(fn, st, size_e);
    }
    args[nargs++] = szv;
    for (int i = 3; i < (int)e->u.call.args.len; i++) {
        if (nargs >= IR_CALL_MAX_ARGS) {
            fprintf(stderr, "fakecc: too many sprintf_chk arguments\n");
            exit(1);
        }
        args[nargs++] = (is_v && i == 4)
            ? bos_lower_va_list_arg(fn, st, e->u.call.args.data[i])
            : lower_expr(fn, st, e->u.call.args.data[i]);
    }
    return bos_emit_named_call_w(fn, chk, args, nargs, e->loc, 4, 0,
                                 is_v ? -1 : 4);
}

/* __builtin___stpcpy_chk / __builtin___strcpy_chk (dst, src, size).
 * Fold to stpcpy/strcpy when dest size is unknown (-1) or strlen(src)
 * is proven < size (NUL needs one extra byte). */
static IRValue lower_fortify_stpcpy_chk_call(IRFunction *fn, IRSymTable *st,
                                             const Expr *e) {
    if (fakecc_had_error()) return FAKECC_ERR;
    const char *cn = e->u.call.callee->u.var.name;
    int is_stp = cn && strstr(cn, "stpcpy") != NULL;
    const Expr *size_e = e->u.call.args.data[2];
    unsigned long long size_val = ~(unsigned long long)0;
    int size_const = bos_eval_size_arg(st, size_e, &size_val);
    int fold_plain = 0;
    if (size_const && size_val == ~(unsigned long long)0)
        fold_plain = 1;
    else if (size_const) {
        unsigned long long slen = 0;
        if (bos_cstr_len_max(st, e->u.call.args.data[1], &slen) && slen < size_val)
            fold_plain = 1;
    }
    IRValue dst = lower_expr(fn, st, e->u.call.args.data[0]);
    IRValue src = lower_expr(fn, st, e->u.call.args.data[1]);
    if (fold_plain) {
        IRValue args[2];
        args[0] = dst;
        args[1] = src;
        return bos_emit_named_call(fn, is_stp ? "stpcpy" : "strcpy", args, 2, e->loc);
    }
    IRValue szv = new_value(fn);
    if (size_const) {
        emit_inst_w(fn, IR_CONST, szv, -1, -1, (int64_t)size_val, 8, 1, e->loc);
        set_value_type(fn, szv, 8, 1);
    } else {
        szv = lower_expr(fn, st, size_e);
    }
    IRValue args[3];
    args[0] = dst;
    args[1] = src;
    args[2] = szv;
    return bos_emit_named_call(fn, is_stp ? "__stpcpy_chk" : "__strcpy_chk", args, 3, e->loc);
}

/* __builtin___strcat_chk(dst, src, size).
 * Fold to strcat when dest size is unknown (-1), the source is a known
 * empty string (GCC treats strcat(d,"") as a no-op), or both string
 * lengths are known and strlen(dst)+strlen(src) < size. */
static IRValue lower_fortify_strcat_chk_call(IRFunction *fn, IRSymTable *st,
                                             const Expr *e) {
    if (fakecc_had_error()) return FAKECC_ERR;
    const Expr *size_e = e->u.call.args.data[2];
    unsigned long long size_val = ~(unsigned long long)0;
    int size_const = bos_eval_size_arg(st, size_e, &size_val);
    int fold_plain = 0;
    unsigned long long slen = 0;
    int src_known = bos_cstr_len_max(st, e->u.call.args.data[1], &slen);
    if (size_const && size_val == ~(unsigned long long)0)
        fold_plain = 1;
    else if (src_known && slen == 0)
        fold_plain = 1;
    else if (size_const && src_known) {
        unsigned long long dlen = 0;
        if (bos_cstr_len_max(st, e->u.call.args.data[0], &dlen)
            && dlen + slen < size_val)
            fold_plain = 1;
    }
    IRValue dst = lower_expr(fn, st, e->u.call.args.data[0]);
    IRValue src = lower_expr(fn, st, e->u.call.args.data[1]);
    if (fold_plain) {
        IRValue args[2];
        args[0] = dst;
        args[1] = src;
        return bos_emit_named_call(fn, "strcat", args, 2, e->loc);
    }
    IRValue szv = new_value(fn);
    if (size_const) {
        emit_inst_w(fn, IR_CONST, szv, -1, -1, (int64_t)size_val, 8, 1, e->loc);
        set_value_type(fn, szv, 8, 1);
    } else {
        szv = lower_expr(fn, st, size_e);
    }
    IRValue args[3];
    args[0] = dst;
    args[1] = src;
    args[2] = szv;
    return bos_emit_named_call(fn, "__strcat_chk", args, 3, e->loc);
}

/* __builtin___strncat_chk(dst, src, n, size).
 * Fold to strncat when dest size is unknown (-1), n is 0, or src is empty.
 * If strlen(src) <= n, GCC rewrites to strcat_chk. Otherwise keep
 * __strncat_chk unless dest+src lengths are proven to fit. */
static IRValue lower_fortify_strncat_chk_call(IRFunction *fn, IRSymTable *st,
                                              const Expr *e) {
    if (fakecc_had_error()) return FAKECC_ERR;
    const Expr *size_e = e->u.call.args.data[3];
    unsigned long long size_val = ~(unsigned long long)0;
    int size_const = bos_eval_size_arg(st, size_e, &size_val);
    unsigned long long slen = 0, nmax = 0, dlen = 0;
    int src_known = bos_cstr_len_max(st, e->u.call.args.data[1], &slen);
    int n_known = bos_int_max(st, e->u.call.args.data[2], &nmax);
    int dst_known = bos_cstr_len_max(st, e->u.call.args.data[0], &dlen);
    int fold_strncat = 0;
    int fold_strcat = 0;
    int emit_strcat_chk = 0;
    if (size_const && size_val == ~(unsigned long long)0)
        fold_strncat = 1;
    else if (n_known && nmax == 0)
        fold_strncat = 1;
    else if (src_known && slen == 0)
        fold_strncat = 1;
    else if (src_known && n_known && slen <= nmax) {
        if (size_const && dst_known && dlen + slen < size_val)
            fold_strcat = 1;
        else
            emit_strcat_chk = 1;
    } else if (size_const && src_known && n_known && dst_known) {
        unsigned long long add = slen < nmax ? slen : nmax;
        if (dlen + add < size_val)
            fold_strncat = 1;
    }
    IRValue dst = lower_expr(fn, st, e->u.call.args.data[0]);
    IRValue src = lower_expr(fn, st, e->u.call.args.data[1]);
    IRValue nv = lower_expr(fn, st, e->u.call.args.data[2]);
    if (fold_strcat) {
        IRValue args[2];
        args[0] = dst;
        args[1] = src;
        return bos_emit_named_call(fn, "strcat", args, 2, e->loc);
    }
    if (fold_strncat) {
        IRValue args[3];
        args[0] = dst;
        args[1] = src;
        args[2] = nv;
        return bos_emit_named_call(fn, "strncat", args, 3, e->loc);
    }
    IRValue szv = new_value(fn);
    if (size_const) {
        emit_inst_w(fn, IR_CONST, szv, -1, -1, (int64_t)size_val, 8, 1, e->loc);
        set_value_type(fn, szv, 8, 1);
    } else {
        szv = lower_expr(fn, st, size_e);
    }
    if (emit_strcat_chk) {
        IRValue args[3];
        args[0] = dst;
        args[1] = src;
        args[2] = szv;
        return bos_emit_named_call(fn, "__strcat_chk", args, 3, e->loc);
    }
    IRValue args[4];
    args[0] = dst;
    args[1] = src;
    args[2] = nv;
    args[3] = szv;
    return bos_emit_named_call(fn, "__strncat_chk", args, 4, e->loc);
}


static void emit_zero_bytes(IRFunction *fn, IRValue base, int total, SourceLoc loc) {
    /* Large objects: one memset is O(1) IR.  Unrolling a 32KB zero-init
     * (gcc.c-torture 20151204.c) produces tens of thousands of stores and
     * times out register allocation on an unoptimized host compiler. */
    if (total > STRUCT_COPY_MEMCPY_THRESHOLD) {
        IRValue z = new_value(fn);
        emit_inst_w(fn, IR_CONST, z, -1, -1, 0, 4, 0, loc);
        IRValue sz_val = new_value(fn);
        emit_inst_w(fn, IR_CONST, sz_val, -1, -1, (int64_t)total, 8, 1, loc);
        IRValue args[3];
        args[0] = base;
        args[1] = z;
        args[2] = sz_val;
        emit_runtime_call_void(fn, "memset", args, 3, loc);
        return;
    }
    int off = 0;
    while (off + 8 <= total) {
        IRValue poff = new_value(fn);
        emit_inst_w(fn, IR_CONST, poff, -1, -1, off, 8, 1, loc);
        IRValue p = emit_bin_w(fn, IR_ADD, base, poff, 8, 1, loc);
        IRValue z = new_value(fn);
        emit_inst_w(fn, IR_CONST, z, -1, -1, 0, 8, 1, loc);
        emit_inst_w(fn, IR_STORE_PTR, -1, p, z, 0, 8, 1, loc);
        off += 8;
    }
    if (off + 4 <= total) {
        IRValue poff = new_value(fn);
        emit_inst_w(fn, IR_CONST, poff, -1, -1, off, 8, 1, loc);
        IRValue p = emit_bin_w(fn, IR_ADD, base, poff, 8, 1, loc);
        IRValue z = new_value(fn);
        emit_inst_w(fn, IR_CONST, z, -1, -1, 0, 4, 1, loc);
        emit_inst_w(fn, IR_STORE_PTR, -1, p, z, 0, 4, 1, loc);
        off += 4;
    }
    if (off + 2 <= total) {
        IRValue poff = new_value(fn);
        emit_inst_w(fn, IR_CONST, poff, -1, -1, off, 8, 1, loc);
        IRValue p = emit_bin_w(fn, IR_ADD, base, poff, 8, 1, loc);
        IRValue z = new_value(fn);
        emit_inst_w(fn, IR_CONST, z, -1, -1, 0, 2, 1, loc);
        emit_inst_w(fn, IR_STORE_PTR, -1, p, z, 0, 2, 1, loc);
        off += 2;
    }
    if (off < total) {
        IRValue poff = new_value(fn);
        emit_inst_w(fn, IR_CONST, poff, -1, -1, off, 8, 1, loc);
        IRValue p = emit_bin_w(fn, IR_ADD, base, poff, 8, 1, loc);
        IRValue z = new_value(fn);
        emit_inst_w(fn, IR_CONST, z, -1, -1, 0, 1, 1, loc);
        emit_inst_w(fn, IR_STORE_PTR, -1, p, z, 0, 1, 1, loc);
        off += 1;
    }
}

/* Initializer-list lowering (defined after ir_generate). */
static void pack_init(const IRModule *ir, const Type *ty, const Expr *e,
                      char *bytes, int sz, const char *ctx, SourceLoc loc,
                      IRGlobal *g);
/* Push the rodata globals queued while packing an initializer. */
static void flush_rodata(IRModule *m);
static void lower_init_list(IRFunction *fn, IRSymTable *st, IRValue base,
                            const Type *ty, const Expr *e, SourceLoc loc);

/* Control-flow helpers used by short-circuit lowering (defined below lower_stmt). */
static int  new_label(IRFunction *fn);
static void emit_label(IRFunction *fn, int label, SourceLoc loc);
static void emit_br(IRFunction *fn, int label, SourceLoc loc);
static void emit_cbr(IRFunction *fn, IRValue cond, int t_label, int f_label,
                     SourceLoc loc);

/* Coerce a value to a target (width, is_unsigned) — emits SEXT/ZEXT/TRUNC
 * as needed. `imm` on the conversion op holds the SOURCE width so codegen
 * knows how to extend/mask. Returns the coerced SSA value id. */
static IRValue coerce(IRFunction *fn, IRValue v, int src_w, int src_u,
                      int dst_w, int dst_u, SourceLoc loc) {
    if (src_w == dst_w && src_u == dst_u) return v;
    IROpcode op;
    if (dst_w > src_w) op = src_u ? IR_ZEXT : IR_SEXT;
    else if (dst_w < src_w) op = IR_TRUNC;
    else op = IR_TRUNC;   /* same width, retag signedness */
    IRValue res = new_value(fn);
    emit_inst_w(fn, op, res, v, -1, src_w /* imm carries src width */,
                dst_w, dst_u, loc);
    return res;
}

/* Convert value `v` between float and int domains (or resize float↔double).
 *   to_float: 1 → convert int→float (SITOFP); 0 → float→int (FPTOSI).
 *   For float↔float, FPEXT (widen) or FPTRUNC (narrow).
 * Returns the (possibly new) value id, with value_is_float set appropriately. */
static IRValue convert_numeric(IRFunction *fn, IRValue v,
                               int src_w, int dst_w, int dst_u,
                               int to_float, SourceLoc loc) {
    int src_float = get_value_is_float(fn, v);
    IRValue res = new_value(fn);
    IROpcode op;
    if (src_float && to_float) {
        /* float → float (width change). */
        op = (dst_w > src_w) ? IR_FPEXT : IR_FPTRUNC;
        emit_inst_f(fn, op, res, v, -1, dst_w, loc);
        set_value_type(fn, res, dst_w, 0);
        set_value_float(fn, res, 1);
        return res;
    }
    if (src_float && !to_float) {
        /* float → int. */
        op = IR_FPTOSI;
        emit_inst_w(fn, op, res, v, -1, src_w, dst_w, dst_u, loc);
        set_value_type(fn, res, dst_w, dst_u);
        set_value_float(fn, res, 0);
        return res;
    }
    /* int → float.  The destination is a float, so is_unsigned is free to
     * carry the SOURCE signedness — codegen needs it because the x86
     * conversions (cvtsi2sd, fild) are signed. */
    op = IR_SITOFP;
    emit_inst_w(fn, op, res, v, -1, src_w, dst_w, get_value_is_unsigned(fn, v),
                loc);
    set_value_type(fn, res, dst_w, 0);
    set_value_float(fn, res, 1);
    return res;
}

/* Map a BinOp (AST) to the matching IROpcode (IR). */
static IROpcode bop_to_ir(BinOp op) {
    switch (op) {
    case BOP_ADD:    return IR_ADD;
    case BOP_SUB:    return IR_SUB;
    case BOP_MUL:    return IR_MUL;
    case BOP_DIV:    return IR_DIV;
    case BOP_MOD:    return IR_MOD;
    case BOP_EQ:     return IR_EQ;
    case BOP_NE:     return IR_NE;
    case BOP_LT:     return IR_LT;
    case BOP_LE:     return IR_LE;
    case BOP_GT:     return IR_GT;
    case BOP_GE:     return IR_GE;
    case BOP_BITAND: return IR_BAND;
    case BOP_BITOR:  return IR_BOR;
    case BOP_BITXOR: return IR_BXOR;
    case BOP_SHL:    return IR_SHL;
    case BOP_SHR:    return IR_SHR;
    default:         return IR_ADD;
    }
}

/* Byte stride of a pointer's pointee.  VLAs use the current bound
 * (lower_sizeof_type); incomplete/void types stride by 1, matching GCC. */
static IRValue emit_pointee_stride(IRFunction *fn, IRSymTable *st, Type pointee,
                                   SourceLoc loc) {
    if (type_is_vla(pointee))
        return lower_sizeof_type(fn, st, pointee, loc);
    long long esize = type_size(pointee);
    if (esize <= 0) esize = 1;
    IRValue v = new_value(fn);
    emit_inst_w(fn, IR_CONST, v, -1, -1, esize, 8, 1, loc);
    return v;
}

/* Prepare the right-hand side of a compound assignment: for pointer += / -=,
 * scale the integer rvalue by sizeof(pointee); otherwise coerce it to the
 * lvalue's (width, is_unsigned). */
static IRValue scale_rhs(IRFunction *fn, IRSymTable *st, IRValue rhs, int is_ptr,
                         Type lv_ty, BinOp op, SourceLoc loc) {
    int rw = get_value_width(fn, rhs), ru = get_value_is_unsigned(fn, rhs);
    if (is_ptr && (op == BOP_ADD || op == BOP_SUB) && lv_ty.pointee) {
        IRValue r8 = coerce(fn, rhs, rw, ru, 8, 1, loc);
        IRValue ev = emit_pointee_stride(fn, st, *lv_ty.pointee, loc);
        return emit_bin_w(fn, IR_MUL, r8, ev, 8, 1, loc);
    }
    int lw = is_ptr ? 8 : (lv_ty.width ? lv_ty.width : 4);
    int lu = is_ptr ? 1 : lv_ty.is_unsigned;
    return coerce(fn, rhs, rw, ru, lw, lu, loc);
}

/* Materialize a compound literal's storage and return its address.
 * Used both as an lvalue (`&(int){42}`, `(struct S){}.x`) and as the
 * backing object whose scalar rvalue is then loaded. */
static IRValue lower_compound_literal_addr(IRFunction *fn, IRSymTable *st,
                                           const Expr *e) {
    if (fakecc_had_error()) return FAKECC_ERR;
    Type target = e->u.compound.target_type;
    int total = type_size(target);
    if (total <= 0) total = 8;
    IRValue slot = emit_alloca(fn, total, 8, 1, e->loc);
    IRValue addr = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, e->loc);
    emit_zero_bytes(fn, addr, total, e->loc);
    lower_init_list(fn, st, addr, &target, e->u.compound.init, e->loc);
    return addr;
}

/* Compute the address of an lvalue expression (EX_VAR/EX_DEREF/EX_INDEX/EX_MEMBER).
 * Returns a pointer-typed SSA value. */
static IRValue lower_lvalue_addr(IRFunction *fn, IRSymTable *st, const Expr *e) {
    if (fakecc_had_error()) return FAKECC_ERR;
    switch (e->kind) {
    case EX_VAR: {
        const IRSlot *entry = irsymtable_find(st, e->u.var.name);
        if (!entry) {
            /* Not a variable in scope: a function name used as an assignment
             * target (`f *= 0`), which nothing has diagnosed yet.  Fail with a
             * diagnostic instead of dereferencing NULL. */
            return die_at(e->loc.file, e->loc.line, e->loc.col,
                          "cannot assign to '%s'", e->u.var.name);
        }
        if (entry->is_global) return emit_gaddr(fn, slot_global_name(entry), e->loc, entry->is_tls);
        if (entry->is_vla) {
            IRValue addr = emit_bin_w(fn, IR_ADDR, entry->slot, -1, 8, 1, e->loc);
            IRValue v = new_value(fn);
            emit_inst_w(fn, IR_LOAD_PTR, v, addr, -1, 0, 8, 1, e->loc);
            return v;
        }
        return emit_bin_w(fn, IR_ADDR, entry->slot, -1, 8, 1, e->loc);
    }
    case EX_DEREF:
        return lower_expr(fn, st, e->u.deref.operand);
    case EX_CALL: {
        /* A struct call returns a pointer to its bytes (the sret slot address,
         * or a caller-allocated slot).  That pointer is the base address for
         * member access — e.g. `make(3,4).a`.  Lower the call to materialize
         * it (emitting the sret setup), then use the result as the lvalue. */
        return lower_expr(fn, st, e);
    }
    case EX_INDEX: {
        IRValue base = lower_expr(fn, st, e->u.idx.array);
        IRValue idx  = lower_expr(fn, st, e->u.idx.index);
        IRValue esize_v = lower_sizeof_type(fn, st, e->type, e->loc);
        int iw = get_value_width(fn, idx), iu = get_value_is_unsigned(fn, idx);
        IRValue idx8 = coerce(fn, idx, iw, iu, 8, 0, e->loc);
        IRValue off = emit_bin_w(fn, IR_MUL, idx8, esize_v, 8, 1, e->loc);
        return emit_bin_w(fn, IR_ADD, base, off, 8, 1, e->loc);
    }
    case EX_MEMBER: {
        IRValue base = lower_lvalue_addr(fn, st, e->u.member.obj);
        /* Resolve the struct tag from the member object's type.  For `p->x`
         * the parser desugars to `(*p).x`, so the object is a deref whose
         * type is TY_STRUCT.  But the type may also be TY_PTR (e.g. when the
         * deref has not been inserted, or for nested member access) — in
         * that case the struct tag lives on the pointee. */
        const char *tag = NULL;
        Type *ot = &e->u.member.obj->type;
        if (ot->kind == TY_STRUCT) tag = ot->tag;
        else if (ot->kind == TY_PTR && ot->pointee && ot->pointee->kind == TY_STRUCT)
            tag = ot->pointee->tag;
        const StructDef *sd = tag ? struct_registry_find_c(g_ir_structs, tag) : NULL;
        long long off = 0;
        if (sd)
            struct_lookup_member(g_ir_structs, sd, e->u.member.name, &off);
        IRValue off_v = new_value(fn);
        emit_inst_w(fn, IR_CONST, off_v, -1, -1, off, 8, 1, e->loc);
        return emit_bin_w(fn, IR_ADD, base, off_v, 8, 1, e->loc);
    }
    case EX_COMPOUND_LITERAL:
        /* A compound literal yields an lvalue: its address is the base for `&`
         * or member access (e.g. `(struct S){1,2}.x`). */
        return lower_compound_literal_addr(fn, st, e);
    default: break;
    }
    return -1;
}

static IRValue lower_expr(IRFunction *fn, IRSymTable *st, const Expr *e);

static IRValue lower_complex_binop(IRFunction *fn, IRSymTable *st, const Expr *e,
                                  Type lt, Type rt, BinOp bop,
                                  IRValue forced_l_addr) {
    if (fakecc_had_error()) return FAKECC_ERR;
    int lt_is_cplx = (lt.kind == TY_STRUCT && lt.tag && strncmp(lt.tag, "__complex_", 10) == 0);
    int rt_is_cplx = (rt.kind == TY_STRUCT && rt.tag && strncmp(rt.tag, "__complex_", 10) == 0);
    
    int l_elem_sz = lt_is_cplx ? type_size(lt) / 2 : (lt.kind == TY_PTR ? 8 : (lt.width ? lt.width : 4));
    int r_elem_sz = rt_is_cplx ? type_size(rt) / 2 : (rt.kind == TY_PTR ? 8 : (rt.width ? rt.width : 4));
    int l_is_float = lt_is_cplx ? ((strstr(lt.tag, "float") || strstr(lt.tag, "double") || strstr(lt.tag, "ldouble")) ? 1 : 0) : (lt.kind == TY_FLOAT);
    int r_is_float = rt_is_cplx ? ((strstr(rt.tag, "float") || strstr(rt.tag, "double") || strstr(rt.tag, "ldouble")) ? 1 : 0) : (rt.kind == TY_FLOAT);
    
    int is_float = (l_is_float || r_is_float);
    int elem_sz = l_elem_sz > r_elem_sz ? l_elem_sz : r_elem_sz;
    if (is_float && elem_sz < 4) elem_sz = 4;
    
    int l_is_unsigned = lt_is_cplx ? (strstr(lt.tag, "unsigned") != NULL) : lt.is_unsigned;
    int r_is_unsigned = rt_is_cplx ? (strstr(rt.tag, "unsigned") != NULL) : rt.is_unsigned;
    
    IRValue lv_addr = -1, rv_addr = -1;
    IRValue l_real, l_imag, r_real, r_imag;
    
    /* Load LHS components */
    if (lt_is_cplx) {
        lv_addr = (forced_l_addr >= 0) ? forced_l_addr
                                       : lower_expr(fn, st, e->u.bin.l);
        l_real = new_value(fn);
        emit_inst_w(fn, IR_LOAD_PTR, l_real, lv_addr, -1, 0, l_elem_sz, l_is_unsigned, e->loc);
        if (l_is_float) set_value_float(fn, l_real, 1);
        
        IRValue off = new_value(fn);
        emit_inst_w(fn, IR_CONST, off, -1, -1, l_elem_sz, 8, 1, e->loc);
        IRValue iaddr = emit_bin_w(fn, IR_ADD, lv_addr, off, 8, 1, e->loc);
        l_imag = new_value(fn);
        emit_inst_w(fn, IR_LOAD_PTR, l_imag, iaddr, -1, 0, l_elem_sz, l_is_unsigned, e->loc);
        if (l_is_float) set_value_float(fn, l_imag, 1);
    } else {
        l_real = lower_expr(fn, st, e->u.bin.l);
        if (l_is_float) {
            l_imag = emit_float_const(fn, l_elem_sz, 0, e->loc);
        } else {
            l_imag = new_value(fn);
            emit_inst_w(fn, IR_CONST, l_imag, -1, -1, 0, l_elem_sz, l_is_unsigned, e->loc);
        }
    }
    
    /* Convert LHS to common (elem_sz, is_float) */
    if (l_is_float != is_float || l_elem_sz != elem_sz) {
        if (l_is_float && !is_float) {
            l_real = convert_numeric(fn, l_real, l_elem_sz, elem_sz, 0, 0, e->loc);
            l_imag = convert_numeric(fn, l_imag, l_elem_sz, elem_sz, 0, 0, e->loc);
        } else if (!l_is_float && is_float) {
            l_real = convert_numeric(fn, l_real, l_elem_sz, elem_sz, 0, 1, e->loc);
            l_imag = convert_numeric(fn, l_imag, l_elem_sz, elem_sz, 0, 1, e->loc);
        } else if (is_float) {
            l_real = convert_numeric(fn, l_real, l_elem_sz, elem_sz, 0, 1, e->loc);
            l_imag = convert_numeric(fn, l_imag, l_elem_sz, elem_sz, 0, 1, e->loc);
        } else {
            l_real = coerce(fn, l_real, l_elem_sz, l_is_unsigned, elem_sz, l_is_unsigned, e->loc);
            l_imag = coerce(fn, l_imag, l_elem_sz, l_is_unsigned, elem_sz, l_is_unsigned, e->loc);
        }
    }
    
    /* Load RHS components */
    if (rt_is_cplx) {
        rv_addr = lower_expr(fn, st, e->u.bin.r);
        r_real = new_value(fn);
        emit_inst_w(fn, IR_LOAD_PTR, r_real, rv_addr, -1, 0, r_elem_sz, r_is_unsigned, e->loc);
        if (r_is_float) set_value_float(fn, r_real, 1);
        
        IRValue off = new_value(fn);
        emit_inst_w(fn, IR_CONST, off, -1, -1, r_elem_sz, 8, 1, e->loc);
        IRValue iaddr = emit_bin_w(fn, IR_ADD, rv_addr, off, 8, 1, e->loc);
        r_imag = new_value(fn);
        emit_inst_w(fn, IR_LOAD_PTR, r_imag, iaddr, -1, 0, r_elem_sz, r_is_unsigned, e->loc);
        if (r_is_float) set_value_float(fn, r_imag, 1);
    } else {
        r_real = lower_expr(fn, st, e->u.bin.r);
        if (r_is_float) {
            r_imag = emit_float_const(fn, r_elem_sz, 0, e->loc);
        } else {
            r_imag = new_value(fn);
            emit_inst_w(fn, IR_CONST, r_imag, -1, -1, 0, r_elem_sz, r_is_unsigned, e->loc);
        }
    }
    
    /* Convert RHS to common (elem_sz, is_float) */
    if (r_is_float != is_float || r_elem_sz != elem_sz) {
        if (r_is_float && !is_float) {
            r_real = convert_numeric(fn, r_real, r_elem_sz, elem_sz, 0, 0, e->loc);
            r_imag = convert_numeric(fn, r_imag, r_elem_sz, elem_sz, 0, 0, e->loc);
        } else if (!r_is_float && is_float) {
            r_real = convert_numeric(fn, r_real, r_elem_sz, elem_sz, 0, 1, e->loc);
            r_imag = convert_numeric(fn, r_imag, r_elem_sz, elem_sz, 0, 1, e->loc);
        } else if (is_float) {
            r_real = convert_numeric(fn, r_real, r_elem_sz, elem_sz, 0, 1, e->loc);
            r_imag = convert_numeric(fn, r_imag, r_elem_sz, elem_sz, 0, 1, e->loc);
        } else {
            r_real = coerce(fn, r_real, r_elem_sz, r_is_unsigned, elem_sz, r_is_unsigned, e->loc);
            r_imag = coerce(fn, r_imag, r_elem_sz, r_is_unsigned, elem_sz, r_is_unsigned, e->loc);
        }
    }
    
    if (bop == BOP_EQ || bop == BOP_NE) {
        if (is_float) {
            int cmp_sub = (bop == BOP_EQ) ? 4 : 5;
            IRValue cmp_r = emit_bin_w(fn, IR_FCMP, l_real, r_real, elem_sz, cmp_sub, e->loc);
            IRValue cmp_i = emit_bin_w(fn, IR_FCMP, l_imag, r_imag, elem_sz, cmp_sub, e->loc);
            if (bop == BOP_EQ) return emit_bin_w(fn, IR_BAND, cmp_r, cmp_i, 4, 0, e->loc);
            else return emit_bin_w(fn, IR_BOR, cmp_r, cmp_i, 4, 0, e->loc);
        } else {
            IROpcode cmp_op = (bop == BOP_EQ ? IR_EQ : IR_NE);
            IRValue cmp_r = emit_bin_w(fn, cmp_op, l_real, r_real, elem_sz, 1, e->loc);
            IRValue cmp_i = emit_bin_w(fn, cmp_op, l_imag, r_imag, elem_sz, 1, e->loc);
            if (bop == BOP_EQ) return emit_bin_w(fn, IR_BAND, cmp_r, cmp_i, 4, 0, e->loc);
            else return emit_bin_w(fn, IR_BOR, cmp_r, cmp_i, 4, 0, e->loc);
        }
    }
    
    /* Arithmetic operations */
    IRValue out_r, out_i;
    IROpcode add_op = is_float ? IR_FADD : IR_ADD;
    IROpcode sub_op = is_float ? IR_FSUB : IR_SUB;
    IROpcode mul_op = is_float ? IR_FMUL : IR_MUL;
    IROpcode div_op = is_float ? IR_FDIV : IR_DIV;
    
    if (bop == BOP_ADD) {
        out_r = emit_bin_w(fn, add_op, l_real, r_real, elem_sz, 1, e->loc);
        out_i = emit_bin_w(fn, add_op, l_imag, r_imag, elem_sz, 1, e->loc);
    } else if (bop == BOP_SUB) {
        out_r = emit_bin_w(fn, sub_op, l_real, r_real, elem_sz, 1, e->loc);
        out_i = emit_bin_w(fn, sub_op, l_imag, r_imag, elem_sz, 1, e->loc);
    } else if (bop == BOP_MUL) {
        /* (ac-bd) + (ad+bc)i.  Float intermediates must be marked so RA
         * homes them in XMM; otherwise the FMUL/FSUB results never land. */
        IRValue r1 = emit_bin_w(fn, mul_op, l_real, r_real, elem_sz, 1, e->loc);
        IRValue r2 = emit_bin_w(fn, mul_op, l_imag, r_imag, elem_sz, 1, e->loc);
        if (is_float) {
            set_value_float(fn, r1, 1);
            set_value_float(fn, r2, 1);
        }
        out_r = emit_bin_w(fn, sub_op, r1, r2, elem_sz, 1, e->loc);
        if (is_float) set_value_float(fn, out_r, 1);

        IRValue i1 = emit_bin_w(fn, mul_op, l_real, r_imag, elem_sz, 1, e->loc);
        IRValue i2 = emit_bin_w(fn, mul_op, l_imag, r_real, elem_sz, 1, e->loc);
        if (is_float) {
            set_value_float(fn, i1, 1);
            set_value_float(fn, i2, 1);
        }
        out_i = emit_bin_w(fn, add_op, i1, i2, elem_sz, 1, e->loc);
    } else if (bop == BOP_DIV) {
        if (is_float && elem_sz <= 8 && type_long_double_width() < 16) {
            /* The platform has no float wider than double (arm64 Darwin's
             * long double is an 8-byte double), so an 80-bit/quad scratch
             * is unavailable.  Call the runtime's compiler-rt-style
             * scaled division, which stays in range for exponent-limit
             * operands.  Results return through two stack slots to avoid
             * a complex struct-return ABI special case. */
            const char *dn = elem_sz == 4 ? "__fakecc_divsc3"
                                          : "__fakecc_divdc3";
            IRValue slot_r = emit_alloca(fn, elem_sz, elem_sz, 0, e->loc);
            IRValue slot_i = emit_alloca(fn, elem_sz, elem_sz, 0, e->loc);
            /* The helpers take the output slots by pointer: materialise the
             * alloca addresses as IR_ADDR values (an alloca dst is only a
             * slot identity for IR_LOAD/STORE, not a pointer SSA). */
            IRValue addr_r = emit_bin_w(fn, IR_ADDR, slot_r, -1, 8, 1, e->loc);
            IRValue addr_i = emit_bin_w(fn, IR_ADDR, slot_i, -1, 8, 1, e->loc);
            IRValue cargs[6] = {
                addr_r, addr_i, l_real, l_imag, r_real, r_imag
            };
            emit_runtime_call_void(fn, dn, cargs, 6, e->loc);
            out_r = new_value(fn);
            emit_inst_w(fn, IR_LOAD, out_r, slot_r, -1, 0, elem_sz, 0, e->loc);
            fn->insts.data[fn->insts.len - 1].is_float = 1;
            set_value_float(fn, out_r, 1);
            out_i = new_value(fn);
            emit_inst_w(fn, IR_LOAD, out_i, slot_i, -1, 0, elem_sz, 0, e->loc);
            fn->insts.data[fn->insts.len - 1].is_float = 1;
            set_value_float(fn, out_i, 1);
        } else if (is_float && elem_sz <= 8) {
            /* Float/double complex division: compute via 80-bit long double (16-byte)
             * to prevent intermediate overflow/underflow in denominator and numerators. */
            IRValue a = convert_numeric(fn, l_real, elem_sz, 16, 0, 1, e->loc);
            IRValue b = convert_numeric(fn, l_imag, elem_sz, 16, 0, 1, e->loc);
            IRValue c = convert_numeric(fn, r_real, elem_sz, 16, 0, 1, e->loc);
            IRValue d = convert_numeric(fn, r_imag, elem_sz, 16, 0, 1, e->loc);
            
            IRValue c2 = emit_bin_w(fn, IR_FMUL, c, c, 16, 0, e->loc);
            set_value_float(fn, c2, 1);
            IRValue d2 = emit_bin_w(fn, IR_FMUL, d, d, 16, 0, e->loc);
            set_value_float(fn, d2, 1);
            IRValue denom = emit_bin_w(fn, IR_FADD, c2, d2, 16, 0, e->loc);
            set_value_float(fn, denom, 1);
            
            IRValue ac = emit_bin_w(fn, IR_FMUL, a, c, 16, 0, e->loc);
            set_value_float(fn, ac, 1);
            IRValue bd = emit_bin_w(fn, IR_FMUL, b, d, 16, 0, e->loc);
            set_value_float(fn, bd, 1);
            IRValue num_r = emit_bin_w(fn, IR_FADD, ac, bd, 16, 0, e->loc);
            set_value_float(fn, num_r, 1);
            IRValue div_r = emit_bin_w(fn, IR_FDIV, num_r, denom, 16, 0, e->loc);
            set_value_float(fn, div_r, 1);
            
            IRValue bc = emit_bin_w(fn, IR_FMUL, b, c, 16, 0, e->loc);
            set_value_float(fn, bc, 1);
            IRValue ad = emit_bin_w(fn, IR_FMUL, a, d, 16, 0, e->loc);
            set_value_float(fn, ad, 1);
            IRValue num_i = emit_bin_w(fn, IR_FSUB, bc, ad, 16, 0, e->loc);
            set_value_float(fn, num_i, 1);
            IRValue div_i = emit_bin_w(fn, IR_FDIV, num_i, denom, 16, 0, e->loc);
            set_value_float(fn, div_i, 1);
            
            out_r = convert_numeric(fn, div_r, 16, elem_sz, 0, 1, e->loc);
            out_i = convert_numeric(fn, div_i, 16, elem_sz, 0, 1, e->loc);
        } else if (is_float) {
            /* 16-byte long double complex division: Smith's algorithm */
            IRValue abs_r = emit_float_abs(fn, r_real, elem_sz, e->loc);
            IRValue abs_i = emit_float_abs(fn, r_imag, elem_sz, e->loc);
            IRValue cond = emit_fcmp(fn, abs_i, abs_r, elem_sz, 1 /* LE */, e->loc);
            
            IRValue slot_r = new_value(fn);
            emit_inst_w(fn, IR_ALLOCA, slot_r, -1, -1, 0, elem_sz, 0, e->loc);
            set_value_float(fn, slot_r, 1);
            IRValue slot_i = new_value(fn);
            emit_inst_w(fn, IR_ALLOCA, slot_i, -1, -1, 0, elem_sz, 0, e->loc);
            set_value_float(fn, slot_i, 1);
            
            int L_then = new_label(fn);
            int L_else = new_label(fn);
            int L_done = new_label(fn);
            
            emit_cbr(fn, cond, L_then, L_else, e->loc);
            
            /* Then: |r_imag| <= |r_real| */
            emit_label(fn, L_then, e->loc);
            {
                IRValue ratio = emit_bin_w(fn, div_op, r_imag, r_real, elem_sz, 1, e->loc);
                set_value_float(fn, ratio, 1);
                IRValue d2 = emit_bin_w(fn, mul_op, ratio, r_imag, elem_sz, 1, e->loc);
                set_value_float(fn, d2, 1);
                IRValue denom = emit_bin_w(fn, add_op, r_real, d2, elem_sz, 1, e->loc);
                set_value_float(fn, denom, 1);
                
                IRValue l_imag_div = emit_bin_w(fn, div_op, l_imag, r_real, elem_sz, 1, e->loc);
                set_value_float(fn, l_imag_div, 1);
                IRValue n_r2 = emit_bin_w(fn, mul_op, l_imag_div, r_imag, elem_sz, 1, e->loc);
                set_value_float(fn, n_r2, 1);
                IRValue num_r = emit_bin_w(fn, add_op, l_real, n_r2, elem_sz, 1, e->loc);
                set_value_float(fn, num_r, 1);
                IRValue res_r = emit_bin_w(fn, div_op, num_r, denom, elem_sz, 1, e->loc);
                set_value_float(fn, res_r, 1);
                emit_inst_w(fn, IR_STORE, -1, slot_r, res_r, 0, elem_sz, 0, e->loc);
                fn->insts.data[fn->insts.len - 1].is_float = 1;
                
                IRValue l_real_div = emit_bin_w(fn, div_op, l_real, r_real, elem_sz, 1, e->loc);
                set_value_float(fn, l_real_div, 1);
                IRValue n_i2 = emit_bin_w(fn, mul_op, l_real_div, r_imag, elem_sz, 1, e->loc);
                set_value_float(fn, n_i2, 1);
                IRValue num_i = emit_bin_w(fn, sub_op, l_imag, n_i2, elem_sz, 1, e->loc);
                set_value_float(fn, num_i, 1);
                IRValue res_i = emit_bin_w(fn, div_op, num_i, denom, elem_sz, 1, e->loc);
                set_value_float(fn, res_i, 1);
                emit_inst_w(fn, IR_STORE, -1, slot_i, res_i, 0, elem_sz, 0, e->loc);
                fn->insts.data[fn->insts.len - 1].is_float = 1;
                
                emit_br(fn, L_done, e->loc);
            }
            
            /* Else: |r_imag| > |r_real| */
            emit_label(fn, L_else, e->loc);
            {
                IRValue ratio = emit_bin_w(fn, div_op, r_real, r_imag, elem_sz, 1, e->loc);
                set_value_float(fn, ratio, 1);
                IRValue d2 = emit_bin_w(fn, mul_op, ratio, r_real, elem_sz, 1, e->loc);
                set_value_float(fn, d2, 1);
                IRValue denom = emit_bin_w(fn, add_op, r_imag, d2, elem_sz, 1, e->loc);
                set_value_float(fn, denom, 1);
                
                IRValue l_real_div = emit_bin_w(fn, div_op, l_real, r_imag, elem_sz, 1, e->loc);
                set_value_float(fn, l_real_div, 1);
                IRValue n_r1 = emit_bin_w(fn, mul_op, l_real_div, r_real, elem_sz, 1, e->loc);
                set_value_float(fn, n_r1, 1);
                IRValue num_r = emit_bin_w(fn, add_op, n_r1, l_imag, elem_sz, 1, e->loc);
                set_value_float(fn, num_r, 1);
                IRValue res_r = emit_bin_w(fn, div_op, num_r, denom, elem_sz, 1, e->loc);
                set_value_float(fn, res_r, 1);
                emit_inst_w(fn, IR_STORE, -1, slot_r, res_r, 0, elem_sz, 0, e->loc);
                fn->insts.data[fn->insts.len - 1].is_float = 1;
                
                IRValue l_imag_div = emit_bin_w(fn, div_op, l_imag, r_imag, elem_sz, 1, e->loc);
                set_value_float(fn, l_imag_div, 1);
                IRValue n_i1 = emit_bin_w(fn, mul_op, l_imag_div, r_real, elem_sz, 1, e->loc);
                set_value_float(fn, n_i1, 1);
                IRValue num_i = emit_bin_w(fn, sub_op, n_i1, l_real, elem_sz, 1, e->loc);
                set_value_float(fn, num_i, 1);
                IRValue res_i = emit_bin_w(fn, div_op, num_i, denom, elem_sz, 1, e->loc);
                set_value_float(fn, res_i, 1);
                emit_inst_w(fn, IR_STORE, -1, slot_i, res_i, 0, elem_sz, 0, e->loc);
                fn->insts.data[fn->insts.len - 1].is_float = 1;
                
                emit_br(fn, L_done, e->loc);
            }
            
            emit_label(fn, L_done, e->loc);
            out_r = new_value(fn);
            emit_inst_w(fn, IR_LOAD, out_r, slot_r, -1, 0, elem_sz, 0, e->loc);
            fn->insts.data[fn->insts.len - 1].is_float = 1;
            set_value_float(fn, out_r, 1);
            
            out_i = new_value(fn);
            emit_inst_w(fn, IR_LOAD, out_i, slot_i, -1, 0, elem_sz, 0, e->loc);
            fn->insts.data[fn->insts.len - 1].is_float = 1;
            set_value_float(fn, out_i, 1);
        } else {
            IRValue d1 = emit_bin_w(fn, mul_op, r_real, r_real, elem_sz, 1, e->loc);
            IRValue d2 = emit_bin_w(fn, mul_op, r_imag, r_imag, elem_sz, 1, e->loc);
            IRValue denom = emit_bin_w(fn, add_op, d1, d2, elem_sz, 1, e->loc);
            
            IRValue n_r1 = emit_bin_w(fn, mul_op, l_real, r_real, elem_sz, 1, e->loc);
            IRValue n_r2 = emit_bin_w(fn, mul_op, l_imag, r_imag, elem_sz, 1, e->loc);
            IRValue num_r = emit_bin_w(fn, add_op, n_r1, n_r2, elem_sz, 1, e->loc);
            out_r = emit_bin_w(fn, div_op, num_r, denom, elem_sz, 1, e->loc);
            
            IRValue n_i1 = emit_bin_w(fn, mul_op, l_imag, r_real, elem_sz, 1, e->loc);
            IRValue n_i2 = emit_bin_w(fn, mul_op, l_real, r_imag, elem_sz, 1, e->loc);
            IRValue num_i = emit_bin_w(fn, sub_op, n_i1, n_i2, elem_sz, 1, e->loc);
            out_i = emit_bin_w(fn, div_op, num_i, denom, elem_sz, 1, e->loc);
        }
    } else {
        out_r = l_real; out_i = l_imag;
    }
    if (is_float) {
        set_value_float(fn, out_r, 1);
        set_value_float(fn, out_i, 1);
    }
    
    /* Allocate stack slot for result struct */
    int total_sz = elem_sz * 2;
    IRValue slot = emit_alloca(fn, total_sz, 8, 1, e->loc);
    IRValue addr = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, e->loc);
    emit_inst_w(fn, IR_STORE_PTR, -1, addr, out_r, 0, elem_sz, 1, e->loc);
    IRValue off = new_value(fn);
    emit_inst_w(fn, IR_CONST, off, -1, -1, elem_sz, 8, 1, e->loc);
    IRValue iaddr = emit_bin_w(fn, IR_ADD, addr, off, 8, 1, e->loc);
    emit_inst_w(fn, IR_STORE_PTR, -1, iaddr, out_i, 0, elem_sz, 1, e->loc);
    return addr;
}

static IRValue lower_vector_binop(IRFunction *fn, IRSymTable *st, const Expr *e,
                                  Type lt, Type rt, BinOp bop) {
    if (fakecc_had_error()) return FAKECC_ERR;
    Type vt = lt.is_vector ? lt : rt;
    int total_sz = type_size(vt);
    int esz = vt.elem_type ? type_size(*vt.elem_type) : 4;
    if (esz <= 0) esz = 4;
    int count = vt.length > 0 ? vt.length : (total_sz / esz);
    int is_float = (vt.elem_type && vt.elem_type->kind == TY_FLOAT);
    int is_unsigned = vt.elem_type ? vt.elem_type->is_unsigned : 0;

    IRValue l_addr = lower_expr(fn, st, e->u.bin.l);
    if (!lt.is_vector) {
        int sw = get_value_width(fn, l_addr);
        int sf = get_value_is_float(fn, l_addr);
        int su = get_value_is_unsigned(fn, l_addr);
        if (esz == 16 && !is_float) {
            l_addr = i128_as_addr(fn, l_addr, lt, is_unsigned, e->loc);
        } else if (sf != is_float) {
            l_addr = convert_numeric(fn, l_addr, sw, esz, is_unsigned, is_float, e->loc);
        } else if (is_float) {
            if (sw != esz) l_addr = convert_numeric(fn, l_addr, sw, esz, is_unsigned, 1, e->loc);
        } else {
            l_addr = coerce(fn, l_addr, sw, su, esz, is_unsigned, e->loc);
        }
        IRValue l_slot = emit_alloca(fn, total_sz, 16, 1, e->loc);
        IRValue l_buf = emit_bin_w(fn, IR_ADDR, l_slot, -1, 8, 1, e->loc);
        for (int i = 0; i < count; i++) {
            IRValue off = emit_add_const(fn, l_buf, i * esz, e->loc);
            if (esz == 16 && !is_float) {
                emit_struct_copy(fn, off, l_addr, 16, e->loc);
            } else {
                emit_inst_w(fn, IR_STORE_PTR, -1, off, l_addr, 0, esz, is_unsigned, e->loc);
                if (is_float) fn->insts.data[fn->insts.len - 1].is_float = 1;
            }
        }
        l_addr = l_buf;
    }

    IRValue r_addr = lower_expr(fn, st, e->u.bin.r);
    if (!rt.is_vector) {
        int sw = get_value_width(fn, r_addr);
        int sf = get_value_is_float(fn, r_addr);
        int su = get_value_is_unsigned(fn, r_addr);
        if (esz == 16 && !is_float) {
            r_addr = i128_as_addr(fn, r_addr, rt, is_unsigned, e->loc);
        } else if (sf != is_float) {
            r_addr = convert_numeric(fn, r_addr, sw, esz, is_unsigned, is_float, e->loc);
        } else if (is_float) {
            if (sw != esz) r_addr = convert_numeric(fn, r_addr, sw, esz, is_unsigned, 1, e->loc);
        } else {
            r_addr = coerce(fn, r_addr, sw, su, esz, is_unsigned, e->loc);
        }
        IRValue r_slot = emit_alloca(fn, total_sz, 16, 1, e->loc);
        IRValue r_buf = emit_bin_w(fn, IR_ADDR, r_slot, -1, 8, 1, e->loc);
        for (int i = 0; i < count; i++) {
            IRValue off = emit_add_const(fn, r_buf, i * esz, e->loc);
            if (esz == 16 && !is_float) {
                emit_struct_copy(fn, off, r_addr, 16, e->loc);
            } else {
                emit_inst_w(fn, IR_STORE_PTR, -1, off, r_addr, 0, esz, is_unsigned, e->loc);
                if (is_float) fn->insts.data[fn->insts.len - 1].is_float = 1;
            }
        }
        r_addr = r_buf;
    }

    int is_cmp = (bop == BOP_EQ || bop == BOP_NE || bop == BOP_LT || bop == BOP_GT || bop == BOP_LE || bop == BOP_GE);
    int is_shift = (bop == BOP_SHL || bop == BOP_SHR);

    int is_int_div = ((bop == BOP_DIV || bop == BOP_MOD) && !is_float);
    if (is_cmp || is_shift || total_sz > 16 || is_int_div) {
        IRValue slot = emit_alloca(fn, total_sz, 16, 1, e->loc);
        IRValue dst_addr = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, e->loc);
        for (int i = 0; i < count; i++) {
            IRValue l_off = emit_add_const(fn, l_addr, i * esz, e->loc);
            IRValue r_off = emit_add_const(fn, r_addr, i * esz, e->loc);
            IRValue dst_off = emit_add_const(fn, dst_addr, i * esz, e->loc);
            if (esz == 16 && !is_float) {
                Type et = type_make_int(16, is_unsigned);
                if (is_cmp) {
                    IRValue cval = lower_i128_binop(fn, l_off, r_off, et, et, bop, type_make_int(4, 0), e->loc);
                    int Lt = new_label(fn), Lf = new_label(fn), Ld = new_label(fn);
                    emit_cbr(fn, cval, Lt, Lf, e->loc);
                    emit_label(fn, Lt, e->loc);
                    i128_store2(fn, dst_off, i64imm(fn, -1, e->loc), i64imm(fn, -1, e->loc), e->loc);
                    emit_br(fn, Ld, e->loc);
                    emit_label(fn, Lf, e->loc);
                    i128_store2(fn, dst_off, i64imm(fn, 0, e->loc), i64imm(fn, 0, e->loc), e->loc);
                    emit_br(fn, Ld, e->loc);
                    emit_label(fn, Ld, e->loc);
                } else {
                    IRValue res = lower_i128_binop(fn, l_off, r_off, et, et, bop, et, e->loc);
                    emit_struct_copy(fn, dst_off, res, 16, e->loc);
                }
                continue;
            }
            IRValue lv = new_value(fn);
            emit_inst_w(fn, IR_LOAD_PTR, lv, l_off, -1, 0, esz, is_unsigned, e->loc);
            if (is_float) set_value_float(fn, lv, 1);
            IRValue rv = new_value(fn);
            emit_inst_w(fn, IR_LOAD_PTR, rv, r_off, -1, 0, esz, is_unsigned, e->loc);
            if (is_float) set_value_float(fn, rv, 1);
            IRValue res;
            if (is_cmp) {
                if (is_float) {
                    static const unsigned char cmp_enc[] = {4,5,0,1,2,3};
                    unsigned char enc = cmp_enc[bop - BOP_EQ];
                    IRValue cval = emit_fcmp(fn, lv, rv, esz, enc, e->loc);
                    IRValue neg_cval = emit_bin_w(fn, IR_NEG, cval, -1, 4, 0, e->loc);
                    res = coerce(fn, neg_cval, 4, 0, esz, is_unsigned, e->loc);
                } else {
                    IROpcode cop;
                    switch (bop) {
                    case BOP_EQ: cop = IR_EQ; break;
                    case BOP_NE: cop = IR_NE; break;
                    case BOP_LT: cop = IR_LT; break;
                    case BOP_LE: cop = IR_LE; break;
                    case BOP_GT: cop = IR_GT; break;
                    case BOP_GE: cop = IR_GE; break;
                    default: cop = IR_EQ; break;
                    }
                    IRValue cval = emit_bin_w(fn, cop, lv, rv, esz, is_unsigned, e->loc);
                    IRValue neg_cval = emit_bin_w(fn, IR_NEG, cval, -1, 4, 0, e->loc);
                    res = coerce(fn, neg_cval, 4, 0, esz, is_unsigned, e->loc);
                }
            } else if (is_shift) {
                IROpcode sop = (bop == BOP_SHL) ? IR_SHL : IR_SHR;
                res = emit_bin_w(fn, sop, lv, rv, esz, is_unsigned, e->loc);
            } else {
                IROpcode aop;
                if (is_float) {
                    switch (bop) {
                    case BOP_ADD: aop = IR_FADD; break;
                    case BOP_SUB: aop = IR_FSUB; break;
                    case BOP_MUL: aop = IR_FMUL; break;
                    case BOP_DIV: aop = IR_FDIV; break;
                    default: aop = IR_FADD; break;
                    }
                } else {
                    switch (bop) {
                    case BOP_ADD: aop = IR_ADD; break;
                    case BOP_SUB: aop = IR_SUB; break;
                    case BOP_MUL: aop = IR_MUL; break;
                    case BOP_DIV: aop = IR_DIV; break;
                    case BOP_MOD: aop = IR_MOD; break;
                    case BOP_BITAND: case BOP_AND: aop = IR_BAND; break;
                    case BOP_BITOR:  case BOP_OR:  aop = IR_BOR; break;
                    case BOP_BITXOR: aop = IR_BXOR; break;
                    default: aop = IR_ADD; break;
                    }
                }
                res = emit_bin_w(fn, aop, lv, rv, esz, is_unsigned, e->loc);
                if (is_float) set_value_float(fn, res, 1);
            }
            emit_inst_w(fn, IR_STORE_PTR, -1, dst_off, res, 0, esz, is_unsigned, e->loc);
            if (is_float) fn->insts.data[fn->insts.len - 1].is_float = 1;
        }
        return dst_addr;
    }

    IROpcode vop = IR_VADD;
    switch (bop) {
    case BOP_ADD: vop = IR_VADD; break;
    case BOP_SUB: vop = IR_VSUB; break;
    case BOP_MUL: vop = IR_VMUL; break;
    case BOP_DIV: vop = IR_VDIV; break;
    case BOP_BITAND: case BOP_AND: vop = IR_VBAND; break;
    case BOP_BITOR:  case BOP_OR:  vop = IR_VBOR; break;
    case BOP_BITXOR: vop = IR_VBXOR; break;
    default: vop = IR_VADD; break;
    }

    IRValue slot = emit_alloca(fn, total_sz, 16, 1, e->loc);
    IRValue dst_addr = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, e->loc);
    emit_inst_w(fn, vop, dst_addr, l_addr, r_addr, esz, total_sz, is_unsigned, e->loc);
    if (is_float) fn->insts.data[fn->insts.len - 1].is_float = 1;
    return dst_addr;
}

static IRValue lower_vector_compound_assign(IRFunction *fn, IRSymTable *st,
                                            const Expr *e) {
    if (fakecc_had_error()) return FAKECC_ERR;
    Expr *lv = e->u.comp.lvalue;
    Type vt = lv->type;
    int total_sz = type_size(vt);
    int esz = vt.elem_type ? type_size(*vt.elem_type) : 4;
    if (esz <= 0) esz = 4;
    int count = vt.length > 0 ? vt.length : (total_sz / esz);
    int is_float = (vt.elem_type && vt.elem_type->kind == TY_FLOAT);
    int is_unsigned = vt.elem_type ? vt.elem_type->is_unsigned : 0;
    BinOp op = e->u.comp.op;

    IRValue addr = lower_lvalue_addr(fn, st, lv);
    IRValue rhs_addr = lower_expr(fn, st, e->u.comp.rvalue);
    if (!e->u.comp.rvalue->type.is_vector) {
        int sw = get_value_width(fn, rhs_addr);
        int sf = get_value_is_float(fn, rhs_addr);
        int su = get_value_is_unsigned(fn, rhs_addr);
        if (esz == 16 && !is_float) {
            rhs_addr = i128_as_addr(fn, rhs_addr, e->u.comp.rvalue->type, is_unsigned, e->loc);
        } else if (sf != is_float) {
            rhs_addr = convert_numeric(fn, rhs_addr, sw, esz, is_unsigned, is_float, e->loc);
        } else if (is_float) {
            if (sw != esz) rhs_addr = convert_numeric(fn, rhs_addr, sw, esz, is_unsigned, 1, e->loc);
        } else {
            rhs_addr = coerce(fn, rhs_addr, sw, su, esz, is_unsigned, e->loc);
        }
        IRValue r_slot = emit_alloca(fn, total_sz, 16, 1, e->loc);
        IRValue r_buf = emit_bin_w(fn, IR_ADDR, r_slot, -1, 8, 1, e->loc);
        for (int i = 0; i < count; i++) {
            IRValue off = emit_add_const(fn, r_buf, i * esz, e->loc);
            if (esz == 16 && !is_float) {
                emit_struct_copy(fn, off, rhs_addr, 16, e->loc);
            } else {
                emit_inst_w(fn, IR_STORE_PTR, -1, off, rhs_addr, 0, esz, is_unsigned, e->loc);
                if (is_float) fn->insts.data[fn->insts.len - 1].is_float = 1;
            }
        }
        rhs_addr = r_buf;
    }

    int is_shift = (op == BOP_SHL || op == BOP_SHR);
    int is_int_div = ((op == BOP_DIV || op == BOP_MOD) && !is_float);
    if (is_shift || total_sz > 16 || is_int_div) {
        for (int i = 0; i < count; i++) {
            IRValue l_off = emit_add_const(fn, addr, i * esz, e->loc);
            IRValue r_off = emit_add_const(fn, rhs_addr, i * esz, e->loc);
            IRValue lval = new_value(fn);
            emit_inst_w(fn, IR_LOAD_PTR, lval, l_off, -1, 0, esz, is_unsigned, e->loc);
            if (is_float) set_value_float(fn, lval, 1);
            IRValue rval = new_value(fn);
            emit_inst_w(fn, IR_LOAD_PTR, rval, r_off, -1, 0, esz, is_unsigned, e->loc);
            if (is_float) set_value_float(fn, rval, 1);
            IRValue res;
            if (is_shift) {
                IROpcode sop = (op == BOP_SHL) ? IR_SHL : IR_SHR;
                res = emit_bin_w(fn, sop, lval, rval, esz, is_unsigned, e->loc);
            } else {
                IROpcode aop;
                if (is_float) {
                    switch (op) {
                    case BOP_ADD: aop = IR_FADD; break;
                    case BOP_SUB: aop = IR_FSUB; break;
                    case BOP_MUL: aop = IR_FMUL; break;
                    case BOP_DIV: aop = IR_FDIV; break;
                    default: aop = IR_FADD; break;
                    }
                } else {
                    switch (op) {
                    case BOP_ADD: aop = IR_ADD; break;
                    case BOP_SUB: aop = IR_SUB; break;
                    case BOP_MUL: aop = IR_MUL; break;
                    case BOP_DIV: aop = IR_DIV; break;
                    case BOP_MOD: aop = IR_MOD; break;
                    case BOP_BITAND: case BOP_AND: aop = IR_BAND; break;
                    case BOP_BITOR:  case BOP_OR:  aop = IR_BOR; break;
                    case BOP_BITXOR: aop = IR_BXOR; break;
                    default: aop = IR_ADD; break;
                    }
                }
                res = emit_bin_w(fn, aop, lval, rval, esz, is_unsigned, e->loc);
                if (is_float) set_value_float(fn, res, 1);
            }
            emit_inst_w(fn, IR_STORE_PTR, -1, l_off, res, 0, esz, is_unsigned, e->loc);
            if (is_float) fn->insts.data[fn->insts.len - 1].is_float = 1;
        }
        return addr;
    }

    IROpcode vop = IR_VADD;
    switch (op) {
    case BOP_ADD: vop = IR_VADD; break;
    case BOP_SUB: vop = IR_VSUB; break;
    case BOP_MUL: vop = IR_VMUL; break;
    case BOP_DIV: vop = IR_VDIV; break;
    case BOP_BITAND: case BOP_AND: vop = IR_VBAND; break;
    case BOP_BITOR:  case BOP_OR:  vop = IR_VBOR; break;
    case BOP_BITXOR: vop = IR_VBXOR; break;
    default: vop = IR_VADD; break;
    }

    emit_inst_w(fn, vop, addr, addr, rhs_addr, esz, total_sz, is_unsigned, e->loc);
    if (is_float) fn->insts.data[fn->insts.len - 1].is_float = 1;
    return addr;
}

static IRValue lower_sizeof_type(IRFunction *fn, IRSymTable *st, Type t, SourceLoc loc) {
    if (fakecc_had_error()) return FAKECC_ERR;
    if (type_is_vla(t) && t.kind == TY_ARRAY && t.elem_type) {
        IRValue elem_sz_val = lower_sizeof_type(fn, st, *t.elem_type, loc);
        if (t.vla_dim) {
            IRValue dim_val = lower_expr(fn, st, t.vla_dim);
            int dim_w = get_value_width(fn, dim_val), dim_u = get_value_is_unsigned(fn, dim_val);
            dim_val = coerce(fn, dim_val, dim_w, dim_u, 8, 1, loc);
            return emit_bin_w(fn, IR_MUL, dim_val, elem_sz_val, 8, 1, loc);
        }
        long long n = t.length > 0 ? t.length : 1;
        IRValue nc = new_value(fn);
        emit_inst_w(fn, IR_CONST, nc, -1, -1, n, 8, 1, loc);
        return emit_bin_w(fn, IR_MUL, nc, elem_sz_val, 8, 1, loc);
    }
    long long sz = type_size(t);
    if (sz < 0) sz = 0;
    IRValue v = new_value(fn);
    emit_inst_w(fn, IR_CONST, v, -1, -1, sz, 8, 1, loc);
    return v;
}

static const Expr *expr_skip_casts(const Expr *e) {
    while (e && e->kind == EX_CAST) e = e->u.cast.operand;
    return e;
}

static int expr_is_va_arg_pack(const Expr *e) {
    e = expr_skip_casts(e);
    return e && e->kind == EX_CALL && e->u.call.callee
        && e->u.call.callee->kind == EX_VAR
        && e->u.call.callee->u.var.name
        && strcmp(e->u.call.callee->u.var.name, "__builtin_va_arg_pack") == 0;
}

static int expr_is_va_arg_pack_len(const Expr *e) {
    e = expr_skip_casts(e);
    return e && e->kind == EX_CALL && e->u.call.callee
        && e->u.call.callee->kind == EX_VAR
        && e->u.call.callee->u.var.name
        && strcmp(e->u.call.callee->u.var.name, "__builtin_va_arg_pack_len") == 0;
}

static int expr_has_va_arg_pack(const Expr *e);
static int stmt_has_va_arg_pack(const Stmt *s);

static int expr_has_va_arg_pack(const Expr *e) {
    if (!e) return 0;
    if (expr_is_va_arg_pack(e) || expr_is_va_arg_pack_len(e)) return 1;
    switch (e->kind) {
    case EX_BINOP:
        return expr_has_va_arg_pack(e->u.bin.l) || expr_has_va_arg_pack(e->u.bin.r);
    case EX_UNARY: return expr_has_va_arg_pack(e->u.un.operand);
    case EX_ASSIGN:
        return expr_has_va_arg_pack(e->u.assign.lvalue)
            || expr_has_va_arg_pack(e->u.assign.rvalue);
    case EX_CALL: {
        if (expr_has_va_arg_pack(e->u.call.callee)) return 1;
        for (size_t i = 0; i < e->u.call.args.len; i++)
            if (expr_has_va_arg_pack(e->u.call.args.data[i])) return 1;
        return 0;
    }
    case EX_ADDR: return expr_has_va_arg_pack(e->u.addr.operand);
    case EX_DEREF: return expr_has_va_arg_pack(e->u.deref.operand);
    case EX_INDEX:
        return expr_has_va_arg_pack(e->u.idx.array) || expr_has_va_arg_pack(e->u.idx.index);
    case EX_MEMBER: return expr_has_va_arg_pack(e->u.member.obj);
    case EX_CAST: return expr_has_va_arg_pack(e->u.cast.operand);
    case EX_SIZEOF_EXPR: return expr_has_va_arg_pack(e->u.sizeof_e.operand);
    case EX_ALIGNOF_EXPR: return expr_has_va_arg_pack(e->u.alignof_e.operand);
    case EX_TERNARY:
        return expr_has_va_arg_pack(e->u.tern.cond)
            || expr_has_va_arg_pack(e->u.tern.then)
            || expr_has_va_arg_pack(e->u.tern.else_);
    case EX_INC_DEC: return expr_has_va_arg_pack(e->u.incdec.operand);
    case EX_COMPOUND_ASSIGN:
        return expr_has_va_arg_pack(e->u.comp.lvalue)
            || expr_has_va_arg_pack(e->u.comp.rvalue);
    case EX_COMMA:
        return expr_has_va_arg_pack(e->u.comma.lhs) || expr_has_va_arg_pack(e->u.comma.rhs);
    case EX_INIT_LIST:
        for (int i = 0; i < e->u.init_list.num_elements; i++)
            if (expr_has_va_arg_pack(e->u.init_list.elements[i])) return 1;
        return 0;
    case EX_COMPOUND_LITERAL: return expr_has_va_arg_pack(e->u.compound.init);
    case EX_STMT_EXPR:
        if (e->u.stmt_expr.stmts) {
            for (size_t i = 0; i < e->u.stmt_expr.stmts->len; i++)
                if (stmt_has_va_arg_pack(&e->u.stmt_expr.stmts->data[i])) return 1;
        }
        return 0;
    default: return 0;
    }
}

static int stmt_has_va_arg_pack(const Stmt *s) {
    if (!s) return 0;
    switch (s->kind) {
    case ST_DECL: return expr_has_va_arg_pack(s->u.decl.init);
    case ST_EXPR: return expr_has_va_arg_pack(s->u.expr);
    case ST_RETURN: return expr_has_va_arg_pack(s->u.value);
    case ST_IF:
        return expr_has_va_arg_pack(s->u.if_s.cond)
            || stmt_has_va_arg_pack(s->u.if_s.then_s)
            || stmt_has_va_arg_pack(s->u.if_s.else_s);
    case ST_WHILE:
        return expr_has_va_arg_pack(s->u.while_s.cond)
            || stmt_has_va_arg_pack(s->u.while_s.body);
    case ST_DO_WHILE:
        return expr_has_va_arg_pack(s->u.do_s.cond)
            || stmt_has_va_arg_pack(s->u.do_s.body);
    case ST_FOR:
        return stmt_has_va_arg_pack(s->u.for_s.init)
            || expr_has_va_arg_pack(s->u.for_s.cond)
            || expr_has_va_arg_pack(s->u.for_s.step)
            || stmt_has_va_arg_pack(s->u.for_s.body);
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.len; i++)
            if (stmt_has_va_arg_pack(&s->u.block.data[i])) return 1;
        return 0;
    case ST_LABEL: return stmt_has_va_arg_pack(s->u.label_s.stmt);
    case ST_GOTO: return expr_has_va_arg_pack(s->u.goto_s.target_expr);
    case ST_SWITCH: {
        if (expr_has_va_arg_pack(s->u.switch_s.cond)) return 1;
        if (stmt_has_va_arg_pack(s->u.switch_s.body)) return 1;
        for (int i = 0; i < s->u.switch_s.num_cases; i++) {
            const SwitchCase *c = &s->u.switch_s.cases[i];
            for (size_t j = 0; j < c->stmts.len; j++)
                if (stmt_has_va_arg_pack(&c->stmts.data[j])) return 1;
        }
        return 0;
    }
    default: return 0;
    }
}

static int fn_uses_va_arg_pack(const FunctionDecl *fd) {
    if (!fd) return 0;
    for (size_t i = 0; i < fd->body.len; i++)
        if (stmt_has_va_arg_pack(&fd->body.data[i])) return 1;
    return 0;
}

static const FunctionDecl *find_fn_with_body(const char *name) {
    const FunctionDecl *found = NULL;
    if (!g_ir_tu || !name) return NULL;
    for (size_t i = 0; i < g_ir_tu->functions.len; i++) {
        const FunctionDecl *fd = &g_ir_tu->functions.data[i];
        if (fd->name && strcmp(fd->name, name) == 0 && fd->body.len > 0)
            found = fd;
    }
    return found;
}

static void vapack_store_value(IRFunction *fn, IRSymTable *st, IRValue slot,
                               Type ty, const Expr *src, SourceLoc loc) {
    int pw = ty.kind == TY_PTR ? 8 : (ty.width ? ty.width : 4);
    int pu = ty.kind == TY_PTR ? 1 : ty.is_unsigned;
    IRValue addr = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, loc);
    if (!src) return;
    IRValue rv = lower_expr(fn, st, src);
    if (ty.kind == TY_STRUCT || ty.is_vector || type_is_pair16(ty)) {
        emit_struct_copy(fn, addr, rv, type_size(ty), loc);
        return;
    }
    int rw = get_value_width(fn, rv), ru = get_value_is_unsigned(fn, rv);
    int rf = get_value_is_float(fn, rv) ? 1 : 0;
    int df = (ty.kind == TY_FLOAT);
    IRValue coerced;
    if (rf != df)
        coerced = convert_numeric(fn, rv, rw, pw, pu, df, loc);
    else if (df)
        coerced = (rw == pw) ? rv : convert_numeric(fn, rv, rw, pw, pu, 1, loc);
    else
        coerced = coerce(fn, rv, rw, ru, pw, pu, loc);
    emit_inst_w(fn, IR_STORE_PTR, -1, addr, coerced, 0, pw, pu, loc);
    if (df) fn->insts.data[fn->insts.len - 1].is_float = 1;
}

static IRValue lower_va_arg_pack_inline(IRFunction *fn, IRSymTable *st, const Expr *e,
                                        const FunctionDecl *callee) {
    if (fakecc_had_error()) return FAKECC_ERR;
    if (g_vapack_depth >= (int)(sizeof(g_vapack_stack) / sizeof(g_vapack_stack[0]))) {
        return die_at(e->loc.file, e->loc.line, e->loc.col,
               "too much nested __builtin_va_arg_pack inlining");
    }
    size_t nparams = callee->params.len;
    size_t nargs = e->u.call.args.len;
    Expr **extra = NULL;
    size_t extra_len = 0;
    if (nargs > nparams) {
        extra_len = nargs - nparams;
        extra = e->u.call.args.data + nparams;
    }

    size_t mark = st->len;
    for (size_t i = 0; i < nparams; i++) {
        Type pty = callee->params.data[i].type;
        const char *pname = callee->params.data[i].name;
        SourceLoc ploc = callee->params.data[i].loc;
        int pw = pty.kind == TY_PTR ? 8 : (pty.width ? pty.width : 4);
        int pu = pty.kind == TY_PTR ? 1 : pty.is_unsigned;
        int total = type_size(pty);
        if (total < 1) total = 1;
        IRValue slot = emit_alloca(fn, total, pw, pu, ploc);
        if (i < nargs)
            vapack_store_value(fn, st, slot, pty, e->u.call.args.data[i], e->loc);
        if (pname && pname[0])
            irsymtable_push(st, pname, slot, 1, pty);
    }

    VaArgPackFrame *fr = &g_vapack_stack[g_vapack_depth++];
    memset(fr, 0, sizeof(*fr));
    fr->extra = extra;
    fr->extra_len = extra_len;
    fr->join_label = new_label(fn);
    fr->ret_type = callee->ret_type;
    fr->is_void = (callee->ret_type.kind == TY_VOID);
    fr->result_slot = -1;
    if (!fr->is_void) {
        int total = type_size(callee->ret_type);
        if (total < 1) total = 1;
        int rw = callee->ret_type.kind == TY_PTR ? 8
               : (callee->ret_type.width ? callee->ret_type.width : 4);
        int ru = callee->ret_type.kind == TY_PTR ? 1 : callee->ret_type.is_unsigned;
        fr->result_slot = emit_alloca(fn, total, rw, ru, e->loc);
    }

    for (size_t i = 0; i < callee->body.len; i++)
        lower_stmt(fn, st, &callee->body.data[i], callee);

    emit_label(fn, fr->join_label, e->loc);
    IRValue result = -1;
    if (!fr->is_void) {
        Type rty = fr->ret_type;
        IRValue addr = emit_bin_w(fn, IR_ADDR, fr->result_slot, -1, 8, 1, e->loc);
        if (rty.kind == TY_STRUCT || rty.is_vector || type_is_pair16(rty)) {
            result = addr;
        } else {
            int rw = rty.kind == TY_PTR ? 8 : (rty.width ? rty.width : 4);
            int ru = rty.kind == TY_PTR ? 1 : rty.is_unsigned;
            result = new_value(fn);
            emit_inst_w(fn, IR_LOAD_PTR, result, addr, -1, 0, rw, ru, e->loc);
            set_value_type(fn, result, rw, ru);
            if (rty.kind == TY_FLOAT)
                set_value_float(fn, result, rty.is_decimal ? 2 : 1);
        }
    }
    g_vapack_depth--;
    st->len = mark;
    return result;
}

/* Side-effect-free enough to evaluate once when matching a rotate idiom. */
static int expr_repeatable(const Expr *e) {
    if (!e) return 0;
    switch (e->kind) {
    case EX_INT_LIT:
    case EX_VAR:
        return 1;
    case EX_CAST:
        return expr_repeatable(e->u.cast.operand);
    case EX_UNARY:
        return e->u.un.op == UOP_POS && expr_repeatable(e->u.un.operand);
    case EX_DEREF:
        return expr_repeatable(e->u.deref.operand);
    case EX_ADDR:
        return expr_repeatable(e->u.addr.operand);
    case EX_MEMBER:
        return expr_repeatable(e->u.member.obj);
    case EX_INDEX:
        return expr_repeatable(e->u.idx.array) && expr_repeatable(e->u.idx.index);
    case EX_BINOP:
        if (e->u.bin.op == BOP_ADD || e->u.bin.op == BOP_SUB)
            return expr_repeatable(e->u.bin.l) && expr_repeatable(e->u.bin.r);
        return 0;
    default:
        return 0;
    }
}

static int expr_equiv(const Expr *a, const Expr *b) {
    if (!a || !b) return 0;
    while (a->kind == EX_CAST) a = a->u.cast.operand;
    while (b->kind == EX_CAST) b = b->u.cast.operand;
    if (a->kind != b->kind) return 0;
    switch (a->kind) {
    case EX_INT_LIT:
        return a->u.int_val == b->u.int_val && a->int_hi == b->int_hi;
    case EX_VAR: {
        const char *an = a->u.var.name ? a->u.var.name : "";
        const char *bn = b->u.var.name ? b->u.var.name : "";
        if (strcmp(an, bn) != 0) return 0;
        if (!a->u.var.pkg && !b->u.var.pkg) return 1;
        if (a->u.var.pkg && b->u.var.pkg) return strcmp(a->u.var.pkg, b->u.var.pkg) == 0;
        return 0;
    }
    case EX_UNARY:
        return a->u.un.op == b->u.un.op && expr_equiv(a->u.un.operand, b->u.un.operand);
    case EX_DEREF:
        return expr_equiv(a->u.deref.operand, b->u.deref.operand);
    case EX_ADDR:
        return expr_equiv(a->u.addr.operand, b->u.addr.operand);
    case EX_MEMBER: {
        const char *an = a->u.member.name ? a->u.member.name : "";
        const char *bn = b->u.member.name ? b->u.member.name : "";
        return strcmp(an, bn) == 0 && expr_equiv(a->u.member.obj, b->u.member.obj);
    }
    case EX_INDEX:
        return expr_equiv(a->u.idx.array, b->u.idx.array)
            && expr_equiv(a->u.idx.index, b->u.idx.index);
    case EX_BINOP:
        return a->u.bin.op == b->u.bin.op
            && expr_equiv(a->u.bin.l, b->u.bin.l)
            && expr_equiv(a->u.bin.r, b->u.bin.r);
    default:
        return 0;
    }
}

/* True if `e` is the integer constant `bits` minus `amt`. */
static int expr_is_bits_minus(const Expr *e, const Expr *amt, int bits) {
    if (!e || e->kind != EX_BINOP || e->u.bin.op != BOP_SUB) return 0;
    long long c;
    if (!fold_const_int(e->u.bin.l, &c) || c != bits) return 0;
    return expr_equiv(e->u.bin.r, amt);
}

static int amounts_complement(const Expr *shl_n, const Expr *shr_n, int bits) {
    if (expr_is_bits_minus(shr_n, shl_n, bits)) return 1;
    if (expr_is_bits_minus(shl_n, shr_n, bits)) return 1;
    long long a, b;
    if (fold_const_int(shl_n, &a) && fold_const_int(shr_n, &b))
        return (a + b == bits) || (a + b == 0);
    return 0;
}

/* gcc -O0 instruction selection: (x<<n)|(x>>(W-n)) and the ror dual become ROL.
 * Only matches side-effect-free operands so lowering x/n once is valid. */
static IRValue try_lower_rotate(IRFunction *fn, IRSymTable *st, const Expr *e) {
    if (!e || e->kind != EX_BINOP || e->u.bin.op != BOP_BITOR) return -1;
    if (e->type.kind == TY_FLOAT || e->type.is_vector) return -1;
    if (!e->type.is_unsigned) return -1;
    int op_w = e->type.width ? e->type.width : 4;
    if (op_w != 1 && op_w != 2 && op_w != 4 && op_w != 8) return -1;
    int bits = op_w * 8;
    const Expr *shl = NULL, *shr = NULL;
    if (e->u.bin.l->kind == EX_BINOP && e->u.bin.l->u.bin.op == BOP_SHL) shl = e->u.bin.l;
    if (e->u.bin.l->kind == EX_BINOP && e->u.bin.l->u.bin.op == BOP_SHR) shr = e->u.bin.l;
    if (e->u.bin.r->kind == EX_BINOP && e->u.bin.r->u.bin.op == BOP_SHL) shl = e->u.bin.r;
    if (e->u.bin.r->kind == EX_BINOP && e->u.bin.r->u.bin.op == BOP_SHR) shr = e->u.bin.r;
    if (!shl || !shr) return -1;
    if (!shr->type.is_unsigned) return -1;
    if (!expr_equiv(shl->u.bin.l, shr->u.bin.l)) return -1;
    if (!expr_repeatable(shl->u.bin.l)) return -1;
    if (!expr_repeatable(shl->u.bin.r) && !expr_repeatable(shr->u.bin.r)) return -1;
    if (!amounts_complement(shl->u.bin.r, shr->u.bin.r, bits)) return -1;
    /* ROL amount is the left-shift count: (x<<n)|(x>>(W-n)) and
     * (x>>n)|(x<<(W-n)) both equal rotate_left(x, shl_count). */
    if (!expr_repeatable(shl->u.bin.r)) return -1;
    IRValue x = lower_expr(fn, st, shl->u.bin.l);
    IRValue n = lower_expr(fn, st, shl->u.bin.r);
    int xw = get_value_width(fn, x), xu = get_value_is_unsigned(fn, x);
    int nw = get_value_width(fn, n), nu = get_value_is_unsigned(fn, n);
    x = coerce(fn, x, xw, xu, op_w, 1, e->loc);
    n = coerce(fn, n, nw, nu, op_w, 1, e->loc);
    return emit_bin_w(fn, IR_ROL, x, n, op_w, 1, e->loc);
}

/* Lower an expression to a value id, emitting instructions as needed.
 * Sema has annotated e->type; we lower operands and coerce them per UAC. */
static IRValue lower_expr(IRFunction *fn, IRSymTable *st, const Expr *e) {
    if (fakecc_had_error()) return FAKECC_ERR;
    switch (e->kind) {
    case EX_INT_LIT: {
        if (type_is_i128(e->type)) {
            IRValue dst = i128_alloc(fn, e->loc);
            i128_store2(fn, dst,
                        i64imm(fn, e->u.int_val, e->loc),
                        i64imm(fn, (int64_t)e->int_hi, e->loc),
                        e->loc);
            return dst;
        }
        IRValue v = new_value(fn);
        emit_inst_w(fn, IR_CONST, v, -1, -1, e->u.int_val,
                    e->type.width ? e->type.width : 4,
                    e->type.is_unsigned, e->loc);
        return v;
    }
    case EX_FLOAT_LIT: {
        int w = e->type.width ? e->type.width : 8;
        if (e->type.is_decimal) {
            unsigned long long lo = 0, hi = 0;
            dfp_from_str(e->u.float_text, w, &lo, &hi);
            return emit_dfp_const(fn, w, lo, hi, e->loc);
        }
        if (w == 16)
            return emit_ld_const(fn, strtold(e->u.float_text, NULL), e->loc);
        int64_t bits = 0;
        if (w == 4) { float f = (float)strtod(e->u.float_text, NULL); memcpy(&bits, &f, sizeof(f)); }
        else { double d = strtod(e->u.float_text, NULL); memcpy(&bits, &d, sizeof(d)); }
        return emit_float_const(fn, w, bits, e->loc);
    }
    case EX_STR: {
        /* Register a rodata global holding the bytes, then produce its
         * address via IR_GADDR. */
        char name[32];
        snprintf(name, sizeof name, "__str.%d", g_str_counter++);
        int bytes = e->u.str.len + 1;
        char *init = malloc(bytes);
        memcpy(init, e->u.str.bytes, bytes);
        /* anonymous constant — file-local by construction */
        ir_module_push_global(g_ir_module, name, bytes, init, 1, 1, 0, e->loc);
        return emit_gaddr(fn, name, e->loc, 0);
    }
    case EX_UNARY:
        switch (e->u.un.op) {
        case UOP_NEG: {
            if (e->u.un.operand->type.is_vector) {
                Type vt = e->u.un.operand->type;
                int total_sz = type_size(vt);
                int esz = vt.elem_type ? type_size(*vt.elem_type) : 4;
                if (esz <= 0) esz = 4;
                int count = vt.length > 0 ? vt.length : (total_sz / esz);
                int is_unsigned = vt.elem_type ? vt.elem_type->is_unsigned : 0;
                int is_float = vt.elem_type ? (vt.elem_type->kind == TY_FLOAT) : 0;
                IRValue op_addr = lower_expr(fn, st, e->u.un.operand);
                IRValue slot = emit_alloca(fn, total_sz, 16, 1, e->loc);
                IRValue dst_addr = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, e->loc);
                for (int i = 0; i < count; i++) {
                    IRValue src_off = emit_add_const(fn, op_addr, i * esz, e->loc);
                    IRValue dst_off = emit_add_const(fn, dst_addr, i * esz, e->loc);
                    IRValue val = new_value(fn);
                    emit_inst_w(fn, IR_LOAD_PTR, val, src_off, -1, 0, esz, is_unsigned, e->loc);
                    if (is_float) set_value_float(fn, val, 1);
                    IRValue neg_val;
                    if (is_float) {
                        int64_t negzero = (esz == 4) ? (int64_t)0x80000000
                                                    : (int64_t)0x8000000000000000LL;
                        IRValue zero = emit_float_const(fn, esz, negzero, e->loc);
                        neg_val = emit_bin_w(fn, IR_FSUB, zero, val, esz, 0, e->loc);
                        set_value_float(fn, neg_val, 1);
                    } else {
                        neg_val = emit_bin_w(fn, IR_NEG, val, -1, esz, is_unsigned, e->loc);
                    }
                    emit_inst_w(fn, IR_STORE_PTR, -1, dst_off, neg_val, 0, esz, is_unsigned, e->loc);
                    if (is_float) fn->insts.data[fn->insts.len - 1].is_float = 1;
                }
                return dst_addr;
            }
            if (e->u.un.operand->type.kind == TY_STRUCT && e->u.un.operand->type.tag &&
                strncmp(e->u.un.operand->type.tag, "__complex_", 10) == 0) {
                Type cty = e->u.un.operand->type;
                int total_sz = type_size(cty);
                int elem_sz = total_sz / 2;
                int is_float = (strstr(cty.tag, "float") != NULL || strstr(cty.tag, "double") != NULL || strstr(cty.tag, "ldouble") != NULL);
                int is_unsigned = (strstr(cty.tag, "unsigned") != NULL);
                IRValue op_addr = lower_expr(fn, st, e->u.un.operand);
                IRValue vr = new_value(fn);
                emit_inst_w(fn, IR_LOAD_PTR, vr, op_addr, -1, 0, elem_sz, is_unsigned, e->loc);
                if (is_float) set_value_float(fn, vr, 1);

                IRValue off = new_value(fn);
                emit_inst_w(fn, IR_CONST, off, -1, -1, elem_sz, 8, 1, e->loc);
                IRValue iaddr = emit_bin_w(fn, IR_ADD, op_addr, off, 8, 1, e->loc);
                IRValue vi = new_value(fn);
                emit_inst_w(fn, IR_LOAD_PTR, vi, iaddr, -1, 0, elem_sz, is_unsigned, e->loc);
                if (is_float) set_value_float(fn, vi, 1);

                IRValue neg_vr, neg_vi;
                if (is_float) {
                    int64_t negzero = (elem_sz == 4) ? (int64_t)0x80000000 : (int64_t)0x8000000000000000LL;
                    IRValue zero = emit_float_const(fn, elem_sz, negzero, e->loc);
                    neg_vr = emit_bin_w(fn, IR_FSUB, zero, vr, elem_sz, 0, e->loc);
                    neg_vi = emit_bin_w(fn, IR_FSUB, zero, vi, elem_sz, 0, e->loc);
                    set_value_float(fn, neg_vr, 1);
                    set_value_float(fn, neg_vi, 1);
                } else {
                    neg_vr = emit_bin_w(fn, IR_NEG, vr, -1, elem_sz, 0, e->loc);
                    neg_vi = emit_bin_w(fn, IR_NEG, vi, -1, elem_sz, 0, e->loc);
                }

                IRValue slot = emit_alloca(fn, total_sz, 8, 1, e->loc);
                IRValue addr = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, e->loc);
                emit_inst_w(fn, IR_STORE_PTR, -1, addr, neg_vr, 0, elem_sz, 1, e->loc);
                if (is_float) fn->insts.data[fn->insts.len - 1].is_float = 1;
                IRValue out_iaddr = emit_bin_w(fn, IR_ADD, addr, off, 8, 1, e->loc);
                emit_inst_w(fn, IR_STORE_PTR, -1, out_iaddr, neg_vi, 0, elem_sz, 1, e->loc);
                if (is_float) fn->insts.data[fn->insts.len - 1].is_float = 1;
                return addr;
            }
            if (type_is_i128(e->type) || type_is_i128(e->u.un.operand->type)) {
                IRValue x = lower_expr(fn, st, e->u.un.operand);
                IRValue a = i128_as_addr(fn, x, e->u.un.operand->type,
                                         e->type.is_unsigned, e->loc);
                IRValue dst = i128_alloc(fn, e->loc);
                i128_neg(fn, dst, a, e->loc);
                return dst;
            }
            IRValue x = lower_expr(fn, st, e->u.un.operand);
            int sw = get_value_width(fn, x);
            int su = get_value_is_unsigned(fn, x);
            int sf = get_value_is_float(fn, x);
            int rw = e->type.width ? e->type.width : 4;
            int ru = e->type.is_unsigned;
            if (e->type.is_decimal || e->u.un.operand->type.is_decimal) {
                int w = e->type.width ? e->type.width : 8;
                if (w == 16) {
                    IRValue lo, hi;
                    i128_load2(fn, x, &lo, &hi, e->loc);
                    IRValue mask = i64imm(fn, (int64_t)0x8000000000000000ULL, e->loc);
                    IRValue nhi = emit_bin_w(fn, IR_BXOR, hi, mask, 8, 1, e->loc);
                    IRValue dst = i128_alloc(fn, e->loc);
                    i128_store2(fn, dst, lo, nhi, e->loc);
                    return dst;
                }
                unsigned long long sign = (w == 4) ? 0x80000000ULL : 0x8000000000000000ULL;
                IRValue bits = emit_bits_from_dfp(fn, x, w, e->loc);
                IRValue m = new_value(fn);
                emit_inst_w(fn, IR_CONST, m, -1, -1, (int64_t)sign, w == 4 ? 4 : 8, 1, e->loc);
                IRValue xb = emit_bin_w(fn, IR_BXOR, bits, m, w == 4 ? 4 : 8, 1, e->loc);
                return emit_dfp_from_bits(fn, xb, w, e->loc);
            }
            if (sf) {
                /* Float negation: -x = (-0.0) - x.  Subtracting from +0.0
                 * would turn -0.0 into +0.0, losing the sign IEEE keeps. */
                int64_t negzero = (rw == 4) ? (int64_t)0x80000000
                                            : (int64_t)0x8000000000000000LL;
                IRValue zero = emit_float_const(fn, rw, negzero, e->loc);
                IRValue neg = emit_bin_w(fn, IR_FSUB, zero, x, rw, 0, e->loc);
                set_value_float(fn, neg, 1);
                return neg;
            }
            IRValue px = coerce(fn, x, sw, su, rw, ru, e->loc);
            return emit_bin_w(fn, IR_NEG, px, -1, rw, ru, e->loc);
        }
        case UOP_POS:
            return lower_expr(fn, st, e->u.un.operand);
        case UOP_NOT: {
            /* Logical NOT: !x → (x == 0). Result is int 0/1. */
            if (type_is_i128(e->u.un.operand->type)) {
                IRValue x = lower_expr(fn, st, e->u.un.operand);
                IRValue nz = i128_nz(fn, x, e->loc);
                IRValue one = new_value(fn);
                emit_inst_w(fn, IR_CONST, one, -1, -1, 1, 4, 0, e->loc);
                return emit_bin_w(fn, IR_BXOR, nz, one, 4, 0, e->loc);
            }
            if (e->u.un.operand->type.is_decimal) {
                IRValue x = lower_expr(fn, st, e->u.un.operand);
                int w = e->u.un.operand->type.width ? e->u.un.operand->type.width : 8;
                IRValue z = emit_dfp_zero(fn, w, e->loc);
                return emit_dfp_cmp(fn, w, BOP_EQ, x, z, e->loc);
            }
            if (e->u.un.operand->type.kind == TY_FLOAT
                && !e->u.un.operand->type.is_vector) {
                /* !f is (f == +0.0); IEEE -0.0 is also zero. */
                IRValue x = lower_expr(fn, st, e->u.un.operand);
                int w = get_value_width(fn, x);
                IRValue zero = emit_float_const(fn, w, 0, e->loc);
                return emit_fcmp(fn, x, zero, w, 4 /* EQ */, e->loc);
            }
            IRValue x = lower_expr(fn, st, e->u.un.operand);
            int xw = get_value_width(fn, x), xu = get_value_is_unsigned(fn, x);
            IRValue zero = new_value(fn);
            emit_inst_w(fn, IR_CONST, zero, -1, -1, 0, xw, xu, e->loc);
            IRValue cmp = emit_bin_w(fn, IR_EQ, x, zero, xw, xu, e->loc);
            /* Retag result to int(4,signed). */
            set_value_type(fn, cmp, 4, 0);
            return cmp;
        }
        case UOP_BITNOT: {
            if (e->u.un.operand->type.is_vector) {
                Type vt = e->u.un.operand->type;
                int total_sz = type_size(vt);
                int esz = vt.elem_type ? type_size(*vt.elem_type) : 4;
                if (esz <= 0) esz = 4;
                int count = vt.length > 0 ? vt.length : (total_sz / esz);
                int is_unsigned = vt.elem_type ? vt.elem_type->is_unsigned : 0;
                IRValue op_addr = lower_expr(fn, st, e->u.un.operand);
                IRValue slot = emit_alloca(fn, total_sz, 16, 1, e->loc);
                IRValue dst_addr = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, e->loc);
                for (int i = 0; i < count; i++) {
                    IRValue src_off = emit_add_const(fn, op_addr, i * esz, e->loc);
                    IRValue dst_off = emit_add_const(fn, dst_addr, i * esz, e->loc);
                    IRValue val = new_value(fn);
                    emit_inst_w(fn, IR_LOAD_PTR, val, src_off, -1, 0, esz, is_unsigned, e->loc);
                    IRValue not_val = emit_bin_w(fn, IR_BNOT, val, -1, esz, is_unsigned, e->loc);
                    emit_inst_w(fn, IR_STORE_PTR, -1, dst_off, not_val, 0, esz, is_unsigned, e->loc);
                }
                return dst_addr;
            }
            if (e->u.un.operand->type.kind == TY_STRUCT && e->u.un.operand->type.tag &&
                strncmp(e->u.un.operand->type.tag, "__complex_", 10) == 0) {
                Type cty = e->u.un.operand->type;
                int total_sz = type_size(cty);
                int elem_sz = total_sz / 2;
                int is_float = (strstr(cty.tag, "float") != NULL || strstr(cty.tag, "double") != NULL || strstr(cty.tag, "ldouble") != NULL);
                int is_unsigned = (strstr(cty.tag, "unsigned") != NULL);
                IRValue op_addr = lower_expr(fn, st, e->u.un.operand);
                IRValue vr = new_value(fn);
                emit_inst_w(fn, IR_LOAD_PTR, vr, op_addr, -1, 0, elem_sz, is_unsigned, e->loc);
                if (is_float) set_value_float(fn, vr, 1);
                
                IRValue off = new_value(fn);
                emit_inst_w(fn, IR_CONST, off, -1, -1, elem_sz, 8, 1, e->loc);
                IRValue iaddr = emit_bin_w(fn, IR_ADD, op_addr, off, 8, 1, e->loc);
                IRValue vi = new_value(fn);
                emit_inst_w(fn, IR_LOAD_PTR, vi, iaddr, -1, 0, elem_sz, is_unsigned, e->loc);
                if (is_float) set_value_float(fn, vi, 1);
                
                IRValue neg_vi;
                if (is_float) {
                    int64_t negzero = (elem_sz == 4) ? (int64_t)0x80000000 : (int64_t)0x8000000000000000LL;
                    IRValue zero = emit_float_const(fn, elem_sz, negzero, e->loc);
                    neg_vi = emit_bin_w(fn, IR_FSUB, zero, vi, elem_sz, 0, e->loc);
                    set_value_float(fn, neg_vi, 1);
                } else {
                    neg_vi = emit_bin_w(fn, IR_NEG, vi, -1, elem_sz, 0, e->loc);
                }
                
                IRValue slot = emit_alloca(fn, total_sz, 8, 1, e->loc);
                IRValue addr = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, e->loc);
                emit_inst_w(fn, IR_STORE_PTR, -1, addr, vr, 0, elem_sz, 1, e->loc);
                IRValue out_iaddr = emit_bin_w(fn, IR_ADD, addr, off, 8, 1, e->loc);
                emit_inst_w(fn, IR_STORE_PTR, -1, out_iaddr, neg_vi, 0, elem_sz, 1, e->loc);
                return addr;
            }
            if (type_is_i128(e->type) || type_is_i128(e->u.un.operand->type)) {
                IRValue x = lower_expr(fn, st, e->u.un.operand);
                IRValue a = i128_as_addr(fn, x, e->u.un.operand->type,
                                         e->type.is_unsigned, e->loc);
                IRValue dst = i128_alloc(fn, e->loc);
                i128_bnot(fn, dst, a, e->loc);
                return dst;
            }
            IRValue x = lower_expr(fn, st, e->u.un.operand);
            int sw = get_value_width(fn, x);
            int su = get_value_is_unsigned(fn, x);
            int rw = e->type.width ? e->type.width : 4;
            int ru = e->type.is_unsigned;
            IRValue px = coerce(fn, x, sw, su, rw, ru, e->loc);
            IRValue v = new_value(fn);
            emit_inst_w(fn, IR_BNOT, v, px, -1, 0, rw, ru, e->loc);
            return v;
        }
        }
        break; /* unreachable */
    case EX_BINOP: {
        /* Pointer arithmetic: p+i, i+p, p-i, p-q.
         * Sema has already decayed array operands to pointer types on both
         * l and r; check the l/r Expr's type to know. */
        Type lt = e->u.bin.l->type, rt = e->u.bin.r->type;
        int l_is_ptr = (lt.kind == TY_PTR || lt.kind == TY_ARRAY);
        int r_is_ptr = (rt.kind == TY_PTR || rt.kind == TY_ARRAY);
        BinOp bop = e->u.bin.op;
        if (lt.is_vector || rt.is_vector) {
            return lower_vector_binop(fn, st, e, lt, rt, bop);
        }
        if ((lt.kind == TY_STRUCT && lt.tag && strncmp(lt.tag, "__complex_", 10) == 0) ||
            (rt.kind == TY_STRUCT && rt.tag && strncmp(rt.tag, "__complex_", 10) == 0)) {
            return lower_complex_binop(fn, st, e, lt, rt, bop, -1);
        }
        if (type_is_i128(lt) || type_is_i128(rt) || type_is_i128(e->type)) {
            IRValue lv = lower_expr(fn, st, e->u.bin.l);
            IRValue rv = lower_expr(fn, st, e->u.bin.r);
            return lower_i128_binop(fn, lv, rv, lt, rt, bop, e->type, e->loc);
        }
        if (lt.is_decimal || rt.is_decimal || e->type.is_decimal) {
            Type dty = e->type.is_decimal ? e->type
                     : (lt.is_decimal && rt.is_decimal)
                       ? (lt.width >= rt.width ? lt : rt)
                       : (lt.is_decimal ? lt : rt);
            if (bop >= BOP_EQ && bop <= BOP_GE) {
                if (lt.is_decimal && rt.is_decimal)
                    dty = lt.width >= rt.width ? lt : rt;
                else
                    dty = lt.is_decimal ? lt : rt;
            }
            int w = dty.width ? dty.width : 8;
            IRValue lv = lower_expr(fn, st, e->u.bin.l);
            IRValue rv = lower_expr(fn, st, e->u.bin.r);
            lv = convert_to_decimal(fn, lv, lt, dty, e->loc);
            rv = convert_to_decimal(fn, rv, rt, dty, e->loc);
            if (bop >= BOP_EQ && bop <= BOP_GE)
                return emit_dfp_cmp(fn, w, bop, lv, rv, e->loc);
            int aop = 0;
            if (bop == BOP_SUB) aop = 1;
            else if (bop == BOP_MUL) aop = 2;
            else if (bop == BOP_DIV) aop = 3;
            return emit_dfp_arith(fn, w, aop, lv, rv, e->loc);
        }
        if (bop == BOP_AND || bop == BOP_OR) {
            /* Short-circuit logical operators: lower to control flow writing a
             * temporary alloca, then let mem2reg promote it into a φ-merged
             * SSA value (no IR_PHI opcode needed).
             *   a && b  →  t = a; if (!t) goto L_false; t = (b != 0);
             *              goto L_done; L_false: t = 0; L_done: ... t
             *   a || b  →  t = a; if (t) goto L_true; t = (b != 0);
             *              goto L_done; L_true: t = 1; L_done: ... t */
            IRValue slot = new_value(fn);
            emit_inst_w(fn, IR_ALLOCA, slot, -1, -1, 0, 4, 0, e->loc);

            int L_true  = new_label(fn);
            int L_false = new_label(fn);
            int L_done  = new_label(fn);

            IRValue lv = lower_expr(fn, st, e->u.bin.l);
            emit_cbr(fn, cbr_from_scalar(fn, lv, e->loc), L_true, L_false, e->loc);

            emit_label(fn, L_true, e->loc);
            IRValue true_val;
            if (bop == BOP_OR) {
                /* a != 0 → result is 1, b not evaluated (short-circuit). */
                true_val = new_value(fn);
                emit_inst_w(fn, IR_CONST, true_val, -1, -1, 1, 4, 0, e->loc);
            } else {
                IRValue rv = lower_expr(fn, st, e->u.bin.r);
                int rw = get_value_width(fn, rv), ru = get_value_is_unsigned(fn, rv);
                int rf = get_value_is_float(fn, rv);
                true_val = bool_normalize(fn, rv, rw, ru, rf, e->loc);
            }
            emit_inst_w(fn, IR_STORE, -1, slot, true_val, 0, 4, 0, e->loc);
            emit_br(fn, L_done, e->loc);

            emit_label(fn, L_false, e->loc);
            IRValue false_val;
            if (bop == BOP_AND) {
                /* a == 0 → result is 0, b not evaluated (short-circuit). */
                false_val = new_value(fn);
                emit_inst_w(fn, IR_CONST, false_val, -1, -1, 0, 4, 0, e->loc);
            } else {
                IRValue rv = lower_expr(fn, st, e->u.bin.r);
                int rw = get_value_width(fn, rv), ru = get_value_is_unsigned(fn, rv);
                int rf = get_value_is_float(fn, rv);
                false_val = bool_normalize(fn, rv, rw, ru, rf, e->loc);
            }
            emit_inst_w(fn, IR_STORE, -1, slot, false_val, 0, 4, 0, e->loc);
            emit_br(fn, L_done, e->loc);

            emit_label(fn, L_done, e->loc);
            IRValue result = new_value(fn);
            emit_inst_w(fn, IR_LOAD, result, slot, -1, 0, 4, 0, e->loc);
            return result;
        }
        if ((bop == BOP_ADD || bop == BOP_SUB) && (l_is_ptr || r_is_ptr)) {
            IRValue lv = lower_expr(fn, st, e->u.bin.l);
            IRValue rv = lower_expr(fn, st, e->u.bin.r);
            if (bop == BOP_SUB && l_is_ptr && r_is_ptr) {
                /* ptrdiff: (p - q) / sizeof(*p). */
                Type pointee = type_default_int();
                if (lt.kind == TY_ARRAY && lt.elem_type) pointee = *lt.elem_type;
                else if (lt.kind == TY_PTR && lt.pointee) pointee = *lt.pointee;
                IRValue diff = emit_bin_w(fn, IR_SUB, lv, rv, 8, 0, e->loc);
                IRValue esv = emit_pointee_stride(fn, st, pointee, e->loc);
                return emit_bin_w(fn, IR_DIV, diff, esv, 8, 0, e->loc);
            }
            /* pointer +/- int : scale int by sizeof(pointee). */
            IRValue pv = l_is_ptr ? lv : rv;
            IRValue iv = l_is_ptr ? rv : lv;
            Type pty = l_is_ptr ? lt : rt;
            Type pointee = type_default_int();
            if (pty.kind == TY_ARRAY && pty.elem_type) pointee = *pty.elem_type;
            else if (pty.kind == TY_PTR && pty.pointee) pointee = *pty.pointee;
            int iw = get_value_width(fn, iv), iu = get_value_is_unsigned(fn, iv);
            IRValue iv8 = coerce(fn, iv, iw, iu, 8, 0, e->loc);
            IRValue esv = emit_pointee_stride(fn, st, pointee, e->loc);
            IRValue off = emit_bin_w(fn, IR_MUL, iv8, esv, 8, 1, e->loc);
            IROpcode op = (bop == BOP_ADD) ? IR_ADD : IR_SUB;
            return emit_bin_w(fn, op, pv, off, 8, 1, e->loc);
        }
        if (bop == BOP_BITOR) {
            IRValue rotated = try_lower_rotate(fn, st, e);
            if (rotated >= 0) return rotated;
        }
        IRValue l = lower_expr(fn, st, e->u.bin.l);
        IRValue r = lower_expr(fn, st, e->u.bin.r);
        int lw = get_value_width(fn, l), lu = get_value_is_unsigned(fn, l);
        int rw = get_value_width(fn, r), ru = get_value_is_unsigned(fn, r);
        int lf = get_value_is_float(fn, l), rf = get_value_is_float(fn, r);

        int is_cmp = (e->u.bin.op >= BOP_EQ && e->u.bin.op <= BOP_GE);
        /* For arithmetic the result type follows e->type; for comparisons the
         * result is always int, but a float comparison (either operand float)
         * must lower to IR_FCMP, not integer IR_GT/etc. */
        int res_float = (e->type.kind == TY_FLOAT) || (is_cmp && (lf || rf));
        /* Operand width for the op = e->type for arith; UAC result for cmp. */
        int op_w, op_u;
        if (is_cmp) {
            /* Reapply UAC to operand types to know cmp width/signedness. */
            int aw = lf ? lw : (lw < 4 ? 4 : lw), au = lf ? 0 : (lw < 4 ? 0 : lu);
            int bw = rf ? rw : (rw < 4 ? 4 : rw), bu = rf ? 0 : (rw < 4 ? 0 : ru);
            if (lf || rf) {
                /* Float comparison: width = max float width, neither signed. */
                if (lf && rf) op_w = lw > rw ? lw : rw;
                else if (lf) op_w = lw;
                else op_w = rw;
                op_u = 0;
            } else if (aw == bw && au == bu) { op_w = aw; op_u = au; }
            else if (au == bu) { op_w = aw > bw ? aw : bw; op_u = au; }
            else {
                int uw = au ? aw : bw; int uu = 1;
                int sw = au ? bw : aw;
                if (uw >= sw) { op_w = uw; op_u = uu; }
                else { op_w = sw; op_u = 0; }
            }
        } else {
            op_w = e->type.width ? e->type.width : 4;
            op_u = e->type.is_unsigned;
        }
        IRValue pl, pr;
        if (res_float) {
            /* Coerce both operands to the result float width. */
            pl = lf ? (lw == op_w ? l : convert_numeric(fn, l, lw, op_w, 0, 1, e->loc))
                    : convert_numeric(fn, l, lw, op_w, 0, 1, e->loc);
            pr = rf ? (rw == op_w ? r : convert_numeric(fn, r, rw, op_w, 0, 1, e->loc))
                    : convert_numeric(fn, r, rw, op_w, 0, 1, e->loc);
        } else {
            pl = coerce(fn, l, lw, lu, op_w, op_u, e->loc);
            pr = coerce(fn, r, rw, ru, op_w, op_u, e->loc);
        }
        IROpcode op;
        if (res_float && !is_cmp) {
            switch (e->u.bin.op) {
            case BOP_ADD: op = IR_FADD; break;
            case BOP_SUB: op = IR_FSUB; break;
            case BOP_MUL: op = IR_FMUL; break;
            case BOP_DIV: op = IR_FDIV; break;
            default:      op = IR_FADD; break;
            }
        } else {
            switch (e->u.bin.op) {
            case BOP_ADD:     op = IR_ADD;  break;
            case BOP_SUB:     op = IR_SUB;  break;
            case BOP_MUL:     op = IR_MUL;  break;
            case BOP_DIV:     op = IR_DIV;  break;
            case BOP_MOD:     op = IR_MOD;  break;
            case BOP_EQ:      op = IR_EQ;   break;
            case BOP_NE:      op = IR_NE;   break;
            case BOP_LT:      op = IR_LT;   break;
            case BOP_LE:      op = IR_LE;   break;
            case BOP_GT:      op = IR_GT;   break;
            case BOP_GE:      op = IR_GE;   break;
            case BOP_BITAND:  op = IR_BAND; break;
            case BOP_BITOR:   op = IR_BOR;  break;
            case BOP_BITXOR:  op = IR_BXOR; break;
            case BOP_SHL:     op = IR_SHL;  break;
            case BOP_SHR:     op = IR_SHR;  break;
            default: op = IR_ADD; break;
            }
        }
        int rw_res = is_cmp ? 4 : op_w;
        int ru_res = is_cmp ? 0 : op_u;
        IRValue result;
        if (is_cmp && res_float) {
            /* Float comparison — emit IR_FCMP with the comparison encoded in
             * is_unsigned (0=LT 1=LE 2=GT 3=GE 4=EQ 5=NE).  The BOP ordering
             * (EQ,NE,LT,LE,GT,GE) differs from the encoding order, so remap. */
            static const unsigned char cmp_enc[] = {4,5,0,1,2,3};
            unsigned char enc = cmp_enc[e->u.bin.op - BOP_EQ];
            result = new_value(fn);
            emit_inst_f(fn, IR_FCMP, result, pl, pr, op_w, e->loc);
            fn->insts.data[fn->insts.len - 1].is_unsigned = enc;
            set_value_float(fn, result, 0);  /* result is int 0/1 */
            set_value_type(fn, result, 4, 0);
            return result;
        }
        /* Comparison ops carry operand width/signedness for codegen — we
         * encode it as the instruction's own width for cmps too (used only
         * by cmp encoding). Result value's width/signedness recorded via
         * emit_bin_w's set_value_type. */
        result = emit_bin_w(fn, op, pl, pr, op_w, op_u, e->loc);
        if (res_float) set_value_float(fn, result, 1);
        if (is_cmp) {
            /* Retag the result's own SSA width to int(4,signed). */
            set_value_type(fn, result, rw_res, ru_res);
        } else if (e->type.bitfield_width > 0 && e->type.bitfield_width < op_w * 8) {
            int64_t msk = bitfield_mask64(e->type.bitfield_width);
            IRValue mv = new_value(fn);
            emit_inst_w(fn, IR_CONST, mv, -1, -1, msk, op_w, 1, e->loc);
            IRValue masked = new_value(fn);
            emit_inst_w(fn, IR_BAND, masked, result, mv, 0, op_w, op_u, e->loc);
            result = masked;
        }
        return result;
    }
    case EX_TERNARY: {
        if (e->type.kind == TY_VOID) {
            int L_then = new_label(fn);
            int L_else = new_label(fn);
            int L_done = new_label(fn);
            IRValue cond = lower_expr(fn, st, e->u.tern.cond);
            emit_cbr(fn, cbr_from_scalar(fn, cond, e->loc), L_then, L_else, e->loc);
            emit_label(fn, L_then, e->loc);
            if (e->u.tern.then)
                lower_expr(fn, st, e->u.tern.then);
            emit_br(fn, L_done, e->loc);
            emit_label(fn, L_else, e->loc);
            lower_expr(fn, st, e->u.tern.else_);
            emit_br(fn, L_done, e->loc);
            emit_label(fn, L_done, e->loc);
            return -1;
        }
        if (type_is_i128(e->type)) {
            IRValue slot = emit_alloca(fn, 16, 16, 1, e->loc);
            IRValue addr = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, e->loc);
            int L_then = new_label(fn);
            int L_else = new_label(fn);
            int L_done = new_label(fn);
            IRValue cond = lower_expr(fn, st, e->u.tern.cond);
            emit_cbr(fn, cbr_from_scalar(fn, cond, e->loc), L_then, L_else, e->loc);
            emit_label(fn, L_then, e->loc);
            {
                IRValue tv = e->u.tern.then ? lower_expr(fn, st, e->u.tern.then) : cond;
                Type then_ty = e->u.tern.then ? e->u.tern.then->type : e->u.tern.cond->type;
                IRValue ta = i128_as_addr(fn, tv, then_ty,
                                          e->type.is_unsigned, e->loc);
                emit_struct_copy(fn, addr, ta, 16, e->loc);
            }
            emit_br(fn, L_done, e->loc);
            emit_label(fn, L_else, e->loc);
            {
                IRValue ev = lower_expr(fn, st, e->u.tern.else_);
                IRValue ea = i128_as_addr(fn, ev, e->u.tern.else_->type,
                                          e->type.is_unsigned, e->loc);
                emit_struct_copy(fn, addr, ea, 16, e->loc);
            }
            emit_br(fn, L_done, e->loc);
            emit_label(fn, L_done, e->loc);
            return addr;
        }
        if (e->type.kind == TY_STRUCT) {
            int total_sz = type_size(e->type);
            int is_cplx = (e->type.tag && strncmp(e->type.tag, "__complex_", 10) == 0);
            int elem_sz = is_cplx ? total_sz / 2 : 4;
            int is_float = is_cplx ? (strstr(e->type.tag, "float") || strstr(e->type.tag, "double") || strstr(e->type.tag, "ldouble")) : 0;
            IRValue slot = emit_alloca(fn, total_sz, 8, 1, e->loc);
            IRValue addr = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, e->loc);
            int L_then = new_label(fn);
            int L_else = new_label(fn);
            int L_done = new_label(fn);
            IRValue cond = lower_expr(fn, st, e->u.tern.cond);
            emit_cbr(fn, cbr_from_scalar(fn, cond, e->loc), L_then, L_else, e->loc);

            emit_label(fn, L_then, e->loc);
            {
                Expr *te = e->u.tern.then ? e->u.tern.then : e->u.tern.cond;
                if (te->type.kind == TY_STRUCT) {
                    IRValue ta = lower_expr(fn, st, te);
                    emit_struct_copy(fn, addr, ta, total_sz, e->loc);
                } else if (is_cplx) {
                    IRValue tv = lower_expr(fn, st, te);
                    int tw = get_value_width(fn, tv);
                    if (is_float) {
                        if (!get_value_is_float(fn, tv)) tv = convert_numeric(fn, tv, tw, elem_sz, 0, 1, e->loc);
                        else if (tw != elem_sz) tv = convert_numeric(fn, tv, tw, elem_sz, 0, 1, e->loc);
                        emit_inst_w(fn, IR_STORE_PTR, -1, addr, tv, 0, elem_sz, 1, e->loc);
                        fn->insts.data[fn->insts.len - 1].is_float = 1;
                        IRValue zero = emit_float_const(fn, elem_sz, 0, e->loc);
                        IRValue off = new_value(fn);
                        emit_inst_w(fn, IR_CONST, off, -1, -1, elem_sz, 8, 1, e->loc);
                        IRValue iaddr = emit_bin_w(fn, IR_ADD, addr, off, 8, 1, e->loc);
                        emit_inst_w(fn, IR_STORE_PTR, -1, iaddr, zero, 0, elem_sz, 1, e->loc);
                        fn->insts.data[fn->insts.len - 1].is_float = 1;
                    } else {
                        tv = coerce(fn, tv, tw, get_value_is_unsigned(fn, tv), elem_sz, 0, e->loc);
                        emit_inst_w(fn, IR_STORE_PTR, -1, addr, tv, 0, elem_sz, 1, e->loc);
                        IRValue zero = new_value(fn);
                        emit_inst_w(fn, IR_CONST, zero, -1, -1, 0, elem_sz, 0, e->loc);
                        IRValue off = new_value(fn);
                        emit_inst_w(fn, IR_CONST, off, -1, -1, elem_sz, 8, 1, e->loc);
                        IRValue iaddr = emit_bin_w(fn, IR_ADD, addr, off, 8, 1, e->loc);
                        emit_inst_w(fn, IR_STORE_PTR, -1, iaddr, zero, 0, elem_sz, 1, e->loc);
                    }
                }
            }
            emit_br(fn, L_done, e->loc);

            emit_label(fn, L_else, e->loc);
            {
                Expr *ee = e->u.tern.else_;
                if (ee->type.kind == TY_STRUCT) {
                    IRValue ea = lower_expr(fn, st, ee);
                    emit_struct_copy(fn, addr, ea, total_sz, e->loc);
                } else if (is_cplx) {
                    IRValue ev = lower_expr(fn, st, ee);
                    int ew = get_value_width(fn, ev);
                    if (is_float) {
                        if (!get_value_is_float(fn, ev)) ev = convert_numeric(fn, ev, ew, elem_sz, 0, 1, e->loc);
                        else if (ew != elem_sz) ev = convert_numeric(fn, ev, ew, elem_sz, 0, 1, e->loc);
                        emit_inst_w(fn, IR_STORE_PTR, -1, addr, ev, 0, elem_sz, 1, e->loc);
                        fn->insts.data[fn->insts.len - 1].is_float = 1;
                        IRValue zero = emit_float_const(fn, elem_sz, 0, e->loc);
                        IRValue off = new_value(fn);
                        emit_inst_w(fn, IR_CONST, off, -1, -1, elem_sz, 8, 1, e->loc);
                        IRValue iaddr = emit_bin_w(fn, IR_ADD, addr, off, 8, 1, e->loc);
                        emit_inst_w(fn, IR_STORE_PTR, -1, iaddr, zero, 0, elem_sz, 1, e->loc);
                        fn->insts.data[fn->insts.len - 1].is_float = 1;
                    } else {
                        ev = coerce(fn, ev, ew, get_value_is_unsigned(fn, ev), elem_sz, 0, e->loc);
                        emit_inst_w(fn, IR_STORE_PTR, -1, addr, ev, 0, elem_sz, 1, e->loc);
                        IRValue zero = new_value(fn);
                        emit_inst_w(fn, IR_CONST, zero, -1, -1, 0, elem_sz, 0, e->loc);
                        IRValue off = new_value(fn);
                        emit_inst_w(fn, IR_CONST, off, -1, -1, elem_sz, 8, 1, e->loc);
                        IRValue iaddr = emit_bin_w(fn, IR_ADD, addr, off, 8, 1, e->loc);
                        emit_inst_w(fn, IR_STORE_PTR, -1, iaddr, zero, 0, elem_sz, 1, e->loc);
                    }
                }
            }
            emit_br(fn, L_done, e->loc);
            emit_label(fn, L_done, e->loc);
            return addr;
        }
        /* Lower cond ? then : else to control flow writing a temporary
         * alloca, then let mem2reg promote it into a φ-merged SSA value
         * (no IR_PHI opcode needed).
         *   c ? t : e  →  slot = alloca; if (c) goto L_then; goto L_else;
         *                  L_then: store slot = t; goto L_done;
         *                  L_else: store slot = e; goto L_done;
         *                  L_done: result = load slot */
        int rw = e->type.width ? e->type.width : 4;
        int ru = e->type.is_unsigned;
        int rf = (e->type.kind == TY_FLOAT);
        int use_ptr = (rw > 8);
        IRValue slot, addr = -1;
        if (use_ptr) {
            slot = emit_alloca(fn, rw, 8, 1, e->loc);
            addr = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, e->loc);
        } else {
            slot = new_value(fn);
            emit_inst_w(fn, IR_ALLOCA, slot, -1, -1, 0, rw, ru, e->loc);
            if (rf) set_value_float(fn, slot, 1);
        }
        int L_then = new_label(fn);
        int L_else = new_label(fn);
        int L_done = new_label(fn);
        IRValue cond = lower_expr(fn, st, e->u.tern.cond);
        emit_cbr(fn, cbr_from_scalar(fn, cond, e->loc), L_then, L_else, e->loc);
        emit_label(fn, L_then, e->loc);
        IRValue tv = e->u.tern.then ? lower_expr(fn, st, e->u.tern.then) : cond;
        int tw = get_value_width(fn, tv), tu = get_value_is_unsigned(fn, tv), tf = get_value_is_float(fn, tv);
        IRValue tc;
        if (tf != rf) tc = convert_numeric(fn, tv, tw, rw, ru, rf, e->loc);
        else if (rf) tc = (tw == rw) ? tv : convert_numeric(fn, tv, tw, rw, ru, 1, e->loc);
        else tc = coerce(fn, tv, tw, tu, rw, ru, e->loc);
        if (use_ptr) emit_inst_w(fn, IR_STORE_PTR, -1, addr, tc, 0, rw, ru, e->loc);
        else {
            emit_inst_w(fn, IR_STORE, -1, slot, tc, 0, rw, ru, e->loc);
            if (rf) fn->insts.data[fn->insts.len - 1].is_float = 1;
        }
        emit_br(fn, L_done, e->loc);
        emit_label(fn, L_else, e->loc);
        IRValue ev = lower_expr(fn, st, e->u.tern.else_);
        int ew = get_value_width(fn, ev), eu = get_value_is_unsigned(fn, ev), ef = get_value_is_float(fn, ev);
        IRValue ec;
        if (ef != rf) ec = convert_numeric(fn, ev, ew, rw, ru, rf, e->loc);
        else if (rf) ec = (ew == rw) ? ev : convert_numeric(fn, ev, ew, rw, ru, 1, e->loc);
        else ec = coerce(fn, ev, ew, eu, rw, ru, e->loc);
        if (use_ptr) emit_inst_w(fn, IR_STORE_PTR, -1, addr, ec, 0, rw, ru, e->loc);
        else {
            emit_inst_w(fn, IR_STORE, -1, slot, ec, 0, rw, ru, e->loc);
            if (rf) fn->insts.data[fn->insts.len - 1].is_float = 1;
        }
        emit_br(fn, L_done, e->loc);
        emit_label(fn, L_done, e->loc);
        IRValue result = new_value(fn);
        if (use_ptr) emit_inst_w(fn, IR_LOAD_PTR, result, addr, -1, 0, rw, ru, e->loc);
        else {
            emit_inst_w(fn, IR_LOAD, result, slot, -1, 0, rw, ru, e->loc);
            if (rf) fn->insts.data[fn->insts.len - 1].is_float = 1;
        }
        if (rf) set_value_float(fn, result, 1);
        return result;
    }
    case EX_VAR: {
        if (strcmp(e->u.var.name, "NULL") == 0) {
            IRValue v = new_value(fn);
            emit_inst_w(fn, IR_CONST, v, -1, -1, 0, 8, 1, e->loc);
            return v;
        }
        if (strcmp(e->u.var.name, "__CHAR_BIT__") == 0) {
            IRValue v = new_value(fn);
            emit_inst_w(fn, IR_CONST, v, -1, -1, 8, 4, 0, e->loc);
            return v;
        }
        if (strcmp(e->u.var.name, "__INT_MAX__") == 0) {
            IRValue v = new_value(fn);
            emit_inst_w(fn, IR_CONST, v, -1, -1, 0x7fffffff, 4, 0, e->loc);
            return v;
        }
        if (strcmp(e->u.var.name, "__FLT_MAX__") == 0) {
            /* 0x7f7fffff — IEEE-754 binary32 FLT_MAX.  Integer bits, not a
             * C float literal: fakecc's own float immediates are not yet a
             * bootstrap fixed point. */
            return emit_float_const(fn, 4, 0x7f7fffff, e->loc);
        }
        const IRSlot *entry = irsymtable_find(st, e->u.var.name);
        if (!entry) {
            /* Not a variable — is it a function name?  A function lvalue
             * (e.g. `add` used as a value, or `&add`) decays to a function
             * pointer; emit an FADDR so the function's address is loaded
             * (patched against the code symbol table, not .data). */
            if (e->u.var.pkg) {
                /* Qualified import (runtime.printf): ELF name is unmangled. */
                IRValue v = new_value(fn);
                emit_inst_w(fn, IR_FADDR, v, -1, -1, 0, 8, 1, e->loc);
                fn->insts.data[fn->insts.len - 1].call_name = xstrdup(e->u.var.name);
                return v;
            }
            if (g_ir_tu) {
                for (size_t i = 0; i < g_ir_tu->functions.len; i++) {
                    if (strcmp(g_ir_tu->functions.data[i].name, e->u.var.name) == 0) {
                        const char *target_name = g_ir_tu->functions.data[i].alias_target
                                                ? g_ir_tu->functions.data[i].alias_target
                                                : e->u.var.name;
                        IRValue v = new_value(fn);
                        emit_inst_w(fn, IR_FADDR, v, -1, -1, 0, 8, 1, e->loc);
                        fn->insts.data[fn->insts.len - 1].call_name =
                            xstrdup(target_name);
                        return v;
                    }
                }
            }
            return -1;
        }
        if (entry->is_global) {
            IRValue addr = emit_gaddr(fn, slot_global_name(entry), e->loc, entry->is_tls);
            if (entry->ty.kind == TY_ARRAY || entry->ty.kind == TY_STRUCT || entry->ty.is_vector || type_is_pair16(entry->ty)) return addr;
            IRValue v = new_value(fn);
            emit_inst_w(fn, IR_LOAD_PTR, v, addr, -1, 0,
                        entry->width, entry->is_unsigned, e->loc);
            mark_last_volatile(fn, entry->ty.is_volatile || e->type.is_volatile);
            if (entry->ty.kind == TY_FLOAT)
                set_value_float(fn, v, entry->ty.is_decimal ? 2 : 1);
            return v;
        }
        if (entry->is_vla) {
            IRValue addr = emit_bin_w(fn, IR_ADDR, entry->slot, -1, 8, 1, e->loc);
            IRValue v = new_value(fn);
            emit_inst_w(fn, IR_LOAD_PTR, v, addr, -1, 0, 8, 1, e->loc);
            return v;
        }
        if (entry->ty.kind == TY_ARRAY || entry->ty.kind == TY_STRUCT || entry->ty.is_vector || type_is_pair16(entry->ty))
            return emit_bin_w(fn, IR_ADDR, entry->slot, -1, 8, 1, e->loc);
        if (entry->pinned) {
            IRValue addr = emit_bin_w(fn, IR_ADDR, entry->slot, -1, 8, 1, e->loc);
            IRValue v = new_value(fn);
            emit_inst_w(fn, IR_LOAD_PTR, v, addr, -1, 0,
                        entry->width, entry->is_unsigned, e->loc);
            mark_last_volatile(fn, entry->ty.is_volatile || e->type.is_volatile);
            if (entry->ty.kind == TY_FLOAT)
                set_value_float(fn, v, entry->ty.is_decimal ? 2 : 1);
            return v;
        }
        IRValue v = new_value(fn);
        emit_inst_w(fn, IR_LOAD, v, entry->slot, -1, 0,
                    e->type.width, e->type.is_unsigned, e->loc);
        mark_last_volatile(fn, entry->ty.is_volatile || e->type.is_volatile);
        return v;
    }
    case EX_ASSIGN: {
        Expr *lv = e->u.assign.lvalue;
        IRValue rv = lower_expr(fn, st, e->u.assign.rvalue);
        int rw = get_value_width(fn, rv), ru = get_value_is_unsigned(fn, rv);
        int rf = get_value_is_float(fn, rv);
        int lw = lv->type.kind == TY_PTR ? 8 : (lv->type.width ? lv->type.width : 4);
        int lu = lv->type.is_unsigned;
        int lf = (lv->type.kind == TY_FLOAT);
        IRValue coerced;
        if (lv->type.is_decimal || e->u.assign.rvalue->type.is_decimal) {
            if (lv->type.is_bool) {
                IRValue n = bool_normalize(fn, rv, rw, ru, rf, e->loc);
                coerced = coerce(fn, n, 4, 0, lw, lu, e->loc);
            } else if (lv->type.is_decimal) {
                coerced = convert_to_decimal(fn, rv, e->u.assign.rvalue->type, lv->type, e->loc);
            } else {
                coerced = convert_from_decimal(fn, rv, e->u.assign.rvalue->type, lv->type, e->loc);
            }
            if (type_is_d128(lv->type)) {
                IRValue dst;
                if (lv->kind == EX_VAR) {
                    const IRSlot *entry = irsymtable_find(st, lv->u.var.name);
                    if (!entry) return -1;
                    if (entry->is_global)
                        dst = emit_gaddr(fn, slot_global_name(entry), e->loc, entry->is_tls);
                    else
                        dst = emit_bin_w(fn, IR_ADDR, entry->slot, -1, 8, 1, e->loc);
                } else {
                    dst = lower_lvalue_addr(fn, st, lv);
                }
                emit_struct_copy(fn, dst, coerced, 16, e->loc);
                return coerced;
            }
        } else if (lv->type.is_bool) {
            IRValue n = bool_normalize(fn, rv, rw, ru, rf, e->loc);
            coerced = coerce(fn, n, 4, 0, lw, lu, e->loc);
        } else if (rf != lf)
            coerced = convert_numeric(fn, rv, rw, lw, lu, lf, e->loc);
        else if (lf)
            coerced = (rw == lw) ? rv : convert_numeric(fn, rv, rw, lw, lu, 1, e->loc);
        else
            coerced = coerce(fn, rv, rw, ru, lw, lu, e->loc);
        /* Slice 13 — struct lvalue: both sides are pointers (a struct value is
         * its byte address), so copy the bytes instead of storing the pointer. */
        if (lv->type.kind == TY_STRUCT || lv->type.is_vector || type_is_i128(lv->type)) {
            IRValue src = rv;
            if (type_is_i128(lv->type))
                src = i128_as_addr(fn, rv, e->u.assign.rvalue->type,
                                   lv->type.is_unsigned, e->loc);
            IRValue dst;
            if (lv->kind == EX_VAR) {
                const IRSlot *entry = irsymtable_find(st, lv->u.var.name);
                if (!entry) return -1;
                if (entry->is_global)
                    dst = emit_gaddr(fn, slot_global_name(entry), e->loc, entry->is_tls);
                else
                    dst = emit_bin_w(fn, IR_ADDR, entry->slot, -1, 8, 1, e->loc);
            } else if (lv->kind == EX_DEREF) {
                dst = lower_expr(fn, st, lv->u.deref.operand);
            } else if (lv->kind == EX_INDEX || lv->kind == EX_MEMBER) {
                dst = lower_lvalue_addr(fn, st, lv);
            } else {
                return coerced;
            }
            if (lv->type.tag && strncmp(lv->type.tag, "__complex_", 10) == 0 &&
                (e->u.assign.rvalue->type.kind != TY_STRUCT ||
                 !e->u.assign.rvalue->type.tag ||
                 strcmp(lv->type.tag, e->u.assign.rvalue->type.tag) != 0)) {
                /* Scalar to complex assignment or different precision complex */
                Expr fake_cast;
                memset(&fake_cast, 0, sizeof(fake_cast));
                fake_cast.kind = EX_CAST;
                fake_cast.type = lv->type;
                fake_cast.loc = e->loc;
                fake_cast.u.cast.target = lv->type;
                fake_cast.u.cast.operand = e->u.assign.rvalue;
                IRValue cplx_rv = lower_expr(fn, st, &fake_cast);
                emit_struct_copy(fn, dst, cplx_rv, type_size(lv->type), e->loc);
                return dst;
            }
            emit_struct_copy(fn, dst, src, type_size(lv->type), e->loc);
            return dst;
        }
        if (lv->kind == EX_VAR) {
            const IRSlot *entry = irsymtable_find(st, lv->u.var.name);
            if (!entry) return -1;
            if (entry->is_global) {
                IRValue addr = emit_gaddr(fn, slot_global_name(entry), e->loc, entry->is_tls);
                emit_inst_w(fn, IR_STORE_PTR, -1, addr, coerced, 0, lw, lu, e->loc);
            } else if (entry->pinned) {
                IRValue addr = emit_bin_w(fn, IR_ADDR, entry->slot, -1, 8, 1, e->loc);
                emit_inst_w(fn, IR_STORE_PTR, -1, addr, coerced, 0, lw, lu, e->loc);
            } else {
                emit_inst_w(fn, IR_STORE, -1, entry->slot, coerced, 0, lw, lu, e->loc);
            }
            return coerced;
        } else if (lv->kind == EX_DEREF) {
            IRValue ptr = lower_expr(fn, st, lv->u.deref.operand);
            emit_inst_w(fn, IR_STORE_PTR, -1, ptr, coerced, 0, lw, lu, e->loc);
            return coerced;
        } else if (lv->kind == EX_INDEX || lv->kind == EX_MEMBER) {
            IRValue addr = lower_lvalue_addr(fn, st, lv);
            int bit_width = 0, bit_offset = 0, unit_width = 0, is_be = 0;
            if (lv->kind == EX_MEMBER
                && member_bitfield(lv, &bit_width, &bit_offset, &unit_width, &is_be, NULL)) {
                /* Bitfield store: read-modify-write the storage unit. */
                BFUnit unit = emit_bf_load(fn, addr, bit_offset, bit_width,
                                           unit_width, is_be, e->loc);
                int uw = bf_work_width(&unit);
                unit = emit_bf_insert(fn, unit, coerced, bit_offset, bit_width, e->loc);
                emit_bf_store(fn, addr, unit, is_be, e->loc);
                int64_t mask = bitfield_mask64(bit_width);
                IRValue m = emit_iconst_w(fn, mask, uw, 1, e->loc);
                IRValue vm = new_value(fn);
                IRValue cv = coerce(fn, coerced, get_value_width(fn, coerced),
                                    get_value_is_unsigned(fn, coerced), uw, 1, e->loc);
                emit_inst_w(fn, IR_BAND, vm, cv, m, 0, uw, 1, e->loc);
                if (!e->type.is_unsigned && bit_width < uw * 8) {
                    int shift = uw * 8 - bit_width;
                    IRValue s_val = emit_iconst_w(fn, shift, 8, 1, e->loc);
                    IRValue shl = new_value(fn);
                    emit_inst_w(fn, IR_SHL, shl, vm, s_val, 0, uw, 0, e->loc);
                    IRValue ashr = new_value(fn);
                    emit_inst_w(fn, IR_SHR, ashr, shl, s_val, 0, uw, 0, e->loc);
                    return ashr;
                }
                return vm;
            }
            IRValue to_store = coerced;
            if (member_struct_be(lv) && (lw == 2 || lw == 4 || lw == 8)
                && lv->type.kind != TY_STRUCT && lv->type.kind != TY_ARRAY)
                to_store = emit_bswap_val(fn, coerced, lw, e->loc);
            emit_inst_w(fn, IR_STORE_PTR, -1, addr, to_store, 0, lw, lu, e->loc);
            return coerced;
        }
        return coerced;
    }
    case EX_CALL: {
        if (e->u.call.callee && e->u.call.callee->kind == EX_VAR
            && e->u.call.callee->u.var.name) {
            const char *pack_cn = e->u.call.callee->u.var.name;
            if (strcmp(pack_cn, "__builtin_va_arg_pack_len") == 0) {
                int n = (g_vapack_depth > 0)
                      ? (int)g_vapack_stack[g_vapack_depth - 1].extra_len : 0;
                IRValue v = new_value(fn);
                emit_inst_w(fn, IR_CONST, v, -1, -1, n, 4, 0, e->loc);
                set_value_type(fn, v, 4, 0);
                return v;
            }
            if (strcmp(pack_cn, "__builtin_va_arg_pack") == 0) {
                /* Valid uses are spliced into the enclosing call below. */
                IRValue v = new_value(fn);
                emit_inst_w(fn, IR_CONST, v, -1, -1, 0, 4, 0, e->loc);
                set_value_type(fn, v, 4, 0);
                return v;
            }
            if (e->u.call.args.len > 0
                && expr_is_va_arg_pack(e->u.call.args.data[e->u.call.args.len - 1])) {
                if (g_vapack_depth <= 0) {
                    return die_at(e->loc.file, e->loc.line, e->loc.col,
                           "invalid use of __builtin_va_arg_pack ()");
                }
                VaArgPackFrame *fr = &g_vapack_stack[g_vapack_depth - 1];
                size_t keep = e->u.call.args.len - 1;
                size_t n = keep + fr->extra_len;
                Expr **args = malloc((n ? n : 1) * sizeof(Expr *));
                if (!args) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
                for (size_t i = 0; i < keep; i++)
                    args[i] = e->u.call.args.data[i];
                for (size_t i = 0; i < fr->extra_len; i++)
                    args[keep + i] = fr->extra[i];
                Expr spliced = *e;
                spliced.u.call.args.data = args;
                spliced.u.call.args.len = n;
                spliced.u.call.args.cap = n;
                IRValue r = lower_expr(fn, st, &spliced);
                free(args);
                return r;
            }
            const FunctionDecl *pack_fd = find_fn_with_body(pack_cn);
            if (pack_fd && fn_uses_va_arg_pack(pack_fd))
                return lower_va_arg_pack_inline(fn, st, e, pack_fd);
        }
        if (e->u.call.callee->kind == EX_VAR) {
            const char *bos_cn = e->u.call.callee->u.var.name;
            if (strcmp(bos_cn, "__builtin_object_size") == 0 && e->u.call.args.len >= 1) {
                long long bos_type = 0;
                if (e->u.call.args.len >= 2)
                    fold_const_int(e->u.call.args.data[1], &bos_type);
                unsigned long long sz = compute_builtin_object_size(st, e->u.call.args.data[0], (int)bos_type);
                IRValue v = new_value(fn);
                emit_inst_w(fn, IR_CONST, v, -1, -1, (int64_t)sz, 8, 1, e->loc);
                set_value_type(fn, v, 8, 1);
                return v;
            }
            if (bos_is_snprintf_chk_builtin(bos_cn) && e->u.call.args.len >= 5)
                return lower_fortify_snprintf_chk_call(fn, st, e);
            if (bos_is_sprintf_chk_builtin(bos_cn) && e->u.call.args.len >= 4)
                return lower_fortify_sprintf_chk_call(fn, st, e);
            if (bos_is_stpcpy_chk_builtin(bos_cn) && e->u.call.args.len >= 3)
                return lower_fortify_stpcpy_chk_call(fn, st, e);
            if (bos_is_strcat_chk_builtin(bos_cn) && e->u.call.args.len >= 3)
                return lower_fortify_strcat_chk_call(fn, st, e);
            if (bos_is_strncat_chk_builtin(bos_cn) && e->u.call.args.len >= 4)
                return lower_fortify_strncat_chk_call(fn, st, e);
            if (bos_is_chk_builtin(bos_cn) && e->u.call.args.len >= 4)
                return lower_fortify_chk_call(fn, st, e);
        }
        if (e->u.call.callee->kind == EX_VAR && e->u.call.args.len > 0) {
            const char *cn = e->u.call.callee->u.var.name;
            if (strcmp(cn, "__builtin_conjf") == 0 || strcmp(cn, "__builtin_conj") == 0 ||
                strcmp(cn, "__builtin_conjl") == 0 ||
                strcmp(cn, "conjf") == 0 || strcmp(cn, "conj") == 0 || strcmp(cn, "conjl") == 0) {
                Expr fake_un;
                memset(&fake_un, 0, sizeof(fake_un));
                fake_un.kind = EX_UNARY;
                fake_un.type = e->type;
                fake_un.loc = e->loc;
                fake_un.u.un.op = UOP_BITNOT;
                fake_un.u.un.operand = e->u.call.args.data[0];
                return lower_expr(fn, st, &fake_un);
            }
            if (strcmp(cn, "__builtin_crealf") == 0 || strcmp(cn, "__builtin_creal") == 0 ||
                strcmp(cn, "__builtin_creall") == 0 ||
                strcmp(cn, "crealf") == 0 || strcmp(cn, "creal") == 0 || strcmp(cn, "creall") == 0 ||
                strcmp(cn, "__builtin_cimagf") == 0 || strcmp(cn, "__builtin_cimag") == 0 ||
                strcmp(cn, "__builtin_cimagl") == 0 ||
                strcmp(cn, "cimagf") == 0 || strcmp(cn, "cimag") == 0 || strcmp(cn, "cimagl") == 0) {
                int imag = strstr(cn, "cimag") != NULL;
                Type cty = e->u.call.args.data[0]->type;
                int total_sz = type_size(cty);
                int elem_sz = total_sz / 2;
                int is_unsigned = (cty.tag && strstr(cty.tag, "unsigned") != NULL);
                int is_float = (cty.tag && (strstr(cty.tag, "float") || strstr(cty.tag, "double") ||
                                            strstr(cty.tag, "ldouble")));
                IRValue op_addr = lower_expr(fn, st, e->u.call.args.data[0]);
                if (imag) {
                    IRValue off = new_value(fn);
                    emit_inst_w(fn, IR_CONST, off, -1, -1, elem_sz, 8, 1, e->loc);
                    op_addr = emit_bin_w(fn, IR_ADD, op_addr, off, 8, 1, e->loc);
                }
                IRValue v = new_value(fn);
                emit_inst_w(fn, IR_LOAD_PTR, v, op_addr, -1, 0, elem_sz, is_unsigned, e->loc);
                if (is_float) set_value_float(fn, v, 1);
                return v;
            }
        }
        if (e->u.call.callee->kind == EX_VAR &&
            strncmp(e->u.call.callee->u.var.name, "__sync_", 7) == 0) {
            const char *sname = e->u.call.callee->u.var.name;
            if (strcmp(sname, "__sync_synchronize") == 0) {
                /* Full barrier.  This clang uses dmb ish, option 11. */
                emit_fakecc_dmb(fn, 11, e->loc);
                return -1;
            }
            IRValue addr = lower_expr(fn, st, e->u.call.args.data[0]);
            int sz = type_size(e->type);
            int is_u = e->type.is_unsigned;
            if (strcmp(sname, "__sync_lock_release") == 0) {
                sz = 4;
                if (e->u.call.args.data[0]->type.kind == TY_PTR && e->u.call.args.data[0]->type.pointee) {
                    sz = type_size(*e->u.call.args.data[0]->type.pointee);
                    is_u = e->u.call.args.data[0]->type.pointee->is_unsigned;
                }
                IRValue zero = new_value(fn);
                emit_inst_w(fn, IR_CONST, zero, -1, -1, 0, sz, is_u, e->loc);
                /* Release store.  stlr covers 1/2/4/8; other sizes stay plain. */
                int ak = (sz == 1 || sz == 2 || sz == 4 || sz == 8) ? 3 : 0;
                emit_inst_w(fn, IR_STORE_PTR, -1, addr, zero, ak, sz, is_u, e->loc);
                return -1;
            }
            if (strcmp(sname, "__sync_bool_compare_and_swap") == 0 ||
                strcmp(sname, "__sync_val_compare_and_swap") == 0) {
                int is_bool = strcmp(sname, "__sync_bool_compare_and_swap") == 0;
                int val_sz = sz;
                int val_u = is_u;
                int is_f = 0;
                if (e->u.call.args.data[0]->type.kind == TY_PTR && e->u.call.args.data[0]->type.pointee) {
                    Type *pt = e->u.call.args.data[0]->type.pointee;
                    val_sz = type_size(*pt);
                    val_u = pt->is_unsigned;
                    is_f = pt->kind == TY_FLOAT;
                }
                IRValue old_expected = lower_expr(fn, st, e->u.call.args.data[1]);
                IRValue new_desired = lower_expr(fn, st, e->u.call.args.data[2]);
                /* __sync is a full barrier, so this is casal.  Float and
                 * odd sizes stay a plain pair. */
                if (!is_f && (val_sz == 1 || val_sz == 2 || val_sz == 4 || val_sz == 8)) {
                    IRValue oldv = emit_fakecc_cas_old(fn, addr, old_expected, new_desired,
                                                      3, val_sz, val_u, e->loc);
                    if (val_sz < 8 && !(val_u && val_sz == 4)) {
                        IRValue n = new_value(fn);
                        emit_inst_w(fn, val_u ? IR_ZEXT : IR_SEXT, n, oldv, -1, 0,
                                    8, val_u, e->loc);
                        set_value_type(fn, n, val_sz, val_u);
                        oldv = n;
                    }
                    if (is_bool)
                        return emit_bin_w(fn, IR_EQ, oldv, old_expected, 4, 0, e->loc);
                    return oldv;
                }
                IRValue cur_val = new_value(fn);
                emit_inst_w(fn, IR_LOAD_PTR, cur_val, addr, -1, 0, val_sz, val_u, e->loc);
                set_value_type(fn, cur_val, val_sz, val_u);
                int L_match = new_label(fn);
                int L_done = new_label(fn);
                IRValue eq = emit_bin_w(fn, IR_EQ, cur_val, old_expected, 4, 0, e->loc);
                emit_inst_w(fn, IR_CBR, -1, eq, L_done, L_match, 4, 0, e->loc);
                emit_inst_w(fn, IR_LABEL, -1, -1, -1, L_match, 0, 0, e->loc);
                emit_inst_w(fn, IR_STORE_PTR, -1, addr, new_desired, 0, val_sz, val_u, e->loc);
                emit_inst_w(fn, IR_LABEL, -1, -1, -1, L_done, 0, 0, e->loc);
                if (strcmp(sname, "__sync_bool_compare_and_swap") == 0) {
                    return eq;
                } else {
                    return cur_val;
                }
            }
            if (strcmp(sname, "__sync_lock_test_and_set") == 0) {
                IRValue new_val = lower_expr(fn, st, e->u.call.args.data[1]);
                /* Acquire exchange.  Odd sizes stay a plain pair. */
                if (sz == 1 || sz == 2 || sz == 4 || sz == 8)
                    return emit_fakecc_swp(fn, addr, new_val, 1, sz, is_u, e->loc);
                IRValue old_val = new_value(fn);
                emit_inst_w(fn, IR_LOAD_PTR, old_val, addr, -1, 0, sz, is_u, e->loc);
                set_value_type(fn, old_val, sz, is_u);
                emit_inst_w(fn, IR_STORE_PTR, -1, addr, new_val, 0, sz, is_u, e->loc);
                return old_val;
            }
            IRValue delta = lower_expr(fn, st, e->u.call.args.data[1]);
            int op = IR_ADD;
            int is_nand = strstr(sname, "nand") != NULL;
            if (!is_nand) {
                if (strstr(sname, "_sub_") || strstr(sname, "fetch_and_sub")) op = IR_SUB;
                else if (strstr(sname, "_and_and_") || strstr(sname, "fetch_and_and")) op = IR_BAND;
                else if (strstr(sname, "_or_and_") || strstr(sname, "fetch_and_or")) op = IR_BOR;
                else if (strstr(sname, "_xor_and_") || strstr(sname, "fetch_and_xor")) op = IR_BXOR;
                else if (strstr(sname, "_add_") || strstr(sname, "fetch_and_add")) op = IR_ADD;
            }
            int fetch_old = strncmp(sname, "__sync_fetch_", 13) == 0;
            /* __sync_* is a full barrier, so the LSE op is the seq_cst form.
             * nand has no single instruction and stays a plain pair. */
            if (!is_nand && (sz == 1 || sz == 2 || sz == 4 || sz == 8)
                && (op == IR_ADD || op == IR_SUB || op == IR_BAND
                    || op == IR_BOR || op == IR_BXOR)) {
                const char *lse = "__fakecc_ldadd";
                IRValue mem_op = delta;
                IROpcode rop = op;
                IRValue rval = delta;
                if (op == IR_SUB) {
                    mem_op = new_value(fn);
                    emit_inst_w(fn, IR_NEG, mem_op, delta, -1, 0,
                                sz == 8 ? 8 : 4, 0, e->loc);
                    set_value_type(fn, mem_op, sz == 8 ? 8 : 4, 0);
                    rop = IR_ADD;
                    rval = mem_op;
                } else if (op == IR_BAND) {
                    lse = "__fakecc_ldclr";
                } else if (op == IR_BOR) {
                    lse = "__fakecc_ldset";
                } else if (op == IR_BXOR) {
                    lse = "__fakecc_ldeor";
                }
                IRValue oldv = emit_fakecc_lse(fn, lse, addr, mem_op, 3, sz, is_u, e->loc);
                if (fetch_old) return oldv;
                return atomic_add_result(fn, rop, oldv, rval, sz, is_u, e->loc);
            }
            IRValue old_val = new_value(fn);
            emit_inst_w(fn, IR_LOAD_PTR, old_val, addr, -1, 0, sz, is_u, e->loc);
            set_value_type(fn, old_val, sz, is_u);
            IRValue res = emit_bin_w(fn, op, old_val, delta, sz, is_u, e->loc);
            emit_inst_w(fn, IR_STORE_PTR, -1, addr, res, 0, sz, is_u, e->loc);
            if (fetch_old) return old_val;
            return res;
        }
        /* Same model as __sync_*: plain loads and stores.  Darwin images
         * do not link libatomic, and a missing call name used to crash
         * the arm64 backend. */
        if (e->u.call.callee->kind == EX_VAR &&
            strncmp(e->u.call.callee->u.var.name, "__atomic_", 9) == 0) {
            const char *sname = e->u.call.callee->u.var.name;
            if (strcmp(sname, "__atomic_thread_fence") == 0 ||
                strcmp(sname, "__atomic_signal_fence") == 0) {
                for (size_t i = 0; i < e->u.call.args.len; i++)
                    lower_expr(fn, st, e->u.call.args.data[i]);
                /* signal_fence is only a compiler barrier.  thread_fence
                 * matches this clang: relaxed is empty, acquire and consume
                 * are dmb ishld (9), and release and stronger are dmb ish (11). */
                if (strcmp(sname, "__atomic_thread_fence") == 0) {
                    long long ord = 5;
                    if (e->u.call.args.len >= 1)
                        fold_const_int(e->u.call.args.data[0], &ord);
                    if (ord != 0) {
                        int crm = (ord == 1 || ord == 2) ? 9 : 11;
                        emit_fakecc_dmb(fn, crm, e->loc);
                    }
                }
                return -1;
            }
            if (e->u.call.args.len < 1)
                return -1;
            Expr *p0 = e->u.call.args.data[0];
            int sz = 4, is_u = 0, is_f = 0;
            if (p0->type.kind == TY_PTR && p0->type.pointee) {
                int psz = type_size(*p0->type.pointee);
                if (psz > 0) sz = psz;
                is_u = p0->type.pointee->is_unsigned;
                is_f = p0->type.pointee->kind == TY_FLOAT;
            }
            IRValue addr = lower_expr(fn, st, p0);
            if (strcmp(sname, "__atomic_load_n") == 0) {
                long long ord = 5;
                if (e->u.call.args.len > 1) {
                    lower_expr(fn, st, e->u.call.args.data[1]);
                    fold_const_int(e->u.call.args.data[1], &ord);
                }
                /* 1 = ldapr (acquire/consume), 2 = ldar (seq_cst).
                 * Float and double use the same integer load, then fmov. */
                int ak = 0;
                int barrier = !is_f || sz == 4 || sz == 8;
                if (barrier && (ord == 1 || ord == 2)) ak = 1;
                else if (barrier && ord >= 5) ak = 2;
                IRValue v = new_value(fn);
                emit_inst_w(fn, IR_LOAD_PTR, v, addr, -1, ak, sz, is_u, e->loc);
                set_value_type(fn, v, sz, is_u);
                if (is_f) set_value_float(fn, v, 1);
                return v;
            }
            if (strcmp(sname, "__atomic_store_n") == 0) {
                if (e->u.call.args.len < 2) return -1;
                IRValue val = lower_expr(fn, st, e->u.call.args.data[1]);
                long long ord = 5;
                if (e->u.call.args.len > 2) {
                    lower_expr(fn, st, e->u.call.args.data[2]);
                    fold_const_int(e->u.call.args.data[2], &ord);
                }
                /* 3 = stlr (release, acq_rel, seq_cst).  Float bits go via a GPR. */
                int barrier = !is_f || sz == 4 || sz == 8;
                int ak = (barrier && (ord == 3 || ord == 4 || ord >= 5)) ? 3 : 0;
                emit_inst_w(fn, IR_STORE_PTR, -1, addr, val, ak, sz, is_u, e->loc);
                return -1;
            }
            if (strcmp(sname, "__atomic_exchange_n") == 0) {
                if (e->u.call.args.len < 2) return -1;
                IRValue val = lower_expr(fn, st, e->u.call.args.data[1]);
                long long ord = 5;
                if (e->u.call.args.len > 2) {
                    lower_expr(fn, st, e->u.call.args.data[2]);
                    fold_const_int(e->u.call.args.data[2], &ord);
                }
                /* Oversized objects stay a plain pair.  Integer 1/2/4/8 and
                 * float/double use one SWP so the read and write cannot tear. */
                if ((is_f && sz != 4 && sz != 8) ||
                    (!is_f && sz != 1 && sz != 2 && sz != 4 && sz != 8)) {
                    IRValue oldv = new_value(fn);
                    emit_inst_w(fn, IR_LOAD_PTR, oldv, addr, -1, 0, sz, is_u, e->loc);
                    set_value_type(fn, oldv, sz, is_u);
                    if (is_f) set_value_float(fn, oldv, 1);
                    emit_inst_w(fn, IR_STORE_PTR, -1, addr, val, 0, sz, is_u, e->loc);
                    return oldv;
                }
                /* 0 relaxed, 1 acquire (consume too), 2 release, 3 acq_rel/seq_cst. */
                int kind = 3;
                if (ord == 0) kind = 0;
                else if (ord == 1 || ord == 2) kind = 1;
                else if (ord == 3) kind = 2;
                IRValue oldv = emit_fakecc_swp(fn, addr, val, kind, sz, is_u, e->loc);
                if (is_f) set_value_float(fn, oldv, 1);
                return oldv;
            }
            if (strcmp(sname, "__atomic_load") == 0) {
                if (e->u.call.args.len < 2) return -1;
                IRValue retp = lower_expr(fn, st, e->u.call.args.data[1]);
                long long ord = 5;
                if (e->u.call.args.len > 2) {
                    lower_expr(fn, st, e->u.call.args.data[2]);
                    fold_const_int(e->u.call.args.data[2], &ord);
                }
                /* Same barrier as __atomic_load_n.  The copy into ret is plain. */
                int ak = 0;
                int barrier = !is_f || sz == 4 || sz == 8;
                if (barrier && (ord == 1 || ord == 2)) ak = 1;
                else if (barrier && ord >= 5) ak = 2;
                IRValue v = new_value(fn);
                emit_inst_w(fn, IR_LOAD_PTR, v, addr, -1, ak, sz, is_u, e->loc);
                set_value_type(fn, v, sz, is_u);
                if (is_f) set_value_float(fn, v, 1);
                emit_inst_w(fn, IR_STORE_PTR, -1, retp, v, 0, sz, is_u, e->loc);
                return -1;
            }
            if (strcmp(sname, "__atomic_store") == 0) {
                if (e->u.call.args.len < 2) return -1;
                IRValue vp = lower_expr(fn, st, e->u.call.args.data[1]);
                long long ord = 5;
                if (e->u.call.args.len > 2) {
                    lower_expr(fn, st, e->u.call.args.data[2]);
                    fold_const_int(e->u.call.args.data[2], &ord);
                }
                IRValue v = new_value(fn);
                emit_inst_w(fn, IR_LOAD_PTR, v, vp, -1, 0, sz, is_u, e->loc);
                set_value_type(fn, v, sz, is_u);
                if (is_f) set_value_float(fn, v, 1);
                /* Same barrier as __atomic_store_n.  The load of *val is plain. */
                int barrier = !is_f || sz == 4 || sz == 8;
                int ak = (barrier && (ord == 3 || ord == 4 || ord >= 5)) ? 3 : 0;
                emit_inst_w(fn, IR_STORE_PTR, -1, addr, v, ak, sz, is_u, e->loc);
                return -1;
            }
            if (strcmp(sname, "__atomic_exchange") == 0) {
                if (e->u.call.args.len < 3) return -1;
                IRValue vp = lower_expr(fn, st, e->u.call.args.data[1]);
                IRValue retp = lower_expr(fn, st, e->u.call.args.data[2]);
                long long ord = 5;
                if (e->u.call.args.len > 3) {
                    lower_expr(fn, st, e->u.call.args.data[3]);
                    fold_const_int(e->u.call.args.data[3], &ord);
                }
                /* *val is a plain load.  Only the object is exchanged. */
                IRValue val = new_value(fn);
                emit_inst_w(fn, IR_LOAD_PTR, val, vp, -1, 0, sz, is_u, e->loc);
                set_value_type(fn, val, sz, is_u);
                if (is_f) set_value_float(fn, val, 1);
                if ((is_f && sz != 4 && sz != 8) ||
                    (!is_f && sz != 1 && sz != 2 && sz != 4 && sz != 8)) {
                    IRValue oldv = new_value(fn);
                    emit_inst_w(fn, IR_LOAD_PTR, oldv, addr, -1, 0, sz, is_u, e->loc);
                    set_value_type(fn, oldv, sz, is_u);
                    if (is_f) set_value_float(fn, oldv, 1);
                    emit_inst_w(fn, IR_STORE_PTR, -1, addr, val, 0, sz, is_u, e->loc);
                    emit_inst_w(fn, IR_STORE_PTR, -1, retp, oldv, 0, sz, is_u, e->loc);
                    return -1;
                }
                int kind = 3;
                if (ord == 0) kind = 0;
                else if (ord == 1 || ord == 2) kind = 1;
                else if (ord == 3) kind = 2;
                IRValue oldv = emit_fakecc_swp(fn, addr, val, kind, sz, is_u, e->loc);
                if (is_f) set_value_float(fn, oldv, 1);
                emit_inst_w(fn, IR_STORE_PTR, -1, retp, oldv, 0, sz, is_u, e->loc);
                return -1;
            }
            if (strcmp(sname, "__atomic_clear") == 0) {
                long long ord = 5;
                if (e->u.call.args.len > 1) {
                    lower_expr(fn, st, e->u.call.args.data[1]);
                    fold_const_int(e->u.call.args.data[1], &ord);
                }
                /* The flag is one byte.  Release and stronger use stlrb. */
                IRValue zero = new_value(fn);
                emit_inst_w(fn, IR_CONST, zero, -1, -1, 0, 1, 1, e->loc);
                int ak = (ord == 3 || ord == 4 || ord >= 5) ? 3 : 0;
                emit_inst_w(fn, IR_STORE_PTR, -1, addr, zero, ak, 1, 1, e->loc);
                return -1;
            }
            if (strcmp(sname, "__atomic_test_and_set") == 0) {
                long long ord = 5;
                if (e->u.call.args.len > 1) {
                    lower_expr(fn, st, e->u.call.args.data[1]);
                    fold_const_int(e->u.call.args.data[1], &ord);
                }
                /* Swap the byte with 1.  The result is a bool: nonzero → 1. */
                IRValue one = new_value(fn);
                emit_inst_w(fn, IR_CONST, one, -1, -1, 1, 1, 1, e->loc);
                int kind = 3;
                if (ord == 0) kind = 0;
                else if (ord == 1 || ord == 2) kind = 1;
                else if (ord == 3) kind = 2;
                IRValue oldv = emit_fakecc_swp(fn, addr, one, kind, 1, 1, e->loc);
                IRValue zero = new_value(fn);
                emit_inst_w(fn, IR_CONST, zero, -1, -1, 0, 4, 0, e->loc);
                return emit_bin_w(fn, IR_NE, oldv, zero, 4, 0, e->loc);
            }
            if (strcmp(sname, "__atomic_compare_exchange_n") == 0 ||
                strcmp(sname, "__atomic_compare_exchange") == 0) {
                int is_n = strcmp(sname, "__atomic_compare_exchange_n") == 0;
                if (e->u.call.args.len < 3) return -1;
                IRValue exp_ptr = lower_expr(fn, st, e->u.call.args.data[1]);
                IRValue desired;
                if (is_n) {
                    desired = lower_expr(fn, st, e->u.call.args.data[2]);
                } else {
                    IRValue dp = lower_expr(fn, st, e->u.call.args.data[2]);
                    desired = new_value(fn);
                    emit_inst_w(fn, IR_LOAD_PTR, desired, dp, -1, 0, sz, is_u, e->loc);
                    set_value_type(fn, desired, sz, is_u);
                    if (is_f) set_value_float(fn, desired, 1);
                }
                for (size_t i = 3; i < e->u.call.args.len; i++)
                    lower_expr(fn, st, e->u.call.args.data[i]);
                /* Success order picks cas/casa/casl/casal.  A strong CAS
                 * is a valid weak CAS.  Odd sizes stay a plain pair.
                 * Float and double compare the IEEE bits. */
                if ((!is_f && (sz == 1 || sz == 2 || sz == 4 || sz == 8))
                    || (is_f && (sz == 4 || sz == 8))) {
                    long long succ = 5;
                    if (e->u.call.args.len > 4)
                        fold_const_int(e->u.call.args.data[4], &succ);
                    int kind = 3;
                    if (succ == 0) kind = 0;
                    else if (succ == 1 || succ == 2) kind = 1;
                    else if (succ == 3) kind = 2;
                    return emit_fakecc_cas(fn, addr, exp_ptr, desired, kind, sz, e->loc);
                }
                IRValue cur = new_value(fn);
                emit_inst_w(fn, IR_LOAD_PTR, cur, addr, -1, 0, sz, is_u, e->loc);
                set_value_type(fn, cur, sz, is_u);
                if (is_f) set_value_float(fn, cur, 1);
                IRValue expv = new_value(fn);
                emit_inst_w(fn, IR_LOAD_PTR, expv, exp_ptr, -1, 0, sz, is_u, e->loc);
                set_value_type(fn, expv, sz, is_u);
                if (is_f) set_value_float(fn, expv, 1);
                IRValue eq = emit_bin_w(fn, IR_EQ, cur, expv, 4, 0, e->loc);
                int Lok = new_label(fn);
                int Lfail = new_label(fn);
                int Ldone = new_label(fn);
                emit_inst_w(fn, IR_CBR, -1, eq, Lfail, Lok, 4, 0, e->loc);
                emit_inst_w(fn, IR_LABEL, -1, -1, -1, Lok, 0, 0, e->loc);
                emit_inst_w(fn, IR_STORE_PTR, -1, addr, desired, 0, sz, is_u, e->loc);
                emit_inst(fn, IR_BR, -1, -1, -1, Ldone, e->loc);
                emit_inst_w(fn, IR_LABEL, -1, -1, -1, Lfail, 0, 0, e->loc);
                emit_inst_w(fn, IR_STORE_PTR, -1, exp_ptr, cur, 0, sz, is_u, e->loc);
                emit_inst_w(fn, IR_LABEL, -1, -1, -1, Ldone, 0, 0, e->loc);
                return eq;
            }
            {
                int opc = 0, fetch_old = 0, is_nand = 0, is_rmw = 1;
                if (strcmp(sname, "__atomic_add_fetch") == 0) opc = IR_ADD;
                else if (strcmp(sname, "__atomic_fetch_add") == 0) { opc = IR_ADD; fetch_old = 1; }
                else if (strcmp(sname, "__atomic_sub_fetch") == 0) opc = IR_SUB;
                else if (strcmp(sname, "__atomic_fetch_sub") == 0) { opc = IR_SUB; fetch_old = 1; }
                else if (strcmp(sname, "__atomic_and_fetch") == 0) opc = IR_BAND;
                else if (strcmp(sname, "__atomic_fetch_and") == 0) { opc = IR_BAND; fetch_old = 1; }
                else if (strcmp(sname, "__atomic_or_fetch") == 0) opc = IR_BOR;
                else if (strcmp(sname, "__atomic_fetch_or") == 0) { opc = IR_BOR; fetch_old = 1; }
                else if (strcmp(sname, "__atomic_xor_fetch") == 0) opc = IR_BXOR;
                else if (strcmp(sname, "__atomic_fetch_xor") == 0) { opc = IR_BXOR; fetch_old = 1; }
                else if (strcmp(sname, "__atomic_nand_fetch") == 0) is_nand = 1;
                else if (strcmp(sname, "__atomic_fetch_nand") == 0) { is_nand = 1; fetch_old = 1; }
                else is_rmw = 0;
                if (is_rmw) {
                    if (e->u.call.args.len < 2) return -1;
                    IRValue delta = lower_expr(fn, st, e->u.call.args.data[1]);
                    long long ord = 5;
                    if (e->u.call.args.len > 2) {
                        lower_expr(fn, st, e->u.call.args.data[2]);
                        fold_const_int(e->u.call.args.data[2], &ord);
                    }
                    /* Float add/sub is a CAS retry.  Integer add/sub are LDADD. */
                    if (is_f && (sz == 4 || sz == 8)
                        && (opc == IR_ADD || opc == IR_SUB)) {
                        int kind = 3;
                        if (ord == 0) kind = 0;
                        else if (ord == 1 || ord == 2) kind = 1;
                        else if (ord == 3) kind = 2;
                        IRValue r = emit_fakecc_fadd(fn, addr, delta, kind,
                                                    opc == IR_SUB, fetch_old,
                                                    sz, e->loc);
                        set_value_float(fn, r, 1);
                        return r;
                    }
                    /* Add/sub are LDADD (sub adds the negation).  and is
                     * LDCLR of the inverted mask, or is LDSET, xor is LDEOR.
                     * nand has no single LSE op and stays a plain pair. */
                    if (!is_f && !is_nand && (sz == 1 || sz == 2 || sz == 4 || sz == 8)
                        && (opc == IR_ADD || opc == IR_SUB || opc == IR_BAND
                            || opc == IR_BOR || opc == IR_BXOR)) {
                        const char *lse = "__fakecc_ldadd";
                        IRValue mem_op = delta;
                        IROpcode rop = opc;
                        IRValue rval = delta;
                        if (opc == IR_SUB) {
                            mem_op = new_value(fn);
                            emit_inst_w(fn, IR_NEG, mem_op, delta, -1, 0,
                                        sz == 8 ? 8 : 4, 0, e->loc);
                            set_value_type(fn, mem_op, sz == 8 ? 8 : 4, 0);
                            rop = IR_ADD;
                            rval = mem_op;
                        } else if (opc == IR_BAND) {
                            lse = "__fakecc_ldclr";
                        } else if (opc == IR_BOR) {
                            lse = "__fakecc_ldset";
                        } else if (opc == IR_BXOR) {
                            lse = "__fakecc_ldeor";
                        }
                        int kind = 3;
                        if (ord == 0) kind = 0;
                        else if (ord == 1 || ord == 2) kind = 1;
                        else if (ord == 3) kind = 2;
                        IRValue oldv = emit_fakecc_lse(fn, lse, addr, mem_op, kind,
                                                       sz, is_u, e->loc);
                        if (fetch_old) return oldv;
                        return atomic_add_result(fn, rop, oldv, rval, sz, is_u, e->loc);
                    }
                    IRValue oldv = new_value(fn);
                    emit_inst_w(fn, IR_LOAD_PTR, oldv, addr, -1, 0, sz, is_u, e->loc);
                    set_value_type(fn, oldv, sz, is_u);
                    IRValue res;
                    if (is_nand) {
                        IRValue both = emit_bin_w(fn, IR_BAND, oldv, delta, sz, is_u, e->loc);
                        res = new_value(fn);
                        emit_inst_w(fn, IR_BNOT, res, both, -1, 0, sz, is_u, e->loc);
                        set_value_type(fn, res, sz, is_u);
                    } else {
                        res = emit_bin_w(fn, opc, oldv, delta, sz, is_u, e->loc);
                    }
                    emit_inst_w(fn, IR_STORE_PTR, -1, addr, res, 0, sz, is_u, e->loc);
                    return fetch_old ? oldv : res;
                }
            }
        }
        if (e->u.call.callee->kind == EX_VAR && strcmp(e->u.call.callee->u.var.name, "__builtin_prefetch") == 0) {
            for (size_t i = 0; i < e->u.call.args.len; i++)
                lower_expr(fn, st, e->u.call.args.data[i]);
            return -1;
        }
        if (e->u.call.callee->kind == EX_VAR && strcmp(e->u.call.callee->u.var.name, "__builtin_assume") == 0) {
            /* The condition is evaluated for its side effects.  A false
             * condition is undefined; the freestanding image does not trap. */
            if (e->u.call.args.len >= 1)
                lower_expr(fn, st, e->u.call.args.data[0]);
            return -1;
        }
        if (e->u.call.callee->kind == EX_VAR && strcmp(e->u.call.callee->u.var.name, "__builtin_unpredictable") == 0) {
            if (e->u.call.args.len >= 1)
                return lower_expr(fn, st, e->u.call.args.data[0]);
            return -1;
        }
        if (e->u.call.callee->kind == EX_VAR && strcmp(e->u.call.callee->u.var.name, "__builtin_frame_address") == 0) {
            long long level = 0;
            if (e->u.call.args.len > 0) fold_const_int(e->u.call.args.data[0], &level);
            IRValue v = new_value(fn);
            emit_inst_w(fn, IR_FRAME_ADDR, v, -1, -1, (int64_t)level, 8, 1, e->loc);
            set_value_type(fn, v, 8, 1);
            return v;
        }
        if (e->u.call.callee->kind == EX_VAR && strcmp(e->u.call.callee->u.var.name, "__builtin_return_address") == 0) {
            long long level = 0;
            if (e->u.call.args.len > 0) fold_const_int(e->u.call.args.data[0], &level);
            IRValue v = new_value(fn);
            emit_inst_w(fn, IR_RETURN_ADDR, v, -1, -1, (int64_t)level, 8, 1, e->loc);
            set_value_type(fn, v, 8, 1);
            return v;
        }
        if (e->u.call.callee->kind == EX_VAR && (strcmp(e->u.call.callee->u.var.name, "__builtin_alloca") == 0 ||
                                                strcmp(e->u.call.callee->u.var.name, "__builtin_alloca_with_align") == 0 ||
                                                strcmp(e->u.call.callee->u.var.name, "alloca") == 0)) {
            IRValue sz = lower_expr(fn, st, e->u.call.args.data[0]);
            /* Second argument is an alignment in bits.  -1 keeps the
             * historical 16-byte bump used by plain alloca. */
            IRValue al = -1;
            if (strcmp(e->u.call.callee->u.var.name, "__builtin_alloca_with_align") == 0
                && e->u.call.args.len > 1)
                al = lower_expr(fn, st, e->u.call.args.data[1]);
            IRValue v = new_value(fn);
            emit_inst_w(fn, IR_DYN_ALLOCA, v, sz, al, 0, 8, 1, e->loc);
            set_value_type(fn, v, 8, 1);
            fn->has_dyn_alloca = 1;
            return v;
        }
        if (e->u.call.callee->kind == EX_VAR && strcmp(e->u.call.callee->u.var.name, "__builtin_stack_save") == 0) {
            IRValue v = new_value(fn);
            emit_inst_w(fn, IR_STACK_SAVE, v, -1, -1, 0, 8, 1, e->loc);
            set_value_type(fn, v, 8, 1);
            fn->has_dyn_alloca = 1;
            return v;
        }
        if (e->u.call.callee->kind == EX_VAR && strcmp(e->u.call.callee->u.var.name, "__builtin_stack_restore") == 0) {
            IRValue ptr = lower_expr(fn, st, e->u.call.args.data[0]);
            emit_inst_w(fn, IR_STACK_RESTORE, -1, ptr, -1, 0, 8, 1, e->loc);
            fn->has_dyn_alloca = 1;
            return -1;
        }
        if (e->u.call.callee->kind == EX_VAR && strcmp(e->u.call.callee->u.var.name, "__builtin_setjmp") == 0) {
            IRValue buf_ptr = lower_expr(fn, st, e->u.call.args.data[0]);
            IRValue ret_slot = emit_alloca(fn, 4, 4, 0, e->loc);
            IRValue addr_ret = emit_bin_w(fn, IR_ADDR, ret_slot, -1, 8, 1, e->loc);
            IRValue zero = new_value(fn);
            emit_inst_w(fn, IR_CONST, zero, -1, -1, 0, 4, 0, e->loc);
            emit_inst_w(fn, IR_STORE_PTR, -1, addr_ret, zero, 0, 4, 0, e->loc);

            IRValue frame_val = new_value(fn);
            emit_inst_w(fn, IR_FRAME_ADDR, frame_val, -1, -1, 0, 8, 1, e->loc);
            emit_inst_w(fn, IR_STORE_PTR, -1, buf_ptr, frame_val, 0, 8, 1, e->loc);

            int L_resume = new_label(fn);
            int L_cont = new_label(fn);
            IRValue laddr = new_value(fn);
            emit_inst_w(fn, IR_LADDR, laddr, -1, -1, L_resume, 8, 1, e->loc);
            IRValue c8 = new_value(fn);
            emit_inst_w(fn, IR_CONST, c8, -1, -1, 8, 8, 1, e->loc);
            IRValue buf_off8 = emit_bin_w(fn, IR_ADD, buf_ptr, c8, 8, 1, e->loc);
            emit_inst_w(fn, IR_STORE_PTR, -1, buf_off8, laddr, 0, 8, 1, e->loc);

            IRValue sp_val = new_value(fn);
            emit_inst_w(fn, IR_STACK_SAVE, sp_val, -1, -1, 0, 8, 1, e->loc);
            IRValue c16 = new_value(fn);
            emit_inst_w(fn, IR_CONST, c16, -1, -1, 16, 8, 1, e->loc);
            IRValue buf_off16 = emit_bin_w(fn, IR_ADD, buf_ptr, c16, 8, 1, e->loc);
            emit_inst_w(fn, IR_STORE_PTR, -1, buf_off16, sp_val, 0, 8, 1, e->loc);

            emit_br(fn, L_cont, e->loc);
            emit_label(fn, L_resume, e->loc);
            IRValue addr_ret2 = emit_bin_w(fn, IR_ADDR, ret_slot, -1, 8, 1, e->loc);
            IRValue one = new_value(fn);
            emit_inst_w(fn, IR_CONST, one, -1, -1, 1, 4, 0, e->loc);
            emit_inst_w(fn, IR_STORE_PTR, -1, addr_ret2, one, 0, 4, 0, e->loc);
            emit_label(fn, L_cont, e->loc);

            IRValue addr_ret3 = emit_bin_w(fn, IR_ADDR, ret_slot, -1, 8, 1, e->loc);
            IRValue res = new_value(fn);
            emit_inst_w(fn, IR_LOAD_PTR, res, addr_ret3, -1, 0, 4, 0, e->loc);
            set_value_type(fn, res, 4, 0);
            fn->has_dyn_alloca = 1;
            return res;
        }
        if (e->u.call.callee->kind == EX_VAR && strcmp(e->u.call.callee->u.var.name, "__builtin_expect") == 0) {
            /* __builtin_expect(val, exp) returns val; the hint is for
             * branch prediction and does not affect the result.
             * Both arguments must be evaluated for side effects. */
            if (e->u.call.args.len >= 2) {
                IRValue exp = lower_expr(fn, st, e->u.call.args.data[1]);
                (void)exp;
                return lower_expr(fn, st, e->u.call.args.data[0]);
            }
            if (e->u.call.args.len >= 1)
                return lower_expr(fn, st, e->u.call.args.data[0]);
            IRValue v = new_value(fn);
            emit_inst_w(fn, IR_CONST, v, -1, -1, 0, 8, 0, e->loc);
            set_value_type(fn, v, 8, 0);
            return v;
        }
        if (e->u.call.callee->kind == EX_VAR &&
            (strcmp(e->u.call.callee->u.var.name, "__builtin_expect_with_probability") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_assume_aligned") == 0)) {
            /* Both return the first argument.  The rest are hints. */
            for (size_t i = 1; i < e->u.call.args.len; i++)
                lower_expr(fn, st, e->u.call.args.data[i]);
            if (e->u.call.args.len >= 1)
                return lower_expr(fn, st, e->u.call.args.data[0]);
            return -1;
        }
        if (e->u.call.callee->kind == EX_VAR &&
            (strcmp(e->u.call.callee->u.var.name, "__builtin_align_up") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_align_down") == 0)) {
            if (e->u.call.args.len < 2) return -1;
            int up = strcmp(e->u.call.callee->u.var.name, "__builtin_align_up") == 0;
            int w = type_size(e->type);
            if (w < 1) w = 8;
            int uns = e->type.is_unsigned || e->type.kind == TY_PTR;
            IRValue val = lower_expr(fn, st, e->u.call.args.data[0]);
            IRValue al = lower_expr(fn, st, e->u.call.args.data[1]);
            IRValue one = new_value(fn);
            emit_inst_w(fn, IR_CONST, one, -1, -1, 1, w, uns, e->loc);
            set_value_type(fn, one, w, uns);
            IRValue am1 = emit_bin_w(fn, IR_SUB, al, one, w, uns, e->loc);
            IRValue nmask = new_value(fn);
            emit_inst_w(fn, IR_BNOT, nmask, am1, -1, 0, w, uns, e->loc);
            set_value_type(fn, nmask, w, uns);
            IRValue base = val;
            if (up)
                base = emit_bin_w(fn, IR_ADD, val, am1, w, uns, e->loc);
            return emit_bin_w(fn, IR_BAND, base, nmask, w, uns, e->loc);
        }
        if (e->u.call.callee->kind == EX_VAR &&
            (strcmp(e->u.call.callee->u.var.name, "__builtin_fshl") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_fshr") == 0)) {
            /* Concatenate the first two arguments and shift.  A zero count
             * returns the high half for fshl and the low half for fshr;
             * shifting by the full width is not a hardware shift. */
            if (e->u.call.args.len < 3) return -1;
            int left = strcmp(e->u.call.callee->u.var.name, "__builtin_fshl") == 0;
            int w = type_size(e->type);
            if (w < 1) w = 4;
            int bits = w * 8;
            IRValue hi = lower_expr(fn, st, e->u.call.args.data[0]);
            IRValue lo = lower_expr(fn, st, e->u.call.args.data[1]);
            IRValue sh = lower_expr(fn, st, e->u.call.args.data[2]);
            IRValue mask = new_value(fn);
            emit_inst_w(fn, IR_CONST, mask, -1, -1, bits - 1, w, 1, e->loc);
            set_value_type(fn, mask, w, 1);
            IRValue n = emit_bin_w(fn, IR_BAND, sh, mask, w, 1, e->loc);
            IRValue zero = new_value(fn);
            emit_inst_w(fn, IR_CONST, zero, -1, -1, 0, w, 1, e->loc);
            set_value_type(fn, zero, w, 1);
            IRValue is0 = emit_bin_w(fn, IR_EQ, n, zero, 4, 0, e->loc);
            IRValue slot = emit_alloca(fn, w, w, 1, e->loc);
            IRValue addr = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, e->loc);
            int Lsh = new_label(fn);
            int Lzero = new_label(fn);
            int Ldone = new_label(fn);
            emit_inst_w(fn, IR_CBR, -1, is0, Lsh, Lzero, 4, 0, e->loc);
            emit_inst_w(fn, IR_LABEL, -1, -1, -1, Lzero, 0, 0, e->loc);
            emit_inst_w(fn, IR_STORE_PTR, -1, addr, left ? hi : lo, 0, w, 1, e->loc);
            emit_inst(fn, IR_BR, -1, -1, -1, Ldone, e->loc);
            emit_inst_w(fn, IR_LABEL, -1, -1, -1, Lsh, 0, 0, e->loc);
            IRValue widthv = new_value(fn);
            emit_inst_w(fn, IR_CONST, widthv, -1, -1, bits, w, 1, e->loc);
            set_value_type(fn, widthv, w, 1);
            IRValue wn = emit_bin_w(fn, IR_SUB, widthv, n, w, 1, e->loc);
            IRValue top, bot;
            if (left) {
                top = emit_bin_w(fn, IR_SHL, hi, n, w, 1, e->loc);
                bot = emit_bin_w(fn, IR_SHR, lo, wn, w, 1, e->loc);
            } else {
                top = emit_bin_w(fn, IR_SHL, hi, wn, w, 1, e->loc);
                bot = emit_bin_w(fn, IR_SHR, lo, n, w, 1, e->loc);
            }
            IRValue merged = emit_bin_w(fn, IR_BOR, top, bot, w, 1, e->loc);
            emit_inst_w(fn, IR_STORE_PTR, -1, addr, merged, 0, w, 1, e->loc);
            emit_inst_w(fn, IR_LABEL, -1, -1, -1, Ldone, 0, 0, e->loc);
            IRValue out = new_value(fn);
            emit_inst_w(fn, IR_LOAD_PTR, out, addr, -1, 0, w, 1, e->loc);
            set_value_type(fn, out, w, 1);
            return out;
        }
        if (e->u.call.callee->kind == EX_VAR && strcmp(e->u.call.callee->u.var.name, "__builtin_longjmp") == 0) {
            IRValue buf_ptr = lower_expr(fn, st, e->u.call.args.data[0]);
            emit_inst_w(fn, IR_LONGJMP, -1, buf_ptr, -1, 0, 8, 1, e->loc);
            fn->has_dyn_alloca = 1;
            return -1;
        }
        if (e->u.call.callee->kind == EX_VAR && strcmp(e->u.call.callee->u.var.name, "__builtin_classify_type") == 0) {
            int tc = -1;
            if (e->u.call.args.len > 0) {
                const Type *t = &e->u.call.args.data[0]->type;
                if (t->kind == TY_FLOAT) tc = 8;
                else if (t->kind == TY_INT) tc = 1;
                else if (t->kind == TY_PTR) tc = 5;
                else if (t->kind == TY_STRUCT) {
                    const StructDef *sd = struct_registry_find_c(g_ir_structs, t->tag);
                    tc = (sd && sd->is_union) ? 13 : 12;
                }
                else if (t->kind == TY_ARRAY) tc = 14;
                else if (t->kind == TY_VOID) tc = 0;
            }
            IRValue v = new_value(fn);
            emit_inst_w(fn, IR_CONST, v, -1, -1, tc, 4, 0, e->loc);
            set_value_type(fn, v, 4, 0);
            return v;
        }
        if (e->u.call.callee->kind == EX_VAR &&
            (strcmp(e->u.call.callee->u.var.name, "__builtin_signbit") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_signbitf") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_signbitl") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "signbit") == 0) &&
            e->u.call.args.len > 0) {
            Expr *a0 = e->u.call.args.data[0];
            Type aty = a0->type;
            if (aty.kind == TY_FLOAT) {
                IRValue fv = lower_expr(fn, st, a0);
                IRValue slot = emit_alloca(fn, 16, 8, 1, e->loc);
                IRValue addr = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, e->loc);
                emit_inst_w(fn, IR_STORE_PTR, -1, addr, fv, 0, aty.width, 1, e->loc);
                int sign_byte_off = (aty.width == 4) ? 3 : (aty.width == 8) ? 7 : 9;
                IRValue sign_byte_addr = emit_add_const(fn, addr, sign_byte_off, e->loc);
                IRValue bval = new_value(fn);
                emit_inst_w(fn, IR_LOAD_PTR, bval, sign_byte_addr, -1, 0, 1, 1, e->loc);
                IRValue s7 = new_value(fn);
                emit_inst_w(fn, IR_CONST, s7, -1, -1, 7, 8, 1, e->loc);
                IRValue sign = emit_bin_w(fn, IR_SHR, bval, s7, 1, 1, e->loc);
                return coerce(fn, sign, 1, 1, 4, 0, e->loc);
            } else if (aty.kind == TY_INT) {
                IRValue iv = lower_expr(fn, st, a0);
                if (aty.is_unsigned) {
                    IRValue zero = new_value(fn);
                    emit_inst_w(fn, IR_CONST, zero, -1, -1, 0, 4, 0, e->loc);
                    return zero;
                }
                int iw = get_value_width(fn, iv), iu = get_value_is_unsigned(fn, iv);
                IRValue iv8 = coerce(fn, iv, iw, iu, 8, 0, e->loc);
                IRValue zero = new_value(fn);
                emit_inst_w(fn, IR_CONST, zero, -1, -1, 0, 8, 0, e->loc);
                IRValue lt = emit_bin_w(fn, IR_LT, iv8, zero, 8, 0, e->loc);
                return coerce(fn, lt, 8, 0, 4, 0, e->loc);
            }
        }
        if (e->u.call.callee->kind == EX_VAR &&
            (strcmp(e->u.call.callee->u.var.name, "__builtin_isinf") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_isinff") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_isinfl") == 0) &&
            e->u.call.args.len > 0) {
            /* __builtin_isinf(f): returns 1 if f is +/- infinity, 0 otherwise.
             * IEEE 754: inf has exponent all 1s and mantissa all 0s.
             * float:  mask 0x7FFFFFFF, compare to 0x7F800000
             * double: mask 0x7FFFFFFFFFFFFFFF, compare to 0x7FF0000000000000 */
            Expr *a0 = e->u.call.args.data[0];
            Type aty = a0->type;
            if (aty.kind == TY_FLOAT) {
                IRValue fv = lower_expr(fn, st, a0);
                if (aty.width == 4) {
                    IRValue slot = emit_alloca(fn, 4, 4, 1, e->loc);
                    IRValue addr = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, e->loc);
                    emit_inst_w(fn, IR_STORE_PTR, -1, addr, fv, 0, 4, 1, e->loc);
                    IRValue ival = new_value(fn);
                    emit_inst_w(fn, IR_LOAD_PTR, ival, addr, -1, 0, 4, 1, e->loc);
                    /* Zero-extend to 64-bit for uniform comparison */
                    IRValue ival64 = coerce(fn, ival, 4, 1, 8, 1, e->loc);
                    IRValue mask = new_value(fn);
                    emit_inst_w(fn, IR_CONST, mask, -1, -1, 0x7FFFFFFF, 8, 1, e->loc);
                    IRValue masked = emit_bin_w(fn, IR_BAND, ival64, mask, 8, 1, e->loc);
                    IRValue inf_val = new_value(fn);
                    emit_inst_w(fn, IR_CONST, inf_val, -1, -1, 0x7F800000, 8, 1, e->loc);
                    IRValue is_inf = emit_bin_w(fn, IR_EQ, masked, inf_val, 8, 1, e->loc);
                    return coerce(fn, is_inf, 8, 1, 4, 0, e->loc);
                } else if (aty.width == 8) {
                    IRValue slot = emit_alloca(fn, 8, 8, 1, e->loc);
                    IRValue addr = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, e->loc);
                    emit_inst_w(fn, IR_STORE_PTR, -1, addr, fv, 0, 8, 1, e->loc);
                    IRValue ival = new_value(fn);
                    emit_inst_w(fn, IR_LOAD_PTR, ival, addr, -1, 0, 8, 1, e->loc);
                    IRValue mask = new_value(fn);
                    emit_inst_w(fn, IR_CONST, mask, -1, -1, 0x7FFFFFFFFFFFFFFFLL, 8, 1, e->loc);
                    IRValue masked = emit_bin_w(fn, IR_BAND, ival, mask, 8, 1, e->loc);
                    IRValue inf_val = new_value(fn);
                    emit_inst_w(fn, IR_CONST, inf_val, -1, -1, 0x7FF0000000000000LL, 8, 1, e->loc);
                    IRValue is_inf = emit_bin_w(fn, IR_EQ, masked, inf_val, 8, 1, e->loc);
                    return coerce(fn, is_inf, 8, 1, 4, 0, e->loc);
                } else {
                    /* long double (80-bit x87, stored in 16 bytes).
                     * Layout: 10 bytes data (LE) + 6 bytes padding.
                     * +inf bytes: 00 00 00 00 00 00 00 80 FF 7F
                     * Check exponent (bytes 8-9, 15 bits) == 0x7FFF
                     * and mantissa integer bit set, fraction clear. */
                    IRValue slot = emit_alloca(fn, 16, 16, 1, e->loc);
                    IRValue addr = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, e->loc);
                    emit_inst_w(fn, IR_STORE_PTR, -1, addr, fv, 0, 16, 1, e->loc);
                    /* Load bytes 8-9 as little-endian 16-bit = exponent + sign */
                    IRValue exp_addr = emit_add_const(fn, addr, 8, e->loc);
                    IRValue exp16 = new_value(fn);
                    emit_inst_w(fn, IR_LOAD_PTR, exp16, exp_addr, -1, 0, 2, 1, e->loc);
                    IRValue exp16_64 = coerce(fn, exp16, 2, 1, 8, 1, e->loc);
                    IRValue exp_mask = new_value(fn);
                    emit_inst_w(fn, IR_CONST, exp_mask, -1, -1, 0x7FFF, 8, 1, e->loc);
                    IRValue exp_bits = emit_bin_w(fn, IR_BAND, exp16_64, exp_mask, 8, 1, e->loc);
                    IRValue exp_max = new_value(fn);
                    emit_inst_w(fn, IR_CONST, exp_max, -1, -1, 0x7FFF, 8, 1, e->loc);
                    IRValue exp_ok = emit_bin_w(fn, IR_EQ, exp_bits, exp_max, 8, 1, e->loc);
                    /* Check mantissa == 0x8000000000000000 (integer=1, fraction=0) */
                    IRValue mant = new_value(fn);
                    emit_inst_w(fn, IR_LOAD_PTR, mant, addr, -1, 0, 8, 1, e->loc);
                    IRValue mant_mask = new_value(fn);
                    emit_inst_w(fn, IR_CONST, mant_mask, -1, -1, 0x7FFFFFFFFFFFFFFFLL, 8, 1, e->loc);
                    IRValue mant_masked = emit_bin_w(fn, IR_BAND, mant, mant_mask, 8, 1, e->loc);
                    IRValue mant_expected = new_value(fn);
                    emit_inst_w(fn, IR_CONST, mant_expected, -1, -1, 0x8000000000000000LL, 8, 1, e->loc);
                    IRValue mant_ok = emit_bin_w(fn, IR_EQ, mant_masked, mant_expected, 8, 1, e->loc);
                    IRValue is_inf = emit_bin_w(fn, IR_BAND, exp_ok, mant_ok, 8, 1, e->loc);
                    return coerce(fn, is_inf, 8, 1, 4, 0, e->loc);
                }
            }
        }
        if (e->u.call.callee->kind == EX_VAR &&
            (strcmp(e->u.call.callee->u.var.name, "__builtin_isnormal") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_isnormalf") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_isnormall") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "isnormal") == 0) &&
            e->u.call.args.len > 0) {
            /* Normal means the exponent is neither zero nor all-ones, so
             * zero, subnormal, infinity, and NaN are rejected together. */
            Expr *a0 = e->u.call.args.data[0];
            Type aty = a0->type;
            if (aty.kind == TY_FLOAT && (aty.width == 4 || aty.width == 8)) {
                IRValue fv = lower_expr(fn, st, a0);
                int w = (int)aty.width;
                IRValue slot = emit_alloca(fn, w, w, 1, e->loc);
                IRValue addr = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, e->loc);
                emit_inst_w(fn, IR_STORE_PTR, -1, addr, fv, 0, w, 1, e->loc);
                IRValue ival = new_value(fn);
                emit_inst_w(fn, IR_LOAD_PTR, ival, addr, -1, 0, w, 1, e->loc);
                int shamt = w == 4 ? 23 : 52;
                int expmask = w == 4 ? 0xff : 0x7ff;
                IRValue shc = new_value(fn);
                emit_inst_w(fn, IR_CONST, shc, -1, -1, shamt, w, 1, e->loc);
                IRValue shifted = emit_bin_w(fn, IR_SHR, ival, shc, w, 1, e->loc);
                IRValue mask = new_value(fn);
                emit_inst_w(fn, IR_CONST, mask, -1, -1, expmask, w, 1, e->loc);
                IRValue exp = emit_bin_w(fn, IR_BAND, shifted, mask, w, 1, e->loc);
                IRValue zero = new_value(fn);
                emit_inst_w(fn, IR_CONST, zero, -1, -1, 0, w, 1, e->loc);
                IRValue nz = emit_bin_w(fn, IR_NE, exp, zero, w, 1, e->loc);
                IRValue maxc = new_value(fn);
                emit_inst_w(fn, IR_CONST, maxc, -1, -1, expmask, w, 1, e->loc);
                IRValue nall = emit_bin_w(fn, IR_NE, exp, maxc, w, 1, e->loc);
                IRValue both = emit_bin_w(fn, IR_BAND, nz, nall, w, 1, e->loc);
                return coerce(fn, both, w, 1, 4, 0, e->loc);
            }
        }
        if (e->u.call.callee->kind == EX_VAR &&
            (strcmp(e->u.call.callee->u.var.name, "__builtin_add_overflow") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_add_overflow_p") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_sadd_overflow") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_sadd_overflow_p") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_saddl_overflow") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_saddl_overflow_p") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_saddll_overflow") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_saddll_overflow_p") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_uadd_overflow") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_uadd_overflow_p") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_uaddl_overflow") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_uaddl_overflow_p") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_uaddll_overflow") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_uaddll_overflow_p") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_sub_overflow") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_sub_overflow_p") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_ssub_overflow") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_ssub_overflow_p") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_ssubl_overflow") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_ssubl_overflow_p") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_ssubll_overflow") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_ssubll_overflow_p") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_usub_overflow") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_usub_overflow_p") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_usubl_overflow") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_usubl_overflow_p") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_usubll_overflow") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_usubll_overflow_p") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_mul_overflow") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_mul_overflow_p") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_smul_overflow") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_smul_overflow_p") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_smull_overflow") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_smull_overflow_p") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_smulll_overflow") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_smulll_overflow_p") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_umul_overflow") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_umul_overflow_p") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_umull_overflow") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_umull_overflow_p") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_umulll_overflow") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_umulll_overflow_p") == 0) &&
            e->u.call.args.len == 3) {
            const char *cname = e->u.call.callee->u.var.name;
            int is_add = strstr(cname, "add") != NULL;
            int is_sub = strstr(cname, "sub") != NULL;
            int is_p = strstr(cname, "_p") != NULL;
            Expr *arg0 = e->u.call.args.data[0];
            Expr *arg1 = e->u.call.args.data[1];
            Expr *arg2 = e->u.call.args.data[2];
            Type res_ty;
            if (is_p) {
                res_ty = arg2->type;
            } else {
                res_ty = (arg2->type.kind == TY_PTR && arg2->type.pointee) ? *arg2->type.pointee : type_default_int();
            }
            int tw = res_ty.width ? res_ty.width : 4;
            int tu = res_ty.is_unsigned;
            IRValue a = lower_expr(fn, st, arg0);
            IRValue b = lower_expr(fn, st, arg1);
            /* GCC: promote each operand to infinite precision using its own
             * signedness, compute, then ask whether the mathematical result
             * fits in the last argument's type.  64-bit dest-type arithmetic
             * cannot represent both "signed * unsigned → unsigned long"
             * (pr30314: 42 * ~0UL/42 fits) and "signed * signed → unsigned
             * long long" (pr91450: -2 * 1 is negative and overflows). */
            IRValue a128 = overflow_lift_i128(fn, a, arg0->type, e->loc);
            IRValue b128 = overflow_lift_i128(fn, b, arg1->type, e->loc);
            IRValue prod = i128_alloc(fn, e->loc);
            if (is_add) i128_add(fn, prod, a128, b128, e->loc);
            else if (is_sub) i128_sub(fn, prod, a128, b128, e->loc);
            else i128_mul(fn, prod, a128, b128, e->loc);
            IRValue lo, hi;
            i128_load2(fn, prod, &lo, &hi, e->loc);
            if (!is_p) {
                IRValue rptr = lower_expr(fn, st, arg2);
                if (tw >= 16 && type_is_i128(res_ty)) {
                    i128_store2(fn, rptr, lo, hi, e->loc);
                } else {
                    IRValue narrow_res = coerce(fn, lo, 8, tu, tw, tu, e->loc);
                    emit_inst_w(fn, IR_STORE_PTR, -1, rptr, narrow_res, 0, tw, tu, e->loc);
                }
            }
            return i128_not_in_dest(fn, lo, hi, tw, tu, e->loc);
        }
        if (e->u.call.callee->kind == EX_VAR &&
            (strcmp(e->u.call.callee->u.var.name, "abs") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "labs") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "llabs") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "imaxabs") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_abs") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_labs") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_llabs") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_imaxabs") == 0) &&
            e->u.call.args.len > 0 &&
            !ir_builtin_disabled(e->u.call.callee->u.var.name)) {
            const char *bname = e->u.call.callee->u.var.name;
            int bw = (strstr(bname, "llabs") || strstr(bname, "labs") || strstr(bname, "imaxabs")) ? 8 : 4;
            IRValue arg = lower_expr(fn, st, e->u.call.args.data[0]);
            arg = coerce(fn, arg, get_value_width(fn, arg), get_value_is_unsigned(fn, arg), bw, 0, e->loc);
            IRValue slot = new_value(fn);
            emit_inst_w(fn, IR_ALLOCA, slot, -1, -1, 0, bw, 0, e->loc);
            int L_neg = new_label(fn);
            int L_pos = new_label(fn);
            int L_done = new_label(fn);
            IRValue zero = new_value(fn);
            emit_inst_w(fn, IR_CONST, zero, -1, -1, 0, bw, 0, e->loc);
            IRValue cond = new_value(fn);
            emit_inst_w(fn, IR_LT, cond, arg, zero, 0, bw, 0, e->loc);
            emit_cbr(fn, cond, L_neg, L_pos, e->loc);
            emit_label(fn, L_neg, e->loc);
            IRValue neg = new_value(fn);
            emit_inst_w(fn, IR_NEG, neg, arg, -1, 0, bw, 0, e->loc);
            emit_inst_w(fn, IR_STORE, -1, slot, neg, 0, bw, 0, e->loc);
            emit_br(fn, L_done, e->loc);
            emit_label(fn, L_pos, e->loc);
            emit_inst_w(fn, IR_STORE, -1, slot, arg, 0, bw, 0, e->loc);
            emit_br(fn, L_done, e->loc);
            emit_label(fn, L_done, e->loc);
            IRValue result = new_value(fn);
            emit_inst_w(fn, IR_LOAD, result, slot, -1, 0, bw, 0, e->loc);
            set_value_type(fn, result, bw, 0);
            return result;
        }
        if (e->u.call.callee->kind == EX_VAR &&
            (strcmp(e->u.call.callee->u.var.name, "uabs") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "ulabs") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "ullabs") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "umaxabs") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_uabs") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_ulabs") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_ullabs") == 0 ||
             strcmp(e->u.call.callee->u.var.name, "__builtin_umaxabs") == 0) &&
            e->u.call.args.len > 0 &&
            !ir_builtin_disabled(e->u.call.callee->u.var.name)) {
            const char *bname = e->u.call.callee->u.var.name;
            int bw = (strstr(bname, "ulabs") || strstr(bname, "ullabs") || strstr(bname, "umaxabs")) ? 8 : 4;
            IRValue arg = lower_expr(fn, st, e->u.call.args.data[0]);
            arg = coerce(fn, arg, get_value_width(fn, arg), get_value_is_unsigned(fn, arg), bw, 0, e->loc);
            IRValue slot = new_value(fn);
            emit_inst_w(fn, IR_ALLOCA, slot, -1, -1, 0, bw, 1, e->loc);
            int L_neg = new_label(fn);
            int L_pos = new_label(fn);
            int L_done = new_label(fn);
            IRValue zero = new_value(fn);
            emit_inst_w(fn, IR_CONST, zero, -1, -1, 0, bw, 0, e->loc);
            IRValue cond = new_value(fn);
            emit_inst_w(fn, IR_LT, cond, arg, zero, 0, bw, 0, e->loc);
            emit_cbr(fn, cond, L_neg, L_pos, e->loc);
            emit_label(fn, L_neg, e->loc);
            IRValue neg = new_value(fn);
            emit_inst_w(fn, IR_NEG, neg, arg, -1, 0, bw, 1, e->loc);
            emit_inst_w(fn, IR_STORE, -1, slot, neg, 0, bw, 1, e->loc);
            emit_br(fn, L_done, e->loc);
            emit_label(fn, L_pos, e->loc);
            emit_inst_w(fn, IR_STORE, -1, slot, arg, 0, bw, 1, e->loc);
            emit_br(fn, L_done, e->loc);
            emit_label(fn, L_done, e->loc);
            IRValue result = new_value(fn);
            emit_inst_w(fn, IR_LOAD, result, slot, -1, 0, bw, 1, e->loc);
            set_value_type(fn, result, bw, 1);
            return result;
        }
        /* The va_start / va_arg / va_end builtins.  They are lowered to a
         * named IR_CALL (call_name = the builtin) so codegen can dispatch by
         * name, exactly like __syscall.  The va_list is already a pointer to
         * the struct's bytes (fakecc's struct-as-pointer value model), so the
         * first arg lowers directly to the struct base address. */
        if (e->u.call.callee->kind == EX_VAR) {
            const char *cname = e->u.call.callee->u.var.name;
            if (strcmp(cname, "__builtin_apply_args") == 0) {
                /* GCC saves incoming arg registers at function entry. Flag
                 * the function so codegen emits that save in the prologue. */
                fn->needs_apply_args = 1;
                IRInst inst;
                memset(&inst, 0, sizeof(inst));
                inst.c = -1;
                inst.d = -1;
                inst.op = IR_CALL;
                inst.dst = new_value(fn);
                inst.a = -1; inst.b = -1;
                inst.loc = e->loc;
                inst.call_name = xstrdup("__builtin_apply_args");
                inst.call_callee = -1;
                inst.call_nargs = 0;
                inst.width = 8;
                inst.is_unsigned = 1;
                ir_inst_array_push(&fn->insts, inst);
                set_value_type(fn, inst.dst, 8, 1);
                return inst.dst;
            }
            if (strcmp(cname, "__builtin_apply") == 0) {
                fn->needs_apply = 1;
                fn->has_dyn_alloca = 1;
                IRValue fptr = lower_expr(fn, st, e->u.call.args.data[0]);
                IRValue ablk = lower_expr(fn, st, e->u.call.args.data[1]);
                IRValue asz = lower_expr(fn, st, e->u.call.args.data[2]);
                IRInst inst;
                memset(&inst, 0, sizeof(inst));
                inst.c = -1;
                inst.d = -1;
                inst.op = IR_CALL;
                inst.dst = new_value(fn);
                inst.a = -1; inst.b = -1;
                inst.loc = e->loc;
                inst.call_name = xstrdup("__builtin_apply");
                inst.call_callee = -1;
                ir_call_reserve_args(&inst, 3);
                inst.call_args[0] = fptr;
                inst.call_args[1] = ablk;
                inst.call_args[2] = asz;
                inst.width = 8;
                inst.is_unsigned = 1;
                ir_inst_array_push(&fn->insts, inst);
                set_value_type(fn, inst.dst, 8, 1);
                return inst.dst;
            }
            if (strcmp(cname, "__builtin_return") == 0) {
                IRValue rp = lower_expr(fn, st, e->u.call.args.data[0]);
                IRInst inst;
                memset(&inst, 0, sizeof(inst));
                inst.c = -1;
                inst.d = -1;
                inst.op = IR_CALL;
                inst.dst = -1;
                inst.a = -1; inst.b = -1;
                inst.loc = e->loc;
                inst.call_name = xstrdup("__builtin_return");
                inst.call_callee = -1;
                ir_call_reserve_args(&inst, 1);
                inst.call_args[0] = rp;
                ir_inst_array_push(&fn->insts, inst);
                return -1;
            }
            if (strcmp(cname, "va_copy") == 0
                || strcmp(cname, "__builtin_va_copy") == 0) {
                IRValue dst = lower_expr(fn, st, e->u.call.args.data[0]);
                IRValue src = lower_expr(fn, st, e->u.call.args.data[1]);
                emit_struct_copy(fn, dst, src, abi_sret_uses_gp() ? 24 : 8, e->loc);
                return -1;
            }
            if (strcmp(cname, "__builtin_shuffle") == 0) {
                Type vt = e->type;
                int total_sz = type_size(vt);
                int esz = vt.elem_type ? type_size(*vt.elem_type) : 4;
                if (esz <= 0) esz = 4;
                int count = vt.length > 0 ? vt.length : (total_sz / esz);
                int is_float = (vt.elem_type && vt.elem_type->kind == TY_FLOAT);
                int is_unsigned = vt.elem_type ? vt.elem_type->is_unsigned : 0;
                IRValue v1_addr = lower_expr(fn, st, e->u.call.args.data[0]);
                IRValue v2_addr = -1;
                Expr *mask_expr;
                if (e->u.call.args.len == 2) {
                    v2_addr = v1_addr;
                    mask_expr = e->u.call.args.data[1];
                } else {
                    v2_addr = lower_expr(fn, st, e->u.call.args.data[1]);
                    mask_expr = e->u.call.args.data[2];
                }
                IRValue mask_addr = lower_expr(fn, st, mask_expr);
                Type mt = mask_expr->type;
                int mesz = mt.elem_type ? type_size(*mt.elem_type) : 4;
                if (mesz <= 0) mesz = 4;
                int mu = mt.elem_type ? mt.elem_type->is_unsigned : 0;

                IRValue slot = emit_alloca(fn, total_sz, 16, 1, e->loc);
                IRValue dst_addr = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, e->loc);
                for (int i = 0; i < count; i++) {
                    IRValue m_off = emit_add_const(fn, mask_addr, i * mesz, e->loc);
                    IRValue m_val = new_value(fn);
                    emit_inst_w(fn, IR_LOAD_PTR, m_val, m_off, -1, 0, mesz, mu, e->loc);
                    int total_elems = (e->u.call.args.len == 2) ? count : 2 * count;
                    IRValue mod_v = new_value(fn);
                    emit_inst_w(fn, IR_CONST, mod_v, -1, -1, total_elems, mesz, 1, e->loc);
                    IRValue idx = emit_bin_w(fn, IR_MOD, m_val, mod_v, mesz, 1, e->loc);
                    idx = coerce(fn, idx, mesz, 1, 8, 1, e->loc);

                    IRValue dst_off = emit_add_const(fn, dst_addr, i * esz, e->loc);
                    if (e->u.call.args.len == 2) {
                        IRValue esz_v = new_value(fn);
                        emit_inst_w(fn, IR_CONST, esz_v, -1, -1, esz, 8, 1, e->loc);
                        IRValue byte_off = emit_bin_w(fn, IR_MUL, idx, esz_v, 8, 1, e->loc);
                        IRValue src_off = emit_bin_w(fn, IR_ADD, v1_addr, byte_off, 8, 1, e->loc);
                        IRValue elem_val = new_value(fn);
                        emit_inst_w(fn, IR_LOAD_PTR, elem_val, src_off, -1, 0, esz, is_unsigned, e->loc);
                        if (is_float) set_value_float(fn, elem_val, 1);
                        emit_inst_w(fn, IR_STORE_PTR, -1, dst_off, elem_val, 0, esz, is_unsigned, e->loc);
                        if (is_float) fn->insts.data[fn->insts.len - 1].is_float = 1;
                    } else {
                        IRValue count_v = new_value(fn);
                        emit_inst_w(fn, IR_CONST, count_v, -1, -1, count, 8, 1, e->loc);
                        IRValue cond = emit_bin_w(fn, IR_LT, idx, count_v, 8, 1, e->loc);
                        int L_v1 = new_label(fn), L_v2 = new_label(fn), L_done = new_label(fn);
                        emit_cbr(fn, cond, L_v1, L_v2, e->loc);

                        emit_label(fn, L_v1, e->loc);
                        IRValue esz_v1 = new_value(fn);
                        emit_inst_w(fn, IR_CONST, esz_v1, -1, -1, esz, 8, 1, e->loc);
                        IRValue byte_off1 = emit_bin_w(fn, IR_MUL, idx, esz_v1, 8, 1, e->loc);
                        IRValue src_off1 = emit_bin_w(fn, IR_ADD, v1_addr, byte_off1, 8, 1, e->loc);
                        IRValue elem1 = new_value(fn);
                        emit_inst_w(fn, IR_LOAD_PTR, elem1, src_off1, -1, 0, esz, is_unsigned, e->loc);
                        if (is_float) set_value_float(fn, elem1, 1);
                        emit_inst_w(fn, IR_STORE_PTR, -1, dst_off, elem1, 0, esz, is_unsigned, e->loc);
                        if (is_float) fn->insts.data[fn->insts.len - 1].is_float = 1;
                        emit_br(fn, L_done, e->loc);

                        emit_label(fn, L_v2, e->loc);
                        IRValue idx2 = emit_bin_w(fn, IR_SUB, idx, count_v, 8, 1, e->loc);
                        IRValue esz_v2 = new_value(fn);
                        emit_inst_w(fn, IR_CONST, esz_v2, -1, -1, esz, 8, 1, e->loc);
                        IRValue byte_off2 = emit_bin_w(fn, IR_MUL, idx2, esz_v2, 8, 1, e->loc);
                        IRValue src_off2 = emit_bin_w(fn, IR_ADD, v2_addr, byte_off2, 8, 1, e->loc);
                        IRValue elem2 = new_value(fn);
                        emit_inst_w(fn, IR_LOAD_PTR, elem2, src_off2, -1, 0, esz, is_unsigned, e->loc);
                        if (is_float) set_value_float(fn, elem2, 1);
                        emit_inst_w(fn, IR_STORE_PTR, -1, dst_off, elem2, 0, esz, is_unsigned, e->loc);
                        if (is_float) fn->insts.data[fn->insts.len - 1].is_float = 1;
                        emit_br(fn, L_done, e->loc);

                        emit_label(fn, L_done, e->loc);
                    }
                }
                return dst_addr;
            }
            if (strcmp(cname, "va_start") == 0 || strcmp(cname, "va_end") == 0
                || strcmp(cname, "va_arg") == 0
                || strcmp(cname, "__builtin_va_start") == 0 || strcmp(cname, "__builtin_va_end") == 0
                || strcmp(cname, "__builtin_va_arg") == 0
                || strcmp(cname, "__builtin_c23_va_start") == 0) {
                IRValue ap = lower_expr(fn, st, e->u.call.args.data[0]);
                /* va_start's second arg (last named param) must stay live so the
                 * optimizer doesn't eliminate the param as dead.  Lower it and
                 * keep it in call_args (codegen ignores it). */
                IRValue last = -1;
                int is_start = (strcmp(cname, "va_start") == 0 || strcmp(cname, "__builtin_va_start") == 0 || strcmp(cname, "__builtin_c23_va_start") == 0);
                int is_end = (strcmp(cname, "va_end") == 0 || strcmp(cname, "__builtin_va_end") == 0);
                int is_arg = (strcmp(cname, "va_arg") == 0 || strcmp(cname, "__builtin_va_arg") == 0);
                if (is_start && e->u.call.args.len >= 2)
                    last = lower_expr(fn, st, e->u.call.args.data[1]);
                IRInst inst;
                memset(&inst, 0, sizeof(inst));
                inst.c = -1;
                inst.d = -1;
                inst.op = IR_CALL;
                inst.a = -1; inst.b = -1; inst.imm = 0;
                inst.loc = e->loc;
                inst.call_name = xstrdup(is_start ? "va_start" : is_end ? "va_end" : "va_arg");
                inst.call_callee = -1;
                /* Reserve 2 slots up front: va_arg(struct) needs a second
                 * slot for the destination address even when there is no
                 * "last named param" operand. */
                ir_call_reserve_args(&inst, 2);
                inst.call_args[0] = ap;
                inst.call_args[1] = last;
                if (last < 0)
                    inst.call_nargs = 1;
                if (is_arg) {
                    if (type_is_empty_struct(e->va_arg_type)) {
                        /* gcc: size-0 / `[0]` structs consume no va_arg slot. */
                        free(inst.call_name);
                        free(inst.call_args);
                        free(inst.call_arg_on_stack);
                        free(inst.call_arg_nbytes);
                        {
                            IRValue slot = emit_alloca(fn, 1, 8, 1, e->loc);
                            return emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, e->loc);
                        }
                    }
                    if (e->va_arg_type.kind == TY_STRUCT || e->va_arg_type.is_vector
                        || type_is_pair16(e->va_arg_type)) {
                        int sz = type_is_pair16(e->va_arg_type) ? 16
                               : type_size(e->va_arg_type);
                        if (sz <= 0) sz = 8;
                        SysVRegClass cls[2];
                        int nreg;
                        if (type_is_i128(e->va_arg_type)) {
                            cls[0] = SYSV_CLS_INTEGER;
                            cls[1] = SYSV_CLS_INTEGER;
                            nreg = 2;
                        } else if (type_is_d128(e->va_arg_type)) {
                            /* Match pair16 call lowering (two eightbytes). */
                            cls[0] = SYSV_CLS_INTEGER;
                            cls[1] = SYSV_CLS_INTEGER;
                            nreg = 2;
                        } else {
                            nreg = sysv_classify_agg(e->va_arg_type, cls);
                        }
                        IRValue slot = emit_alloca_ty(fn, e->va_arg_type, e->loc);
                        IRValue addr = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, e->loc);
                        inst.dst = new_value(fn);
                        inst.width = 8;
                        inst.is_unsigned = 1;
                        inst.is_float = 0;
                        inst.imm = sz;
                        inst.force_stack = (nreg == 0);
                        inst.align16 = type_stack_align(e->va_arg_type);
                        inst.float_imm = (nreg > 0 && cls[0] == SYSV_CLS_SSE ? 1 : 0) |
                                         (nreg > 1 && cls[1] == SYSV_CLS_SSE ? 2 : 0);
                        /* nreg==1 SSE is one XMM/YMM (SSE+SSEUP), not two
                         * eightbytes.  Distinguishes `struct { __m128 }` from
                         * `{double; long}` which also has float_imm==1. */
                        if (nreg == 1 && cls[0] == SYSV_CLS_SSE && sz >= 16)
                            inst.float_imm |= 0x10;
                        inst.call_nargs = 2;
                        inst.call_args[1] = addr;
                        ir_inst_array_push(&fn->insts, inst);
                        set_value_type(fn, inst.dst, 8, 1);
                        return inst.dst;
                    }
                    inst.dst = new_value(fn);
                    inst.width = e->va_arg_type.width ? e->va_arg_type.width : 4;
                    inst.is_unsigned = e->va_arg_type.is_unsigned;
                    inst.is_float = (e->va_arg_type.kind == TY_FLOAT);
                } else {
                    inst.dst = -1;   /* void: va_start / va_end */
                    inst.width = 0;
                    inst.is_unsigned = 0;
                    inst.is_float = 0;
                }
                ir_inst_array_push(&fn->insts, inst);
                if (inst.dst >= 0)
                    set_value_type(fn, inst.dst, inst.width, inst.is_unsigned);
                if (inst.is_float) set_value_float(fn, inst.dst, 1);
                return inst.dst;
            }
        }
        if (e->u.call.callee && e->u.call.callee->kind == EX_VAR) {
            const char *name = e->u.call.callee->u.var.name;
            if (strcmp(name, "__builtin_isnan") == 0 || strcmp(name, "__builtin_isnanf") == 0 ||
                strcmp(name, "__builtin_isnanl") == 0 || strcmp(name, "isnan") == 0) {
                IRValue arg = lower_expr(fn, st, e->u.call.args.data[0]);
                int w = get_value_width(fn, arg);
                return emit_fcmp(fn, arg, arg, w, 5 /* NE */, e->loc);
            }
            if (strcmp(name, "__builtin_isfinite") == 0 || strcmp(name, "__builtin_isfinitef") == 0 ||
                strcmp(name, "__builtin_isfinitel") == 0 || strcmp(name, "isfinite") == 0) {
                IRValue arg = lower_expr(fn, st, e->u.call.args.data[0]);
                int w = get_value_width(fn, arg);
                IRValue diff = emit_bin_w(fn, IR_FSUB, arg, arg, w, 0, e->loc);
                set_value_float(fn, diff, 1);
                IRValue zero = emit_float_const(fn, w, 0, e->loc);
                return emit_fcmp(fn, diff, zero, w, 4 /* EQ */, e->loc);
            }
            if (strcmp(name, "__builtin_isinf") == 0 || strcmp(name, "__builtin_isinff") == 0 ||
                strcmp(name, "__builtin_isinfl") == 0 || strcmp(name, "isinf") == 0) {
                IRValue arg = lower_expr(fn, st, e->u.call.args.data[0]);
                int w = get_value_width(fn, arg);
                IRValue diff = emit_bin_w(fn, IR_FSUB, arg, arg, w, 0, e->loc);
                set_value_float(fn, diff, 1);
                IRValue zero = emit_float_const(fn, w, 0, e->loc);
                IRValue not_fin = emit_fcmp(fn, diff, zero, w, 5 /* NE */, e->loc);
                IRValue not_nan = emit_fcmp(fn, arg, arg, w, 4 /* EQ */, e->loc);
                return emit_bin_w(fn, IR_BAND, not_fin, not_nan, 4, 0, e->loc);
            }
            if (strcmp(name, "__builtin_isgreater") == 0) {
                IRValue a = lower_expr(fn, st, e->u.call.args.data[0]);
                IRValue b = lower_expr(fn, st, e->u.call.args.data[1]);
                int w = get_value_width(fn, a);
                return emit_fcmp(fn, a, b, w, 2 /* GT */, e->loc);
            }
            if (strcmp(name, "__builtin_isgreaterequal") == 0) {
                IRValue a = lower_expr(fn, st, e->u.call.args.data[0]);
                IRValue b = lower_expr(fn, st, e->u.call.args.data[1]);
                int w = get_value_width(fn, a);
                return emit_fcmp(fn, a, b, w, 3 /* GE */, e->loc);
            }
            if (strcmp(name, "__builtin_isless") == 0) {
                IRValue a = lower_expr(fn, st, e->u.call.args.data[0]);
                IRValue b = lower_expr(fn, st, e->u.call.args.data[1]);
                int w = get_value_width(fn, a);
                return emit_fcmp(fn, a, b, w, 0 /* LT */, e->loc);
            }
            if (strcmp(name, "__builtin_islessequal") == 0) {
                IRValue a = lower_expr(fn, st, e->u.call.args.data[0]);
                IRValue b = lower_expr(fn, st, e->u.call.args.data[1]);
                int w = get_value_width(fn, a);
                return emit_fcmp(fn, a, b, w, 1 /* LE */, e->loc);
            }
            if (strcmp(name, "__builtin_islessgreater") == 0) {
                IRValue a = lower_expr(fn, st, e->u.call.args.data[0]);
                IRValue b = lower_expr(fn, st, e->u.call.args.data[1]);
                int w = get_value_width(fn, a);
                IRValue lt = emit_fcmp(fn, a, b, w, 0 /* LT */, e->loc);
                IRValue gt = emit_fcmp(fn, a, b, w, 2 /* GT */, e->loc);
                return emit_bin_w(fn, IR_BOR, lt, gt, 4, 0, e->loc);
            }
            if (strcmp(name, "__builtin_isunordered") == 0) {
                IRValue a = lower_expr(fn, st, e->u.call.args.data[0]);
                IRValue b = lower_expr(fn, st, e->u.call.args.data[1]);
                int wa = get_value_width(fn, a);
                int wb = get_value_width(fn, b);
                IRValue nan_a = emit_fcmp(fn, a, a, wa, 5 /* NE */, e->loc);
                IRValue nan_b = emit_fcmp(fn, b, b, wb, 5 /* NE */, e->loc);
                return emit_bin_w(fn, IR_BOR, nan_a, nan_b, 4, 0, e->loc);
            }
            if (strcmp(name, "__builtin_nans") == 0 || strcmp(name, "__builtin_nansf") == 0 ||
                strcmp(name, "__builtin_nansl") == 0) {
                unsigned long long lo = 0, hi = 0;
                int w = e->type.width ? e->type.width : 8;
                nans_builtin_bits(e, w, &lo, &hi);
                if (w == 16) {
                    unsigned char buf[16];
                    long double ld = 0;
                    memset(buf, 0, sizeof buf);
                    memcpy(buf, &lo, 8);
                    memcpy(buf + 8, &hi, 8);
                    memcpy(&ld, buf, sizeof ld < sizeof buf ? sizeof ld : sizeof buf);
                    return emit_ld_const(fn, ld, e->loc);
                }
                return emit_float_const(fn, w, (int64_t)lo, e->loc);
            }
            if (strcmp(name, "__builtin_inf") == 0 || strcmp(name, "__builtin_inff") == 0 ||
                strcmp(name, "__builtin_infl") == 0 || strcmp(name, "__builtin_huge_val") == 0 ||
                strcmp(name, "__builtin_huge_valf") == 0 || strcmp(name, "__builtin_huge_vall") == 0 ||
                strcmp(name, "__builtin_nan") == 0 || strcmp(name, "__builtin_nanf") == 0 ||
                strcmp(name, "__builtin_nanl") == 0) {
                long double ld = 0;
                fold_const_float(e, &ld, NULL);
                int w = e->type.width ? e->type.width : 8;
                if (w == 16) return emit_ld_const(fn, ld, e->loc);
                int64_t bits = 0;
                if (w == 4) { float f = (float)ld; memcpy(&bits, &f, sizeof(f)); }
                else { double d = (double)ld; memcpy(&bits, &d, sizeof(d)); }
                return emit_float_const(fn, w, bits, e->loc);
            }
        }
        IRValue *arg_vals = malloc(IR_CALL_MAX_ARGS * sizeof(IRValue));
        unsigned char *arg_on_stack = malloc(IR_CALL_MAX_ARGS);
        int *arg_nbytes = calloc(IR_CALL_MAX_ARGS, sizeof(int));
        if (!arg_vals || !arg_on_stack || !arg_nbytes) {
            fprintf(stderr, "fakecc: OOM\n");
            exit(1);
        }
        int nargs = 0;
        memset(arg_on_stack, 0, IR_CALL_MAX_ARGS);
        /* Reserve a slot if the return needs a hidden sret pointer. */
        int is_void_pre = (e->type.kind == TY_VOID);
        int is_ret_i128_pre = !is_void_pre && type_is_pair16(e->type);
        int is_ret_struct_pre = (!is_void_pre && (e->type.kind == TY_STRUCT || e->type.is_vector || is_ret_i128_pre));
        SysVRegClass ret_cls_pre[4] = {0};
        int ret_nreg_pre = 0;
        if (is_ret_i128_pre) {
            ret_cls_pre[0] = SYSV_CLS_INTEGER;
            ret_cls_pre[1] = SYSV_CLS_INTEGER;
            ret_nreg_pre = 2;
        } else if (is_ret_struct_pre) {
            ret_nreg_pre = sysv_classify_agg(e->type, ret_cls_pre);
            /* Capacity 4: a three- or four-element HFA result comes back in
             * v0-v3, exactly as such an aggregate is passed. */
            abi_adjust_cls(e->type, ret_cls_pre, &ret_nreg_pre, 4, e->loc);
        }
        int ret_in_mem_pre = is_ret_struct_pre && ret_nreg_pre == 0;
        if (type_is_complex_ldouble(e->type)) {
            ret_nreg_pre = 2;
            ret_in_mem_pre = 0;
        }
        int is_x87_st0 = sysv_agg_ret_x87(e->type);
        if (is_x87_st0)
            ret_in_mem_pre = 0;
        /* GNU empty structs return in no slot and take no hidden sret
         * (SysV/GCC).  classify_agg reports MEMORY for size 0, which would
         * otherwise steal RDI and shift every later argument. */
        if (type_is_empty_struct(e->type))
            ret_in_mem_pre = 0;
        int arg_limit = IR_CALL_MAX_ARGS - (ret_in_mem_pre ? 1 : 0);
        /* Register-class aggregates expand into per-eightbyte SSA args.
         * MEMORY-class aggregates are one stack blob (pointer + nbytes)
         * so huge by-value structs do not explode IR_CALL. */
        int call_used_gp = (ret_in_mem_pre && abi_sret_uses_gp()) ? 1 : 0;
        int call_used_xmm = 0;
        Type callee_ty = e->u.call.callee->type;
        while (callee_ty.kind == TY_PTR && callee_ty.pointee)
            callee_ty = *callee_ty.pointee;
        int call_variadic = callee_ty.kind == TY_FUNC
            && (callee_ty.func_is_variadic || callee_ty.func_is_unprototyped);
        /* Darwin puts anonymous args on the stack only for a real
         * variadic callee.  An unprototyped call still uses registers. */
        int call_real_variadic = callee_ty.kind == TY_FUNC
            && callee_ty.func_is_variadic;
        int call_nparams = callee_ty.kind == TY_FUNC ? callee_ty.func_nparams : 0;
        const char *callee_name = NULL;
        if (e->u.call.callee && e->u.call.callee->kind == EX_VAR)
            callee_name = e->u.call.callee->u.var.name;
        int call_fake_variadic = prototype_less_builtin(callee_name);
        for (int i = 0; i < (int)e->u.call.args.len; i++) {
            Expr *arg = e->u.call.args.data[i];
            IRValue av = lower_expr(fn, st, arg);
            if (arg->type.kind == TY_STRUCT || arg->type.is_vector || type_is_pair16(arg->type)) {
                SysVRegClass cls[4];
                int nreg;
                int asz = type_size(arg->type);
                /* An anonymous variadic argument has no register on Darwin:
                 * every unnamed argument rides the vararg stack slots.
                 * Classifying it as if it were named put a __m128 in an XMM
                 * register the callee never reads, and va_arg then picked up
                 * whatever happened to be on the stack -- which is why the
                 * 16-byte vector case returned its low half and nothing
                 * else.  Structs suffered the same way. */
                if (darwin_unnamed_va(call_real_variadic, i, call_nparams,
                                      call_fake_variadic)) {
                    if (asz <= 0) {
                        /* A zero-sized aggregate consumes no slot at all --
                         * gcc agrees, and ir.c's va_arg already skips those,
                         * so reserving one here would shift every argument
                         * after it. */
                        continue;
                    }
                    if (nargs >= arg_limit) {
                        fprintf(stderr, "fakecc: too many call arguments (max %d)\n",
                                IR_CALL_MAX_ARGS);
                        exit(1);
                    }
                    arg_vals[nargs] = av;
                    arg_on_stack[nargs] = (unsigned char)
                        (sysv_stack_arg_flags(1, arg->type) | CALL_ARG_AGG);
                    arg_nbytes[nargs] = asz;
                    nargs++;
                    continue;
                }
                if (type_is_pair16(arg->type)) {
                    cls[0] = SYSV_CLS_INTEGER;
                    cls[1] = SYSV_CLS_INTEGER;
                    nreg = 2;
                    asz = 16;
                } else {
                    nreg = sysv_classify_agg(arg->type, cls);
                    abi_adjust_cls(arg->type, cls, &nreg, 4, e->loc);
                }
                int is_memory = (nreg == 0);
                /* SysV: unnamed __m256/__m512 to a variadic or unprototyped
                 * callee are MEMORY — the XMM save area is 16 bytes/slot. */
                if (!is_memory && call_variadic && i >= call_nparams
                    && nreg == 1 && cls[0] == SYSV_CLS_SSE && asz >= 32) {
                    is_memory = 1;
                    nreg = 0;
                }
                if (type_is_empty_struct(arg->type)) {
                    /* GNU empty structs occupy no argument slots. */
                    continue;
                }
                if (darwin_unnamed_va(call_real_variadic, i, call_nparams, call_fake_variadic)) {
                    /* Anonymous Darwin argument: 8-byte stack slots.  An
                     * aggregate larger than 16 bytes is one pointer slot;
                     * a smaller one is its eightbytes, in order. */
                    if (is_memory || asz > 16) {
                        int copy_sz = asz > 0 ? asz : 1;
                        IRValue tmp_alloca = emit_alloca(fn, copy_sz, 8, 1, e->loc);
                        IRValue tmp_addr = emit_bin_w(fn, IR_ADDR, tmp_alloca, -1,
                                                      8, 1, e->loc);
                        emit_struct_copy(fn, tmp_addr, av, asz > 0 ? asz : 1, e->loc);
                        if (nargs >= arg_limit) {
                            fprintf(stderr, "fakecc: too many call arguments (max %d)\n",
                                    IR_CALL_MAX_ARGS);
                            exit(1);
                        }
                        arg_vals[nargs] = tmp_addr;
                        arg_on_stack[nargs] = CALL_ARG_STACK;
                        nargs++;
                        continue;
                    }
                    SysVRegClass icls[2] = { SYSV_CLS_INTEGER, SYSV_CLS_INTEGER };
                    IRValue *ebs = malloc((size_t)(nreg > 0 ? nreg : 1) * sizeof(IRValue));
                    if (!ebs) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
                    load_agg_regs(fn, av, asz, nreg, icls, ebs, e->loc);
                    for (int k = 0; k < nreg; k++) {
                        if (nargs >= arg_limit) {
                            fprintf(stderr, "fakecc: too many call arguments (max %d)\n",
                                    IR_CALL_MAX_ARGS);
                            exit(1);
                        }
                        arg_vals[nargs] = ebs[k];
                        arg_on_stack[nargs] = CALL_ARG_STACK;
                        nargs++;
                    }
                    free(ebs);
                    continue;
                }
                if (is_memory && (sysv_memory_pass_as_pointer(arg->type)
                                  || abi_indirect_agg(arg->type))) {
                    /* Pointer to a stack copy: SysV va_list, or a Darwin
                     * aggregate larger than 16 bytes. */
                    int copy_sz = asz;
                    if (copy_sz < 1) copy_sz = 1;
                    IRValue tmp_alloca = emit_alloca(fn, copy_sz, 8, 1, e->loc);
                    IRValue tmp_addr = emit_bin_w(fn, IR_ADDR, tmp_alloca, -1, 8, 1,
                                                  e->loc);
                    emit_struct_copy(fn, tmp_addr, av, asz, e->loc);
                    if (nargs >= arg_limit) {
                        fprintf(stderr, "fakecc: too many call arguments (max %d)\n",
                                IR_CALL_MAX_ARGS);
                        exit(1);
                    }
                    arg_vals[nargs] = tmp_addr;
                    if (call_used_gp < abi_gp_nregs()) {
                        arg_on_stack[nargs] = 0;
                        call_used_gp++;
                    } else if (!abi_sret_uses_gp()) {
                        arg_on_stack[nargs] = CALL_ARG_STACK;
                    } else {
                        arg_on_stack[nargs] = 0;
                    }
                    nargs++;
                    continue;
                }
                if (is_memory) {
                    /* SysV MEMORY: copy the object onto the outgoing stack.
                     * One IR arg (pointer to the source) plus a blob flag. */
                    if (nargs >= arg_limit) {
                        fprintf(stderr, "fakecc: too many call arguments (max %d)\n",
                                IR_CALL_MAX_ARGS);
                        exit(1);
                    }
                    arg_vals[nargs] = av;
                    arg_on_stack[nargs] = (unsigned char)(sysv_stack_arg_flags(1, arg->type)
                                                          | CALL_ARG_BLOB);
                    arg_nbytes[nargs] = asz > 0 ? asz : 1;
                    nargs++;
                    continue;
                }
                int need_gp = 0, need_fp = 0;
                for (int k = 0; k < nreg; k++) {
                    if (cls[k] == SYSV_CLS_SSE) need_fp++;
                    else need_gp++;
                }
                int fits_in_regs = (call_used_gp + need_gp <= abi_gp_nregs()
                                    && call_used_xmm + need_fp <= 8);
                if (fits_in_regs) {
                    call_used_gp += need_gp;
                    call_used_xmm += need_fp;
                }
                int hfa = abi_is_hfa(arg->type);
                SysVRegClass *acls = malloc((size_t)nreg * sizeof(SysVRegClass));
                IRValue *ebs = malloc((size_t)nreg * sizeof(IRValue));
                if (!acls || !ebs) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
                /* HFA bits stay in the GP file; the call boundary moves
                 * them into V registers.  Loading as INTEGER keeps mem2reg
                 * and the GP allocator on the bit pattern. */
                for (int k = 0; k < nreg; k++)
                    acls[k] = hfa ? SYSV_CLS_INTEGER : cls[k];
                load_agg_regs(fn, av, asz, nreg, acls, ebs, e->loc);
                for (int k = 0; k < nreg; k++) {
                    if (nargs >= arg_limit) {
                        fprintf(stderr, "fakecc: too many call arguments (max %d)\n",
                                IR_CALL_MAX_ARGS);
                        exit(1);
                    }
                    arg_vals[nargs] = ebs[k];
                    if (!fits_in_regs)
                        arg_on_stack[nargs] = (k == 0)
                            ? sysv_stack_arg_flags(1, arg->type) : CALL_ARG_STACK;
                    else if (hfa)
                        arg_on_stack[nargs] = CALL_ARG_HFA;
                    else
                        arg_on_stack[nargs] = 0;
                    nargs++;
                }
                free(acls);
                free(ebs);
                continue;
            }
            if (nargs >= arg_limit) {
                fprintf(stderr, "fakecc: too many call arguments (max %d)\n",
                        IR_CALL_MAX_ARGS);
                exit(1);
            }
            if (darwin_unnamed_va(call_real_variadic, i, call_nparams, call_fake_variadic)) {
                arg_vals[nargs] = va_float_bits(fn, av, e->loc);
                arg_on_stack[nargs] = CALL_ARG_STACK;
                nargs++;
                continue;
            }
            arg_vals[nargs] = av;
            arg_on_stack[nargs] = 0;
            if (get_value_is_float(fn, av)) {
                if (call_used_xmm < 8) call_used_xmm++;
                else if (!abi_sret_uses_gp())
                    arg_on_stack[nargs] = CALL_ARG_STACK;
            } else {
                if (call_used_gp < abi_gp_nregs()) call_used_gp++;
            }
            nargs++;
        }
        /* Void call: no result value (width 0).  Still emit the call for its
         * side effects; dst=-1 marks "no result". */
        int is_void = is_void_pre;
        /* Struct return: MEMORY class uses hidden sret; register class
         * returns bits in RAX(/RDX) or XMM0(/XMM1). */
        int is_ret_struct = is_ret_struct_pre;
        SysVRegClass ret_cls[4];
        for (int k = 0; k < 4; k++) ret_cls[k] = ret_cls_pre[k];
        int ret_nreg = ret_nreg_pre;
        int ret_in_mem = ret_in_mem_pre;
        IRValue sret_addr = -1;
        if (ret_in_mem) {
            int total = type_size(e->type);
            /* Pin the slot (alloca_bytes > 0) even for size-0 types such as
             * empty unions — codegen's IR_ADDR requires a pinned alloca. */
            if (total < 1) total = 1;
            IRValue slot = emit_alloca_ty(fn, e->type, e->loc);
            sret_addr = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, e->loc);
        }
        IRValue slot_addr = -1;
        if (is_ret_struct && (ret_nreg > 0 || is_x87_st0)) {
            int total = type_size(e->type);
            if (total < 1) total = 1;
            IRValue slot = emit_alloca_ty(fn, e->type, e->loc);
            slot_addr = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, e->loc);
        }
        IRValue v;
        if (ret_in_mem) {
            v = sret_addr;
        } else if (is_ret_struct && (ret_nreg > 0 || is_x87_st0)) {
            v = slot_addr;
        } else if (is_void) {
            v = -1;
        } else if (type_is_empty_struct(e->type)) {
            /* Dummy empty object; the call itself is void-like (no dest). */
            IRValue slot = emit_alloca(fn, 1, 8, 1, e->loc);
            v = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, e->loc);
        } else {
            v = new_value(fn);
        }
        IRValue ret_lo = -1, ret_hi = -1, ret_c = -1, ret_d = -1;
        int is_cld_ret = type_is_complex_ldouble(e->type);
        int is_x87_ret = is_cld_ret || is_x87_st0;
        if (is_ret_struct && ret_nreg > 0 && !is_cld_ret) {
            ret_lo = new_value(fn);
            if (ret_nreg > 1) ret_hi = new_value(fn);
            /* A three- or four-element HFA comes back in v0-v3. */
            if (ret_nreg > 2) ret_c = new_value(fn);
            if (ret_nreg > 3) ret_d = new_value(fn);
        }
        int total_nargs = nargs + (ret_in_mem ? 1 : 0);
        IRInst inst;
        memset(&inst, 0, sizeof(inst));
        inst.c = -1;
        inst.d = -1;
        inst.op = IR_CALL;
        inst.dst = ret_in_mem ? -1
                 : is_x87_ret ? -1
                 : (is_ret_struct && ret_nreg > 0) ? ret_lo
                 : (type_is_empty_struct(e->type) ? -1 : v);
        inst.a = is_x87_ret ? slot_addr : -1;
        inst.b = ret_hi;   /* second return eightbyte, or -1 */
        inst.c = ret_c;    /* third, for a wide HFA result */
        inst.d = ret_d;    /* fourth */
        inst.x87_pair = is_x87_ret;
        inst.imm = is_x87_st0 ? 16 : (is_cld_ret ? 32 : 0);
        if (ret_in_mem && !abi_sret_uses_gp())
            inst.align16 = A64_MARK_SRET;
        inst.loc = e->loc;
        inst.call_name = NULL;
        inst.call_callee = -1;
        /* Direct named call: callee is `EX_VAR` whose name is a known function
         * in the TU (matches sema's direct-call path).  Otherwise it's an
         * indirect call through a function pointer. */
        if (e->u.call.callee->kind == EX_VAR) {
            const char *cname = e->u.call.callee->u.var.name;
            /* `__syscall` is an intrinsic — treat it as a named call so codegen
             * emits the raw `syscall` instruction.  It is not in the function
             * table, so the direct-call lookup below would miss it. */
            if (strcmp(cname, "__syscall") == 0 || strcmp(cname, "__clone") == 0
                || strcmp(cname, "__paciasp") == 0 || strcmp(cname, "__pacia") == 0
                || strcmp(cname, "__autda") == 0 || strcmp(cname, "__retab") == 0) {
                inst.call_name = xstrdup(cname);
            } else if (strcmp(cname, "__builtin_ctzll") == 0) {
                /* ctz intrinsic — codegen emits a `bsf` + fixup. */
                inst.call_name = xstrdup(cname);
            } else if (strncmp(cname, "__builtin_", 10) == 0) {
                if (strstr(cname, "__builtin_clz") || strstr(cname, "__builtin_ctz")
                    || strstr(cname, "__builtin_ffs") || strstr(cname, "__builtin_popcount")
                    || strstr(cname, "__builtin_parity") || strstr(cname, "__builtin_clrsb")
                    || strstr(cname, "__builtin_bswap"))
                    inst.call_name = xstrdup(cname);
                /* abort() must stay distinct from __builtin_trap(): libc abort
                 * raises SIGABRT while the builtin traps with SIGTRAP, and a
                 * caller can observe the difference in the wait status.
                 * __builtin_abort keeps the library meaning, so fold it onto
                 * `abort` and leave __builtin_trap under its own name. */
                else if (strcmp(cname, "__builtin_abort") == 0) inst.call_name = xstrdup("abort");
                else if (strcmp(cname, "__builtin_exit") == 0) inst.call_name = xstrdup("exit");
                else if (strcmp(cname, "__builtin_trap") == 0) inst.call_name = xstrdup("__builtin_trap");
                else if (strcmp(cname, "__builtin_memset") == 0) inst.call_name = xstrdup("memset");
                else if (strcmp(cname, "__builtin_memcpy") == 0) inst.call_name = xstrdup("memcpy");
                else if (strcmp(cname, "__builtin_bzero") == 0) inst.call_name = xstrdup("bzero");
                else if (strcmp(cname, "__builtin_bcopy") == 0) inst.call_name = xstrdup("bcopy");
                else if (strcmp(cname, "__builtin_memcmp") == 0) inst.call_name = xstrdup("memcmp");
                else if (strcmp(cname, "__builtin_strcmp") == 0) inst.call_name = xstrdup("strcmp");
                else if (strcmp(cname, "__builtin_strncmp") == 0) inst.call_name = xstrdup("strncmp");
                else if (strcmp(cname, "__builtin_strlen") == 0) inst.call_name = xstrdup("strlen");
                else if (strcmp(cname, "__builtin_strspn") == 0) inst.call_name = xstrdup("strspn");
                else if (strcmp(cname, "__builtin_strcpy") == 0) inst.call_name = xstrdup("strcpy");
                else if (strcmp(cname, "__builtin_strcat") == 0) inst.call_name = xstrdup("strcat");
                else if (strcmp(cname, "__builtin_stpcpy") == 0) inst.call_name = xstrdup("stpcpy");
                else if (strcmp(cname, "__builtin_stpncpy") == 0) inst.call_name = xstrdup("stpncpy");
                else if (strcmp(cname, "__builtin_strncpy") == 0) inst.call_name = xstrdup("strncpy");
                else if (strcmp(cname, "__builtin_strncat") == 0) inst.call_name = xstrdup("strncat");
                else if (strcmp(cname, "__builtin_fabs") == 0) inst.call_name = xstrdup("fabs");
                else if (strcmp(cname, "__builtin_fabsf") == 0) inst.call_name = xstrdup("fabsf");
                else if (strcmp(cname, "__builtin_fabsl") == 0) inst.call_name = xstrdup("fabsl");
                else inst.call_name = xstrdup(cname + 10);
            } else {
                int is_direct = 0;
                /* If the callee's type is a bare function type (TY_FUNC),
                 * it's a direct call — even if the function is only declared
                 * `extern` in this TU (not defined, so absent from g_ir_tu).
                 * Without this, extern function calls are wrongly lowered to
                 * indirect calls through an uninitialized function-pointer
                 * variable, which crashes at runtime. */
                if (e->u.call.callee->type.kind == TY_FUNC) {
                    is_direct = 1;
                } else if (e->u.call.callee->u.var.pkg) {
                    /* Package-qualified name (runtime.printf) — always a direct
                     * named call; the defining TU is linked separately. */
                    is_direct = 1;
                } else if (g_ir_tu) {
                    for (size_t i = 0; i < g_ir_tu->functions.len; i++) {
                        if (strcmp(g_ir_tu->functions.data[i].name, cname) == 0) {
                            is_direct = 1;
                            break;
                        }
                    }
                }
                if (is_direct) {
                    const char *aliased = lookup_asm_alias(cname);
                    inst.call_name = xstrdup(aliased ? aliased : cname);
                } else {
                    /* Function-pointer variable: the callee is loaded from a slot.
                     * lower_expr returns the loaded SSA value. */
                    inst.call_callee = lower_expr(fn, st, e->u.call.callee);
                }
            }
        } else {
            /* Indirect call: lower the callee expression to an SSA value. */
            inst.call_callee = lower_expr(fn, st, e->u.call.callee);
        }
        ir_call_reserve_args(&inst, total_nargs);
        if (ret_in_mem) {
            inst.call_args[0] = sret_addr;
            inst.call_arg_on_stack[0] = 0;
            for (int i = 0; i < nargs; i++) {
                inst.call_args[i + 1] = arg_vals[i];
                inst.call_arg_on_stack[i + 1] = arg_on_stack[i];
                inst.call_arg_nbytes[i + 1] = arg_nbytes[i];
            }
        } else {
            for (int i = 0; i < nargs; i++) {
                inst.call_args[i] = arg_vals[i];
                inst.call_arg_on_stack[i] = arg_on_stack[i];
                inst.call_arg_nbytes[i] = arg_nbytes[i];
            }
        }
        /* Register-returned structs: result eightbytes land in dst(/b); the
         * expression value is still the slot address.  MEMORY structs: value
         * is the sret pointer (width 8). */
        if (is_x87_ret) {
            inst.width = 8;
            inst.is_unsigned = 1;
            ir_inst_array_push(&fn->insts, inst);
            /* Keep the result-slot pointer live across the call so RA
             * spills it (or uses a callee-saved home) and codegen can
             * fstpt into it after the callee returns. */
            IRValue keep = emit_alloca(fn, 8, 8, 1, e->loc);
            emit_inst_w(fn, IR_STORE, -1, keep, slot_addr, 0, 8, 1, e->loc);
            free(arg_vals);
            free(arg_on_stack);
            free(arg_nbytes);
            return v;
        }
        if (is_ret_struct && ret_nreg > 0) {
            /* Result eightbytes are INTEGER (RAX/RDX or x0/x1) or SSE
             * (XMM0/XMM1).  A Darwin HFA result is bits in v0/v1; the SSA
             * values stay integer so the GP allocator owns them and the
             * backend fmovs at the call boundary. */
            int hfa_ret = abi_is_hfa(e->type);
            SysVRegClass store_cls[4];
            for (int k = 0; k < ret_nreg && k < 4; k++)
                store_cls[k] = hfa_ret ? SYSV_CLS_INTEGER : ret_cls[k];
            int vecw = sysv_sse_vec_bytes(ret_nreg, type_size(e->type), store_cls);
            inst.width = vecw ? vecw : 8;
            inst.is_float = !hfa_ret && (ret_cls[0] == SYSV_CLS_SSE);
            if (hfa_ret) inst.align16 = A64_MARK_HFA;
            inst.is_unsigned = 1;
            ir_inst_array_push(&fn->insts, inst);
            set_value_type(fn, ret_lo, vecw ? vecw : 8, vecw ? 0 : 1);
            if (!hfa_ret && ret_cls[0] == SYSV_CLS_SSE)
                set_value_float(fn, ret_lo, vecw ? 4 : 1);
            if (ret_hi >= 0) {
                set_value_type(fn, ret_hi, 8, 1);
                if (!hfa_ret && ret_cls[1] == SYSV_CLS_SSE)
                    set_value_float(fn, ret_hi, 1);
            }
            if (ret_c >= 0) {
                set_value_type(fn, ret_c, 8, 1);
                if (!hfa_ret && ret_cls[2] == SYSV_CLS_SSE)
                    set_value_float(fn, ret_c, 1);
            }
            if (ret_d >= 0) {
                set_value_type(fn, ret_d, 8, 1);
                if (!hfa_ret && ret_cls[3] == SYSV_CLS_SSE)
                    set_value_float(fn, ret_d, 1);
            }
            IRValue ebs[4] = { ret_lo, ret_hi, ret_c, ret_d };
            store_agg_regs(fn, slot_addr, type_size(e->type), ret_nreg,
                           store_cls, ebs, e->loc);
            free(arg_vals);
            free(arg_on_stack);
            free(arg_nbytes);
            return slot_addr;
        }
        inst.width = ret_in_mem ? 8
                      : (is_void ? 0 : (e->type.width ? e->type.width : 4));
        inst.is_unsigned = ret_in_mem ? 1 : e->type.is_unsigned;
        ir_inst_array_push(&fn->insts, inst);
        if (!is_void && !is_ret_struct) {
            set_value_type(fn, v, inst.width, inst.is_unsigned);
            if (e->type.kind == TY_FLOAT)
                set_value_float(fn, v, 1);
        } else if (ret_in_mem) {
            set_value_type(fn, v, 8, 1);
        } else if (type_is_empty_struct(e->type)) {
            set_value_type(fn, v, 8, 1);
        }
        free(arg_vals);
        free(arg_on_stack);
        free(arg_nbytes);
        return v;
    }
    case EX_ADDR: {
        Expr *op = e->u.addr.operand;
        if (op->kind == EX_VAR) {
            const IRSlot *entry = irsymtable_find(st, op->u.var.name);
            if (!entry) {
                /* Not a variable — must be a function name (`&func`). */
                IRValue v = new_value(fn);
                emit_inst_w(fn, IR_FADDR, v, -1, -1, 0, 8, 1, e->loc);
                fn->insts.data[fn->insts.len - 1].call_name = xstrdup(op->u.var.name);
                return v;
            }
            if (entry->is_global) return emit_gaddr(fn, slot_global_name(entry), e->loc, entry->is_tls);
            return emit_bin_w(fn, IR_ADDR, entry->slot, -1, 8, 1, e->loc);
        }
        if (op->kind == EX_DEREF) {
            /* &*p == p */
            return lower_expr(fn, st, op->u.deref.operand);
        }
        if (op->kind == EX_INDEX || op->kind == EX_MEMBER
            || op->kind == EX_COMPOUND_LITERAL) {
            return lower_lvalue_addr(fn, st, op);
        }
        return -1;
    }
    case EX_DEREF: {
        IRValue ptr = lower_expr(fn, st, e->u.deref.operand);
        if (e->type.kind == TY_STRUCT || e->type.kind == TY_ARRAY || e->type.is_vector || type_is_pair16(e->type)) return ptr;
        /* Function lvalue (`*fp` where fp : ptr(func)): no load — the function
         * pointer value IS the caldehyde; it decays at the call site. */
        if (e->type.kind == TY_FUNC) return ptr;
        int w = e->type.kind == TY_PTR ? 8 : (e->type.width ? e->type.width : 4);
        int u = e->type.is_unsigned;
        IRValue v = new_value(fn);
        emit_inst_w(fn, IR_LOAD_PTR, v, ptr, -1, 0, w, u, e->loc);
        mark_last_volatile(fn, e->type.is_volatile);
        if (e->type.kind == TY_FLOAT) set_value_float(fn, v, 1);
        return v;
    }
    case EX_INDEX: {
        /* Same as *(a + i*sizeof(elem)) for rvalue use. */
        IRValue base = lower_expr(fn, st, e->u.idx.array);
        IRValue idx  = lower_expr(fn, st, e->u.idx.index);
        IRValue esize_v = lower_sizeof_type(fn, st, e->type, e->loc);
        int iw = get_value_width(fn, idx), iu = get_value_is_unsigned(fn, idx);
        IRValue idx8 = coerce(fn, idx, iw, iu, 8, 0, e->loc);
        IRValue off = emit_bin_w(fn, IR_MUL, idx8, esize_v, 8, 1, e->loc);
        IRValue addr = emit_bin_w(fn, IR_ADD, base, off, 8, 1, e->loc);
        int w = e->type.kind == TY_PTR ? 8 : (e->type.width ? e->type.width : 4);
        int u = e->type.is_unsigned;
        if (e->type.kind == TY_ARRAY) return addr;
        if (e->type.kind == TY_STRUCT || e->type.is_vector || type_is_pair16(e->type)) return addr;
        IRValue v = new_value(fn);
        emit_inst_w(fn, IR_LOAD_PTR, v, addr, -1, 0, w, u, e->loc);
        mark_last_volatile(fn, e->type.is_volatile);
        if (e->type.kind == TY_FLOAT) set_value_float(fn, v, 1);
        return v;
    }
    case EX_MEMBER: {
        IRValue addr = lower_lvalue_addr(fn, st, e);
        /* Struct/array members: return address (decay). Scalar/pointer: load.
         * Also handle array-to-pointer decay performed in-place by sema: the
         * node's type may be TY_PTR but the underlying struct field is TY_ARRAY
         * — in that case the member's address IS the pointer value. */
        if (e->type.kind == TY_STRUCT || e->type.is_vector || type_is_pair16(e->type)) return addr;
        if (e->type.kind == TY_ARRAY)  return addr;
        if (e->type.kind == TY_PTR && member_field_is_array(e)) return addr;
        int bit_width = 0, bit_offset = 0, unit_width = 0, is_be_bf = 0, m_is_unsigned = 0;
        int is_bf = member_bitfield(e, &bit_width, &bit_offset, &unit_width, &is_be_bf, &m_is_unsigned);
        int is_be = member_struct_be(e);
        if (is_bf) {
            BFUnit unit = emit_bf_load(fn, addr, bit_offset, bit_width,
                                       unit_width, is_be_bf, e->loc);
            int w = bf_work_width(&unit);
            IRValue v = emit_bf_extract(fn, unit, bit_offset, bit_width, e->loc);
            mark_last_volatile(fn, e->type.is_volatile);
            int u = 1;
            int is_signed_bf = (!m_is_unsigned && !e->type.is_bool);
            if (e->type.enum_id > 0) {
                is_signed_bf = 0;
                if (g_ir_tu && (size_t)(e->type.enum_id - 1) < g_ir_tu->enums.len) {
                    const EnumDef *ed = &g_ir_tu->enums.data[e->type.enum_id - 1];
                    int k;
                    for (k = 0; k < ed->num_constants; k++) {
                        if (ed->constants[k].value < 0) {
                            is_signed_bf = 1;
                            break;
                        }
                    }
                }
            }
            if (is_signed_bf && bit_width < w * 8) {
                int shift = w * 8 - bit_width;
                IRValue s = emit_iconst_w(fn, shift, 8, 1, e->loc);
                IRValue up = new_value(fn);
                emit_inst_w(fn, IR_SHL, up, v, s, 0, w, 0, e->loc);
                IRValue down = new_value(fn);
                emit_inst_w(fn, IR_SHR, down, up, s, 0, w, 0, e->loc);
                v = down;
                u = 0;
            }
            return coerce(fn, v, w, u, e->type.width ? e->type.width : 4,
                          u, e->loc);
        }
        int w = e->type.kind == TY_PTR ? 8 : (e->type.width ? e->type.width : 4);
        int u = e->type.is_unsigned;
        IRValue v = new_value(fn);
        emit_inst_w(fn, IR_LOAD_PTR, v, addr, -1, 0, w, u, e->loc);
        mark_last_volatile(fn, e->type.is_volatile);
        if (is_be && (w == 2 || w == 4 || w == 8)
            && e->type.kind != TY_STRUCT && e->type.kind != TY_ARRAY)
            v = emit_bswap_val(fn, v, w, e->loc);
        if (e->type.kind == TY_FLOAT) set_value_float(fn, v, 1);
        return v;
    }
    case EX_CAST: {
        if (e->type.kind == TY_STRUCT && e->type.tag && strncmp(e->type.tag, "__complex_", 10) == 0) {
            Type cty = e->type;
            int total_sz = type_size(cty);
            int elem_sz = total_sz / 2;
            int is_float = (strstr(cty.tag, "float") != NULL || strstr(cty.tag, "double") != NULL || strstr(cty.tag, "ldouble") != NULL);
            
            int is_unsigned = (strstr(cty.tag, "unsigned") != NULL);
            IRValue vr, vi;
            if (e->u.cast.operand->type.kind == TY_STRUCT && e->u.cast.operand->type.tag &&
                strncmp(e->u.cast.operand->type.tag, "__complex_", 10) == 0) {
                /* Complex to complex cast (e.g. float to double, double to int) */
                IRValue src_addr = lower_expr(fn, st, e->u.cast.operand);
                int src_elem_sz = type_size(e->u.cast.operand->type) / 2;
                int src_is_float = (strstr(e->u.cast.operand->type.tag, "float") || strstr(e->u.cast.operand->type.tag, "double") || strstr(e->u.cast.operand->type.tag, "ldouble")) ? 1 : 0;
                int src_is_unsigned = (strstr(e->u.cast.operand->type.tag, "unsigned") != NULL);
                vr = new_value(fn);
                emit_inst_w(fn, IR_LOAD_PTR, vr, src_addr, -1, 0, src_elem_sz, src_is_unsigned, e->loc);
                if (src_is_float) set_value_float(fn, vr, 1);
                IRValue off = new_value(fn);
                emit_inst_w(fn, IR_CONST, off, -1, -1, src_elem_sz, 8, 1, e->loc);
                IRValue iaddr = emit_bin_w(fn, IR_ADD, src_addr, off, 8, 1, e->loc);
                vi = new_value(fn);
                emit_inst_w(fn, IR_LOAD_PTR, vi, iaddr, -1, 0, src_elem_sz, src_is_unsigned, e->loc);
                if (src_is_float) set_value_float(fn, vi, 1);
                
                if (src_is_float != is_float) {
                    if (src_is_float) {
                        vr = convert_numeric(fn, vr, src_elem_sz, elem_sz, is_unsigned, 0, e->loc);
                        vi = convert_numeric(fn, vi, src_elem_sz, elem_sz, is_unsigned, 0, e->loc);
                    } else {
                        vr = convert_numeric(fn, vr, src_elem_sz, elem_sz, 0, 1, e->loc);
                        vi = convert_numeric(fn, vi, src_elem_sz, elem_sz, 0, 1, e->loc);
                    }
                } else if (is_float) {
                    if (src_elem_sz != elem_sz) {
                        vr = convert_numeric(fn, vr, src_elem_sz, elem_sz, 0, 1, e->loc);
                        vi = convert_numeric(fn, vi, src_elem_sz, elem_sz, 0, 1, e->loc);
                    }
                } else {
                    if (src_elem_sz != elem_sz) {
                        vr = coerce(fn, vr, src_elem_sz, src_is_unsigned, elem_sz, is_unsigned, e->loc);
                        vi = coerce(fn, vi, src_elem_sz, src_is_unsigned, elem_sz, is_unsigned, e->loc);
                    }
                }
            } else {
                /* Scalar to complex cast: imag is 0 */
                vr = lower_expr(fn, st, e->u.cast.operand);
                if (is_float) {
                    if (!get_value_is_float(fn, vr) || get_value_width(fn, vr) != elem_sz)
                        vr = convert_numeric(fn, vr, get_value_width(fn, vr), elem_sz, 0, 1, e->loc);
                    vi = emit_float_const(fn, elem_sz, 0, e->loc);
                } else {
                    if (get_value_is_float(fn, vr)) {
                        vr = convert_numeric(fn, vr, get_value_width(fn, vr), elem_sz, is_unsigned, 0, e->loc);
                    } else if (get_value_width(fn, vr) != elem_sz) {
                        vr = coerce(fn, vr, get_value_width(fn, vr), get_value_is_unsigned(fn, vr), elem_sz, is_unsigned, e->loc);
                    }
                    vi = new_value(fn);
                    emit_inst_w(fn, IR_CONST, vi, -1, -1, 0, elem_sz, is_unsigned, e->loc);
                }
            }
            IRValue slot = emit_alloca(fn, total_sz, 8, 1, e->loc);
            IRValue addr = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, e->loc);
            emit_inst_w(fn, IR_STORE_PTR, -1, addr, vr, 0, elem_sz, is_unsigned, e->loc);
            IRValue off = new_value(fn);
            emit_inst_w(fn, IR_CONST, off, -1, -1, elem_sz, 8, 1, e->loc);
            IRValue iaddr = emit_bin_w(fn, IR_ADD, addr, off, 8, 1, e->loc);
            emit_inst_w(fn, IR_STORE_PTR, -1, iaddr, vi, 0, elem_sz, is_unsigned, e->loc);
            return addr;
        }
        if (e->u.cast.operand->type.kind == TY_STRUCT && e->u.cast.operand->type.tag &&
            strncmp(e->u.cast.operand->type.tag, "__complex_", 10) == 0 &&
            (e->type.kind == TY_INT || e->type.kind == TY_FLOAT || e->type.is_bool)) {
            IRValue src_addr = lower_expr(fn, st, e->u.cast.operand);
            int src_elem_sz = type_size(e->u.cast.operand->type) / 2;
            int src_is_float = (strstr(e->u.cast.operand->type.tag, "float") || strstr(e->u.cast.operand->type.tag, "double") || strstr(e->u.cast.operand->type.tag, "ldouble")) ? 1 : 0;
            IRValue vr = new_value(fn);
            emit_inst_w(fn, IR_LOAD_PTR, vr, src_addr, -1, 0, src_elem_sz, 1, e->loc);
            if (src_is_float) set_value_float(fn, vr, 1);
            int vw = get_value_width(fn, vr), vu = get_value_is_unsigned(fn, vr), vf = get_value_is_float(fn, vr);
            int dw = e->type.kind == TY_PTR ? 8 : (e->type.width ? e->type.width : 4);
            int du = e->type.is_unsigned, df = (e->type.kind == TY_FLOAT);
            if (e->type.is_bool) {
                IRValue n = bool_normalize(fn, vr, vw, vu, vf, e->loc);
                return coerce(fn, n, 4, 0, dw, du, e->loc);
            }
            if (vf != df) return convert_numeric(fn, vr, vw, dw, du, df, e->loc);
            if (df) return (vw == dw) ? vr : convert_numeric(fn, vr, vw, dw, du, 1, e->loc);
            return coerce(fn, vr, vw, vu, dw, du, e->loc);
        }
        if (e->type.is_vector) {
            IRValue x = lower_expr(fn, st, e->u.cast.operand);
            if (e->u.cast.operand->type.is_vector) return x;
            int sw = get_value_width(fn, x);
            int sf = get_value_is_float(fn, x);
            IRValue slot = emit_alloca(fn, e->type.width, 16, 1, e->loc);
            IRValue addr = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, e->loc);
            emit_zero_bytes(fn, addr, e->type.width, e->loc);
            int store_w = (sw > e->type.width) ? e->type.width : (sw > 0 ? sw : e->type.width);
            emit_inst_w(fn, IR_STORE_PTR, -1, addr, x, 0, store_w, 1, e->loc);
            if (sf) fn->insts.data[fn->insts.len - 1].is_float = 1;
            return addr;
        }
        if (e->u.cast.operand->type.is_vector) {
            IRValue src_addr = lower_expr(fn, st, e->u.cast.operand);
            int dw = e->type.kind == TY_PTR ? 8 : (e->type.width ? e->type.width : 4);
            int du = e->type.is_unsigned;
            int df = (e->type.kind == TY_FLOAT);
            IRValue v = new_value(fn);
            emit_inst_w(fn, IR_LOAD_PTR, v, src_addr, -1, 0, dw, du, e->loc);
            if (df) set_value_float(fn, v, 1);
            return v;
        }
        if (e->type.is_decimal || e->u.cast.operand->type.is_decimal) {
            IRValue x = lower_expr(fn, st, e->u.cast.operand);
            if (e->type.is_decimal)
                return convert_to_decimal(fn, x, e->u.cast.operand->type, e->type, e->loc);
            return convert_from_decimal(fn, x, e->u.cast.operand->type, e->type, e->loc);
        }
        if (type_is_i128(e->type) || type_is_i128(e->u.cast.operand->type)) {
            IRValue x = lower_expr(fn, st, e->u.cast.operand);
            if (type_is_i128(e->type) && type_is_i128(e->u.cast.operand->type))
                return x;
            if (type_is_i128(e->type)) {
                if (get_value_is_float(fn, x))
                    return emit_float_to_i128(fn, x, e->type.is_unsigned, e->loc);
                return i128_from_scalar(fn, x, get_value_width(fn, x),
                                        get_value_is_unsigned(fn, x),
                                        e->type.is_unsigned, e->loc);
            }
            IRValue lo, hi;
            i128_load2(fn, x, &lo, &hi, e->loc);
            if (e->type.is_bool)
                return i128_nz(fn, x, e->loc);
            if (e->type.kind == TY_FLOAT) {
                return emit_i128_to_float(fn, x, e->u.cast.operand->type.is_unsigned,
                                          e->type.width, e->loc);
            }
            int dw = e->type.kind == TY_PTR ? 8 : (e->type.width ? e->type.width : 4);
            int du = e->type.kind == TY_PTR ? 1 : e->type.is_unsigned;
            return coerce(fn, lo, 8, 1, dw, du, e->loc);
        }
        IRValue x = lower_expr(fn, st, e->u.cast.operand);
        int sw = get_value_width(fn, x), su = get_value_is_unsigned(fn, x);
        int sf = get_value_is_float(fn, x);
        int dw = e->type.kind == TY_PTR ? 8 : (e->type.width ? e->type.width : 4);
        int du = e->type.kind == TY_PTR ? 1 : e->type.is_unsigned;
        int df = (e->type.kind == TY_FLOAT);
        if (e->type.is_bool) {
            IRValue n = bool_normalize(fn, x, sw, su, sf, e->loc);
            return coerce(fn, n, 4, 0, dw, du, e->loc);
        }
        if (sf != df) {
            long double fv;
            if (sf && !df && fold_const_float(e->u.cast.operand, &fv, NULL)) {
                /* GCC folds out-of-range float→int constants to the destination
                 * extrema (INT_MAX / LLONG_MAX / ULLONG_MAX, etc.), not to the
                 * x86 integer-indefinite 0x8000… used at runtime. */
                int bits = dw * 8;
                if (bits <= 0) bits = 32;
                if (bits > 64) bits = 64;
                long long imm;
                if (du) {
                    unsigned long long maxu = (bits >= 64)
                        ? ~0ULL
                        : ((1ULL << bits) - 1ULL);
                    long double hi = (long double)maxu;
                    unsigned long long uv;
                    if (fv >= hi) uv = maxu;
                    else if (fv <= 0) uv = 0;
                    else uv = (unsigned long long)fv;
                    imm = (long long)uv;
                } else {
                    unsigned long long mag = (bits >= 64)
                        ? (1ULL << 63)
                        : (1ULL << (bits - 1));
                    long double hi = (long double)(mag - 1ULL);
                    long double lo = -(long double)mag;
                    if (fv > hi) fv = hi;
                    if (fv < lo) fv = lo;
                    imm = (long long)fv;
                    if (bits >= 64 && fv <= lo)
                        imm = (long long)0x8000000000000000ULL;
                }
                IRValue c = new_value(fn);
                emit_inst_w(fn, IR_CONST, c, -1, -1, imm, dw, du, e->loc);
                return c;
            }
            /* Cross-domain cast: float ↔ int (SITOFP / FPTOSI). */
            return convert_numeric(fn, x, sw, dw, du, df, e->loc);
        }
        if (df) {
            /* float ↔ float width change (float ↔ double). */
            if (sw == dw) return x;
            return convert_numeric(fn, x, sw, dw, du, 1, e->loc);
        }
        return coerce(fn, x, sw, su, dw, du, e->loc);
    }
    case EX_INC_DEC: {
        /* ++lvalue / --lvalue (prefix or postfix).
         * Sema guarantees operand is an lvalue of int or pointer type.
         * Step = 1 for int, sizeof(pointee) for pointer. */
        Expr *lv = e->u.incdec.operand;
        int is_inc = e->u.incdec.is_inc;
        int is_prefix = e->u.incdec.is_prefix;
        int is_ptr = (lv->type.kind == TY_PTR);
        int lw = is_ptr ? 8 : (lv->type.width ? lv->type.width : 4);
        int lu = is_ptr ? 1 : lv->type.is_unsigned;
        IRValue step_v = -1;
        if (is_ptr && lv->type.pointee)
            step_v = emit_pointee_stride(fn, st, *lv->type.pointee, e->loc);

        if (type_is_i128(lv->type)) {
            IRValue addr = lower_lvalue_addr(fn, st, lv);
            IRValue one = i128_alloc(fn, e->loc);
            IRValue c1 = i64imm(fn, 1, e->loc);
            i128_store2(fn, one, c1, i64imm(fn, 0, e->loc), e->loc);
            IRValue neu = i128_alloc(fn, e->loc);
            if (is_inc) i128_add(fn, neu, addr, one, e->loc);
            else i128_sub(fn, neu, addr, one, e->loc);
            IRValue old = i128_alloc(fn, e->loc);
            emit_struct_copy(fn, old, addr, 16, e->loc);
            emit_struct_copy(fn, addr, neu, 16, e->loc);
            return is_prefix ? neu : old;
        }

        if (lv->type.is_decimal) {
            IRValue addr = lower_lvalue_addr(fn, st, lv);
            int w = lv->type.width ? lv->type.width : 8;
            IRValue oldv;
            if (w == 16) {
                oldv = i128_alloc(fn, e->loc);
                emit_struct_copy(fn, oldv, addr, 16, e->loc);
            } else {
                oldv = new_value(fn);
                emit_inst_w(fn, IR_LOAD_PTR, oldv, addr, -1, 0, w, 0, e->loc);
                set_value_decimal(fn, oldv);
            }
            unsigned long long lo = 0, hi = 0;
            dfp_from_str("1", w, &lo, &hi);
            IRValue one = emit_dfp_const(fn, w, lo, hi, e->loc);
            IRValue neu = emit_dfp_arith(fn, w, is_inc ? 0 : 1, oldv, one, e->loc);
            if (w == 16)
                emit_struct_copy(fn, addr, neu, 16, e->loc);
            else {
                emit_inst_w(fn, IR_STORE_PTR, -1, addr, neu, 0, w, 0, e->loc);
                fn->insts.data[fn->insts.len - 1].is_float = 1;
            }
            return is_prefix ? neu : oldv;
        }

        if (lv->type.kind == TY_FLOAT) {
            /* ++x on a float steps by 1.0 in the float domain; the integer
             * path below would add the integer 1 to the bit pattern. */
            IRValue addr = lower_lvalue_addr(fn, st, lv);
            IRValue old = new_value(fn);
            emit_inst_w(fn, IR_LOAD_PTR, old, addr, -1, 0, lw, lu, e->loc);
            set_value_float(fn, old, 1);
            IRValue one;
            if (lw == 16) {
                one = emit_ld_const(fn, 1.0L, e->loc);
            } else {
                int64_t bits;
                if (lw == 4) { float f = 1.0f; bits = 0; memcpy(&bits, &f, sizeof f); }
                else { double d = 1.0; memcpy(&bits, &d, sizeof d); }
                one = emit_float_const(fn, lw, bits, e->loc);
            }
            IRValue neu = emit_bin_w(fn, is_inc ? IR_FADD : IR_FSUB, old, one,
                                     lw, lu, e->loc);
            set_value_float(fn, neu, 1);
            fn->insts.data[fn->insts.len - 1].is_float = 1;
            emit_inst_w(fn, IR_STORE_PTR, -1, addr, neu, 0, lw, lu, e->loc);
            fn->insts.data[fn->insts.len - 1].is_float = 1;
            return is_prefix ? neu : old;
        }

        /* Promotable path: simple non-pinned, non-global scalar variable.
         * Use IR_LOAD/IR_STORE on the slot so mem2reg can still promote it. */
        if (lv->kind == EX_VAR) {
            const IRSlot *entry = irsymtable_find(st, lv->u.var.name);
            if (entry && !entry->is_global && !entry->pinned) {
                IRValue old = new_value(fn);
                emit_inst_w(fn, IR_LOAD, old, entry->slot, -1, 0, lw, lu, e->loc);
                IRValue s = step_v;
                if (s < 0) {
                    s = new_value(fn);
                    emit_inst_w(fn, IR_CONST, s, -1, -1, 1, lw, lu, e->loc);
                } else {
                    s = coerce(fn, s, 8, 1, lw, lu, e->loc);
                }
                IRValue neu = emit_bin_w(fn, is_inc ? IR_ADD : IR_SUB,
                                         old, s, lw, lu, e->loc);
                if (lv->type.is_bool) {
                    neu = bool_normalize(fn, neu, lw, lu, 0, e->loc);
                    neu = coerce(fn, neu, 4, 0, lw, lu, e->loc);
                }
                emit_inst_w(fn, IR_STORE, -1, entry->slot, neu, 0, lw, lu, e->loc);
                return is_prefix ? neu : old;
            }
        }

        /* General path (pinned var, global, deref, index, member): go
         * through the address so the store side-effect reaches memory. */
        IRValue addr = lower_lvalue_addr(fn, st, lv);

        /* Bitfield member: full RMW with masking. */
        int bf_width = 0, bf_offset = 0, bf_unit = 0, is_be = 0, m_is_unsigned = 0;
        if (lv->kind == EX_MEMBER
            && member_bitfield(lv, &bf_width, &bf_offset, &bf_unit, &is_be, &m_is_unsigned)) {
            BFUnit unit = emit_bf_load(fn, addr, bf_offset, bf_width, bf_unit, is_be, e->loc);
            int uw = bf_work_width(&unit);
            int64_t mask = bitfield_mask64(bf_width);
            IRValue old_bits = emit_bf_extract(fn, unit, bf_offset, bf_width, e->loc);
            IRValue old_val = bitfield_to_promoted(fn, old_bits, uw, bf_width,
                                                   m_is_unsigned, lv->type.is_bool,
                                                   e->loc);
            int pw = uw > 4 ? uw : 4;
            int pu = (!m_is_unsigned && !lv->type.is_bool) ? 0 : 1;
            IRValue one = emit_iconst_w(fn, 1, pw, pu, e->loc);
            IRValue new_val = new_value(fn);
            emit_inst_w(fn, is_inc ? IR_ADD : IR_SUB, new_val, old_val, one, 0, pw, pu, e->loc);
            IRValue new_u = coerce(fn, new_val, pw, pu, uw, 1, e->loc);
            IRValue m_mask = emit_iconst_w(fn, mask, uw, 1, e->loc);
            IRValue new_masked = new_value(fn);
            emit_inst_w(fn, IR_BAND, new_masked, new_u, m_mask, 0, uw, 1, e->loc);
            unit = emit_bf_insert(fn, unit, new_masked, bf_offset, bf_width, e->loc);
            emit_bf_store(fn, addr, unit, is_be, e->loc);
            if (is_prefix) {
                IRValue neu = bitfield_to_promoted(fn, new_masked, uw, bf_width,
                                                   m_is_unsigned, lv->type.is_bool,
                                                   e->loc);
                return coerce(fn, neu, pw, pu,
                              lv->type.width ? lv->type.width : 4,
                              pu, e->loc);
            }
            return coerce(fn, old_val, pw, pu,
                          lv->type.width ? lv->type.width : 4,
                          pu, e->loc);
        }

        IRValue old = new_value(fn);
        emit_inst_w(fn, IR_LOAD_PTR, old, addr, -1, 0, lw, lu, e->loc);
        IRValue s = step_v;
        if (s < 0) {
            s = new_value(fn);
            emit_inst_w(fn, IR_CONST, s, -1, -1, 1, lw, lu, e->loc);
        } else {
            s = coerce(fn, s, 8, 1, lw, lu, e->loc);
        }
        IRValue neu = emit_bin_w(fn, is_inc ? IR_ADD : IR_SUB,
                                 old, s, lw, lu, e->loc);
        if (lv->type.is_bool) {
            neu = bool_normalize(fn, neu, lw, lu, 0, e->loc);
            neu = coerce(fn, neu, 4, 0, lw, lu, e->loc);
        }
        emit_inst_w(fn, IR_STORE_PTR, -1, addr, neu, 0, lw, lu, e->loc);
        return is_prefix ? neu : old;
    }
    case EX_COMPOUND_ASSIGN: {
        /* lvalue op= rvalue  →  read lvalue, compute lvalue op rvalue, store
         * back. The lvalue address is evaluated once. For pointers, += / -=
         * scale the rvalue by sizeof(pointee). */
        Expr *lv = e->u.comp.lvalue;
        BinOp op = e->u.comp.op;
        if (lv->type.is_vector) {
            return lower_vector_compound_assign(fn, st, e);
        }
        if (type_is_i128(lv->type)) {
            IRValue addr = lower_lvalue_addr(fn, st, lv);
            IRValue rhs = lower_expr(fn, st, e->u.comp.rvalue);
            IRValue res = lower_i128_binop(fn, addr, rhs, lv->type,
                                           e->u.comp.rvalue->type, op,
                                           lv->type, e->loc);
            emit_struct_copy(fn, addr, res, 16, e->loc);
            return res;
        }
        if (lv->type.kind == TY_STRUCT && lv->type.tag && strncmp(lv->type.tag, "__complex_", 10) == 0) {
            IRValue addr = lower_lvalue_addr(fn, st, lv);
            Expr fake_bin;
            memset(&fake_bin, 0, sizeof(fake_bin));
            fake_bin.kind = EX_BINOP;
            fake_bin.type = lv->type;
            fake_bin.loc = e->loc;
            fake_bin.u.bin.op = op;
            fake_bin.u.bin.l = lv;
            fake_bin.u.bin.r = e->u.comp.rvalue;
            IRValue res_addr = lower_complex_binop(fn, st, &fake_bin, lv->type,
                                                   e->u.comp.rvalue->type, op,
                                                   addr);
            emit_struct_copy(fn, addr, res_addr, type_size(lv->type), e->loc);
            return res_addr;
        }
        if (lv->type.is_decimal) {
            IRValue addr = lower_lvalue_addr(fn, st, lv);
            int w = lv->type.width ? lv->type.width : 8;
            IRValue oldv;
            if (w == 16) {
                oldv = i128_alloc(fn, e->loc);
                emit_struct_copy(fn, oldv, addr, 16, e->loc);
            } else {
                oldv = new_value(fn);
                emit_inst_w(fn, IR_LOAD_PTR, oldv, addr, -1, 0, w, 0, e->loc);
                set_value_decimal(fn, oldv);
            }
            IRValue rhs = lower_expr(fn, st, e->u.comp.rvalue);
            rhs = convert_to_decimal(fn, rhs, e->u.comp.rvalue->type, lv->type, e->loc);
            int aop = 0;
            if (op == BOP_SUB) aop = 1;
            else if (op == BOP_MUL) aop = 2;
            else if (op == BOP_DIV) aop = 3;
            IRValue res = emit_dfp_arith(fn, w, aop, oldv, rhs, e->loc);
            if (w == 16)
                emit_struct_copy(fn, addr, res, 16, e->loc);
            else {
                emit_inst_w(fn, IR_STORE_PTR, -1, addr, res, 0, w, 0, e->loc);
                fn->insts.data[fn->insts.len - 1].is_float = 1;
            }
            return res;
        }
        int is_ptr = (lv->type.kind == TY_PTR);
        int is_float = (lv->type.kind == TY_FLOAT);
        int lw = is_ptr ? 8 : (lv->type.width ? lv->type.width : 4);
        int lu = is_ptr ? 1 : lv->type.is_unsigned;
        /* C: E1 op= E2 is E1 = E1 op E2 (E1 once).  Integer operands are
         * promoted before the op, then the result converted back for the store
         * (20030128-1.c: unsigned char x /= short y). */
        int arith_w = lw, arith_u = lu;
        if (!is_ptr && !is_float && lw < 4) {
            arith_w = 4;
            arith_u = 0;
        }
        IROpcode ir_op;
        if (is_float) {
            switch (op) {
            case BOP_SUB: ir_op = IR_FSUB; break;
            case BOP_MUL: ir_op = IR_FMUL; break;
            case BOP_DIV: ir_op = IR_FDIV; break;
            default:      ir_op = IR_FADD; break;
            }
        } else {
            ir_op = bop_to_ir(op);
        }

        /* Promotable path: simple non-pinned, non-global scalar variable. */
        if (lv->kind == EX_VAR && !is_float) {
            const IRSlot *entry = irsymtable_find(st, lv->u.var.name);
            if (entry && !entry->is_global && !entry->pinned) {
                IRValue rhs = lower_expr(fn, st, e->u.comp.rvalue);
                IRValue old = new_value(fn);
                emit_inst_w(fn, IR_LOAD, old, entry->slot, -1, 0, lw, lu, e->loc);
                IRValue scaled = is_ptr
                    ? scale_rhs(fn, st, rhs, is_ptr, lv->type, op, e->loc)
                    : (get_value_is_float(fn, rhs)
                       ? convert_numeric(fn, rhs, get_value_width(fn, rhs), arith_w, arith_u, 0, e->loc)
                       : coerce(fn, rhs, get_value_width(fn, rhs),
                                get_value_is_unsigned(fn, rhs), arith_w, arith_u, e->loc));
                IRValue old_p = (arith_w == lw && arith_u == lu)
                    ? old : coerce(fn, old, lw, lu, arith_w, arith_u, e->loc);
                IRValue neu = emit_bin_w(fn, ir_op, old_p, scaled, arith_w, arith_u, e->loc);
                IRValue back = (arith_w == lw && arith_u == lu)
                    ? neu : coerce(fn, neu, arith_w, arith_u, lw, lu, e->loc);
                if (lv->type.is_bool) {
                    back = bool_normalize(fn, back, lw, lu, 0, e->loc);
                    back = coerce(fn, back, 4, 0, lw, lu, e->loc);
                }
                emit_inst_w(fn, IR_STORE, -1, entry->slot, back, 0, lw, lu, e->loc);
                return back;
            }
        }

        /* General path (pinned var, global, deref, index, member). */
        IRValue addr = lower_lvalue_addr(fn, st, lv);

        /* Bitfield member: extract, op, insert.  A plain LOAD/STORE of the
         * storage unit would clobber adjacent fields sharing the same word
         * (gcc.c-torture/execute/20000113-1.c). */
        int bf_width = 0, bf_offset = 0, bf_unit = 0, is_be = 0, m_is_unsigned = 0;
        if (!is_float && !is_ptr && lv->kind == EX_MEMBER
            && member_bitfield(lv, &bf_width, &bf_offset, &bf_unit, &is_be, &m_is_unsigned)) {
            IRValue rhs = lower_expr(fn, st, e->u.comp.rvalue);
            BFUnit unit = emit_bf_load(fn, addr, bf_offset, bf_width, bf_unit, is_be, e->loc);
            int uw = bf_work_width(&unit);
            int64_t mask = bitfield_mask64(bf_width);
            IRValue extracted = emit_bf_extract(fn, unit, bf_offset, bf_width, e->loc);
            /* Use the member's *declared* signedness, not the promoted
             * EX_MEMBER type.  An `unsigned int : 6` promotes to signed int
             * as an rvalue, but compound-assign must still extract it as
             * unsigned (51/3 == 17, not sign-extended -13/3). */
            IRValue old_val;
            if (!m_is_unsigned && !lv->type.is_bool && bf_width < uw * 8) {
                int shift = uw * 8 - bf_width;
                IRValue s = emit_iconst_w(fn, shift, 8, 1, e->loc);
                IRValue up = new_value(fn);
                emit_inst_w(fn, IR_SHL, up, extracted, s, 0, uw, 0, e->loc);
                old_val = new_value(fn);
                emit_inst_w(fn, IR_SHR, old_val, up, s, 0, uw, 0, e->loc);
            } else {
                old_val = extracted;
            }
            int cw = (uw > 4) ? uw : 4;
            int cu = (m_is_unsigned && bf_width >= 32) ? 1 : 0;
            if (uw > 4) cu = m_is_unsigned;
            IRValue old_p = coerce(fn, old_val, uw,
                                   (!m_is_unsigned && !lv->type.is_bool) ? 0 : 1,
                                   cw, cu, e->loc);
            int rw = get_value_width(fn, rhs), ru = get_value_is_unsigned(fn, rhs);
            IRValue rhs_p = coerce(fn, rhs, rw, ru, cw, cu, e->loc);
            IRValue neu = emit_bin_w(fn, ir_op, old_p, rhs_p, cw, cu, e->loc);
            IRValue neu_u = coerce(fn, neu, cw, cu, uw, 1, e->loc);
            IRValue m_mask2 = emit_iconst_w(fn, mask, uw, 1, e->loc);
            IRValue new_masked = new_value(fn);
            emit_inst_w(fn, IR_BAND, new_masked, neu_u, m_mask2, 0, uw, 1, e->loc);
            unit = emit_bf_insert(fn, unit, new_masked, bf_offset, bf_width, e->loc);
            emit_bf_store(fn, addr, unit, is_be, e->loc);
            IRValue ret = bitfield_to_promoted(fn, new_masked, uw, bf_width,
                                               m_is_unsigned, lv->type.is_bool,
                                               e->loc);
            return coerce(fn, ret, (uw > 4) ? uw : 4,
                          (!m_is_unsigned && !lv->type.is_bool) ? 0 : 1,
                          lv->type.width ? lv->type.width : 4,
                          (!m_is_unsigned && !lv->type.is_bool) ? 0 : lv->type.is_unsigned,
                          e->loc);
        }

        IRValue rhs = lower_expr(fn, st, e->u.comp.rvalue);
        IRValue old = new_value(fn);
        emit_inst_w(fn, IR_LOAD_PTR, old, addr, -1, 0, lw, lu, e->loc);
        if (is_float) set_value_float(fn, old, 1);
        IRValue scaled;
        int op_w = is_float ? lw : arith_w;
        int op_u = is_float ? lu : arith_u;
        if (is_float) {
            /* C99 6.5.16.2: E1 op= E2 is E1 = E1 op E2 (E1 once), so the
             * usual arithmetic conversions apply before the op.  `float f;
             * f -= 1e20;` is `(float)((double)f - 1e20)`, not a float sub. */
            int rw = get_value_width(fn, rhs);
            int rf = get_value_is_float(fn, rhs);
            if (rf && rw > op_w) op_w = rw;
            scaled = (rf && rw == op_w)
                     ? rhs
                     : convert_numeric(fn, rhs, rw, op_w, 0, 1, e->loc);
        } else if (is_ptr) {
            scaled = scale_rhs(fn, st, rhs, is_ptr, lv->type, op, e->loc);
        } else {
            scaled = (get_value_is_float(fn, rhs))
                     ? convert_numeric(fn, rhs, get_value_width(fn, rhs), arith_w, arith_u, 0, e->loc)
                     : coerce(fn, rhs, get_value_width(fn, rhs),
                              get_value_is_unsigned(fn, rhs), arith_w, arith_u, e->loc);
        }
        IRValue old_p = old;
        if (is_float && op_w != lw)
            old_p = convert_numeric(fn, old, lw, op_w, 0, 1, e->loc);
        else if (!is_float && !is_ptr && (arith_w != lw || arith_u != lu))
            old_p = coerce(fn, old, lw, lu, arith_w, arith_u, e->loc);
        IRValue neu = emit_bin_w(fn, ir_op, old_p, scaled, op_w, op_u, e->loc);
        if (is_float) set_value_float(fn, neu, 1);
        IRValue back = neu;
        if (is_float && op_w != lw)
            back = convert_numeric(fn, neu, op_w, lw, 0, 1, e->loc);
        else if (!is_float && !is_ptr && (arith_w != lw || arith_u != lu))
            back = coerce(fn, neu, arith_w, arith_u, lw, lu, e->loc);
        if (lv->type.is_bool) {
            back = bool_normalize(fn, back, lw, lu, 0, e->loc);
            back = coerce(fn, back, 4, 0, lw, lu, e->loc);
        }
        emit_inst_w(fn, IR_STORE_PTR, -1, addr, back, 0, lw, lu, e->loc);
        return back;
    }
    case EX_COMMA: {
        /* a, b: evaluate a for side effects (discard result), then b. */
        lower_expr(fn, st, e->u.comma.lhs);
        return lower_expr(fn, st, e->u.comma.rhs);
    }
    case EX_SIZEOF_TYPE:
        return lower_sizeof_type(fn, st, e->u.sizeof_t.target, e->loc);
    case EX_SIZEOF_EXPR: {
        /* sizeof of a VLA object is the size captured at the declaration,
         * not a re-evaluation of the bound.  sizeof of a VLA type still
         * re-evaluates via lower_sizeof_type. */
        const Expr *op = e->u.sizeof_e.operand;
        if (op && op->kind == EX_VAR) {
            const IRSlot *entry = irsymtable_find(st, op->u.var.name);
            if (entry && entry->is_vla && entry->vla_nbytes_slot >= 0) {
                IRValue addr = emit_bin_w(fn, IR_ADDR, entry->vla_nbytes_slot,
                                          -1, 8, 1, e->loc);
                IRValue v = new_value(fn);
                emit_inst_w(fn, IR_LOAD_PTR, v, addr, -1, 0, 8, 1, e->loc);
                return v;
            }
        }
        return lower_sizeof_type(fn, st, op ? op->type : e->type, e->loc);
    }
    case EX_ALIGNOF_TYPE: {
        IRValue v = new_value(fn);
        emit_inst_w(fn, IR_CONST, v, -1, -1, type_align(e->u.alignof_t.target),
                    8, 1, e->loc);
        return v;
    }
    case EX_ALIGNOF_EXPR: {
        long long al = type_align(e->u.alignof_e.operand->type);
        IRValue v = new_value(fn);
        emit_inst_w(fn, IR_CONST, v, -1, -1, al, 8, 1, e->loc);
        return v;
    }
    case EX_COMPOUND_LITERAL: {
        /* (Type){ ... }: materialize stack storage, initialize it, and return a
         * pointer to it (aggregates) or the loaded scalar value (scalars). */
        Type target = e->u.compound.target_type;
        IRValue addr = lower_compound_literal_addr(fn, st, e);
        if (target.kind == TY_ARRAY || target.kind == TY_STRUCT || target.is_vector)
            return addr;
        IRValue v = new_value(fn);
        emit_inst_w(fn, IR_LOAD_PTR, v, addr, -1, 0,
                    target.width ? target.width : 4, target.is_unsigned, e->loc);
        if (target.kind == TY_FLOAT) set_value_float(fn, v, 1);
        return v;
    }
    case EX_STMT_EXPR: {
        size_t mark = st->len;
        IRValue res = -1;
        const StmtArray *stmts = e->u.stmt_expr.stmts;
        if (stmts) {
            for (size_t i = 0; i < stmts->len; i++) {
                const Stmt *s = &stmts->data[i];
                if (i == stmts->len - 1 && s->kind == ST_EXPR && s->u.expr) {
                    res = lower_expr(fn, st, s->u.expr);
                } else {
                    lower_stmt(fn, st, s, g_ir_cur_fd);
                }
            }
        }
        st->len = mark;
        return res;
    }
    case EX_LABEL_ADDR: {
        /* &&label — emit IR_LADDR with the label's IR id.
         * The label must have been pre-assigned in the label pre-pass. */
        const char *lbl_name = e->u.label_addr.label;
        int lbl_id = g_ir_label_map ? labelmap_find(g_ir_label_map, lbl_name) : -1;
        if (lbl_id < 0) {
            /* A library call must not take the process down with it. */
            die_at(e->loc.file, e->loc.line, e->loc.col,
                   "use of undeclared label '%s'", lbl_name);
            return -1;
        }
        IRValue dst = new_value(fn);
        /* A label address is a code pointer, not a 32-bit int.  Width 4
         * makes the backend sign-extend it and drop the high half. */
        emit_inst_w(fn, IR_LADDR, dst, -1, -1, lbl_id, 8, 1, e->loc);
        return dst;
    }
    default: break;   /* EX_INIT_LIST is lowered in ST_DECL, not here */
    }
    /* unreachable */
    return -1;
}

/* Allocate a fresh label id. */
static int new_label(IRFunction *fn) {
    return fn->next_label_id++;
}

/* Emit a LABEL pseudo-instruction. */
static void emit_label(IRFunction *fn, int label, SourceLoc loc) {
    emit_inst(fn, IR_LABEL, -1, -1, -1, label, loc);
}

/* Emit unconditional branch. */
static void emit_br(IRFunction *fn, int label, SourceLoc loc) {
    emit_inst(fn, IR_BR, -1, -1, -1, label, loc);
}

/* Emit conditional branch: if cond true jump to t_label else f_label. */
static void emit_cbr(IRFunction *fn, IRValue cond, int t_label, int f_label,
                     SourceLoc loc) {
    /* CBR encoding: a=cond, imm=t_label, b=f_label */
    emit_inst(fn, IR_CBR, -1, cond, f_label, t_label, loc);
}

static IRValue lower_condition(IRFunction *fn, IRSymTable *st, const Expr *e) {
    if (fakecc_had_error()) return FAKECC_ERR;
    if (type_is_i128(e->type)) {
        IRValue addr = lower_expr(fn, st, e);
        return i128_nz(fn, addr, e->loc);
    }
    if (e->type.is_decimal) {
        IRValue v = lower_expr(fn, st, e);
        int w = e->type.width ? e->type.width : 8;
        IRValue z = emit_dfp_zero(fn, w, e->loc);
        return emit_dfp_cmp(fn, w, BOP_NE, v, z, e->loc);
    }
    if (e->type.kind == TY_STRUCT && e->type.tag && strncmp(e->type.tag, "__complex_", 10) == 0) {
        Type cty = e->type;
        int elem_sz = type_size(cty) / 2;
        int is_float = (strstr(cty.tag, "float") || strstr(cty.tag, "double") || strstr(cty.tag, "ldouble")) ? 1 : 0;
        IRValue addr = lower_expr(fn, st, e);
        IRValue vr = new_value(fn);
        emit_inst_w(fn, IR_LOAD_PTR, vr, addr, -1, 0, elem_sz, 1, e->loc);
        if (is_float) set_value_float(fn, vr, 1);
        IRValue off = new_value(fn);
        emit_inst_w(fn, IR_CONST, off, -1, -1, elem_sz, 8, 1, e->loc);
        IRValue iaddr = emit_bin_w(fn, IR_ADD, addr, off, 8, 1, e->loc);
        IRValue vi = new_value(fn);
        emit_inst_w(fn, IR_LOAD_PTR, vi, iaddr, -1, 0, elem_sz, 1, e->loc);
        if (is_float) set_value_float(fn, vi, 1);
        
        IRValue cmp_r, cmp_i;
        if (is_float) {
            IRValue zero = emit_float_const(fn, elem_sz, 0, e->loc);
            cmp_r = emit_bin_w(fn, IR_FCMP, vr, zero, elem_sz, 5 /* NE */, e->loc);
            cmp_i = emit_bin_w(fn, IR_FCMP, vi, zero, elem_sz, 5 /* NE */, e->loc);
        } else {
            IRValue zero = new_value(fn);
            emit_inst_w(fn, IR_CONST, zero, -1, -1, 0, elem_sz, 1, e->loc);
            cmp_r = emit_bin_w(fn, IR_NE, vr, zero, elem_sz, 1, e->loc);
            cmp_i = emit_bin_w(fn, IR_NE, vi, zero, elem_sz, 1, e->loc);
        }
        return emit_bin_w(fn, IR_BOR, cmp_r, cmp_i, 4, 0, e->loc);
    }
    if (e->type.kind == TY_FLOAT && !e->type.is_vector) {
        IRValue v = lower_expr(fn, st, e);
        return cbr_from_scalar(fn, v, e->loc);
    }
    return lower_expr(fn, st, e);
}

/* Label map: label name → IR label id.  Populated by a pre-pass over the
 * function body so forward gotos resolve to ids assigned before lowering. */
/* (LabelMap typedef and g_ir_label_map are forward-declared at the top of this file) */

static void labelmap_init(LabelMap *lm) {
    lm->names = NULL; lm->ids = NULL; lm->len = 0; lm->cap = 0;
}

static void labelmap_free(LabelMap *lm) {
    for (size_t i = 0; i < lm->len; i++) free(lm->names[i]);
    free(lm->names); free(lm->ids);
    lm->names = NULL; lm->ids = NULL; lm->len = 0; lm->cap = 0;
}

static int labelmap_find(const LabelMap *lm, const char *name) {
    for (size_t i = 0; i < lm->len; i++)
        if (strcmp(lm->names[i], name) == 0) return lm->ids[i];
    return -1;
}

static void labelmap_add(LabelMap *lm, const char *name, int id) {
    if (lm->len >= lm->cap) {
        lm->cap = lm->cap ? lm->cap * 2 : 8;
        lm->names = realloc(lm->names, lm->cap * sizeof(char *));
        lm->ids   = realloc(lm->ids, lm->cap * sizeof(int));
        if (!lm->names || !lm->ids) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
    }
    lm->names[lm->len] = xstrdup(name);
    lm->ids[lm->len] = id;
    lm->len++;
}

static void assign_label_ids(IRFunction *fn, LabelMap *lm, const Stmt *s);

static void assign_label_ids_expr(IRFunction *fn, LabelMap *lm, const Expr *e) {
    if (!e) return;
    switch (e->kind) {
    case EX_STMT_EXPR:
        if (e->u.stmt_expr.stmts) {
            for (size_t i = 0; i < e->u.stmt_expr.stmts->len; i++)
                assign_label_ids(fn, lm, &e->u.stmt_expr.stmts->data[i]);
        }
        break;
    case EX_UNARY:
        assign_label_ids_expr(fn, lm, e->u.un.operand);
        break;
    case EX_INC_DEC:
        assign_label_ids_expr(fn, lm, e->u.incdec.operand);
        break;
    case EX_BINOP:
        assign_label_ids_expr(fn, lm, e->u.bin.l);
        assign_label_ids_expr(fn, lm, e->u.bin.r);
        break;
    case EX_TERNARY:
        assign_label_ids_expr(fn, lm, e->u.tern.cond);
        assign_label_ids_expr(fn, lm, e->u.tern.then);
        assign_label_ids_expr(fn, lm, e->u.tern.else_);
        break;
    case EX_ASSIGN:
        assign_label_ids_expr(fn, lm, e->u.assign.lvalue);
        assign_label_ids_expr(fn, lm, e->u.assign.rvalue);
        break;
    case EX_COMPOUND_ASSIGN:
        assign_label_ids_expr(fn, lm, e->u.comp.lvalue);
        assign_label_ids_expr(fn, lm, e->u.comp.rvalue);
        break;
    case EX_COMMA:
        assign_label_ids_expr(fn, lm, e->u.comma.lhs);
        assign_label_ids_expr(fn, lm, e->u.comma.rhs);
        break;
    case EX_CAST:
        assign_label_ids_expr(fn, lm, e->u.cast.operand);
        break;
    case EX_CALL:
        assign_label_ids_expr(fn, lm, e->u.call.callee);
        for (size_t i = 0; i < e->u.call.args.len; i++)
            assign_label_ids_expr(fn, lm, e->u.call.args.data[i]);
        break;
    case EX_MEMBER:
        assign_label_ids_expr(fn, lm, e->u.member.obj);
        break;
    case EX_INDEX:
        assign_label_ids_expr(fn, lm, e->u.idx.array);
        assign_label_ids_expr(fn, lm, e->u.idx.index);
        break;
    case EX_DEREF:
        assign_label_ids_expr(fn, lm, e->u.deref.operand);
        break;
    case EX_ADDR:
        assign_label_ids_expr(fn, lm, e->u.addr.operand);
        break;
    case EX_SIZEOF_EXPR:
        assign_label_ids_expr(fn, lm, e->u.sizeof_e.operand);
        break;
    case EX_ALIGNOF_EXPR:
        assign_label_ids_expr(fn, lm, e->u.alignof_e.operand);
        break;
    case EX_COMPOUND_LITERAL:
        assign_label_ids_expr(fn, lm, e->u.compound.init);
        break;
    case EX_INIT_LIST:
        for (int i = 0; i < e->u.init_list.num_elements; i++)
            assign_label_ids_expr(fn, lm, e->u.init_list.elements[i]);
        break;
    default:
        break;
    }
}

/* Recursively assign a label id to every ST_LABEL in a statement. */
static void assign_label_ids(IRFunction *fn, LabelMap *lm, const Stmt *s) {
    if (!s) return;
    switch (s->kind) {
    case ST_LABEL: {
        int id = new_label(fn);
        labelmap_add(lm, s->u.label_s.name, id);
        assign_label_ids(fn, lm, s->u.label_s.stmt);
        break;
    }
    case ST_EXPR:
        assign_label_ids_expr(fn, lm, s->u.expr);
        break;
    case ST_DECL:
        if (s->u.decl.init) assign_label_ids_expr(fn, lm, s->u.decl.init);
        break;
    case ST_RETURN:
        if (s->u.value) assign_label_ids_expr(fn, lm, s->u.value);
        break;
    case ST_GOTO:
        if (s->u.goto_s.target_expr) assign_label_ids_expr(fn, lm, s->u.goto_s.target_expr);
        break;
    case ST_IF:
        assign_label_ids_expr(fn, lm, s->u.if_s.cond);
        assign_label_ids(fn, lm, s->u.if_s.then_s);
        if (s->u.if_s.else_s) assign_label_ids(fn, lm, s->u.if_s.else_s);
        break;
    case ST_WHILE:
        assign_label_ids_expr(fn, lm, s->u.while_s.cond);
        assign_label_ids(fn, lm, s->u.while_s.body);
        break;
    case ST_DO_WHILE:
        assign_label_ids(fn, lm, s->u.do_s.body);
        assign_label_ids_expr(fn, lm, s->u.do_s.cond);
        break;
    case ST_FOR:
        if (s->u.for_s.init) assign_label_ids(fn, lm, s->u.for_s.init);
        if (s->u.for_s.cond) assign_label_ids_expr(fn, lm, s->u.for_s.cond);
        if (s->u.for_s.step) assign_label_ids_expr(fn, lm, s->u.for_s.step);
        assign_label_ids(fn, lm, s->u.for_s.body);
        break;
    case ST_SWITCH:
        assign_label_ids_expr(fn, lm, s->u.switch_s.cond);
        if (s->u.switch_s.body) assign_label_ids(fn, lm, s->u.switch_s.body);
        for (int i = 0; i < s->u.switch_s.num_cases; i++) {
            const SwitchCase *arm = &s->u.switch_s.cases[i];
            for (size_t j = 0; j < arm->stmts.len; j++)
                assign_label_ids(fn, lm, &arm->stmts.data[j]);
        }
        break;
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.len; i++)
            assign_label_ids(fn, lm, &s->u.block.data[i]);
        break;
    default:
        break;
    }
}

static int vla_walk_top(void) {
    if (g_vla_walk_sp <= 0) return -1;
    return g_vla_walk_stack[g_vla_walk_sp - 1];
}

static void snapshot_vla_stmt(const LabelMap *lm, const Stmt *s);
static void snapshot_vla_expr(const LabelMap *lm, const Expr *e) {
    if (!e) return;
    if (e->kind == EX_STMT_EXPR && e->u.stmt_expr.stmts) {
        int saved = g_vla_walk_sp;
        for (size_t i = 0; i < e->u.stmt_expr.stmts->len; i++)
            snapshot_vla_stmt(lm, &e->u.stmt_expr.stmts->data[i]);
        g_vla_walk_sp = saved;
        return;
    }
    if (e->kind == EX_LABEL_ADDR && lm) {
        int id = labelmap_find(lm, e->u.label_addr.label);
        if (id >= 0 && id < g_ir_label_vla_id_count) {
            int vid = g_ir_label_vla_id[id];
            if (g_ir_computed_goto_vla_id == -2)
                g_ir_computed_goto_vla_id = vid;
            else if (g_ir_computed_goto_vla_id != vid)
                g_ir_computed_goto_vla_id = -3; /* mixed */
        }
        return;
    }
    switch (e->kind) {
    case EX_UNARY: snapshot_vla_expr(lm, e->u.un.operand); break;
    case EX_INC_DEC: snapshot_vla_expr(lm, e->u.incdec.operand); break;
    case EX_BINOP:
        snapshot_vla_expr(lm, e->u.bin.l);
        snapshot_vla_expr(lm, e->u.bin.r);
        break;
    case EX_TERNARY:
        snapshot_vla_expr(lm, e->u.tern.cond);
        snapshot_vla_expr(lm, e->u.tern.then);
        snapshot_vla_expr(lm, e->u.tern.else_);
        break;
    case EX_ASSIGN:
        snapshot_vla_expr(lm, e->u.assign.lvalue);
        snapshot_vla_expr(lm, e->u.assign.rvalue);
        break;
    case EX_COMPOUND_ASSIGN:
        snapshot_vla_expr(lm, e->u.comp.lvalue);
        snapshot_vla_expr(lm, e->u.comp.rvalue);
        break;
    case EX_COMMA:
        snapshot_vla_expr(lm, e->u.comma.lhs);
        snapshot_vla_expr(lm, e->u.comma.rhs);
        break;
    case EX_CAST: snapshot_vla_expr(lm, e->u.cast.operand); break;
    case EX_CALL:
        snapshot_vla_expr(lm, e->u.call.callee);
        for (size_t i = 0; i < e->u.call.args.len; i++)
            snapshot_vla_expr(lm, e->u.call.args.data[i]);
        break;
    case EX_MEMBER: snapshot_vla_expr(lm, e->u.member.obj); break;
    case EX_INDEX:
        snapshot_vla_expr(lm, e->u.idx.array);
        snapshot_vla_expr(lm, e->u.idx.index);
        break;
    case EX_DEREF: snapshot_vla_expr(lm, e->u.deref.operand); break;
    case EX_ADDR: snapshot_vla_expr(lm, e->u.addr.operand); break;
    case EX_SIZEOF_EXPR: snapshot_vla_expr(lm, e->u.sizeof_e.operand); break;
    case EX_ALIGNOF_EXPR: snapshot_vla_expr(lm, e->u.alignof_e.operand); break;
    case EX_COMPOUND_LITERAL: snapshot_vla_expr(lm, e->u.compound.init); break;
    case EX_INIT_LIST:
        for (int i = 0; i < e->u.init_list.num_elements; i++)
            snapshot_vla_expr(lm, e->u.init_list.elements[i]);
        break;
    default: break;
    }
}

static void snapshot_vla_stmt(const LabelMap *lm, const Stmt *s) {
    if (!s) return;
    switch (s->kind) {
    case ST_DECL:
        if (!g_vla_scan_addrs_only && type_is_vla(s->u.decl.type) && g_vla_walk_sp < 256)
            g_vla_walk_stack[g_vla_walk_sp++] = g_vla_walk_serial++;
        if (s->u.decl.init) snapshot_vla_expr(lm, s->u.decl.init);
        break;
    case ST_LABEL: {
        if (!g_vla_scan_addrs_only) {
            int id = labelmap_find(lm, s->u.label_s.name);
            if (id >= 0 && id < g_ir_label_vla_id_count)
                g_ir_label_vla_id[id] = vla_walk_top();
        }
        snapshot_vla_stmt(lm, s->u.label_s.stmt);
        break;
    }
    case ST_EXPR: snapshot_vla_expr(lm, s->u.expr); break;
    case ST_RETURN: snapshot_vla_expr(lm, s->u.value); break;
    case ST_GOTO: snapshot_vla_expr(lm, s->u.goto_s.target_expr); break;
    case ST_IF: {
        snapshot_vla_expr(lm, s->u.if_s.cond);
        int saved = g_vla_walk_sp;
        snapshot_vla_stmt(lm, s->u.if_s.then_s);
        g_vla_walk_sp = saved;
        snapshot_vla_stmt(lm, s->u.if_s.else_s);
        g_vla_walk_sp = saved;
        break;
    }
    case ST_WHILE: {
        snapshot_vla_expr(lm, s->u.while_s.cond);
        int saved = g_vla_walk_sp;
        snapshot_vla_stmt(lm, s->u.while_s.body);
        g_vla_walk_sp = saved;
        break;
    }
    case ST_DO_WHILE: {
        int saved = g_vla_walk_sp;
        snapshot_vla_stmt(lm, s->u.do_s.body);
        g_vla_walk_sp = saved;
        snapshot_vla_expr(lm, s->u.do_s.cond);
        break;
    }
    case ST_FOR: {
        int saved = g_vla_walk_sp;
        if (s->u.for_s.init) snapshot_vla_stmt(lm, s->u.for_s.init);
        snapshot_vla_expr(lm, s->u.for_s.cond);
        snapshot_vla_expr(lm, s->u.for_s.step);
        snapshot_vla_stmt(lm, s->u.for_s.body);
        g_vla_walk_sp = saved;
        break;
    }
    case ST_SWITCH: {
        snapshot_vla_expr(lm, s->u.switch_s.cond);
        int saved = g_vla_walk_sp;
        if (s->u.switch_s.body) snapshot_vla_stmt(lm, s->u.switch_s.body);
        for (int i = 0; i < s->u.switch_s.num_cases; i++)
            for (size_t j = 0; j < s->u.switch_s.cases[i].stmts.len; j++)
                snapshot_vla_stmt(lm, &s->u.switch_s.cases[i].stmts.data[j]);
        g_vla_walk_sp = saved;
        break;
    }
    case ST_BLOCK: {
        int saved = g_vla_walk_sp;
        for (size_t i = 0; i < s->u.block.len; i++)
            snapshot_vla_stmt(lm, &s->u.block.data[i]);
        g_vla_walk_sp = saved;
        break;
    }
    default:
        break;
    }
}

static void emit_restore_vla_sp(IRFunction *fn, int vla_id, SourceLoc loc) {
    int slot = -1;
    if (vla_id >= 0 && vla_id < g_ir_vla_count && g_ir_vla_sp_slots)
        slot = g_ir_vla_sp_slots[vla_id];
    else if (vla_id < 0)
        slot = g_ir_entry_sp_slot;
    if (slot < 0) return;
    IRValue slot_addr = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, loc);
    IRValue saved_sp = new_value(fn);
    emit_inst_w(fn, IR_LOAD_PTR, saved_sp, slot_addr, -1, 0, 8, 1, loc);
    emit_inst_w(fn, IR_STACK_RESTORE, -1, saved_sp, -1, 0, 8, 1, loc);
}

/* (g_ir_label_map is declared at the top of this file.) */


/* Lower a single statement, emitting instructions as needed. */
static void lower_stmt(IRFunction *fn, IRSymTable *st, const Stmt *s,
                       const FunctionDecl *cur_fd);

static void lower_stmt(IRFunction *fn, IRSymTable *st, const Stmt *s,
                       const FunctionDecl *cur_fd) {
    if (fakecc_had_error()) return;
    switch (s->kind) {
    case ST_DECL: {
        Type dty = s->u.decl.type;
        int dw = dty.kind == TY_ARRAY ? type_size(*dty.elem_type)
                : (dty.kind == TY_PTR ? 8
                   : (dty.kind == TY_STRUCT ? 8 : dty.width));
        int du = dty.is_unsigned;

        /* Block-scope `extern` is a re-declaration of a file-scope symbol, not
         * a definition: it must not allocate storage.  Bind the name to the
         * global so reads/writes go through IR_GADDR, exactly as a file-scope
         * declaration would.  Falling through to the ordinary local path
         * instead would give the name a fresh zero-initialized stack slot that
         * silently shadows the global.
         *
         * A block-scope `extern` function declaration needs no binding at all:
         * the call site dispatches on the callee's TY_FUNC type, and binding
         * the name as a data global would misroute an address-of. */
        if (dty.kind == TY_FUNC || s->u.decl.storage_class == 2) {
            if (dty.kind != TY_FUNC)
                irsymtable_push_global(st, s->u.decl.name, dty,
                                       s->u.decl.is_tls);
            break;
        }

        /* static local → allocate in static storage via a mangled global
         * `fn.varname`.  Reads/writes resolve through the global_name path.
         * Exception: if the initializer contains code label addresses (&&label),
         * it must be initialized dynamically within the function stack. */
        if (s->u.decl.storage_class == 1 && s->u.decl.init && expr_has_label_addr(s->u.decl.init)) {
            /* fall through to local stack allocation + lower_init_list */
        } else if (s->u.decl.storage_class == 1) {
            int sz = type_size(dty);
            if (sz <= 0) sz = 8;
            char *bytes = calloc(sz, 1);
            if (!bytes) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
            char mangled[256];
            snprintf(mangled, sizeof mangled, "%s.%s", cur_fd->name,
                     s->u.decl.name);
            /* mangled name is file-local by construction → LOCAL linkage.
             * Created before packing so a pointer slot in the initializer can
             * attach its link-time fixup to this global. */
            IRGlobal *sg = ir_module_push_global(g_ir_module, mangled, sz, bytes,
                                                 0, 1, 0, s->loc);
            {
                int al = (int)type_align(dty);
                if (s->u.decl.align > al) al = s->u.decl.align;
                if (al > 0) sg->align = al;
            }
            if (type_is_const_obj(dty)) sg->is_const_obj = 1;
            if (s->u.decl.init) {
                pack_init(g_ir_module, &dty, s->u.decl.init, bytes, sz,
                          s->u.decl.name, s->loc, sg);
                flush_rodata(g_ir_module);
            }
            irsymtable_push_static_local(st, s->u.decl.name, mangled, dty);
            break;
        }

        if (type_is_vla(dty)) {
            IRValue total_sz = lower_sizeof_type(fn, st, dty, s->loc);
            IRValue dyn_ptr = new_value(fn);
            emit_inst_w(fn, IR_DYN_ALLOCA, dyn_ptr, total_sz, -1, 0, 8, 1, s->loc);
            fn->has_dyn_alloca = 1;
            if (g_ir_vla_sp_slots && g_ir_vla_seq < g_ir_vla_count
                && g_ir_vla_sp_slots[g_ir_vla_seq] >= 0) {
                IRValue cur_sp = new_value(fn);
                emit_inst_w(fn, IR_STACK_SAVE, cur_sp, -1, -1, 0, 8, 1, s->loc);
                IRValue slot_addr = emit_bin_w(fn, IR_ADDR, g_ir_vla_sp_slots[g_ir_vla_seq], -1, 8, 1, s->loc);
                emit_inst_w(fn, IR_STORE_PTR, -1, slot_addr, cur_sp, 0, 8, 1, s->loc);
                g_ir_vla_seq++;
            }
            IRValue ptr_slot = emit_alloca(fn, 8, 8, 1, s->loc);
            IRValue slot_addr = emit_bin_w(fn, IR_ADDR, ptr_slot, -1, 8, 1, s->loc);
            emit_inst_w(fn, IR_STORE_PTR, -1, slot_addr, dyn_ptr, 0, 8, 1, s->loc);
            IRValue sz_slot = emit_alloca(fn, 8, 8, 1, s->loc);
            IRValue sz_addr = emit_bin_w(fn, IR_ADDR, sz_slot, -1, 8, 1, s->loc);
            emit_inst_w(fn, IR_STORE_PTR, -1, sz_addr, total_sz, 0, 8, 1, s->loc);
            irsymtable_push_vla(st, s->u.decl.name, ptr_slot, dty);
            st->data[st->len - 1].vla_nbytes_slot = sz_slot;
            ir_add_dbg_var(fn, s->u.decl.name, s->loc, IR_DBG_LOCAL, dty, ptr_slot, -1);
            break;
        }

        int pinned = is_pinned_in_body(cur_fd, s->u.decl.name, dty);
        if (g_ir_pin_locals)
            pinned = 1;
        /* Function-pointer variables are always pinned: their value must
         * survive in memory so indirect calls can load them.  Without this,
         * mem2reg promotes them and the store/load chain breaks. */
        if (dty.kind == TY_PTR && dty.pointee && dty.pointee->kind == TY_FUNC)
            pinned = 1;
        /* Float variables are always pinned too: their SSA values live in the
         * XMM register file (separate from the GP file the scalar regalloc
         * targets), so mem2reg must not promote them.  Codegen loads/stores
         * through the pinned slot on every use. */
        if (dty.kind == TY_FLOAT || dty.is_vector)
            pinned = 1;
        if (dty.kind == TY_INT && dty.width == 16)
            pinned = 1;
        if (dty.is_volatile)
            pinned = 1;
        IRValue v;
        if (pinned) {
            int total = type_size(dty);
            /* A pinned alloca must reserve at least 1 byte so codegen can
             * form its address (empty unions/structs have size 0). */
            if (total < 1) total = 1;
            int al = (int)type_align(dty);
            if (s->u.decl.align > al) al = s->u.decl.align;
            if (dty.kind == TY_STRUCT || dty.is_vector || al >= 16)
                v = emit_alloca_aligned(fn, total, al, s->loc);
            else
                v = emit_alloca(fn, total, dw, du, s->loc);
        } else {
            v = new_value(fn);
            emit_inst_w(fn, IR_ALLOCA, v, -1, -1, 0, dw, du, s->loc);
        }
        irsymtable_push(st, s->u.decl.name, v, pinned, dty);
        ir_add_dbg_var(fn, s->u.decl.name, s->loc, IR_DBG_LOCAL, dty, v, -1);
        if (s->u.decl.init) {
            if (s->u.decl.init->kind == EX_INIT_LIST) {
                if (dty.kind == TY_ARRAY || dty.kind == TY_STRUCT || dty.is_vector) {
                    IRValue addr = emit_bin_w(fn, IR_ADDR, v, -1, 8, 1, s->loc);
                    int total = type_size(dty);
                    if (total > 0) emit_zero_bytes(fn, addr, total, s->loc);
                    lower_init_list(fn, st, addr, &dty, s->u.decl.init, s->loc);
                } else {
                    /* Scalar target with a single-element brace list
                     * (`int x = {5}`): store element[0]. */
                    IRValue rv = lower_expr(fn, st,
                                            s->u.decl.init->u.init_list.elements[0]);
                    int rw = get_value_width(fn, rv), ru = get_value_is_unsigned(fn, rv);
                    IRValue coerced = coerce(fn, rv, rw, ru, dw, du, s->loc);
                    if (pinned) {
                        IRValue addr = emit_bin_w(fn, IR_ADDR, v, -1, 8, 1, s->loc);
                        emit_inst_w(fn, IR_STORE_PTR, -1, addr, coerced, 0, dw, du, s->loc);
                    } else {
                        emit_inst_w(fn, IR_STORE, -1, v, coerced, 0, dw, du, s->loc);
                    }
                }
            } else if (dty.kind == TY_STRUCT || dty.is_vector || type_is_pair16(dty)) {
                IRValue addr = emit_bin_w(fn, IR_ADDR, v, -1, 8, 1, s->loc);
                if (type_is_d128(dty)) {
                    IRValue rv = lower_expr(fn, st, s->u.decl.init);
                    if (!s->u.decl.init->type.is_decimal)
                        rv = convert_to_decimal(fn, rv, s->u.decl.init->type, dty, s->loc);
                    emit_struct_copy(fn, addr, rv, 16, s->loc);
                    break;
                }
                if (type_is_i128(dty)) {
                    IRValue rv = lower_expr(fn, st, s->u.decl.init);
                    IRValue src = i128_as_addr(fn, rv, s->u.decl.init->type,
                                               dty.is_unsigned, s->loc);
                    emit_struct_copy(fn, addr, src, 16, s->loc);
                    break;
                }
                if (dty.tag && strncmp(dty.tag, "__complex_", 10) == 0 &&
                    (s->u.decl.init->type.kind != TY_STRUCT ||
                     !s->u.decl.init->type.tag ||
                     strcmp(dty.tag, s->u.decl.init->type.tag) != 0)) {
                    Expr fake_cast;
                    memset(&fake_cast, 0, sizeof(fake_cast));
                    fake_cast.kind = EX_CAST;
                    fake_cast.type = dty;
                    fake_cast.loc = s->loc;
                    fake_cast.u.cast.target = dty;
                    fake_cast.u.cast.operand = s->u.decl.init;
                    IRValue cplx_rv = lower_expr(fn, st, &fake_cast);
                    emit_struct_copy(fn, addr, cplx_rv, type_size(dty), s->loc);
                    break;
                }
                IRValue rv = lower_expr(fn, st, s->u.decl.init);
                emit_struct_copy(fn, addr, rv, type_size(dty), s->loc);
                break;
            } else {
                IRValue rv = lower_expr(fn, st, s->u.decl.init);
                int rw = get_value_width(fn, rv), ru = get_value_is_unsigned(fn, rv);
                int rf = get_value_is_float(fn, rv) ? 1 : 0;
                int df = (dty.kind == TY_FLOAT);
                IRValue coerced;
                if (dty.is_bool) {
                    /* _Bool initializer: normalize to 0/1 (width 4) then narrow. */
                    IRValue n = bool_normalize(fn, rv, rw, ru, rf, s->loc);
                    coerced = coerce(fn, n, 4, 0, dw, du, s->loc);
                } else if (dty.is_decimal || s->u.decl.init->type.is_decimal) {
                    if (dty.is_decimal)
                        coerced = convert_to_decimal(fn, rv, s->u.decl.init->type, dty, s->loc);
                    else
                        coerced = convert_from_decimal(fn, rv, s->u.decl.init->type, dty, s->loc);
                } else if (rf != df)
                    coerced = convert_numeric(fn, rv, rw, dw, du, df, s->loc);
                else if (df)
                    coerced = (rw == dw) ? rv : convert_numeric(fn, rv, rw, dw, du, 1, s->loc);
                else
                    coerced = coerce(fn, rv, rw, ru, dw, du, s->loc);
                if (pinned) {
                    IRValue addr = emit_bin_w(fn, IR_ADDR, v, -1, 8, 1, s->loc);
                    emit_inst_w(fn, IR_STORE_PTR, -1, addr, coerced, 0, dw, du, s->loc);
                    if (dty.kind == TY_FLOAT) fn->insts.data[fn->insts.len - 1].is_float = 1;
                } else {
                    emit_inst_w(fn, IR_STORE, -1, v, coerced, 0, dw, du, s->loc);
                    if (dty.kind == TY_FLOAT) fn->insts.data[fn->insts.len - 1].is_float = 1;
                }
            }
        }
        break;
    }
    case ST_EXPR:
        lower_expr(fn, st, s->u.expr);
        break;
    case ST_RETURN: {
        if (g_vapack_depth > 0) {
            VaArgPackFrame *fr = &g_vapack_stack[g_vapack_depth - 1];
            if (fr->is_void) {
                if (s->u.value) lower_expr(fn, st, s->u.value);
            } else if (s->u.value) {
                vapack_store_value(fn, st, fr->result_slot, fr->ret_type,
                                   s->u.value, s->loc);
            }
            emit_br(fn, fr->join_label, s->loc);
            break;
        }
        if (g_instrument_functions && cur_fd && !cur_fd->no_instrument) {
            emit_profile_call(fn, "__cyg_profile_func_exit", cur_fd, s->loc);
        }
        if (fn->ret_width == 0 && !fn->ret_is_float) {
            /* void function: bare `return;` or `return void_expr;` */
            if (s->u.value) lower_expr(fn, st, s->u.value);
            emit_inst_w(fn, IR_RETURN, -1, -1, -1, 0, 0, 0, s->loc);
        } else if (!s->u.value) {
            /* GNU C `return;` in a non-void function: typed zero. */
            int dw = fn->ret_width, du = fn->ret_is_unsigned;
            if (fn->ret_is_struct) {
                if (fn->ret_x87_bytes > 0) {
                    IRValue slot = emit_alloca(fn, fn->ret_x87_bytes, 16, 1, s->loc);
                    IRValue addr = emit_bin_w(fn, IR_ADDR, slot, -1, 8, 1, s->loc);
                    emit_inst_w(fn, IR_RETURN, -1, addr, -1, fn->ret_x87_bytes,
                                8, 1, s->loc);
                    fn->insts.data[fn->insts.len - 1].x87_pair = 1;
                } else if (fn->ret_reg_n > 0) {
                    IRValue z = new_value(fn);
                    emit_inst_w(fn, IR_CONST, z, -1, -1, 0, 8, 1, s->loc);
                    IRValue hi = -1, third = -1, fourth = -1;
                    if (fn->ret_reg_n > 1) {
                        hi = new_value(fn);
                        emit_inst_w(fn, IR_CONST, hi, -1, -1, 0, 8, 1, s->loc);
                    }
                    if (fn->ret_reg_n > 2) {
                        third = new_value(fn);
                        emit_inst_w(fn, IR_CONST, third, -1, -1, 0, 8, 1, s->loc);
                    }
                    if (fn->ret_reg_n > 3) {
                        fourth = new_value(fn);
                        emit_inst_w(fn, IR_CONST, fourth, -1, -1, 0, 8, 1, s->loc);
                    }
                    emit_inst_w(fn, IR_RETURN, -1, z, hi, 0, 8, 1, s->loc);
                    IRInst *ri = &fn->insts.data[fn->insts.len - 1];
                    ri->c = third;
                    ri->d = fourth;
                    if (g_ir_cur_fd && abi_is_hfa(g_ir_cur_fd->ret_type))
                        ri->align16 = A64_MARK_HFA;
                } else {
                    emit_inst_w(fn, IR_RETURN, -1, fn->sret_value, -1, 0,
                                8, 1, s->loc);
                }
            } else if (fn->ret_is_float) {
                IRValue z = emit_float_const(fn, dw, 0, s->loc);
                emit_inst_w(fn, IR_RETURN, -1, z, -1, 0, dw, du, s->loc);
                fn->insts.data[fn->insts.len - 1].is_float = 1;
            } else {
                IRValue z = new_value(fn);
                emit_inst_w(fn, IR_CONST, z, -1, -1, 0, dw, du, s->loc);
                emit_inst_w(fn, IR_RETURN, -1, z, -1, 0, dw, du, s->loc);
            }
        } else if (fn->ret_is_struct) {
            IRValue v = lower_expr(fn, st, s->u.value);
            if (fn->ret_is_complex_ld || fn->ret_x87_bytes > 0) {
                emit_inst_w(fn, IR_RETURN, -1, v, -1,
                            fn->ret_x87_bytes ? fn->ret_x87_bytes : 32,
                            8, 1, s->loc);
                fn->insts.data[fn->insts.len - 1].x87_pair = 1;
            } else if (fn->ret_reg_n > 0) {
                /* SysV register return: load eightbytes into RAX(/RDX) or
                 * XMM0(/XMM1), and for a Darwin wide HFA into v0-v3.
                 * IR_RETURN.a/.b are the first two eightbytes; .c/.d carry
                 * the third and fourth. */
                SysVRegClass cls[4];
                for (int k = 0; k < 4; k++)
                    cls[k] = (SysVRegClass)fn->ret_reg_cls[k];
                IRValue ebs[4];
                int hfa_ret = g_ir_cur_fd && abi_is_hfa(g_ir_cur_fd->ret_type);
                SysVRegClass lcls[4];
                for (int k = 0; k < 4; k++)
                    lcls[k] = hfa_ret ? SYSV_CLS_INTEGER : cls[k];
                load_agg_regs(fn, v, fn->ret_width, fn->ret_reg_n, lcls, ebs,
                              s->loc);
                IRValue hi = (fn->ret_reg_n > 1) ? ebs[1] : -1;
                IRValue third = (fn->ret_reg_n > 2) ? ebs[2] : -1;
                IRValue fourth = (fn->ret_reg_n > 3) ? ebs[3] : -1;
                emit_inst_w(fn, IR_RETURN, -1, ebs[0], hi, 0, 8, 1, s->loc);
                IRInst *ri = &fn->insts.data[fn->insts.len - 1];
                if (third >= 0 || fourth >= 0) {
                    ri->c = third;
                    ri->d = fourth;
                }
                if (hfa_ret)
                    ri->align16 = A64_MARK_HFA;
                else if (cls[0] == SYSV_CLS_SSE)
                    ri->is_float = 1;
            } else {
                /* MEMORY class: copy into hidden sret slot; return pointer. */
                emit_struct_copy(fn, fn->sret_value, v, fn->ret_width, s->loc);
                emit_inst_w(fn, IR_RETURN, -1, fn->sret_value, -1, 0,
                            8, 1, s->loc);
            }
        } else {
            IRValue v = lower_expr(fn, st, s->u.value);
            int vw = get_value_width(fn, v), vu = get_value_is_unsigned(fn, v);
            int vf = get_value_is_float(fn, v) ? 1 : 0;
            int dw = fn->ret_width, du = fn->ret_is_unsigned;
            int df = fn->ret_is_float;
            IRValue coerced;
            if (fn->ret_is_bool) {
                IRValue n = bool_normalize(fn, v, vw, vu, vf, s->loc);
                coerced = coerce(fn, n, 4, 0, dw, du, s->loc);
            } else if (g_ir_cur_fd && (g_ir_cur_fd->ret_type.is_decimal
                                      || (s->u.value && s->u.value->type.is_decimal))) {
                if (g_ir_cur_fd->ret_type.is_decimal)
                    coerced = convert_to_decimal(fn, v, s->u.value->type,
                                                 g_ir_cur_fd->ret_type, s->loc);
                else
                    coerced = convert_from_decimal(fn, v, s->u.value->type,
                                                   g_ir_cur_fd->ret_type, s->loc);
            } else if (vf != df)
                coerced = convert_numeric(fn, v, vw, dw, du, df, s->loc);
            else if (df)
                coerced = (vw == dw) ? v : convert_numeric(fn, v, vw, dw, du, 1, s->loc);
            else
                coerced = coerce(fn, v, vw, vu, dw, du, s->loc);
            emit_inst_w(fn, IR_RETURN, -1, coerced, -1, 0,
                        dw, du, s->loc);
            if (df) fn->insts.data[fn->insts.len - 1].is_float = 1;
        }
        break;
    }
    case ST_IF: {
        IRValue cond = lower_condition(fn, st, s->u.if_s.cond);
        int L_then = new_label(fn);
        int L_end  = new_label(fn);
        int L_else = s->u.if_s.else_s ? new_label(fn) : L_end;
        emit_cbr(fn, cond, L_then, L_else, s->loc);
        emit_label(fn, L_then, s->loc);
        lower_stmt(fn, st, s->u.if_s.then_s, cur_fd);
        if (s->u.if_s.else_s) {
            emit_br(fn, L_end, s->loc);
            emit_label(fn, L_else, s->loc);
            lower_stmt(fn, st, s->u.if_s.else_s, cur_fd);
        }
        emit_label(fn, L_end, s->loc);
        break;
    }
    case ST_WHILE: {
        int L_head = new_label(fn);
        int L_body = new_label(fn);
        int L_exit = new_label(fn);
        emit_br(fn, L_head, s->loc);
        emit_label(fn, L_head, s->loc);
        IRValue cond = lower_condition(fn, st, s->u.while_s.cond);
        emit_cbr(fn, cond, L_body, L_exit, s->loc);
        emit_label(fn, L_body, s->loc);
        push_loop(L_head, L_exit);
        lower_stmt(fn, st, s->u.while_s.body, cur_fd);
        pop_loop();
        emit_br(fn, L_head, s->loc);
        emit_label(fn, L_exit, s->loc);
        break;
    }
    case ST_DO_WHILE: {
        /* Lowering:
         *   L_body: body
         *           goto L_cond
         *   L_cond: [cond ? goto L_body : goto L_exit]
         *   L_exit:
         * continue → L_cond, break → L_exit. */
        int L_body = new_label(fn);
        int L_cond = new_label(fn);
        int L_exit = new_label(fn);
        emit_br(fn, L_body, s->loc);
        emit_label(fn, L_body, s->loc);
        /* Body is its own block (the loop header) so mem2reg places φ
         * nodes here; the back-edge from L_cond merges into it. */
        push_loop(L_cond, L_exit);
        lower_stmt(fn, st, s->u.do_s.body, cur_fd);
        pop_loop();
        emit_br(fn, L_cond, s->loc);
        emit_label(fn, L_cond, s->loc);
        IRValue cond = lower_condition(fn, st, s->u.do_s.cond);
        emit_cbr(fn, cond, L_body, L_exit, s->loc);
        emit_label(fn, L_exit, s->loc);
        break;
    }
    case ST_FOR: {
        /* Lowering:
         *   [init]
         *   L_head: [cond ? goto L_body : goto L_exit]  (or unconditional
         *           goto L_body if cond absent)
         *   L_body: body
         *   L_step: [step]
         *           goto L_head
         *   L_exit:
         * continue → L_step, break → L_exit. */
        size_t mark = st->len;
        if (s->u.for_s.init) {
            if (s->u.for_s.init->kind == ST_BLOCK) {
                /* Multi-declarator init wrapped in a block by the parser.
                 * Lower its stmts directly so the declarations stay in the
                 * for scope (lower_stmt(ST_BLOCK) would pop them, hiding the
                 * loop variable from cond/step/body). */
                for (size_t i = 0; i < s->u.for_s.init->u.block.len; i++)
                    lower_stmt(fn, st, &s->u.for_s.init->u.block.data[i], cur_fd);
            } else {
                lower_stmt(fn, st, s->u.for_s.init, cur_fd);
            }
        }
        int L_head = new_label(fn);
        int L_body = new_label(fn);
        int L_step = new_label(fn);
        int L_exit = new_label(fn);
        emit_br(fn, L_head, s->loc);
        emit_label(fn, L_head, s->loc);
        if (s->u.for_s.cond) {
            IRValue cond = lower_condition(fn, st, s->u.for_s.cond);
            emit_cbr(fn, cond, L_body, L_exit, s->loc);
        } else {
            emit_br(fn, L_body, s->loc);
        }
        emit_label(fn, L_body, s->loc);
        push_loop(L_step, L_exit);
        lower_stmt(fn, st, s->u.for_s.body, cur_fd);
        pop_loop();
        emit_br(fn, L_step, s->loc);
        emit_label(fn, L_step, s->loc);
        if (s->u.for_s.step) lower_expr(fn, st, s->u.for_s.step);
        emit_br(fn, L_head, s->loc);
        emit_label(fn, L_exit, s->loc);
        st->len = mark;
        break;
    }
    case ST_BREAK:
        emit_br(fn, g_loops[g_loop_depth - 1].break_label, s->loc);
        break;
    case ST_CONTINUE:
        emit_br(fn, g_loops[g_loop_depth - 1].cont_label, s->loc);
        break;
    case ST_BLOCK: {
        int vla_saved_slot = -1;
        int has_block_vla = 0;
        for (size_t i = 0; i < s->u.block.len; i++) {
            if (stmt_has_vla(&s->u.block.data[i])) {
                has_block_vla = 1;
                break;
            }
        }
        if (has_block_vla) {
            vla_saved_slot = emit_alloca(fn, 8, 8, 1, s->loc);
            IRValue cur_sp = new_value(fn);
            emit_inst_w(fn, IR_STACK_SAVE, cur_sp, -1, -1, 0, 8, 1, s->loc);
            IRValue slot_addr = emit_bin_w(fn, IR_ADDR, vla_saved_slot, -1, 8, 1, s->loc);
            emit_inst_w(fn, IR_STORE_PTR, -1, slot_addr, cur_sp, 0, 8, 1, s->loc);
            fn->has_dyn_alloca = 1;
        }
        for (size_t i = 0; i < s->u.block.len; i++) {
            lower_stmt(fn, st, &s->u.block.data[i], cur_fd);
        }
        if (vla_saved_slot >= 0) {
            IRValue slot_addr = emit_bin_w(fn, IR_ADDR, vla_saved_slot, -1, 8, 1, s->loc);
            IRValue saved_sp = new_value(fn);
            emit_inst_w(fn, IR_LOAD_PTR, saved_sp, slot_addr, -1, 0, 8, 1, s->loc);
            emit_inst_w(fn, IR_STACK_RESTORE, -1, saved_sp, -1, 0, 8, 1, s->loc);
        }
        break;
    }
    case ST_GOTO: {
        if (s->u.goto_s.target_expr) {
            IRValue ptr = lower_expr(fn, st, s->u.goto_s.target_expr);
            /* Destination VLA depth is restored at the label, so mixed
             * &&label depths do not need a single restore at jmp *. */
            emit_inst(fn, IR_JMP_PTR, -1, ptr, -1, -1, s->loc);
        } else {
            int id = labelmap_find(g_ir_label_map, s->u.goto_s.target);
            if (id < 0) {
                fprintf(stderr, "fakecc: goto to unknown label '%s'\n",
                        s->u.goto_s.target);
                exit(1);
            }
            if (g_ir_entry_sp_slot >= 0 && id >= 0 && id < g_ir_label_vla_id_count)
                emit_restore_vla_sp(fn, g_ir_label_vla_id[id], s->loc);
            emit_br(fn, id, s->loc);
        }
        break;
    }
    case ST_LABEL: {
        int id = labelmap_find(g_ir_label_map, s->u.label_s.name);
        if (id < 0) {
            fprintf(stderr, "fakecc: label '%s' has no assigned id\n",
                    s->u.label_s.name);
            exit(1);
        }
        emit_label(fn, id, s->loc);
        if (g_ir_entry_sp_slot >= 0 && id >= 0 && id < g_ir_label_vla_id_count)
            emit_restore_vla_sp(fn, g_ir_label_vla_id[id], s->loc);
        lower_stmt(fn, st, s->u.label_s.stmt, cur_fd);
        break;
    }
    case ST_SWITCH: {
        /* Lowering (fall-through semantics):
         *   v = <cond>
         *   if (v == val_0) goto L_body_0
         *   if (v == val_1) goto L_body_1
         *   ...
         *   goto L_default_or_exit        // no case matched
         *   L_body_0: <body 0>            // falls through to L_body_1
         *   L_body_1: <body 1>
         *   ...
         *   L_default: <default body>     (only if a default arm exists)
         *   L_exit:
         * break → L_exit.  Bodies are laid out sequentially so a missing
         * break falls through to the next arm. */
        IRValue v = lower_expr(fn, st, s->u.switch_s.cond);
        int is_i128 = s->u.switch_s.cond ? type_is_i128(s->u.switch_s.cond->type) : 0;
        int vw = is_i128 ? 16 : get_value_width(fn, v);
        int vu = is_i128 ? s->u.switch_s.cond->type.is_unsigned : get_value_is_unsigned(fn, v);
        IRValue vlo = -1, vhi = -1;
        if (is_i128) {
            i128_load2(fn, v, &vlo, &vhi, s->loc);
        }

        int n = s->u.switch_s.num_cases;
        int has_default = 0;
        for (int i = 0; i < n; i++)
            if (s->u.switch_s.cases[i].is_default) { has_default = 1; break; }

        int exit_label = new_label(fn);

        if (s->u.switch_s.body) {
            int fallthrough_label = exit_label;
            for (int i = 0; i < n; i++) {
                if (s->u.switch_s.cases[i].is_default && s->u.switch_s.cases[i].label_name) {
                    fallthrough_label = labelmap_find(g_ir_label_map, s->u.switch_s.cases[i].label_name);
                    break;
                }
            }

            for (int i = 0; i < n; i++) {
                SwitchCase *arm = &s->u.switch_s.cases[i];
                if (arm->is_default) continue;
                int target_lbl = arm->label_name ? labelmap_find(g_ir_label_map, arm->label_name) : exit_label;
                int next_check = new_label(fn);
                if (is_i128) {
                    int64_t arm_lo = arm->value;
                    int64_t arm_hi = (arm_lo < 0 && !vu) ? -1LL : 0LL;
                    IRValue eq_lo = emit_bin_w(fn, IR_EQ, vlo, i64imm(fn, arm_lo, s->loc), 8, 1, s->loc);
                    IRValue eq_hi = emit_bin_w(fn, IR_EQ, vhi, i64imm(fn, arm_hi, s->loc), 8, 1, s->loc);
                    IRValue eq = emit_bin_w(fn, IR_BAND, eq_lo, eq_hi, 4, 0, s->loc);
                    emit_cbr(fn, eq, target_lbl, next_check, s->loc);
                } else if (arm->is_range) {
                    IRValue low_val = new_value(fn);
                    emit_inst_w(fn, IR_CONST, low_val, -1, -1, arm->value, vw, vu, s->loc);
                    IRValue high_val = new_value(fn);
                    emit_inst_w(fn, IR_CONST, high_val, -1, -1, arm->high_value, vw, vu, s->loc);
                    IRValue ge = emit_bin_w(fn, IR_GE, v, low_val, vw, vu, s->loc);
                    IRValue le = emit_bin_w(fn, IR_LE, v, high_val, vw, vu, s->loc);
                    IRValue in_range = emit_bin_w(fn, IR_BAND, ge, le, vw, vu, s->loc);
                    emit_cbr(fn, in_range, target_lbl, next_check, s->loc);
                } else {
                    IRValue cmp_val = new_value(fn);
                    emit_inst_w(fn, IR_CONST, cmp_val, -1, -1, arm->value, vw, vu, s->loc);
                    IRValue eq = emit_bin_w(fn, IR_EQ, v, cmp_val, vw, vu, s->loc);
                    emit_cbr(fn, eq, target_lbl, next_check, s->loc);
                }
                emit_label(fn, next_check, s->loc);
            }
            emit_br(fn, fallthrough_label, s->loc);

            push_loop(/*cont*/ exit_label, /*brk*/ exit_label);
            lower_stmt(fn, st, s->u.switch_s.body, cur_fd);
            pop_loop();

            emit_label(fn, exit_label, s->loc);
            break;
        }

        /* Pre-create a body label for every arm. */
        int *body_label = xmalloc(n * sizeof(int));
        for (int i = 0; i < n; i++) body_label[i] = new_label(fn);

        /* Collect the non-default arm indices and create a "check" label for
         * each, forming a chain: L_check_i does the comparison and on miss
         * falls through to L_check_{i+1}. */
        int ndispatch = 0;
        for (int i = 0; i < n; i++)
            if (!s->u.switch_s.cases[i].is_default) ndispatch++;

        int *check_label = xmalloc(ndispatch * sizeof(int));
        int *check_case = xmalloc(ndispatch * sizeof(int)); // arm index
        int dc = 0;
        for (int i = 0; i < n; i++) {
            if (s->u.switch_s.cases[i].is_default) continue;
            check_label[dc] = new_label(fn);
            check_case[dc] = i;
            dc++;
        }
        /* The label to jump to when no case matches. */
        int fallthrough_label = has_default ? -1 : exit_label;
        if (has_default) {
            for (int i = 0; i < n; i++)
                if (s->u.switch_s.cases[i].is_default) { fallthrough_label = body_label[i]; break; }
        }

        /* Emit the dispatch chain. */
        for (int c = 0; c < ndispatch; c++) {
            int arm_idx = check_case[c];
            SwitchCase *arm = &s->u.switch_s.cases[arm_idx];
            emit_label(fn, check_label[c], s->loc);
            int next = (c + 1 < ndispatch) ? check_label[c + 1] : fallthrough_label;
            if (arm->is_range) {
                IRValue low_val = new_value(fn);
                emit_inst_w(fn, IR_CONST, low_val, -1, -1, arm->value, vw, vu, s->loc);
                IRValue high_val = new_value(fn);
                emit_inst_w(fn, IR_CONST, high_val, -1, -1, arm->high_value, vw, vu, s->loc);
                IRValue ge = emit_bin_w(fn, IR_GE, v, low_val, vw, vu, s->loc);
                IRValue le = emit_bin_w(fn, IR_LE, v, high_val, vw, vu, s->loc);
                IRValue in_range = emit_bin_w(fn, IR_BAND, ge, le, vw, vu, s->loc);
                emit_cbr(fn, in_range, body_label[arm_idx], next, s->loc);
            } else {
                IRValue cmp_val = new_value(fn);
                emit_inst_w(fn, IR_CONST, cmp_val, -1, -1, arm->value, vw, vu, s->loc);
                IRValue eq = emit_bin_w(fn, IR_EQ, v, cmp_val, vw, vu, s->loc);
                emit_cbr(fn, eq, body_label[arm_idx], next, s->loc);
            }
        }

        /* Bodies, laid out sequentially for fall-through.  Push a loop
         * frame so `break` inside any arm jumps to the exit label. */
        push_loop(/*cont*/ exit_label, /*brk*/ exit_label);
        for (int i = 0; i < n; i++) {
            SwitchCase *arm = &s->u.switch_s.cases[i];
            emit_label(fn, body_label[i], s->loc);
            for (size_t j = 0; j < arm->stmts.len; j++)
                lower_stmt(fn, st, &arm->stmts.data[j], cur_fd);
        }
        pop_loop();

        emit_label(fn, exit_label, s->loc);
        free(body_label);
        free(check_label);
        free(check_case);
        break;
    }
    }
}

/* Find a previously-packed global by name (for const-global references in
 * initializers, e.g. `.regs = ALLOCATABLE_REGS`).  Returns NULL if not found. */
static const IRGlobal *find_packed_global(const IRModule *m, const char *name) {
    if (!m || !name) return NULL;
    for (size_t i = 0; i < m->globals.len; i++)
        if (m->globals.data[i].name
            && strcmp(m->globals.data[i].name, name) == 0)
            return &m->globals.data[i];
    return NULL;
}

/* Static locals are stored as mangled `fn.varname` globals.  Initializers
 * still name the C identifier (`static int *hx = gx`). */
static const IRGlobal *find_init_global(const IRModule *ir, const char *name) {
    const IRGlobal *g = find_packed_global(ir, name);
    if (g) return g;
    if (!g_ir_cur_fd || !g_ir_cur_fd->name || !name) return NULL;
    char mangled[256];
    snprintf(mangled, sizeof mangled, "%s.%s", g_ir_cur_fd->name, name);
    return find_packed_global(ir, mangled);
}

/* Globals created while packing an initializer are queued instead of pushed
 * immediately: pushing reallocs the module's global array, which would dangle
 * the `IRGlobal *g` that pack_init is attaching fixups to.  Two kinds:
 *   - readonly: string-literal bytes (`const char *p = "x"`);
 *   - writable: file-scope compound literals (`&((T){...})`), which GCC gives
 *     static storage so their address is a link-time constant. */
typedef struct {
    char *name;
    char *bytes;
    int   size;
    int   is_readonly;
    SourceLoc loc;
    IRGlobal *tmp_g; /* scratch global for nested fixups, transferred on flush */
} PendingGlobal;
static PendingGlobal *g_pending_globals = NULL;
static int g_pending_globals_len = 0;
static int g_pending_globals_cap = 0;

static void queue_pending_global(const char *name, char *bytes, int size,
                                 int is_readonly, SourceLoc loc) {
    if (g_pending_globals_len >= g_pending_globals_cap) {
        int nc = g_pending_globals_cap ? g_pending_globals_cap * 2 : 4;
        g_pending_globals = realloc(g_pending_globals, nc * sizeof(PendingGlobal));
        if (!g_pending_globals) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        g_pending_globals_cap = nc;
    }
    g_pending_globals[g_pending_globals_len].name = xstrdup(name);
    g_pending_globals[g_pending_globals_len].bytes = bytes;
    g_pending_globals[g_pending_globals_len].size = size;
    g_pending_globals[g_pending_globals_len].is_readonly = is_readonly;
    g_pending_globals[g_pending_globals_len].loc = loc;
    g_pending_globals[g_pending_globals_len].tmp_g = NULL;
    g_pending_globals_len++;
}

static void flush_pending_globals(IRModule *m) {
    for (int i = 0; i < g_pending_globals_len; i++) {
        IRGlobal *g = ir_module_push_global(m, g_pending_globals[i].name,
                                             g_pending_globals[i].size,
                                             g_pending_globals[i].bytes,
                                             g_pending_globals[i].is_readonly,
                                             1, 0, g_pending_globals[i].loc);
        if (g_pending_globals[i].size >= 16 && (g_pending_globals[i].size % 16) == 0)
            g->align = g_pending_globals[i].size >= 32 ? 32 : 16;
        /* Transfer any fixups collected while packing the literal's own init.
         * tmp->init_bytes aliases the buffer the real global now owns, so it
         * must not be freed here. */
        if (g_pending_globals[i].tmp_g) {
            IRGlobal *t = g_pending_globals[i].tmp_g;
            if (t->num_fixups > 0) {
                g->fixups = t->fixups;
                g->num_fixups = t->num_fixups;
                g->cap_fixups = t->cap_fixups;
                t->fixups = NULL;
                t->num_fixups = 0;
                t->cap_fixups = 0;
            }
            free(t->name);
            free(t);
        }
        free(g_pending_globals[i].name);
    }
    g_pending_globals_len = 0;
}

/* Legacy wrappers kept for the call sites below. */
static void queue_rodata(const char *name, char *bytes, int size, SourceLoc loc) {
    queue_pending_global(name, bytes, size, 1, loc);
}
static void flush_rodata(IRModule *m) {
    flush_pending_globals(m);
}

/* Evaluate a constant expression as a long double, so a float-typed global can
 * be packed in its own format.  Integer constants are accepted too: `double
 * d = 1;` must store 1.0, not the bit pattern of the integer 1. */
static long double ld_two64(void) {
    /* Build 2^64 in long double without a 80-bit source literal: glibc
     * strtold and the freestanding runtime disagree on that constant, which
     * breaks the bootstrap fixed point.  2^16 is exact in any format. */
    long double t = 65536.0L;
    t = t * t;
    t = t * t;
    return t;
}

static long double int_lit_to_ld(const Expr *e) {
    unsigned long long lo = (unsigned long long)e->u.int_val;
    unsigned long long hi = e->int_hi;
    if (e->type.width != 16) {
        if (!e->type.is_unsigned)
            return (long double)e->u.int_val;
        return (long double)lo;
    }
    int neg = !e->type.is_unsigned && (hi >> 63);
    long double scale = ld_two64();
    if (neg) {
        unsigned long long nlo = ~lo + 1ULL;
        unsigned long long nhi = ~hi + (nlo == 0);
        return -((long double)nlo + (long double)nhi * scale);
    }
    return (long double)lo + (long double)hi * scale;
}

static int fold_const_float(const Expr *e, long double *out, const IRModule *ir) {
    if (fakecc_had_error()) return 0;
    if (!e) return 0;
    if (e->kind == EX_FLOAT_LIT) {
        *out = strtold(e->u.float_text, NULL);
        return 1;
    }
    if (e->kind == EX_VAR && strcmp(e->u.var.name, "__FLT_MAX__") == 0) {
        unsigned u = 0x7f7fffffu;
        float f;
        memcpy(&f, &u, 4);
        *out = (long double)f;
        return 1;
    }
    if (e->kind == EX_CALL && e->u.call.callee && e->u.call.callee->kind == EX_VAR) {
        const char *name = e->u.call.callee->u.var.name;
        if (strcmp(name, "__builtin_inf") == 0 || strcmp(name, "__builtin_huge_val") == 0) {
            *out = __builtin_inf();
            return 1;
        }
        if (strcmp(name, "__builtin_inff") == 0 || strcmp(name, "__builtin_huge_valf") == 0) {
            *out = __builtin_inff();
            return 1;
        }
        if (strcmp(name, "__builtin_infl") == 0 || strcmp(name, "__builtin_huge_vall") == 0) {
            *out = __builtin_infl();
            return 1;
        }
        if (strcmp(name, "__builtin_nan") == 0) {
            const char *tag = "";
            if (e->u.call.args.len > 0 && e->u.call.args.data[0]->kind == EX_STR)
                tag = e->u.call.args.data[0]->u.str.bytes;
            *out = __builtin_nan(tag);
            return 1;
        }
        if (strcmp(name, "__builtin_nanf") == 0) {
            const char *tag = "";
            if (e->u.call.args.len > 0 && e->u.call.args.data[0]->kind == EX_STR)
                tag = e->u.call.args.data[0]->u.str.bytes;
            *out = __builtin_nanf(tag);
            return 1;
        }
        if (strcmp(name, "__builtin_nanl") == 0) {
            const char *tag = "";
            if (e->u.call.args.len > 0 && e->u.call.args.data[0]->kind == EX_STR)
                tag = e->u.call.args.data[0]->u.str.bytes;
            *out = __builtin_nanl(tag);
            return 1;
        }
    }
    /* In a global-init context, resolve EX_VAR to the referenced global's
     * already-packed bytes (e.g. `const double f = 1 + (double)a;` where `a`
     * is a const char).  `ir` is NULL when called from non-init sites. */
    if (e->kind == EX_VAR && ir) {
        const IRGlobal *src = find_packed_global(ir, e->u.var.name);
        if (src && src->init_bytes && src->size > 0) {
            long long iv = 0;
            memcpy(&iv, src->init_bytes, src->size < 8 ? src->size : 8);
            *out = (long double)iv;
            return 1;
        }
    }
    if (e->kind == EX_CAST)
        return fold_const_float(e->u.cast.operand, out, ir);
    if (e->kind == EX_UNARY
        && (e->u.un.op == UOP_NEG || e->u.un.op == UOP_POS)) {
        if (!fold_const_float(e->u.un.operand, out, ir)) return 0;
        if (e->u.un.op == UOP_NEG) *out = -*out;
        return 1;
    }
    if (e->kind == EX_INT_LIT) {
        *out = int_lit_to_ld(e);
        return 1;
    }
    if (e->kind == EX_BINOP) {
        long double l = 0, r = 0;
        if (!fold_const_float(e->u.bin.l, &l, ir) || !fold_const_float(e->u.bin.r, &r, ir))
            return 0;
        switch (e->u.bin.op) {
        case BOP_ADD: *out = l + r; return 1;
        case BOP_SUB: *out = l - r; return 1;
        case BOP_MUL: *out = l * r; return 1;
        case BOP_DIV: *out = l / r; return 1;
        default: return 0;
        }
    }
    long long iv;
    if (fold_const_int(e, &iv)) {
        *out = (long double)iv;
        return 1;
    }
    return 0;
}

/* Variant of fold_const_float for global initializer packing: also resolves
 * EX_VAR to the referenced global's initializer when that global has already
 * been packed (e.g. `const double f = 1 + (double)a;` where `a` is a const
 * char with a known value).  `ir` may be NULL (falls back to fold_const_float). */

static int fold_const_complex(const Expr *e, long double *r_out, long double *i_out) {
    if (fakecc_had_error()) return 0;
    if (!e) return 0;
    if (e->kind == EX_FLOAT_LIT) {
        *r_out = strtold(e->u.float_text, NULL);
        *i_out = 0.0;
        return 1;
    }
    if (e->kind == EX_INT_LIT) {
        *r_out = int_lit_to_ld(e);
        *i_out = 0.0;
        return 1;
    }
    if (e->kind == EX_CAST)
        return fold_const_complex(e->u.cast.operand, r_out, i_out);
    if (e->kind == EX_COMPOUND_LITERAL && e->u.compound.init && e->u.compound.init->kind == EX_INIT_LIST) {
        int n = e->u.compound.init->u.init_list.num_elements;
        long double r = 0.0, i = 0.0, dummy = 0.0;
        if (n >= 1) fold_const_complex(e->u.compound.init->u.init_list.elements[0], &r, &dummy);
        if (n >= 2) fold_const_complex(e->u.compound.init->u.init_list.elements[1], &i, &dummy);
        *r_out = r;
        *i_out = i;
        return 1;
    }
    if (e->kind == EX_UNARY) {
        long double r = 0.0, i = 0.0;
        if (!fold_const_complex(e->u.un.operand, &r, &i)) return 0;
        if (e->u.un.op == UOP_NEG) { *r_out = -r; *i_out = -i; return 1; }
        if (e->u.un.op == UOP_POS) { *r_out = r; *i_out = i; return 1; }
        if (e->u.un.op == UOP_BITNOT) { *r_out = r; *i_out = -i; return 1; }
    }
    if (e->kind == EX_BINOP) {
        long double lr = 0.0, li = 0.0, rr = 0.0, ri = 0.0;
        if (!fold_const_complex(e->u.bin.l, &lr, &li)) return 0;
        if (!fold_const_complex(e->u.bin.r, &rr, &ri)) return 0;
        if (e->u.bin.op == BOP_ADD) { *r_out = lr + rr; *i_out = li + ri; return 1; }
        if (e->u.bin.op == BOP_SUB) { *r_out = lr - rr; *i_out = li - ri; return 1; }
        if (e->u.bin.op == BOP_MUL) { *r_out = lr * rr - li * ri; *i_out = lr * ri + li * rr; return 1; }
        if (e->u.bin.op == BOP_DIV) {
            long double denom = rr * rr + ri * ri;
            if (denom == 0.0) return 0;
            *r_out = (lr * rr + li * ri) / denom;
            *i_out = (li * rr - lr * ri) / denom;
            return 1;
        }
    }
    return 0;
}

static int fold_const_complex_rel(const Expr *e, long long *out) {
    if (fakecc_had_error()) return 0;
    if (!e || e->kind != EX_BINOP) return 0;
    if (e->u.bin.op != BOP_EQ && e->u.bin.op != BOP_NE) return 0;
    long double lr = 0.0, li = 0.0, rr = 0.0, ri = 0.0;
    if (!fold_const_complex(e->u.bin.l, &lr, &li)) return 0;
    if (!fold_const_complex(e->u.bin.r, &rr, &ri)) return 0;
    int eq = (lr == rr && li == ri);
    *out = (e->u.bin.op == BOP_EQ) ? (eq ? 1 : 0) : (eq ? 0 : 1);
    return 1;
}

/* Pack a compile-time constant initializer into `bytes` (size `sz`) for a
 * global of type `ty`.  Handles integer literals, negated literals, nested
 * initializer lists (positional, per-element/per-member offset), string
 * literals for `char[]="..."`, cast expressions, and references to previously
 * packed const globals.  `ir` is the module whose globals may be referenced.
 * Sema guarantees every element is constant. */
static int get_global_elem_size(const Expr *e) {
    if (!e) return 1;
    if (e->type.kind == TY_PTR && e->type.pointee)
        return type_size(*e->type.pointee);
    if (e->type.kind == TY_ARRAY && e->type.elem_type)
        return type_size(*e->type.elem_type);
    if (e->kind == EX_VAR && g_ir_tu) {
        for (size_t i = 0; i < g_ir_tu->globals.len; i++) {
            if (g_ir_tu->globals.data[i].kind == ST_DECL &&
                strcmp(g_ir_tu->globals.data[i].u.decl.name, e->u.var.name) == 0) {
                Type gt = g_ir_tu->globals.data[i].u.decl.type;
                if (gt.kind == TY_ARRAY && gt.elem_type)
                    return type_size(*gt.elem_type);
                if (gt.kind == TY_PTR && gt.pointee)
                    return type_size(*gt.pointee);
            }
        }
    }
    if (e->kind == EX_INDEX) {
        if (e->type.kind == TY_ARRAY && e->type.elem_type)
            return type_size(*e->type.elem_type);
        if (e->type.kind == TY_PTR && e->type.pointee)
            return type_size(*e->type.pointee);
        return get_global_elem_size(e->u.idx.array);
    }
    if (e->kind == EX_BINOP) {
        return get_global_elem_size(e->u.bin.l);
    }
    if (e->kind == EX_CAST) {
        return get_global_elem_size(e->u.cast.operand);
    }
    return 1;
}

static Type get_global_obj_struct_type(const Expr *e) {
    Type t = e->type;
    if (t.kind == TY_VOID && e->kind == EX_DEREF) {
        return get_global_obj_struct_type(e->u.deref.operand);
    }
    if (t.kind == TY_VOID && e->kind == EX_VAR && g_ir_tu) {
        for (size_t i = 0; i < g_ir_tu->globals.len; i++) {
            if (g_ir_tu->globals.data[i].kind == ST_DECL &&
                strcmp(g_ir_tu->globals.data[i].u.decl.name, e->u.var.name) == 0) {
                t = g_ir_tu->globals.data[i].u.decl.type;
                break;
            }
        }
    }
    if (t.kind == TY_VOID && e->kind == EX_BINOP) {
        return get_global_obj_struct_type(e->u.bin.l);
    }
    if (t.kind == TY_PTR && t.pointee)
        t = *t.pointee;
    else if (t.kind == TY_ARRAY && t.elem_type)
        t = *t.elem_type;
    return t;
}

/* Scale for `addr ± integer` in a static initializer.  After the address is
 * converted to an integer type, further ± uses a byte addend (GCC relocatable
 * integer initializers: `(unsigned long)&sym - C`).  Pointer arithmetic uses
 * the pointee size; GNU `void*` arithmetic uses 1. */
static int addr_const_elem_size(const Expr *addr_side) {
    const Expr *e = addr_side;
    while (e && e->kind == EX_CAST) {
        if (e->type.kind == TY_INT)
            return 1;
        e = e->u.cast.operand;
    }
    int esz = get_global_elem_size(addr_side);
    if (esz < 1) esz = 1;
    return esz;
}

/* True if `e` is built from an address constant (`&obj`, string, &&label),
 * not a scalar object's value.  Used so integer-typed slots record a reloc
 * for `(uintptr_t)&sym + C` without treating `int x = y` as `&y`. */
static int expr_has_address_constant(const Expr *e) {
    if (!e) return 0;
    while (e->kind == EX_CAST) e = e->u.cast.operand;
    if (!e) return 0;
    if (e->kind == EX_ADDR || e->kind == EX_LABEL_ADDR || e->kind == EX_STR)
        return 1;
    if (e->kind == EX_BINOP)
        return expr_has_address_constant(e->u.bin.l)
            || expr_has_address_constant(e->u.bin.r);
    if (e->kind == EX_UNARY)
        return expr_has_address_constant(e->u.un.operand);
    if (e->kind == EX_MEMBER)
        return expr_has_address_constant(e->u.member.obj);
    if (e->kind == EX_INDEX)
        return expr_has_address_constant(e->u.idx.array);
    if (e->kind == EX_DEREF)
        return expr_has_address_constant(e->u.deref.operand);
    return 0;
}

/* C99 6.6p9: an integer constant cast to pointer type is an address constant.
 * `&((T *)0x4000)->m` is that address plus a member offset, stored as an
 * absolute pointer value (no reloc). */
static int eval_abs_addr_const(const Expr *e, unsigned long long *out) {
    if (!e) return 0;
    if (e->kind == EX_CAST)
        return eval_abs_addr_const(e->u.cast.operand, out);
    if (e->kind == EX_ADDR) {
        const Expr *sub = e->u.addr.operand;
        while (sub && sub->kind == EX_CAST) sub = sub->u.cast.operand;
        if (!sub) return 0;
        if (sub->kind == EX_MEMBER) {
            unsigned long long base = 0;
            if (!eval_abs_addr_const(sub->u.member.obj, &base)) return 0;
            Type mty = get_global_obj_struct_type(sub->u.member.obj);
            long long moff = 0;
            if (mty.kind == TY_STRUCT && mty.tag) {
                const StructDef *sd = struct_registry_find_c(g_ir_structs, mty.tag);
                if (sd)
                    struct_lookup_member(g_ir_structs, sd, sub->u.member.name, &moff);
            }
            *out = base + (unsigned long long)moff;
            return 1;
        }
        if (sub->kind == EX_DEREF)
            return eval_abs_addr_const(sub->u.deref.operand, out);
        if (sub->kind == EX_INDEX) {
            unsigned long long base = 0;
            if (!eval_abs_addr_const(sub->u.idx.array, &base)) return 0;
            long long idx = 0;
            if (!fold_const_int(sub->u.idx.index, &idx)) return 0;
            int esz = get_global_elem_size(sub->u.idx.array);
            if (esz < 1) esz = 1;
            *out = base + (unsigned long long)(idx * esz);
            return 1;
        }
        return eval_abs_addr_const(sub, out);
    }
    if (e->kind == EX_DEREF)
        return eval_abs_addr_const(e->u.deref.operand, out);
    if (e->kind == EX_MEMBER) {
        unsigned long long base = 0;
        if (!eval_abs_addr_const(e->u.member.obj, &base)) return 0;
        Type mty = get_global_obj_struct_type(e->u.member.obj);
        long long moff = 0;
        if (mty.kind == TY_STRUCT && mty.tag) {
            const StructDef *sd = struct_registry_find_c(g_ir_structs, mty.tag);
            if (sd)
                struct_lookup_member(g_ir_structs, sd, e->u.member.name, &moff);
        }
        *out = base + (unsigned long long)moff;
        return 1;
    }
    if (e->kind == EX_INDEX) {
        unsigned long long base = 0;
        if (!eval_abs_addr_const(e->u.idx.array, &base)) return 0;
        long long idx = 0;
        if (!fold_const_int(e->u.idx.index, &idx)) return 0;
        int esz = get_global_elem_size(e->u.idx.array);
        if (esz < 1) esz = 1;
        *out = base + (unsigned long long)(idx * esz);
        return 1;
    }
    if (e->kind == EX_BINOP) {
        unsigned long long base = 0;
        long long delta = 0;
        if (e->u.bin.op == BOP_ADD) {
            if (eval_abs_addr_const(e->u.bin.l, &base) && fold_const_int(e->u.bin.r, &delta)) {
                int esz = addr_const_elem_size(e->u.bin.l);
                *out = base + (unsigned long long)(delta * esz);
                return 1;
            }
            if (eval_abs_addr_const(e->u.bin.r, &base) && fold_const_int(e->u.bin.l, &delta)) {
                int esz = addr_const_elem_size(e->u.bin.r);
                *out = base + (unsigned long long)(delta * esz);
                return 1;
            }
        } else if (e->u.bin.op == BOP_SUB) {
            if (eval_abs_addr_const(e->u.bin.l, &base) && fold_const_int(e->u.bin.r, &delta)) {
                int esz = addr_const_elem_size(e->u.bin.l);
                *out = base - (unsigned long long)(delta * esz);
                return 1;
            }
        }
        return 0;
    }
    {
        long long imm = 0;
        if (fold_const_int(e, &imm)) {
            *out = (unsigned long long)imm;
            return 1;
        }
    }
    return 0;
}

static int eval_global_addr_offset(const Expr *e, const char **out_sym, int *out_offset) {
    if (!e) return 0;
    if (e->kind == EX_CAST) {
        return eval_global_addr_offset(e->u.cast.operand, out_sym, out_offset);
    }
    if (e->kind == EX_VAR) {
        const IRGlobal *sg = find_init_global(g_ir_module, e->u.var.name);
        *out_sym = sg ? sg->name : e->u.var.name;
        *out_offset = 0;
        return 1;
    }
    /* An array member like `s.f` (where f is `char[3]`) decays to &s.f[0]
     * — treat it as an address-of-member for offset purposes. */
    if (e->kind == EX_MEMBER && e->type.kind == TY_ARRAY) {
        Expr fake_addr;
        memset(&fake_addr, 0, sizeof(fake_addr));
        fake_addr.kind = EX_ADDR;
        fake_addr.u.addr.operand = (Expr *)e;
        return eval_global_addr_offset(&fake_addr, out_sym, out_offset);
    }
    if (e->kind == EX_STR) {
        char name[32];
        snprintf(name, sizeof name, "__str.%d", g_str_counter++);
        int nbytes = e->u.str.len + 1;
        char *init = malloc(nbytes);
        if (!init) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        memcpy(init, e->u.str.bytes, nbytes);
        queue_rodata(name, init, nbytes, e->loc);
        *out_sym = xstrdup(name);
        *out_offset = 0;
        return 1;
    }
    if (e->kind == EX_ADDR) {
        const Expr *sub = e->u.addr.operand;
        while (sub && sub->kind == EX_CAST) sub = sub->u.cast.operand;
        if (!sub) return 0;
        if (sub->kind == EX_VAR) {
            const IRGlobal *sg = find_init_global(g_ir_module, sub->u.var.name);
            *out_sym = sg ? sg->name : sub->u.var.name;
            *out_offset = 0;
            return 1;
        }
        if (sub->kind == EX_MEMBER) {
            const char *sym = NULL;
            int off = 0;
            if (!eval_global_addr_offset(sub->u.member.obj, &sym, &off))
                return 0;
            Type m_target_ty = get_global_obj_struct_type(sub->u.member.obj);
            if (m_target_ty.kind == TY_STRUCT && m_target_ty.tag) {
                const StructDef *sd = struct_registry_find_c(g_ir_structs, m_target_ty.tag);
                if (sd) {
                    long long moff = 0;
                    if (struct_lookup_member(g_ir_structs, sd, sub->u.member.name, &moff)) {
                        off += (int)moff;
                        *out_sym = sym;
                        *out_offset = off;
                        return 1;
                    }
                }
            }
            *out_sym = sym;
            *out_offset = off;
            return 1;
        }
        if (sub->kind == EX_DEREF) {
            return eval_global_addr_offset(sub->u.deref.operand, out_sym, out_offset);
        }
        if (sub->kind == EX_INDEX) {
            const char *sym = NULL;
            int off = 0;
            if (!eval_global_addr_offset(sub->u.idx.array, &sym, &off))
                return 0;
            long long idx = 0;
            if (!fold_const_int(sub->u.idx.index, &idx))
                return 0;
            int esz = get_global_elem_size(sub->u.idx.array);
            *out_sym = sym;
            *out_offset = off + (int)(idx * esz);
            return 1;
        }
        return eval_global_addr_offset(sub, out_sym, out_offset);
    }
    if (e->kind == EX_BINOP) {
        if (e->u.bin.op == BOP_ADD) {
            const char *sym = NULL;
            int off = 0;
            long long delta = 0;
            if (eval_global_addr_offset(e->u.bin.l, &sym, &off) && fold_const_int(e->u.bin.r, &delta)) {
                int esz = addr_const_elem_size(e->u.bin.l);
                *out_sym = sym;
                *out_offset = off + (int)(delta * esz);
                return 1;
            }
            if (eval_global_addr_offset(e->u.bin.r, &sym, &off) && fold_const_int(e->u.bin.l, &delta)) {
                int esz = addr_const_elem_size(e->u.bin.r);
                *out_sym = sym;
                *out_offset = off + (int)(delta * esz);
                return 1;
            }
        } else if (e->u.bin.op == BOP_SUB) {
            const char *sym = NULL;
            int off = 0;
            long long delta = 0;
            if (eval_global_addr_offset(e->u.bin.l, &sym, &off) && fold_const_int(e->u.bin.r, &delta)) {
                int esz = addr_const_elem_size(e->u.bin.l);
                *out_sym = sym;
                *out_offset = off - (int)(delta * esz);
                return 1;
            }
        }
    }
    if (e->kind == EX_MEMBER) {
        const char *sym = NULL;
        int off = 0;
        if (eval_global_addr_offset(e->u.member.obj, &sym, &off)) {
            Type m_target_ty = get_global_obj_struct_type(e->u.member.obj);
            if (m_target_ty.kind == TY_STRUCT && m_target_ty.tag) {
                const StructDef *sd = struct_registry_find_c(g_ir_structs, m_target_ty.tag);
                if (sd) {
                    long long moff = 0;
                    if (struct_lookup_member(g_ir_structs, sd, e->u.member.name, &moff)) {
                        off += (int)moff;
                        *out_sym = sym;
                        *out_offset = off;
                        return 1;
                    }
                }
            }
            *out_sym = sym;
            *out_offset = off;
            return 1;
        }
    }
    if (e->kind == EX_INDEX) {
        const char *sym = NULL;
        int off = 0;
        if (!eval_global_addr_offset(e->u.idx.array, &sym, &off))
            return 0;
        long long idx = 0;
        if (!fold_const_int(e->u.idx.index, &idx))
            return 0;
        int esz = get_global_elem_size(e->u.idx.array);
        *out_sym = sym;
        *out_offset = off + (int)(idx * esz);
        return 1;
    }
    if (e->kind == EX_DEREF) {
        return eval_global_addr_offset(e->u.deref.operand, out_sym, out_offset);
    }
    return 0;
}

static int eval_strlit_byte_offset(const Expr *e, const char **bytes, int *len, int *off) {
    if (!e) return 0;
    while (e->kind == EX_CAST) e = e->u.cast.operand;
    if (e->kind == EX_STR) {
        *bytes = e->u.str.bytes;
        *len = e->u.str.len;
        *off = 0;
        return 1;
    }
    if (e->kind == EX_ADDR)
        return eval_strlit_byte_offset(e->u.addr.operand, bytes, len, off);
    if (e->kind == EX_DEREF)
        return eval_strlit_byte_offset(e->u.deref.operand, bytes, len, off);
    if (e->kind == EX_INDEX) {
        if (!eval_strlit_byte_offset(e->u.idx.array, bytes, len, off)) return 0;
        long long idx = 0;
        if (!fold_const_int(e->u.idx.index, &idx)) return 0;
        *off += (int)idx;
        return 1;
    }
    return 0;
}

/* Fold the classic offsetof idiom `&((T *)0)->member` to the byte offset of
 * `member` within T.  GCC treats this as a compile-time integer constant.
 * Returns 1 and sets *out to the offset if e matches (casts allowed). */
static int fold_null_offsetof(const Expr *e, long long *out) {
    if (fakecc_had_error()) return 0;
    if (!e) return 0;
    const Expr *ce = e;
    while (ce && ce->kind == EX_CAST) ce = ce->u.cast.operand;
    if (!ce || ce->kind != EX_ADDR) return 0;
    const Expr *sub = ce->u.addr.operand;
    while (sub && sub->kind == EX_CAST) sub = sub->u.cast.operand;
    if (!sub || sub->kind != EX_MEMBER) return 0;
    /* The member chain's root must be a null pointer constant: `(T *)0`.
     * The parser desugars `p->m` to `(*p).m`, so the root is
     * EX_DEREF(EX_CAST(T*, 0)).  For nested access `a.b.c`, the obj chain is
     * EX_MEMBER("c", obj=EX_MEMBER("b", obj=EX_MEMBER("a", obj=root))).  Walk
     * down to the root, collecting member names outer-to-inner; verify the
     * root is `(T*)0`; then walk the chain inner-to-out accumulating the
     * byte offset. */
    const Expr *root = sub;
    int depth = 0;
    while (root && root->kind == EX_MEMBER) {
        root = root->u.member.obj;
        depth++;
    }
    if (!root) return 0;
    /* root should be EX_DEREF(EX_CAST) or EX_CAST of int lit 0 */
    const Expr *obj = root;
    while (obj && (obj->kind == EX_CAST || obj->kind == EX_DEREF)) {
        if (obj->kind == EX_CAST) obj = obj->u.cast.operand;
        else obj = obj->u.deref.operand;
    }
    if (!obj || obj->kind != EX_INT_LIT || obj->u.int_val != 0) return 0;
    /* Recover the root struct type from the null-pointer expression. */
    Type cur_ty = get_global_obj_struct_type(root);
    if (cur_ty.kind != TY_STRUCT || !cur_ty.tag) return 0;
    /* Collect member names from outermost to innermost, then walk the chain
     * from innermost (closest to root) outward, accumulating offsets. */
    const char *names[16];
    int n = 0;
    const Expr *cur = sub;
    while (cur && cur->kind == EX_MEMBER && n < 16) {
        names[n] = cur->u.member.name;
        n++;
        cur = cur->u.member.obj;
    }
    if (n != depth) return 0;
    long long total = 0;
    for (int i = n - 1; i >= 0; i--) {
        const StructDef *sd = struct_registry_find_c(g_ir_structs, cur_ty.tag);
        if (!sd) return 0;
        long long moff = 0;
        const StructMember *sm =
            struct_lookup_member(g_ir_structs, sd, names[i], &moff);
        if (!sm) return 0;
        total += moff;
        if (i > 0) {
            cur_ty = sm->type;
            if (cur_ty.kind != TY_STRUCT || !cur_ty.tag) return 0;
        }
    }
    *out = total;
    return 1;
}

/* Difference of two address constants into the same object: `&a.f - &a` is
 * an integer constant (C address-constant arithmetic / GCC offsetof-style). */
static int fold_global_ptrdiff(const Expr *e, long long *out) {
    if (fakecc_had_error()) return 0;
    if (!e) return 0;
    /* Strip leading casts: `(int)(ptrdiff) + N` → `(ptrdiff) + N` */
    const Expr *ce = e;
    while (ce && ce->kind == EX_CAST) ce = ce->u.cast.operand;
    if (!ce) return 0;
    /* Handle `(ptrdiff) + N` / `(ptrdiff) - N` by peeling the integer offset. */
    long long offset = 0;
    const Expr *bin = ce;
    while (bin && bin->kind == EX_BINOP &&
           (bin->u.bin.op == BOP_ADD || bin->u.bin.op == BOP_SUB)) {
        long long rv = 0;
        if (bin->u.bin.r->kind == EX_INT_LIT) {
            rv = (long long)bin->u.bin.r->u.int_val;
        } else if (bin->u.bin.r->kind == EX_CAST
                   && bin->u.bin.r->u.cast.operand->kind == EX_INT_LIT) {
            rv = (long long)bin->u.bin.r->u.cast.operand->u.int_val;
        } else {
            break;
        }
        offset += (bin->u.bin.op == BOP_ADD) ? rv : -rv;
        bin = bin->u.bin.l;
        /* Strip casts between peel steps */
        while (bin && bin->kind == EX_CAST) bin = bin->u.cast.operand;
    }
    if (!bin || bin->kind != EX_BINOP || bin->u.bin.op != BOP_SUB) {
        /* No pointer-subtraction core.  The whole expression may be a bare
         * offsetof idiom like `&((T *)0)->member` (possibly ± integer), which
         * is itself a compile-time constant. */
        if (bin) {
            long long moff = 0;
            if (fold_null_offsetof(bin, &moff)) {
                *out = moff + offset;
                return 1;
            }
        }
        if (!bin) return 0;
        return 0;
    }
    const Expr *l = bin->u.bin.l;
    const Expr *r = bin->u.bin.r;
    const Expr *lp = l;
    const Expr *rp = r;
    while (lp && lp->kind == EX_CAST) lp = lp->u.cast.operand;
    while (rp && rp->kind == EX_CAST) rp = rp->u.cast.operand;
    if (!lp || !rp) return 0;
    /* EX_MEMBER is also accepted — eval_global_addr_offset handles it
     * (array members decay to &member[0], struct members are link-time
     * constants). */
    if (lp->kind != EX_ADDR && lp->kind != EX_LABEL_ADDR && lp->kind != EX_STR
        && lp->kind != EX_MEMBER)
        return 0;
    if (rp->kind != EX_ADDR && rp->kind != EX_LABEL_ADDR && rp->kind != EX_STR
        && rp->kind != EX_MEMBER)
        return 0;
    {
        const char *b1 = NULL, *b2 = NULL;
        int n1 = 0, n2 = 0, so1 = 0, so2 = 0;
        if (eval_strlit_byte_offset(l, &b1, &n1, &so1)
            && eval_strlit_byte_offset(r, &b2, &n2, &so2)
            && b1 && b2 && n1 == n2 && memcmp(b1, b2, (size_t)n1) == 0) {
            int esz = 1;
            if (l->type.kind == TY_PTR && l->type.pointee)
                esz = type_size(*l->type.pointee);
            if (esz < 1) esz = 1;
            *out = (long long)(so1 - so2) / esz;
            return 1;
        }
    }
    const char *s1 = NULL, *s2 = NULL;
    int o1 = 0, o2 = 0;
    if (!eval_global_addr_offset(l, &s1, &o1)) return 0;
    if (!eval_global_addr_offset(r, &s2, &o2)) return 0;
    if (!s1 || !s2 || strcmp(s1, s2) != 0) return 0;
    int esz = 1;
    if (l->type.kind == TY_PTR && l->type.pointee)
        esz = type_size(*l->type.pointee);
    if (esz < 1) esz = 1;
    *out = (long long)(o1 - o2) / esz + offset;
    return 1;
}

/* `&b - &a` for two different symbols.  The Mach-O linker writes
 * address(pos) - address(neg) + addend.  Same-symbol diffs are constants. */
static int split_global_ptrdiff(const Expr *e, const char **pos, const char **neg,
                                int *addend) {
    if (!e) return 0;
    const Expr *ce = e;
    while (ce && ce->kind == EX_CAST) ce = ce->u.cast.operand;
    if (!ce || ce->kind != EX_BINOP || ce->u.bin.op != BOP_SUB) return 0;
    const char *s1 = NULL, *s2 = NULL;
    int o1 = 0, o2 = 0;
    if (!eval_global_addr_offset(ce->u.bin.l, &s1, &o1)) return 0;
    if (!eval_global_addr_offset(ce->u.bin.r, &s2, &o2)) return 0;
    if (!s1 || !s2 || strcmp(s1, s2) == 0) return 0;
    int esz = 1;
    if (ce->u.bin.l->type.kind == TY_PTR && ce->u.bin.l->type.pointee)
        esz = type_size(*ce->u.bin.l->type.pointee);
    if (esz != 1) return 0;
    *pos = s1;
    *neg = s2;
    *addend = o1 - o2;
    return 1;
}

/* If `e` is an address-constant expression whose root object is a file-scope
 * compound literal (e.g. `&((T){...})`, `&((T){...}).m`, `&((int[]){})[i]`),
 * return that literal and set *out_off to the byte offset of the referenced
 * sub-object within it.  Otherwise return NULL.  Used to detect global
 * initializers that need a compound literal materialized as a static global. */
static const Expr *find_compound_literal_base(const Expr *e, long long *out_off)
{
    if (!e) return NULL;
    long long off = 0;
    const Expr *cur = e;
    for (;;) {
        while (cur && cur->kind == EX_CAST) cur = cur->u.cast.operand;
        if (!cur) return NULL;
        switch (cur->kind) {
        case EX_ADDR:
            cur = cur->u.addr.operand;
            continue;
        case EX_MEMBER: {
            const Expr *obj = cur->u.member.obj;
            while (obj && obj->kind == EX_CAST) obj = obj->u.cast.operand;
            if (!obj) return NULL;
            Type mty = (obj->kind == EX_COMPOUND_LITERAL)
                ? obj->u.compound.target_type
                : get_global_obj_struct_type(obj);
            if (mty.kind != TY_STRUCT || !mty.tag) return NULL;
            const StructDef *sd = struct_registry_find_c(g_ir_structs, mty.tag);
            if (!sd) return NULL;
            long long mm = 0;
            if (!struct_lookup_member(g_ir_structs, sd, cur->u.member.name, &mm))
                return NULL;
            off += mm;
            cur = cur->u.member.obj;
            continue;
        }
        case EX_INDEX: {
            const Expr *arr = cur->u.idx.array;
            while (arr && arr->kind == EX_CAST) arr = arr->u.cast.operand;
            if (!arr) return NULL;
            long long idx = 0;
            if (!fold_const_int(cur->u.idx.index, &idx)) return NULL;
            int esz = get_global_elem_size(arr);
            if (esz < 1) esz = 1;
            off += idx * esz;
            cur = cur->u.idx.array;
            continue;
        }
        case EX_COMPOUND_LITERAL:
            *out_off = off;
            return cur;
        default:
            return NULL;
        }
    }
}

/* Materialize a file-scope compound literal `(T){...}` as an anonymous static
 * global so that its address becomes a link-time constant — matching GCC's
 * treatment of compound literals at file scope (they get static storage
 * duration).  The global is named `__clit.N`.  Returns the symbol name to be
 * referenced by a fixup (caller takes ownership), or NULL if `e` is not an
 * EX_COMPOUND_LITERAL. */
static char *materialize_compound_literal(const IRModule *ir, const Expr *e)
{
    if (!e || e->kind != EX_COMPOUND_LITERAL)
        return NULL;
    Type target = e->u.compound.target_type;
    int sz = type_size(target);
    if (sz <= 0) sz = 8;
    char *bytes = calloc(sz, 1);
    if (!bytes) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
    char name[32];
    snprintf(name, sizeof name, "__clit.%d", g_str_counter++);
    /* Pack the literal's own initializer into `bytes` FIRST (nested pack_init
     * may queue its own rodata and attach pointer fixups).  A scratch global
     * collects those fixups; they are transferred to the real global on flush.
     * We cannot push immediately: that reallocs the module's global array and
     * would dangle the fixup the caller is about to attach. */
    IRGlobal *tmp = calloc(1, sizeof(IRGlobal));
    if (!tmp) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
    tmp->name = xstrdup(name);
    tmp->init_bytes = bytes;
    tmp->size = sz;
    if (e->u.compound.init)
        pack_init(ir, &target, e->u.compound.init, bytes, sz,
                  name, e->loc, tmp);
    queue_pending_global(name, bytes, sz, 0, e->loc);
    /* The most-recently-queued entry is our literal; attach the scratch global
     * so flush_pending_globals can transfer its fixups. */
    g_pending_globals[g_pending_globals_len - 1].tmp_g = tmp;
    return xstrdup(name);
}

static void pack_init(const IRModule *ir, const Type *ty, const Expr *e,
                      char *bytes, int sz, const char *ctx, SourceLoc loc,
                      IRGlobal *g) {
    /* Implicit zero from a sparse array init list.  The destination was
     * calloc'd, so a missing element is already the right bit pattern. */
    if (!e) return;
    if (e->kind == EX_VAR && strcmp(e->u.var.name, "NULL") == 0) {
        memset(bytes, 0, sz);
        return;
    }
    if (ty->kind == TY_STRUCT && ty->tag && strncmp(ty->tag, "__complex_", 10) == 0) {
        long double cr = 0.0, ci = 0.0;
        if (fold_const_complex(e, &cr, &ci)) {
            int esz = sz / 2;
            int is_float = (strstr(ty->tag, "float") || strstr(ty->tag, "double") || strstr(ty->tag, "ldouble")) ? 1 : 0;
            if (is_float) {
                if (esz == 16) {
                    memcpy(bytes, &cr, 10);
                    memcpy(bytes + esz, &ci, 10);
                } else if (esz == 4) {
                    float fr = (float)cr, fi = (float)ci;
                    memcpy(bytes, &fr, 4);
                    memcpy(bytes + esz, &fi, 4);
                } else {
                    double dr = (double)cr, di = (double)ci;
                    memcpy(bytes, &dr, 8);
                    memcpy(bytes + esz, &di, 8);
                }
            } else {
                if (esz == 1) {
                    char c_r = (char)cr, c_i = (char)ci;
                    memcpy(bytes, &c_r, 1);
                    memcpy(bytes + esz, &c_i, 1);
                } else if (esz == 2) {
                    short s_r = (short)cr, s_i = (short)ci;
                    memcpy(bytes, &s_r, 2);
                    memcpy(bytes + esz, &s_i, 2);
                } else if (esz == 4) {
                    int i_r = (int)cr, i_i = (int)ci;
                    memcpy(bytes, &i_r, 4);
                    memcpy(bytes + esz, &i_i, 4);
                } else {
                    int64_t l_r = (int64_t)cr, l_i = (int64_t)ci;
                    memcpy(bytes, &l_r, 8);
                    memcpy(bytes + esz, &l_i, 8);
                }
            }
            return;
        }
    }
    if (e->kind == EX_INIT_LIST) {
        int n = e->u.init_list.num_elements;
        if (ty->is_vector && ty->elem_type) {
            int esz = type_size(*ty->elem_type);
            int cur_idx = 0;
            for (int i = 0; i < n; i++) {
                int elem_idx = cur_idx++;
                if (e->u.init_list.desig_kind && e->u.init_list.desig_kind[i] == 0) {
                    elem_idx = e->u.init_list.desig_index[i];
                    cur_idx = elem_idx + 1;
                }
                if (!e->u.init_list.elements[i]) continue;
                if (elem_idx * esz < sz) {
                    pack_init(ir, ty->elem_type, e->u.init_list.elements[i],
                              bytes + elem_idx * esz, esz, ctx, loc, g);
                }
            }
            return;
        }
        switch (ty->kind) {
        case TY_ARRAY: {
            if (n == 1 && e->u.init_list.elements[0]
                && e->u.init_list.elements[0]->kind == EX_STR
                && ty->elem_type && ty->elem_type->width == 1) {
                pack_init(ir, ty, e->u.init_list.elements[0], bytes, sz, ctx, loc, g);
                break;
            }
            int esz = type_size(*ty->elem_type);
            int cur_idx = 0;
            for (int i = 0; i < n; i++) {
                int elem_idx = cur_idx++;
                if (e->u.init_list.desig_kind && e->u.init_list.desig_kind[i] == 0) {
                    elem_idx = e->u.init_list.desig_index[i];
                    cur_idx = elem_idx + 1;
                }
                if (!e->u.init_list.elements[i]) continue;
                int off = elem_idx * esz;
                if (esz <= 0 || off < 0 || off >= sz) continue;
                int nsz = esz;
                if (off + nsz > sz) nsz = sz - off;
                pack_init(ir, ty->elem_type, e->u.init_list.elements[i],
                          bytes + off, nsz, ctx, loc, g);
            }
            break;
        }
        case TY_STRUCT: {
            const StructDef *sd = struct_registry_find_c(g_ir_structs, ty->tag);
            if (!sd) {
                die_at(loc.file, loc.line, loc.col,
                       "unknown struct 'struct %s'", ty->tag);
                return;
            }
            int cur_idx = 0;
            for (int i = 0; i < n; i++) {
                const StructMember *sm = NULL;
                if (e->u.init_list.desig_kind && e->u.init_list.desig_kind[i] == 1
                    && e->u.init_list.desig_member[i]) {
                    sm = ir_find_struct_member(sd, e->u.init_list.desig_member[i]);
                } else if (e->u.init_list.desig_kind && e->u.init_list.desig_kind[i] == 0) {
                    int dix = e->u.init_list.desig_index[i];
                    if (dix >= 0 && dix < sd->num_members) sm = &sd->members[dix];
                } else {
                    if (cur_idx < sd->num_members) sm = &sd->members[cur_idx++];
                }
                if (!sm || ((sm->name == NULL || sm->name[0] == '\0') && sm->bit_width >= 0)) continue;
                if (sm->bit_width > 0) {
                    long long fv = 0;
                    const Expr *el = e->u.init_list.elements[i];
                    if (el->kind == EX_INT_LIT) fv = el->u.int_val;
                    else fold_const_int(el, &fv);
                    int uw = type_size(sm->type);
                    if (uw > 8) uw = 8;
                    if (uw < 1) uw = 1;
                    if (sm->bit_offset + sm->bit_width > uw * 8)
                        uw = 8;
                    uint64_t mask = (uint64_t)bitfield_mask64(sm->bit_width);
                    uint64_t val_masked = ((uint64_t)fv) & mask;
                    pack_bitfield_host(bytes, sz, sm->offset, sm->bit_offset,
                                       sm->bit_width, val_masked,
                                       sd->is_big_endian, uw);
                } else {
                    int msz = type_size(sm->type);
                    if (msz <= 0 && sm->type.kind == TY_ARRAY && sm->type.elem_type) {
                        const Expr *el = e->u.init_list.elements[i];
                        if (el->kind == EX_STR)
                            msz = el->u.str.len + 1;
                        else if (el->kind == EX_INIT_LIST)
                            msz = el->u.init_list.num_elements * type_size(*sm->type.elem_type);
                    }
                    /* Flexible array members may carry excess initializers
                     * (gcc.c-torture 20030305-1).  Do not write past the
                     * allocated object — C99 forbids FAM init, GCC errors;
                     * FakeCC ignores the excess so compile tests don't ICE. */
                    if (sm->offset < 0 || sm->offset >= sz) continue;
                    if (msz < 0) msz = 0;
                    if (sm->offset + msz > sz) msz = sz - sm->offset;
                    if (msz <= 0) continue;
                    pack_init(ir, &sm->type, e->u.init_list.elements[i],
                              bytes + sm->offset,
                              msz, ctx, loc, g);
                    if (sd->is_big_endian && sm->bit_width <= 0
                        && (msz == 2 || msz == 4 || msz == 8)
                        && sm->type.kind != TY_STRUCT && sm->type.kind != TY_ARRAY) {
                        unsigned char *p = (unsigned char *)(bytes + sm->offset);
                        for (int b = 0; b < msz / 2; b++) {
                            unsigned char t = p[b];
                            p[b] = p[msz - 1 - b];
                            p[msz - 1 - b] = t;
                        }
                    }
                }
            }
            break;
        }
        default:
            /* Scalar with a single-element brace list: `int x = {5}`. */
            pack_init(ir, ty, e->u.init_list.elements[0], bytes, sz, ctx, loc, g);
            break;
        }
        return;
    }
    if (e->kind == EX_COMPOUND_LITERAL) {
        /* `int *p = (int[]){1,2,3}`: the compound literal has static storage
         * and decays to a pointer.  Packing the init list into the pointer
         * slot would store the first element (then crash on dereference). */
        if (g && ty->kind == TY_PTR) {
            char *sym = materialize_compound_literal(ir, e);
            if (sym) {
                add_global_fixup(g, (int)(bytes - g->init_bytes), sym, 0);
                memset(bytes, 0, sz);
                free(sym);
                return;
            }
        }
        pack_init(ir, ty, e->u.compound.init, bytes, sz, ctx, loc, g);
        return;
    }
    if (e->kind == EX_CAST) {
        /* Folded casts (truncation, sign-extend) fall through to the integer
         * packers below.  Non-constant casts (pointer decay of a global, …)
         * pack the operand. */
        long long dummy;
        unsigned long long lo, hi;
        int folded = (ty->kind == TY_INT && ty->width == 16)
                     ? fold_const_int128(e, &lo, &hi)
                     : fold_const_int(e, &dummy);
        if (!folded) {
            pack_init(ir, ty, e->u.cast.operand, bytes, sz, ctx, loc, g);
            return;
        }
    }
    if (ty->kind == TY_FLOAT) {
        if (ty->is_decimal) {
            unsigned long long lo = 0, hi = 0;
            int folded = 0;
            if (e->kind == EX_FLOAT_LIT) {
                folded = dfp_from_str(e->u.float_text, (int)ty->width, &lo, &hi);
            } else if (e->kind == EX_UNARY && e->u.un.op == UOP_NEG) {
                if (e->u.un.operand->kind == EX_FLOAT_LIT &&
                    dfp_from_str(e->u.un.operand->u.float_text, (int)ty->width, &lo, &hi)) {
                    dfp_neg_bits((int)ty->width, &lo, &hi);
                    folded = 1;
                }
            } else if (e->kind == EX_INT_LIT) {
                folded = dfp_from_int((unsigned long long)e->u.int_val, e->int_hi,
                                      e->type.is_unsigned, (int)ty->width, &lo, &hi);
            } else if (e->kind == EX_BINOP) {
                unsigned long long alo = 0, ahi = 0, blo = 0, bhi = 0;
                int aok = 0, bok = 0;
                if (e->u.bin.l->kind == EX_FLOAT_LIT)
                    aok = dfp_from_str(e->u.bin.l->u.float_text, (int)ty->width, &alo, &ahi);
                if (e->u.bin.r->kind == EX_FLOAT_LIT)
                    bok = dfp_from_str(e->u.bin.r->u.float_text, (int)ty->width, &blo, &bhi);
                if (aok && bok) {
                    int op = DFP_ADD;
                    if (e->u.bin.op == BOP_SUB) op = DFP_SUB;
                    else if (e->u.bin.op == BOP_MUL) op = DFP_MUL;
                    else if (e->u.bin.op == BOP_DIV) op = DFP_DIV;
                    else if (e->u.bin.op != BOP_ADD) op = -1;
                    if (op >= 0)
                        folded = dfp_binop_bits(op, (int)ty->width, alo, ahi, blo, bhi, &lo, &hi);
                }
            }
            if (folded) {
                int n = sz < 8 ? sz : 8;
                for (int b = 0; b < n; b++)
                    bytes[b] = (char)((lo >> (8 * b)) & 0xff);
                if (sz > 8) {
                    int n2 = sz - 8;
                    if (n2 > 8) n2 = 8;
                    for (int b = 0; b < n2; b++)
                        bytes[8 + b] = (char)((hi >> (8 * b)) & 0xff);
                }
                return;
            }
        }
        /* Float slots must be packed as the target's binary format, not as the
         * integer the literal would fold to.  Signaling NaNs are copied as
         * bits so a float payload is not rewritten by a wider cast. */
        unsigned long long nlo = 0, nhi = 0;
        if (nans_builtin_bits(e, (int)ty->width, &nlo, &nhi)) {
            unsigned char raw[16];
            int n = sz < 16 ? sz : 16;
            memset(raw, 0, sizeof raw);
            memcpy(raw, &nlo, 8);
            memcpy(raw + 8, &nhi, 8);
            memcpy(bytes, raw, (size_t)n);
            return;
        }
        long double fv;
        if (fold_const_float(e, &fv, ir)) {
            if (ty->width == 16) {
                memcpy(bytes, &fv, 10);
            } else if (ty->width == 4) {
                float f = (float)fv;
                memcpy(bytes, &f, sizeof f);
            } else {
                double d = (double)fv;
                memcpy(bytes, &d, sizeof d);
            }
            return;
        }
    }
    /* For int128 targets, try fold_const_int128 first: it computes both
     * halves correctly.  fold_const_int (below) would silently truncate
     * to 64 bits, losing the high half of values like (1 << 64). */
    if (ty->kind == TY_INT && ty->width == 16) {
        unsigned long long vlo, vhi;
        if (fold_const_int128(e, &vlo, &vhi)) {
            if (ty->is_bool) { vlo = (vlo != 0 || vhi != 0) ? 1 : 0; vhi = 0; }
            int n = sz < 8 ? sz : 8;
            for (int b = 0; b < n; b++)
                bytes[b] = (char)((vlo >> (8 * b)) & 0xff);
            if (sz > 8) {
                int n2 = sz - 8;
                if (n2 > 8) n2 = 8;
                for (int b = 0; b < n2; b++)
                    bytes[8 + b] = (char)((vhi >> (8 * b)) & 0xff);
                unsigned char fill = (!ty->is_unsigned && (vhi >> 63)) ? 0xff : 0;
                for (int b = 8 + n2; b < sz; b++)
                    bytes[b] = (char)fill;
            }
            return;
        }
    }
    long long _fold_v = 0;
    int have_int = (e->kind == EX_INT_LIT)
                || fold_const_int(e, &_fold_v)
                || (ty->kind == TY_INT && fold_global_ptrdiff(e, &_fold_v))
                || (ty->kind == TY_INT && fold_null_offsetof(e, &_fold_v))
                || (ty->kind == TY_INT && fold_const_complex_rel(e, &_fold_v));
    if (!have_int && ty->kind == TY_INT) {
        long double fv;
        if (fold_const_float(e, &fv, ir)) {
            _fold_v = (long long)fv;
            have_int = 1;
        }
    }
    if (have_int) {
        unsigned long long vlo, vhi = 0;
        if (e->kind == EX_INT_LIT) {
            vlo = (unsigned long long)e->u.int_val;
            vhi = 0;
            if (e->type.width == 16)
                vhi = e->int_hi;
            else if (!e->type.is_unsigned && e->u.int_val < 0)
                vhi = ~0ULL;
        } else {
            vlo = (unsigned long long)_fold_v;
            if (_fold_v < 0) vhi = ~0ULL;
        }
        if (ty->is_bool) {
            vlo = (vlo != 0 || vhi != 0) ? 1 : 0;
            vhi = 0;
        }
        int n = sz < 8 ? sz : 8;
        for (int b = 0; b < n; b++)
            bytes[b] = (char)((vlo >> (8 * b)) & 0xff);
        if (sz > 8) {
            int n2 = sz - 8;
            if (n2 > 8) n2 = 8;
            for (int b = 0; b < n2; b++)
                bytes[8 + b] = (char)((vhi >> (8 * b)) & 0xff);
            unsigned char fill = (!ty->is_unsigned && (vhi >> 63)) ? 0xff : 0;
            for (int b = 8 + n2; b < sz; b++)
                bytes[b] = (char)fill;
        }
        return;
    }
    if (e->kind == EX_STR && ty->kind == TY_PTR && g) {
        /* `const char *p = "x"` — the slot holds the address of the bytes, so
         * park them in an anonymous rodata global and let the linker patch the
         * pointer in. */
        char name[32];
        snprintf(name, sizeof name, "__str.%d", g_str_counter++);
        int nbytes = e->u.str.len + 1;
        char *init = malloc(nbytes);
        if (!init) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        memcpy(init, e->u.str.bytes, nbytes);
        queue_rodata(name, init, nbytes, loc);
        add_global_fixup(g, (int)(bytes - g->init_bytes), name, 0);
        memset(bytes, 0, sz);  /* patched by the linker */
        return;
    }
    if (e->kind == EX_STR && ty->kind == TY_ARRAY && ty->elem_type
        && ty->elem_type->width == 1) {
        /* char[] = "..." — copy bytes (up to sz); NUL-terminate only if room. */
        if (sz > 0) {
            int n = e->u.str.len;
            if (n > sz) n = sz;
            if (n > 0) memcpy(bytes, e->u.str.bytes, n);
            if (n < sz) bytes[n] = '\0';
        }
        return;
    }
    /* Reference to a previously-defined const global: `.regs = ALLOCATABLE_REGS`.
     * If the target slot is a pointer, the array/struct must decay to its
     * ADDRESS (a link-time constant) — record a fixup so codegen emits a
     * R_X86_64_64 relocation.  Otherwise copy its already-packed bytes. */
    if (e->kind == EX_VAR && ir) {
        const IRGlobal *src = find_init_global(ir, e->u.var.name);
        if (src) {
            if (ty->kind == TY_PTR && g) {
                int off = (int)(bytes - g->init_bytes);
                add_global_fixup(g, off, src->name, 0);
                memset(bytes, 0, sz);  /* patched by linker */
                return;
            }
            int n = sz < src->size ? sz : src->size;
            if (src->init_bytes)
                memcpy(bytes, src->init_bytes, n);
            return;
        }
    }
    /* Address of a file-scope compound literal: `&((T){...})`,
     * `&((T){...}).member`, `&((int[]){...})[i]`, or any address-constant built
     * from one.  GCC gives such literals static storage, so their address is a
     * link-time constant.  Materialize the literal as an anonymous global and
     * add a pointer fixup referencing it (+ member/element offset). */
    if (g && (ty->kind == TY_PTR
              || (ty->kind == TY_INT && ty->width == 8
                  && expr_has_address_constant(e)))) {
        long long clit_off = 0;
        const Expr *clit = find_compound_literal_base(e, &clit_off);
        if (clit) {
            char *sym = materialize_compound_literal(ir, clit);
            if (sym) {
                add_global_fixup(g, (int)(bytes - g->init_bytes),
                                 sym, (int)clit_off);
                memset(bytes, 0, sz);
                free(sym);
                return;
            }
        }
    }
    /* Address of a global object (with optional member/array offset/arithmetic).
     * GCC also accepts converting that address constant to a pointer-sized
     * integer (`unsigned long x = (unsigned long)&sym - C`); the object file
     * still carries a R_X86_64_64 reloc with the integer addend. */
    const char *addr_sym = NULL;
    int addr_offset = 0;
    if (g && eval_global_addr_offset(e, &addr_sym, &addr_offset)
        && (ty->kind == TY_PTR
            || (ty->kind == TY_INT && ty->width == 8
                && expr_has_address_constant(e)))) {
        add_global_fixup(g, (int)(bytes - g->init_bytes), addr_sym, addr_offset);
        memset(bytes, 0, sz);
        return;
    }
    {
        unsigned long long abs_addr = 0;
        if (ty->kind == TY_PTR && eval_abs_addr_const(e, &abs_addr)) {
            int n = sz < 8 ? sz : 8;
            for (int b = 0; b < n; b++)
                bytes[b] = (char)((abs_addr >> (8 * b)) & 0xff);
            for (int b = n; b < sz; b++)
                bytes[b] = 0;
            return;
        }
    }
    /* `_Bool gp = &x` / `_Bool gp = f`: a non-null address constant converts
     * to 1.  No reloc — the object exists, so the converted value is 1. */
    if (ty->is_bool && sz >= 1) {
        unsigned long long abs_addr = 0;
        if (eval_abs_addr_const(e, &abs_addr)) {
            bytes[0] = (char)(abs_addr != 0);
            for (int b = 1; b < sz; b++) bytes[b] = 0;
            return;
        }
        int is_addr = expr_has_address_constant(e);
        if (!is_addr && e->kind == EX_VAR
            && (e->type.kind == TY_FUNC
                || (e->type.kind == TY_PTR && e->type.pointee
                    && e->type.pointee->kind == TY_FUNC)))
            is_addr = 1;
        if (is_addr) {
            bytes[0] = 1;
            for (int b = 1; b < sz; b++) bytes[b] = 0;
            return;
        }
    }
    const char *dpos = NULL, *dneg = NULL;
    int dadd = 0;
    if (g && ty->kind == TY_INT && (ty->width == 8 || ty->width == 4)
        && split_global_ptrdiff(e, &dpos, &dneg, &dadd)) {
        add_global_fixup(g, (int)(bytes - g->init_bytes), dpos, dadd);
        g->fixups[g->num_fixups - 1].sub = xstrdup(dneg);
        memset(bytes, 0, sz);
        return;
    }
    die_at(loc.file, loc.line, loc.col,
           "global '%s' initializer must be a compile-time constant", ctx);
    return;
}

/* Recursively assign concrete byte offsets for a (possibly nested) scalar
 * initializer list element when the target is an array/struct.  Used by the
 * local (non-global) ST_DECL lowering to emit per-element stores.  `base` is
 * the pointer value to the object; each element is stored at base+offset. */
static void lower_init_list(IRFunction *fn, IRSymTable *st, IRValue base,
                            const Type *ty, const Expr *e, SourceLoc loc) {
    if (fakecc_had_error()) return;
    /* Sparse array holes (NULL) and integer 0 are already the object's
     * background from emit_zero_bytes.  Do not emit a store per gap. */
    if (!e) return;
    if (e->kind == EX_INT_LIT && e->u.int_val == 0 && e->int_hi == 0
        && e->type.kind != TY_FLOAT && !e->type.is_decimal)
        return;
    if (e->kind == EX_INIT_LIST) {
        int n = e->u.init_list.num_elements;
        if (ty->is_vector && ty->elem_type) {
            int esz = type_size(*ty->elem_type);
            int cur_idx = 0;
            for (int i = 0; i < n; i++) {
                int elem_idx = cur_idx++;
                if (e->u.init_list.desig_kind && e->u.init_list.desig_kind[i] == 0) {
                    elem_idx = e->u.init_list.desig_index[i];
                    cur_idx = elem_idx + 1;
                }
                if (!e->u.init_list.elements[i]) continue;
                IRValue off = new_value(fn);
                emit_inst_w(fn, IR_CONST, off, -1, -1, (int64_t)elem_idx * esz, 8, 1, loc);
                IRValue ptr = emit_bin_w(fn, IR_ADD, base, off, 8, 1, loc);
                lower_init_list(fn, st, ptr, ty->elem_type,
                                e->u.init_list.elements[i], loc);
            }
            return;
        }
        switch (ty->kind) {
        case TY_ARRAY: {
            if (n == 1 && e->u.init_list.elements[0]
                && e->u.init_list.elements[0]->kind == EX_STR
                && ty->elem_type && ty->elem_type->width == 1) {
                lower_init_list(fn, st, base, ty, e->u.init_list.elements[0], loc);
                break;
            }
            int esz = type_size(*ty->elem_type);
            int cur_idx = 0;
            for (int i = 0; i < n; i++) {
                int elem_idx = cur_idx++;
                if (e->u.init_list.desig_kind && e->u.init_list.desig_kind[i] == 0) {
                    elem_idx = e->u.init_list.desig_index[i];
                    cur_idx = elem_idx + 1;
                }
                if (!e->u.init_list.elements[i]) continue;
                IRValue off = new_value(fn);
                emit_inst_w(fn, IR_CONST, off, -1, -1, (int64_t)elem_idx * esz, 8, 1, loc);
                IRValue ptr = emit_bin_w(fn, IR_ADD, base, off, 8, 1, loc);
                lower_init_list(fn, st, ptr, ty->elem_type,
                                e->u.init_list.elements[i], loc);
            }
            break;
        }
        case TY_STRUCT: {
            const StructDef *sd = struct_registry_find_c(g_ir_structs, ty->tag);
            if (!sd) {
                die_at(loc.file, loc.line, loc.col,
                       "unknown struct 'struct %s'", ty->tag);
                return;
            }
            int cur_idx = 0;
            for (int i = 0; i < n; i++) {
                const StructMember *sm = NULL;
                if (e->u.init_list.desig_kind && e->u.init_list.desig_kind[i] == 1
                    && e->u.init_list.desig_member[i]) {
                    sm = ir_find_struct_member(sd, e->u.init_list.desig_member[i]);
                } else if (e->u.init_list.desig_kind && e->u.init_list.desig_kind[i] == 0) {
                    int dix = e->u.init_list.desig_index[i];
                    if (dix >= 0 && dix < sd->num_members) sm = &sd->members[dix];
                } else {
                    if (cur_idx < sd->num_members) sm = &sd->members[cur_idx++];
                }
                if (!sm || ((sm->name == NULL || sm->name[0] == '\0') && sm->bit_width >= 0)) continue;
                IRValue off = new_value(fn);
                emit_inst_w(fn, IR_CONST, off, -1, -1, sm->offset, 8, 1, loc);
                IRValue ptr = emit_bin_w(fn, IR_ADD, base, off, 8, 1, loc);
                if (sm->bit_width > 0) {
                    /* Bitfield member: read-modify-write the storage unit so
                     * that we don't overwrite adjacent bitfields sharing the
                     * same storage unit at the same byte offset. */
                    int uw = type_size(sm->type);
                    if (uw > 8) uw = 8;
                    if (uw < 1) uw = 1;
                    if (sm->bit_offset + sm->bit_width > uw * 8)
                        uw = 8;
                    IRValue rv = lower_expr(fn, st, e->u.init_list.elements[i]);
                    BFUnit unit = emit_bf_load(fn, ptr, sm->bit_offset, sm->bit_width,
                                               uw, sd->is_big_endian, loc);
                    unit = emit_bf_insert(fn, unit, rv, sm->bit_offset, sm->bit_width, loc);
                    emit_bf_store(fn, ptr, unit, sd->is_big_endian, loc);
                } else {
                    lower_init_list(fn, st, ptr, &sm->type,
                                    e->u.init_list.elements[i], loc);
                }
            }
            break;
        }
        default:
            lower_init_list(fn, st, base, ty, e->u.init_list.elements[0], loc);
            break;
        }
        return;
    }
    /* A string literal initializing a char array member copies the bytes into
     * the array (`struct { char name[8]; } q = {"xy"}`).  Without this the
     * generic scalar path below would store the literal's *address*, truncated
     * to one byte.  The top-level `char s[] = "hi"` form never reaches here:
     * sema rewrites it into an element list first. */
    if (e->kind == EX_STR && ty->kind == TY_ARRAY && ty->elem_type
        && ty->elem_type->width == 1) {
        int total = ty->length;
        int n = e->u.str.len;
        if (n > total) n = total;
        int eu = ty->elem_type->is_unsigned;
        /* Remainder is already emit_zero_bytes'd, including the trailing NUL
         * when the string is shorter than the array. */
        for (int i = 0; i < n; i++) {
            IRValue off = new_value(fn);
            emit_inst_w(fn, IR_CONST, off, -1, -1, i, 8, 1, loc);
            IRValue ptr = emit_bin_w(fn, IR_ADD, base, off, 8, 1, loc);
            IRValue cv = new_value(fn);
            emit_inst_w(fn, IR_CONST, cv, -1, -1,
                        (unsigned char)e->u.str.bytes[i], 1, eu, loc);
            emit_inst_w(fn, IR_STORE_PTR, -1, ptr, cv, 0, 1, eu, loc);
        }
        return;
    }
    /* Aggregate rvalue (not a brace list): copy bytes like assignment.
     * `struct outers o = { rq() };` must memcpy the returned struct into
     * `inner`, not store rq()'s address as an 8-byte scalar.  Matches GCC.
     *
     * Only do this when the initializer's type is itself an aggregate.
     * Sema fills designated-init / empty `{}` gaps with integer 0; treating
     * that 0 as a source pointer would load from NULL.  The object was
     * already emit_zero_bytes'd, so a scalar zero is a no-op. */
    if ((ty->kind == TY_STRUCT || ty->is_vector || ty->kind == TY_ARRAY)
        && (e->type.kind == TY_STRUCT || e->type.is_vector
            || e->type.kind == TY_ARRAY)) {
        int sz = type_size(*ty);
        if (sz > 0) {
            IRValue agg = lower_expr(fn, st, e);
            emit_struct_copy(fn, base, agg, sz, loc);
        }
        return;
    }
    /* Scalar element: lower, coerce to the target width, store via pointer. */
    IRValue rv = lower_expr(fn, st, e);
    int rw = get_value_width(fn, rv), ru = get_value_is_unsigned(fn, rv);
    int rf = get_value_is_float(fn, rv);
    int df = (ty->kind == TY_FLOAT);
    int sw = ty->kind == TY_PTR ? 8 : (ty->width ? ty->width : 4);
    int su = ty->is_unsigned;
    IRValue coerced;
    if (rf != df)
        coerced = convert_numeric(fn, rv, rw, sw, su, df, loc);
    else if (df)
        coerced = (rw == sw) ? rv : convert_numeric(fn, rv, rw, sw, su, 1, loc);
    else
        coerced = coerce(fn, rv, rw, ru, sw, su, loc);
    emit_inst_w(fn, IR_STORE_PTR, -1, base, coerced, 0, sw, su, loc);
    if (df) fn->insts.data[fn->insts.len - 1].is_float = 1;
}

int ir_generate(const TranslationUnit *tu, IRModule *ir, int pin_locals) {
    fakecc_clear_error();
    /* Publish module + reset string counter for lower_expr's use. */
    g_ir_module = ir;
    g_str_counter = 0;
    g_flt_counter = 0;
    g_ir_structs = &tu->structs;
    g_ir_tu = tu;
    g_ir_pin_locals = pin_locals;

    irsymtable_init(&g_ir_globals_st);
    for (size_t g = 0; g < tu->globals.len; g++) {
        const Stmt *gs = &tu->globals.data[g];
        if (gs->kind == ST_DECL)
            irsymtable_push_global(&g_ir_globals_st, gs->u.decl.name,
                                   gs->u.decl.type,
                                   gs->u.decl.is_tls);
    }

    /* Register named globals from tu->globals.  `extern` globals are
     * declarations only — no storage, no emission (single-TU model: they can
     * never be defined, so any use is an unresolved symbol).  `static` globals
     * are emitted normally here (linkage has no effect in a single-TU static
     * ELF, but the storage + initializer are real). */
    for (size_t i = 0; i < tu->globals.len; i++) {
        const Stmt *s = &tu->globals.data[i];
        if (s->kind != ST_DECL) continue;
        if (s->u.decl.alias_target) {
            ir_module_push_alias(ir, s->u.decl.name, s->u.decl.alias_target,
                                 s->u.decl.storage_class == 1, s->loc);
            ir->aliases.data[ir->aliases.len - 1].is_weak = s->u.decl.is_weak;
            continue;
        }
        /* extern → declaration only: skip emission entirely.
         * `extern __thread T a;` declarations are caught here too: storage_class
         * is 2 (extern) and is_tls is 1, but we still skip because extern means
         * "defined elsewhere" — the linker resolves the symbol. */
        if (s->u.decl.storage_class == 2) {
            if (s->u.decl.is_weak) ir_add_weak_ref(ir, s->u.decl.name);
            continue;
        }
        int is_tls = s->u.decl.is_tls;
        int sz = type_size(s->u.decl.type);
        if (s->u.decl.init && s->u.decl.type.kind == TY_STRUCT && s->u.decl.init->kind == EX_INIT_LIST && s->u.decl.type.tag) {
            const StructDef *sd = struct_registry_find_c(g_ir_structs, s->u.decl.type.tag);
            if (sd) {
                int max_end = sz;
                for (int mi = 0; mi < s->u.decl.init->u.init_list.num_elements && mi < sd->num_members; mi++) {
                    int end = sd->members[mi].offset;
                    const Type *mt = &sd->members[mi].type;
                    int msz = type_size(*mt);
                    const Expr *el = s->u.decl.init->u.init_list.elements[mi];
                    if (el && msz <= 0 && mt->kind == TY_ARRAY && mt->elem_type) {
                        if (el->kind == EX_STR)
                            msz = el->u.str.len + 1;
                        else if (el->kind == EX_INIT_LIST)
                            msz = el->u.init_list.num_elements * type_size(*mt->elem_type);
                    }
                    end += msz;
                    if (end > max_end) max_end = end;
                }
                if (sd->align > 0) max_end = align_up(max_end, sd->align);
                sz = max_end;
            }
        }
        if (sz <= 0) sz = 8;
        int is_static = (s->u.decl.storage_class == 1);
        /* Only allocate init bytes when there is an initializer.  An
         * uninitialized global must have init_bytes == NULL so codegen
         * places it in .bss (SHT_NOBITS); calloc'ing `sz` here for a huge
         * array (e.g. pr65680.c's a[10^18][3]) would allocate tens of GB
         * and then emit it all into .data. */
        char *bytes = NULL;
        if (s->u.decl.init) {
            bytes = calloc(sz, 1);
            if (!bytes) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        }
        /* Create the global FIRST so pack_init can attach pointer fixups to it
         * when an array/struct member decays to a pointer (e.g. `.regs = ARR`). */
        IRGlobal *g = ir_module_push_global(ir, s->u.decl.name, sz, bytes,
                                            0, is_static, is_tls, s->loc);
        {
            int al = (int)type_align(s->u.decl.type);
            if (s->u.decl.align > al) al = s->u.decl.align;
            if (al > 0) g->align = al;
        }
        if (type_is_const_obj(s->u.decl.type)) g->is_const_obj = 1;
        if (s->u.decl.is_weak && !is_static) g->is_weak = 1;
        if (s->u.decl.is_hidden && !is_static) g->is_hidden = 1;
        if (s->u.decl.is_used) g->is_used = 1;
        if (s->u.decl.init) {
            pack_init(ir, &s->u.decl.type, s->u.decl.init, bytes, sz,
                      s->u.decl.name, s->loc, g);
            flush_rodata(ir);
        }
    }

    for (size_t i = 0; i < tu->functions.len; i++) {
        const FunctionDecl *fd = &tu->functions.data[i];
        if (fd->alias_target) {
            ir_module_push_alias(ir, fd->name, fd->alias_target,
                                 fd->is_static, fd->loc);
            ir->aliases.data[ir->aliases.len - 1].is_weak = fd->is_weak;
            continue;
        }

        /* `extern` declarations have no body — no IR is generated for them. */
        if (fd->is_extern) {
            if (fd->is_weak) ir_add_weak_ref(ir, fd->name);
            continue;
        }

        IRFunction irfn;
        memset(&irfn, 0, sizeof(irfn));
        irfn.name = xstrdup(fd->name);
        irfn.loc = fd->loc;
        irfn.insts.data = NULL;
        irfn.insts.len = 0;
        irfn.insts.cap = 0;
        irfn.next_value_id = 0;
        irfn.next_label_id = 0;
        irfn.ra = NULL;
        irfn.ra_xmm = NULL;
        irfn.value_width = NULL;
        irfn.value_is_unsigned = NULL;
        irfn.value_is_float = NULL;
        irfn.value_meta_cap = 0;
        irfn.ret_width = fd->ret_type.width;
        irfn.ret_is_unsigned = fd->ret_type.is_unsigned;
        irfn.ret_is_float = (fd->ret_type.kind == TY_FLOAT && !type_is_d128(fd->ret_type));
        irfn.ret_is_struct = (fd->ret_type.kind == TY_STRUCT || fd->ret_type.is_vector
                             || type_is_pair16(fd->ret_type));
        irfn.ret_is_bool = fd->ret_type.is_bool;
        irfn.ret_is_complex_ld = type_is_complex_ldouble(fd->ret_type);
        irfn.ret_x87_bytes = 0;
        irfn.is_variadic = fd->is_variadic;
        irfn.is_static = fd->is_static;
        irfn.is_weak = fd->is_weak;
        irfn.is_hidden = fd->is_hidden;
        irfn.is_used = fd->is_used;
        irfn.is_constructor = fd->is_constructor;
        irfn.is_destructor = fd->is_destructor;
        irfn.ctor_prio = fd->ctor_prio ? fd->ctor_prio : 65535;
        irfn.dtor_prio = fd->dtor_prio ? fd->dtor_prio : 65535;
        irfn.sret_value = -1;
        irfn.ret_reg_n = 0;
        for (int k = 0; k < 4; k++) irfn.ret_reg_cls[k] = 0;
        if (type_is_pair16(fd->ret_type)) {
            irfn.ret_reg_n = 2;
            irfn.ret_reg_cls[0] = (int)SYSV_CLS_INTEGER;
            irfn.ret_reg_cls[1] = (int)SYSV_CLS_INTEGER;
            irfn.ret_width = 16;
        } else if (irfn.ret_is_struct) {
            SysVRegClass rcls[4] = {0};
            /* Capacity 4: a three- or four-element HFA is returned in
             * v0-v3, exactly as it is passed. */
            irfn.ret_reg_n = sysv_classify_agg(fd->ret_type, rcls);
            abi_adjust_cls(fd->ret_type, rcls, &irfn.ret_reg_n, 4, fd->loc);
            for (int i = 0; i < irfn.ret_reg_n && i < 4; i++)
                irfn.ret_reg_cls[i] = (int)rcls[i];
            /* Struct size for copy/load lives in ret_width. */
            irfn.ret_width = type_size(fd->ret_type);
        }
        if (irfn.ret_is_complex_ld) {
            /* SysV: `_Complex long double` returns in st0/st1, not sret. */
            irfn.ret_reg_n = 2;
            irfn.ret_width = type_size(fd->ret_type);
            irfn.ret_x87_bytes = 32;
        } else if (sysv_agg_ret_x87(fd->ret_type)) {
            /* SysV: one X87+X87UP object returns in st0, not sret. */
            irfn.ret_width = type_size(fd->ret_type);
            irfn.ret_x87_bytes = 16;
        }
        irfn.dbg_vars = NULL;
        irfn.num_dbg_vars = 0;
        irfn.cap_dbg_vars = 0;
        g_ir_cur_fd = fd;

        IRSymTable st;
        irsymtable_init(&st);

        /* Emit all PARAM instructions up front so they stay contiguous at
         * function start (codegen's prologue relies on this layout).  The
         * alloca/addr/store chains follow afterward.
         *
         * SysV: MEMORY-class struct returns get a hidden sret pointer as
         * param 0.  Register-class struct returns need no hidden param.
         * Struct formals ≤16 bytes expand into 1–2 register PARAMs; MEMORY
         * formals are one stack blob (IR_PARAM with alloca_bytes = size). */
        size_t nparams = fd->params.len;
        IRValue **param_ebs = NULL;
        int *param_nreg = NULL;
        if (nparams > 0) {
            param_ebs = calloc(nparams, sizeof(*param_ebs));
            param_nreg = calloc(nparams, sizeof(*param_nreg));
            if (!param_ebs || !param_nreg) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        }
        int next_pidx = 0;
        if (irfn.ret_is_struct && irfn.ret_reg_n == 0
            && !type_is_empty_struct(fd->ret_type)
            && irfn.ret_x87_bytes == 0) {
            irfn.sret_value = new_value(&irfn);
            emit_inst_w(&irfn, IR_PARAM, irfn.sret_value, -1, -1, next_pidx++,
                        8, 1, fd->loc);
        }
        int used_gp = (irfn.sret_value >= 0 && abi_sret_uses_gp()) ? 1 : 0;
        int used_xmm = 0;
        for (size_t p = 0; p < fd->params.len; p++) {
            Type pty = fd->params.data[p].type;
            SourceLoc ploc = fd->params.data[p].loc;
            if (type_is_empty_struct(pty)) {
                param_nreg[p] = 0;
                param_ebs[p] = NULL;
                continue;
            }
            if (pty.kind == TY_STRUCT || pty.is_vector || type_is_pair16(pty)) {
                SysVRegClass cls[4];
                int nreg;
                if (type_is_pair16(pty)) {
                    cls[0] = SYSV_CLS_INTEGER;
                    cls[1] = SYSV_CLS_INTEGER;
                    nreg = 2;
                } else {
                    nreg = sysv_classify_agg(pty, cls);
                    abi_adjust_cls(pty, cls, &nreg, 4, ploc);
                }
                int is_memory = (nreg == 0);
                if (is_memory && (sysv_memory_pass_as_pointer(pty)
                                  || abi_indirect_agg(pty))) {
                    param_nreg[p] = -1;
                    param_ebs[p] = malloc(sizeof(IRValue));
                    if (!param_ebs[p]) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
                    param_ebs[p][0] = new_value(&irfn);
                    emit_inst_w(&irfn, IR_PARAM, param_ebs[p][0], -1, -1,
                                next_pidx++, 8, 1, ploc);
                    if (used_gp < abi_gp_nregs()) used_gp++;
                    else if (!abi_sret_uses_gp())
                        irfn.insts.data[irfn.insts.len - 1].force_stack = 1;
                } else if (is_memory) {
                    /* Incoming stack blob: PARAM dst is LEA [rbp+off]. */
                    int total = type_size(pty);
                    if (total < 1) total = 1;
                    param_nreg[p] = -1;
                    param_ebs[p] = malloc(sizeof(IRValue));
                    if (!param_ebs[p]) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
                    param_ebs[p][0] = new_value(&irfn);
                    emit_inst_w(&irfn, IR_PARAM, param_ebs[p][0], -1, -1,
                                next_pidx++, 8, 1, ploc);
                    irfn.insts.data[irfn.insts.len - 1].force_stack = 1;
                    irfn.insts.data[irfn.insts.len - 1].alloca_bytes = total;
                    {
                        int al = type_stack_align(pty);
                        if (al) irfn.insts.data[irfn.insts.len - 1].align16 = al;
                    }
                } else {
                int need_gp = 0, need_fp = 0;
                for (int k = 0; k < nreg; k++) {
                    if (cls[k] == SYSV_CLS_SSE) need_fp++;
                    else need_gp++;
                }
                /* SysV: an aggregate is passed entirely in registers or
                 * entirely on the stack.  Register-class that do not fit
                 * go on the stack as eightbytes. */
                int hfa = abi_is_hfa(pty);
                int fits = (used_gp + need_gp <= abi_gp_nregs()
                            && used_xmm + need_fp <= 8);
                if (fits) {
                    used_gp += need_gp;
                    used_xmm += need_fp;
                }
                param_nreg[p] = nreg;
                param_ebs[p] = malloc((size_t)nreg * sizeof(IRValue));
                if (!param_ebs[p]) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
                for (int k = 0; k < nreg; k++) {
                    param_ebs[p][k] = new_value(&irfn);
                    int is_sse = (cls[k] == SYSV_CLS_SSE);
                    int pw = 8;
                    if (is_sse && nreg == 1) {
                        /* An HFA eightbyte carries one element, not the
                         * whole aggregate: sizing it from type_size()
                         * charged aligned(16) padding to the parameter, and
                         * the backend then moved the incoming V register
                         * into a GP one 32 bits at a time. */
                        int sz = type_size(pty);
                        if (hfa) {
                            int ew = abi_hfa_elem_size(pty);
                            if (ew > 0) sz = ew;
                        }
                        if (sz == 8 || sz == 16 || sz == 32 || sz == 64)
                            pw = sz;
                    }
                    emit_inst_w(&irfn, IR_PARAM, param_ebs[p][k], -1, -1,
                                next_pidx++, pw, 1, ploc);
                    if (!fits)
                        irfn.insts.data[irfn.insts.len - 1].force_stack = 1;
                    if (!fits && k == 0) {
                        int al = type_stack_align(pty);
                        if (al) irfn.insts.data[irfn.insts.len - 1].align16 = al;
                    }
                    if (is_sse && !hfa)
                        set_value_float(&irfn, param_ebs[p][k],
                                        (pw == 16 || pw == 32 || pw == 64) ? 4 : 1);
                    if (hfa && fits)
                        irfn.insts.data[irfn.insts.len - 1].is_float = 1;
                }
                }
            } else {
                param_nreg[p] = -1;
                int pw = (pty.kind == TY_PTR) ? 8 : (pty.width ? pty.width : 4);
                int pu = (pty.kind == TY_PTR) ? 1 : pty.is_unsigned;
                param_ebs[p] = malloc(sizeof(IRValue));
                if (!param_ebs[p]) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
                param_ebs[p][0] = new_value(&irfn);
                emit_inst_w(&irfn, IR_PARAM, param_ebs[p][0], -1, -1,
                            next_pidx++, pw, pu, ploc);
                if (pty.kind == TY_FLOAT) {
                    set_value_float(&irfn, param_ebs[p][0], pty.is_decimal ? 2 : 1);
                    /* long double is stack-only (codegen keys off value_is_ld). */
                    if (pty.width != 16 && used_xmm < 8) used_xmm++;
                    else if (pty.width != 16 && !abi_sret_uses_gp())
                        irfn.insts.data[irfn.insts.len - 1].force_stack = 1;
                } else {
                    if (used_gp < abi_gp_nregs()) used_gp++;
                }
            }
        }
        for (size_t p = 0; p < fd->params.len; p++) {
            Type pty = fd->params.data[p].type;
            int pw = pty.kind == TY_PTR ? 8 : (pty.width ? pty.width : 4);
            int pu = pty.kind == TY_PTR ? 1 : pty.is_unsigned;
            const char *pname = fd->params.data[p].name;
            SourceLoc ploc = fd->params.data[p].loc;
            int pinned = is_pinned_in_body(fd, pname, pty);
            if (g_ir_pin_locals)
                pinned = 1;
            /* Float params are always pinned: their SSA value lives in the XMM
             * register file (separate from the GP file the scalar regalloc
             * targets), so mem2reg must not promote them.  long double params
             * are pinned for the same reason: they arrive in a 16-byte stack
             * slot and move through x87 st0, and a promoted copy would be
             * renamed into the GP file and reloaded as an integer. */
            if (pty.kind == TY_FLOAT)
                pinned = 1;
            if (pty.is_volatile)
                pinned = 1;
            /* Struct formals are always pinned (live in a stack slot). */
            if (pty.kind == TY_STRUCT || pty.is_vector || type_is_pair16(pty))
                pinned = 1;

            IRValue slot;
            if (type_is_empty_struct(pty)) {
                int total = 1;
                slot = emit_alloca(&irfn, total, 8, 1, ploc);
                irsymtable_push(&st, pname, slot, pinned, pty);
                continue;
            }
            if (pty.kind == TY_STRUCT || pty.is_vector || type_is_pair16(pty)) {
                int total = type_size(pty);
                /* Pin the slot (alloca_bytes > 0) even for size-0 types
                 * (empty unions): codegen's IR_ADDR requires a pinned alloca. */
                if (total < 1) total = 1;
                slot = emit_alloca_ty(&irfn, pty, ploc);
                IRValue addr = emit_bin_w(&irfn, IR_ADDR, slot, -1, 8, 1, ploc);
                if (param_nreg[p] > 0) {
                    int n = param_nreg[p];
                    SysVRegClass *cls = malloc((size_t)n * sizeof(SysVRegClass));
                    if (!cls) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
                    if (type_is_pair16(pty)) {
                        cls[0] = SYSV_CLS_INTEGER;
                        cls[1] = SYSV_CLS_INTEGER;
                    } else {
                        SysVRegClass small[2];
                        int nr = sysv_classify_agg(pty, small);
                        if (nr > 0) {
                            for (int k = 0; k < n; k++)
                                cls[k] = (k < nr) ? small[k] : SYSV_CLS_INTEGER;
                        } else {
                            for (int k = 0; k < n; k++)
                                cls[k] = SYSV_CLS_INTEGER;
                        }
                    }
                    store_agg_regs(&irfn, addr, total, n, cls,
                                   param_ebs[p], ploc);
                    free(cls);
                } else {
                    emit_struct_copy(&irfn, addr, param_ebs[p][0], total, ploc);
                }
            } else if (pinned) {
                int total = type_size(pty);
                slot = emit_alloca(&irfn, total, pw, pu, ploc);
                IRValue addr = emit_bin_w(&irfn, IR_ADDR, slot, -1, 8, 1, ploc);
                emit_inst_w(&irfn, IR_STORE_PTR, -1, addr, param_ebs[p][0], 0,
                            pw, pu, ploc);
            } else {
                slot = new_value(&irfn);
                emit_inst_w(&irfn, IR_ALLOCA, slot, -1, -1, 0, pw, pu, ploc);
                emit_inst_w(&irfn, IR_STORE, -1, slot, param_ebs[p][0], 0, pw,
                            pu, ploc);
            }
            irsymtable_push(&st, pname, slot, pinned, pty);
            {
                /* Debug: first IR param index for this formal (sret shifts). */
                int pidx = 0;
                if (irfn.sret_value >= 0) pidx = 1;
                for (size_t q = 0; q < p; q++) {
                    if (param_nreg[q] > 0) pidx += param_nreg[q];
                    else pidx += 1;
                }
                ir_add_dbg_var(&irfn, pname, ploc, IR_DBG_PARAM, pty, slot, pidx);
            }
        }
        if (param_ebs) {
            for (size_t p = 0; p < nparams; p++) free(param_ebs[p]);
            free(param_ebs);
        }
        free(param_nreg);

        /* C99 §6.7.5.3p21: evaluate parameter VLA dimensions on function entry. */
        for (size_t p = 0; p < fd->params.len; p++) {
            Type pty = fd->params.data[p].type;
            if (pty.vla_dim) {
                lower_expr(&irfn, &st, pty.vla_dim);
            }
        }

        /* Pre-pass: assign label ids to every label in this function so
         * forward gotos resolve. */
        LabelMap lm;
        labelmap_init(&lm);
        g_ir_label_map = &lm;
        for (size_t j = 0; j < fd->body.len; j++)
            assign_label_ids(&irfn, &lm, &fd->body.data[j]);

        int has_vla = fd_has_vla(fd);
        g_ir_label_vla_id = NULL;
        g_ir_label_vla_id_count = 0;
        g_ir_vla_sp_slots = NULL;
        g_ir_vla_count = 0;
        g_ir_entry_sp_slot = -1;
        g_ir_vla_seq = 0;
        g_ir_computed_goto_vla_id = -2;
        if (has_vla) {
            if (lm.len > 0) {
                g_ir_label_vla_id = xmalloc(lm.len * sizeof(int));
                g_ir_label_vla_id_count = (int)lm.len;
                for (size_t i = 0; i < lm.len; i++) g_ir_label_vla_id[i] = -1;
            }
            g_vla_walk_sp = 0;
            g_vla_walk_serial = 0;
            for (size_t j = 0; j < fd->body.len; j++)
                snapshot_vla_stmt(&lm, &fd->body.data[j]);
            g_ir_computed_goto_vla_id = -2;
            g_vla_scan_addrs_only = 1;
            for (size_t j = 0; j < fd->body.len; j++)
                snapshot_vla_stmt(&lm, &fd->body.data[j]);
            g_vla_scan_addrs_only = 0;
            g_ir_vla_count = g_vla_walk_serial;
            g_ir_entry_sp_slot = emit_alloca(&irfn, 8, 8, 1, fd->loc);
            IRValue init_sp = new_value(&irfn);
            emit_inst_w(&irfn, IR_STACK_SAVE, init_sp, -1, -1, 0, 8, 1, fd->loc);
            IRValue entry_addr = emit_bin_w(&irfn, IR_ADDR, g_ir_entry_sp_slot, -1, 8, 1, fd->loc);
            emit_inst_w(&irfn, IR_STORE_PTR, -1, entry_addr, init_sp, 0, 8, 1, fd->loc);
            if (g_ir_vla_count > 0) {
                g_ir_vla_sp_slots = xmalloc((size_t)g_ir_vla_count * sizeof(int));
                for (int i = 0; i < g_ir_vla_count; i++)
                    g_ir_vla_sp_slots[i] = emit_alloca(&irfn, 8, 8, 1, fd->loc);
            }
            irfn.has_dyn_alloca = 1;
        }

        if (g_sanitize_address && strcmp(fd->name, "main") == 0) {
            IRInst init_inst;
            memset(&init_inst, 0, sizeof(init_inst));
            init_inst.op = IR_CALL;
            init_inst.dst = -1;
            init_inst.a = -1;
            init_inst.b = -1;
            init_inst.c = -1;
            init_inst.d = -1;
            init_inst.imm = 0;
            init_inst.loc = fd->loc;
            init_inst.call_name = xstrdup("__asan_init");
            init_inst.call_callee = -1;
            init_inst.call_nargs = 0;
            ir_inst_array_push(&irfn.insts, init_inst);
        }

        if (g_instrument_functions && !fd->no_instrument) {
            emit_profile_call(&irfn, "__cyg_profile_func_enter", fd, fd->loc);
        }

        for (size_t j = 0; j < fd->body.len; j++) {
            lower_stmt(&irfn, &st, &fd->body.data[j], fd);
        }

        if (g_ir_vla_sp_slots) {
            free(g_ir_vla_sp_slots);
            g_ir_vla_sp_slots = NULL;
        }
        if (g_ir_label_vla_id) {
            free(g_ir_label_vla_id);
            g_ir_label_vla_id = NULL;
        }
        g_ir_label_vla_id_count = 0;
        g_ir_vla_count = 0;
        g_ir_entry_sp_slot = -1;
        g_ir_vla_seq = 0;
        g_ir_computed_goto_vla_id = -2;

        /* If function doesn't end with an IR_RETURN, append a default return. */
        int needs_ret = 1;
        if (irfn.insts.len > 0) {
            IROpcode last_op = irfn.insts.data[irfn.insts.len - 1].op;
            if (last_op == IR_RETURN || last_op == IR_BR)
                needs_ret = 0;
        }
        if (needs_ret) {
            if (g_instrument_functions && !fd->no_instrument) {
                emit_profile_call(&irfn, "__cyg_profile_func_exit", fd, fd->loc);
            }
            if (fd->ret_type.kind == TY_VOID) {
                emit_inst_w(&irfn, IR_RETURN, -1, -1, -1, 0, 0, 0, fd->loc);
            } else if (irfn.ret_is_struct && irfn.ret_reg_n > 0) {
                IRValue z0 = new_value(&irfn);
                emit_inst_w(&irfn, IR_CONST, z0, -1, -1, 0, 8, 1, fd->loc);
                IRValue z1 = -1, z2 = -1, z3 = -1;
                if (irfn.ret_reg_n > 1) {
                    z1 = new_value(&irfn);
                    emit_inst_w(&irfn, IR_CONST, z1, -1, -1, 0, 8, 1, fd->loc);
                }
                if (irfn.ret_reg_n > 2) {
                    z2 = new_value(&irfn);
                    emit_inst_w(&irfn, IR_CONST, z2, -1, -1, 0, 8, 1, fd->loc);
                }
                if (irfn.ret_reg_n > 3) {
                    z3 = new_value(&irfn);
                    emit_inst_w(&irfn, IR_CONST, z3, -1, -1, 0, 8, 1, fd->loc);
                }
                emit_inst_w(&irfn, IR_RETURN, -1, z0, z1, 0, 8, 1, fd->loc);
                IRInst *ri = &irfn.insts.data[irfn.insts.len - 1];
                if (z2 >= 0 || z3 >= 0) {
                    ri->c = z2;
                    ri->d = z3;
                }
                /* An HFA must leave through v0-v3; the generic pair path
                 * would only move a/b through x0/x1. */
                if (abi_is_hfa(fd->ret_type))
                    ri->align16 = A64_MARK_HFA;
            } else {
                IRValue zero = new_value(&irfn);
                int rw = fd->ret_type.width ? fd->ret_type.width : 4;
                int ru = fd->ret_type.is_unsigned;
                emit_inst_w(&irfn, IR_CONST, zero, -1, -1, 0, rw, ru, fd->loc);
                emit_inst_w(&irfn, IR_RETURN, -1, zero, -1, 0, rw, ru, fd->loc);
            }
        }

        g_ir_label_map = NULL;
        labelmap_free(&lm);
        g_ir_cur_fd = NULL;

        irsymtable_free(&st);
        ir_func_array_push(&ir->functions, irfn);
    }
    irsymtable_free(&g_ir_globals_st);
    if (fakecc_had_error()) return FAKECC_ERR;
    return FAKECC_OK;
}
