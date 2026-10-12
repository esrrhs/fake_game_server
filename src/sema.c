#include "fakecc/sema.h"
#include "fakecc/pkg.h"
#include "fakecc/common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* Set by sema_check; provides the module StructRegistry for member lookups. */
static const StructRegistry *g_sema_structs = NULL;
static PkgContext *g_sema_pkg = NULL;
static const TranslationUnit *g_sema_tu = NULL;

const StructRegistry *get_sema_structs(void) {
    if (g_sema_structs) return g_sema_structs;
    return g_sema_tu ? &g_sema_tu->structs : NULL;
}

const TranslationUnit *get_sema_tu(void) {
    return g_sema_tu;
}

/* Return type of the function currently being type-checked.  Set per-function
 * before check_stmt_list so ST_RETURN can enforce the void/non-void rules. */
static Type g_sema_ret_type;

/* Error counter for sema phase.  Incremented by error emission
 * helper; checked at the end of sema_check to set process exit code. */
static int g_sema_error_count = 0;

int sema_has_errors(void) {
    if (fakecc_had_error()) return FAKECC_ERR;
    return g_sema_error_count > 0;
}

int sema_error_count(void) {
    if (fakecc_had_error()) return FAKECC_ERR;
    return g_sema_error_count;
}

static void sema_report_error(const SourceLoc *loc, const char *fmt, ...) {
    if (fakecc_had_error()) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s:%d:%d: error: ", loc->file, loc->line, loc->col);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    g_sema_error_count++;
}

typedef struct {
    const char *name;
    int arity;
    int is_variadic;
    int is_unprototyped;
    int is_external;   /* 1 = declared `extern`, no definition in this TU */
    Type ret_type;
    Type *param_types;
    int param_cap; // capacity of param_types array
    SourceLoc loc;
} FunSig;

typedef struct {
    FunSig *data;
    size_t len;
    size_t cap;
} FunTable;

/* File-scope so huge functions never materialize a FunTable* local.
 * Stage 0 miscompiles those from large frames (passes a Type* instead). */
static FunTable g_sema_ft;

static void ftab_init(FunTable *t) { t->data = NULL; t->len = 0; t->cap = 0; }
static void ftab_free(FunTable *t) {
    if (t->data) {
        for (size_t i = 0; i < t->len; i++) {
            if (t->data[i].param_types) {
                for (int k = 0; k < t->data[i].arity; k++)
                    type_free(&t->data[i].param_types[k]);
                free(t->data[i].param_types);
            }
        }
        free(t->data);
        t->data = NULL;
    }
    t->len = 0;
    t->cap = 0;
}

static void ftab_push(FunTable *t, const FunctionDecl *fn) {
    if (t->len >= t->cap) {
        t->cap = t->cap ? t->cap * 2 : 8;
        t->data = realloc(t->data, t->cap * sizeof(FunSig));
        if (!t->data) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
    }
    FunSig *s = &t->data[t->len++];
    s->name = fn->name;
    s->arity = (int)fn->params.len;
    s->is_variadic = fn->is_variadic;
    s->is_unprototyped = fn->is_unprototyped;
    s->is_external = fn->is_extern;
    s->ret_type = fn->ret_type;
    s->loc = fn->loc;
    s->param_types = NULL;
    s->param_cap = 0;
    if (s->arity > 0) {
        s->param_types = malloc(s->arity * sizeof(Type));
        if (!s->param_types) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        s->param_cap = s->arity;
        for (int i = 0; i < s->arity; i++)
            s->param_types[i] = type_clone(fn->params.data[i].type);
    }
}

static const FunSig *ftab_find(const FunTable *t, const char *name) {
    if (!t) return NULL;
    if (t->len > 4096) {
        fprintf(stderr, "fakecc: internal error: corrupt function table (len=%zu)\n",
                t->len);
        exit(1);
    }
    for (size_t i = 0; i < t->len; i++) {
        const char *n = t->data[i].name;
        if (!n) continue;
        if (strcmp(n, name) == 0) return &t->data[i];
    }
    return NULL;
}

static int tu_imports(const TranslationUnit *tu, const char *name) {
    for (size_t i = 0; i < tu->imports.len; i++)
        if (strcmp(tu->imports.data[i].name, name) == 0) return 1;
    return 0;
}

/* Push a package export into the FunTable as an external signature so call
 * checking sees it like an `extern` decl. */
static void ftab_push_export(FunTable *t, const PkgFuncExport *ex) {
    if (ftab_find(t, ex->name)) return;
    if (t->len >= t->cap) {
        t->cap = t->cap ? t->cap * 2 : 8;
        t->data = realloc(t->data, t->cap * sizeof(FunSig));
        if (!t->data) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
    }
    FunSig *s = &t->data[t->len++];
    s->name = ex->name;
    s->arity = ex->arity;
    s->is_variadic = ex->is_variadic;
    s->is_unprototyped = 0;
    s->is_external = 1;
    s->ret_type = ex->ret_type;
    s->loc = ex->loc;
    s->param_types = NULL;
    s->param_cap = 0;
    if (s->arity > 0) {
        s->param_types = malloc(s->arity * sizeof(Type));
        if (!s->param_types) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        s->param_cap = s->arity;
        for (int i = 0; i < s->arity; i++)
            s->param_types[i] = type_clone(ex->param_types[i]);
    }
}

/* Ensure an extern FunctionDecl exists so ir.c's g_ir_tu scan treats calls
 * as direct. Must run before the per-function check loop (reallocating
 * tu->functions mid-check invalidates FunctionDecl pointers). */
static void tu_ensure_extern_func(TranslationUnit *tu, const PkgFuncExport *ex) {
    for (size_t i = 0; i < tu->functions.len; i++)
        if (strcmp(tu->functions.data[i].name, ex->name) == 0)
            return;
    if (tu->functions.len >= tu->functions.cap) {
        size_t nc = tu->functions.cap ? tu->functions.cap * 2 : 4;
        tu->functions.data = realloc(tu->functions.data,
                                     nc * sizeof(FunctionDecl));
        if (!tu->functions.data) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        tu->functions.cap = nc;
    }
    FunctionDecl *fn = &tu->functions.data[tu->functions.len++];
    memset(fn, 0, sizeof(*fn));
    fn->name = xstrdup(ex->name);
    fn->ret_type = type_clone(ex->ret_type);
    param_array_init(&fn->params);
    for (int i = 0; i < ex->arity; i++)
        param_array_push(&fn->params, "", type_clone(ex->param_types[i]),
                         ex->loc);
    fn->loc = ex->loc;
    fn->is_variadic = ex->is_variadic;
    fn->is_unprototyped = 0;
    fn->is_extern = 1;
    fn->is_static = 0;
}

static void import_pkg_funcs(TranslationUnit *tu, FunTable *ft, Package *pkg) {
    if (!pkg) return;
    for (size_t i = 0; i < pkg->nfuncs; i++) {
        ftab_push_export(ft, &pkg->funcs[i]);
        tu_ensure_extern_func(tu, &pkg->funcs[i]);
    }
}

static void tu_ensure_extern_global(TranslationUnit *tu, const PkgGlobalExport *ex) {
    for (size_t i = 0; i < tu->globals.len; i++) {
        Stmt *s = &tu->globals.data[i];
        if (s->kind == ST_DECL && s->u.decl.name
            && strcmp(s->u.decl.name, ex->name) == 0)
            return;
    }
    Stmt s;
    memset(&s, 0, sizeof(s));
    s.kind = ST_DECL;
    s.loc = ex->loc;
    s.u.decl.name = xstrdup(ex->name);
    s.u.decl.type = type_clone(ex->type);
    s.u.decl.init = NULL;
    s.u.decl.storage_class = 2; /* extern */
    stmt_array_push(&tu->globals, s);
}

static void import_pkg_globals(TranslationUnit *tu, Package *pkg) {
    if (!pkg) return;
    for (size_t i = 0; i < pkg->nglobals; i++)
        tu_ensure_extern_global(tu, &pkg->globals[i]);
}

/* Resolve name in package `pkg_name`'s export table. Returns 1 and fills
 * out_func / out_global (one of them non-NULL) on hit. */
static int pkg_resolve_sym(const char *pkg_name, const char *sym,
                           const PkgFuncExport **out_func,
                           const PkgGlobalExport **out_global) {
    *out_func = NULL;
    *out_global = NULL;
    if (!g_sema_pkg) return 0;
    Package *pkg = pkg_find(g_sema_pkg, pkg_name);
    if (!pkg) return 0;
    const PkgFuncExport *f = pkg_find_func(pkg, sym);
    if (f) { *out_func = f; return 1; }
    const PkgGlobalExport *g = pkg_find_global(pkg, sym);
    if (g) { *out_global = g; return 1; }
    return 0;
}

/* A value has va_list type iff it is the builtin __va_list_tag struct. */
static int is_va_list_type(const Type *t) {
    return t->kind == TY_STRUCT && t->tag && strcmp(t->tag, "__va_list_tag") == 0;
}

/* ------------------------------------------------------------------ */
/* Label set — tracks goto targets within one function for validation. */
/* ------------------------------------------------------------------ */

typedef struct {
    char **names;      /* xstrdup'd label names */
    size_t len;
    size_t cap;
} LabelSet;

static void labelset_init(LabelSet *ls) { ls->names = NULL; ls->len = 0; ls->cap = 0; }

static void labelset_free(LabelSet *ls) {
    for (size_t i = 0; i < ls->len; i++) free(ls->names[i]);
    free(ls->names);
    ls->names = NULL; ls->len = 0; ls->cap = 0;
}

static void labelset_add(LabelSet *ls, const char *name) {
    if (ls->len >= ls->cap) {
        ls->cap = ls->cap ? ls->cap * 2 : 8;
        ls->names = realloc(ls->names, ls->cap * sizeof(char *));
        if (!ls->names) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
    }
    ls->names[ls->len++] = xstrdup(name);
}

static int labelset_has(const LabelSet *ls, const char *name) {
    for (size_t i = 0; i < ls->len; i++)
        if (strcmp(ls->names[i], name) == 0) return 1;
    return 0;
}

static LabelSet *g_sema_labels = NULL;

static void collect_labels(LabelSet *ls, const Stmt *s);

static void collect_labels_expr(LabelSet *ls, const Expr *e) {
    if (!e) return;
    switch (e->kind) {
    case EX_STMT_EXPR:
        if (e->u.stmt_expr.stmts) {
            for (size_t i = 0; i < e->u.stmt_expr.stmts->len; i++)
                collect_labels(ls, &e->u.stmt_expr.stmts->data[i]);
        }
        break;
    case EX_UNARY:
        collect_labels_expr(ls, e->u.un.operand);
        break;
    case EX_INC_DEC:
        collect_labels_expr(ls, e->u.incdec.operand);
        break;
    case EX_BINOP:
        collect_labels_expr(ls, e->u.bin.l);
        collect_labels_expr(ls, e->u.bin.r);
        break;
    case EX_TERNARY:
        collect_labels_expr(ls, e->u.tern.cond);
        collect_labels_expr(ls, e->u.tern.then);
        collect_labels_expr(ls, e->u.tern.else_);
        break;
    case EX_ASSIGN:
        collect_labels_expr(ls, e->u.assign.lvalue);
        collect_labels_expr(ls, e->u.assign.rvalue);
        break;
    case EX_COMPOUND_ASSIGN:
        collect_labels_expr(ls, e->u.comp.lvalue);
        collect_labels_expr(ls, e->u.comp.rvalue);
        break;
    case EX_COMMA:
        collect_labels_expr(ls, e->u.comma.lhs);
        collect_labels_expr(ls, e->u.comma.rhs);
        break;
    case EX_CAST:
        collect_labels_expr(ls, e->u.cast.operand);
        break;
    case EX_CALL:
        collect_labels_expr(ls, e->u.call.callee);
        for (size_t i = 0; i < e->u.call.args.len; i++)
            collect_labels_expr(ls, e->u.call.args.data[i]);
        break;
    case EX_MEMBER:
        collect_labels_expr(ls, e->u.member.obj);
        break;
    case EX_INDEX:
        collect_labels_expr(ls, e->u.idx.array);
        collect_labels_expr(ls, e->u.idx.index);
        break;
    case EX_DEREF:
        collect_labels_expr(ls, e->u.deref.operand);
        break;
    case EX_ADDR:
        collect_labels_expr(ls, e->u.addr.operand);
        break;
    case EX_SIZEOF_EXPR:
        collect_labels_expr(ls, e->u.sizeof_e.operand);
        break;
    case EX_ALIGNOF_EXPR:
        collect_labels_expr(ls, e->u.alignof_e.operand);
        break;
    case EX_COMPOUND_LITERAL:
        collect_labels_expr(ls, e->u.compound.init);
        break;
    case EX_INIT_LIST:
        for (int i = 0; i < e->u.init_list.num_elements; i++)
            collect_labels_expr(ls, e->u.init_list.elements[i]);
        break;
    default:
        break;
    }
}

/* Recursively collect every ST_LABEL name in a statement (forward goto must
 * be able to target labels that appear later in the function). */
static void collect_labels(LabelSet *ls, const Stmt *s) {
    if (fakecc_had_error()) return;
    if (!s) return;
    switch (s->kind) {
    case ST_LABEL:
        if (labelset_has(ls, s->u.label_s.name)) {
            die_at(s->loc.file, s->loc.line, s->loc.col,
                   "redefinition of label '%s'", s->u.label_s.name);
            return;
        }
        labelset_add(ls, s->u.label_s.name);
        collect_labels(ls, s->u.label_s.stmt);
        break;
    case ST_EXPR:
        collect_labels_expr(ls, s->u.expr);
        break;
    case ST_DECL:
        if (s->u.decl.init) collect_labels_expr(ls, s->u.decl.init);
        break;
    case ST_RETURN:
        if (s->u.value) collect_labels_expr(ls, s->u.value);
        break;
    case ST_GOTO:
        if (s->u.goto_s.target_expr) collect_labels_expr(ls, s->u.goto_s.target_expr);
        break;
    case ST_IF:
        collect_labels_expr(ls, s->u.if_s.cond);
        collect_labels(ls, s->u.if_s.then_s);
        if (s->u.if_s.else_s) collect_labels(ls, s->u.if_s.else_s);
        break;
    case ST_WHILE:
        collect_labels_expr(ls, s->u.while_s.cond);
        collect_labels(ls, s->u.while_s.body);
        break;
    case ST_DO_WHILE:
        collect_labels(ls, s->u.do_s.body);
        collect_labels_expr(ls, s->u.do_s.cond);
        break;
    case ST_FOR:
        if (s->u.for_s.init) collect_labels(ls, s->u.for_s.init);
        if (s->u.for_s.cond) collect_labels_expr(ls, s->u.for_s.cond);
        if (s->u.for_s.step) collect_labels_expr(ls, s->u.for_s.step);
        collect_labels(ls, s->u.for_s.body);
        break;
    case ST_SWITCH:
        collect_labels_expr(ls, s->u.switch_s.cond);
        if (s->u.switch_s.body) collect_labels(ls, s->u.switch_s.body);
        for (int i = 0; i < s->u.switch_s.num_cases; i++) {
            const SwitchCase *arm = &s->u.switch_s.cases[i];
            for (size_t j = 0; j < arm->stmts.len; j++)
                collect_labels(ls, &arm->stmts.data[j]);
        }
        break;
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.len; i++)
            collect_labels(ls, &s->u.block.data[i]);
        break;
    default:
        break;
    }
}

typedef struct {
    char *name;
    Type type;
    SourceLoc loc;
    int align;
    int is_register;
} Sym;

typedef struct {
    Sym *data;
    size_t len;
    size_t cap;
} SymTable;

static void check_stmt(Stmt *s, size_t scope_mark, int *has_return);
static void check_stmt_list(StmtArray *body, int *has_return);

static void symtable_init(SymTable *st) { st->data = NULL; st->len = 0; st->cap = 0; }

static void symtable_free(SymTable *st) {
    for (size_t i = 0; i < st->len; i++) {
        free(st->data[i].name);
        /* Do NOT free the type here. The symtable's types are clones made
         * via type_clone(), but their sub-type pointers (pointee, func_ret,
         * func_params) may be shared with the AST expressions that reference
         * those symbols. Freeing here would double-free when the AST is
         * later destroyed by tu_free(). The types are owned by the AST
         * expressions that reference them; the symtable only borrows them
         * during sema. */
        /* type_free(&st->data[i].type); */
    }
    free(st->data);
    st->data = NULL; st->len = 0; st->cap = 0;
}

static void symtable_push(SymTable *st, const char *name, Type type, SourceLoc loc, int align, int is_register) {
    if (st->len >= st->cap) {
        st->cap = st->cap ? st->cap * 2 : 8;
        st->data = realloc(st->data, st->cap * sizeof(Sym));
        if (!st->data) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
    }
    st->data[st->len].name = name ? xstrdup(name) : NULL;
    st->data[st->len].type = type_clone(type);   /* own our copy */
    st->data[st->len].loc = loc;
    st->data[st->len].align = align;
    st->data[st->len].is_register = is_register;
    st->len++;
}

static size_t symtable_enter_scope(SymTable *st) { return st->len; }

static void symtable_leave_scope(SymTable *st, size_t mark) {
    while (st->len > mark) {
        st->len--;
        free(st->data[st->len].name);
        /* Do NOT free the type here. The symtable's types are clones made
         * via type_clone(), but their sub-type pointers (pointee, func_ret,
         * func_params) may be shared with the AST expressions that reference
         * those symbols. Freeing here would double-free when the AST is
         * later destroyed by tu_free(). The types are owned by the AST
         * expressions that reference them; the symtable only borrows them
         * during sema. */
        /* type_free(&st->data[st->len].type); */
    }
}

static const Sym *symtable_find(const SymTable *st, const char *name) {
    for (size_t i = st->len; i > 0; i--) {
        const Sym *e = &st->data[i - 1];
        if (e->name && strcmp(e->name, name) == 0) return e;
    }
    return NULL;
}

static int symtable_has_since(const SymTable *st, const char *name, size_t mark) {
    for (size_t i = mark; i < st->len; i++)
        if (st->data[i].name && strcmp(st->data[i].name, name) == 0) return 1;
    return 0;
}

static int type_is_same(const Type *a, const Type *b) {
    if (!a || !b) return a == b;
    while (a && b) {
        if (a->kind != b->kind) return 0;
        if (a->kind == TY_STRUCT) {
            if (a->tag && b->tag) {
                return strcmp(a->tag, b->tag) == 0;
            }
            return a->tag == b->tag;
        }
        if (a->width != b->width) return 0;
        if (a->is_unsigned != b->is_unsigned) return 0;
        if (a->is_vector != b->is_vector) return 0;
        if (a->kind == TY_PTR) {
            a = a->pointee;
            b = b->pointee;
            continue;
        }
        return 1;
    }
    return a == b;
}

static int types_compatible_globals(const Type *a, const Type *b) {
    if (!a || !b) return a == b;
    if (a->kind == TY_ARRAY && b->kind == TY_ARRAY) {
        if (!type_is_same(a->elem_type, b->elem_type)) return 0;
        if (a->length > 0 && b->length > 0 && a->length != b->length) return 0;
        return 1;
    }
    return type_is_same(a, b);
}

static int type_is_scalar(Type t) {
    if (t.kind == TY_INT || t.kind == TY_PTR || t.kind == TY_FLOAT) return 1;
    if (t.kind == TY_STRUCT && t.tag && strncmp(t.tag, "__complex_", 10) == 0) return 1;
    return 0;
}

/* Conversion rank proxy: float types outrank integer types (C §6.3.1.8).
 *   double (108) > float (104) > pointer (50) > long (8) > int (4) > ... */
static int type_rank(Type t) {
    if (t.kind == TY_FLOAT) return 100 + t.width;  /* 104 / 108 */
    if (t.kind == TY_PTR) return 50;
    return t.width;
}

/* Integer promotion (C §6.3.1.6): types narrower than int → int.
 * Float types are NOT integer-promoted — they keep their type.
 * Returns an owned copy: callers release their input right after, so handing
 * back the argument itself would leave the result pointing at freed memory
 * (the same rule usual_arith_conv() follows). */
static Type integer_promote(Type t) {
    if (t.kind == TY_FLOAT) return type_clone(t);
    /* C §6.3.1.1: a bit-field promotes to signed int if every value of the field fits in int. */
    if (t.bitfield_width > 0) {
        if (!t.is_unsigned) {
            if (t.bitfield_width <= 32)
                return type_make_int(4, 0);
            Type res = type_make_int(8, 0);
            res.bitfield_width = t.bitfield_width;
            return res;
        } else {
            if (t.bitfield_width < 32)
                return type_make_int(4, 0);
            if (t.bitfield_width == 32)
                return type_make_int(4, 1);
            Type res = type_make_int(8, 1);
            res.bitfield_width = t.bitfield_width;
            return res;
        }
    }
    if (t.width < 4) return type_make_int(4, 0);
    return type_clone(t);
}

/* Usual arithmetic conversions (C §6.3.1.8).
 *   long double > double > float > (integer ranks) */
static Type usual_arith_conv(Type a, Type b) {
    a = integer_promote(a);
    b = integer_promote(b);
    /* GCC treats a bit-field that does not fit in int as a distinct integer
     * type of that many bits (similar to C23 _BitInt(N)).  Usual arithmetic
     * conversions then pick the wider precision: `u33 * u33` wraps at 33 bits,
     * but `u33 * (unsigned long long)` is a full 64-bit multiply.  ISO C would
     * promote the field to the declared type and never wrap early; we follow
     * GCC because the c-torture suite (bitfld-3, bitfld-5) tests this. */
    int prec_a = a.bitfield_width > 0 ? a.bitfield_width : a.width * 8;
    int prec_b = b.bitfield_width > 0 ? b.bitfield_width : b.width * 8;
    int prec = prec_a > prec_b ? prec_a : prec_b;
    int a_cplx = (a.kind == TY_STRUCT && a.tag && strncmp(a.tag, "__complex_", 10) == 0);
    int b_cplx = (b.kind == TY_STRUCT && b.tag && strncmp(b.tag, "__complex_", 10) == 0);
    if (a_cplx || b_cplx) {
        if (a_cplx && b_cplx) {
            return type_clone(type_size(a) >= type_size(b) ? a : b);
        }
        Type cty = a_cplx ? a : b;
        Type non_cplx = a_cplx ? b : a;
        if (non_cplx.kind == TY_FLOAT && non_cplx.width > (type_size(cty) / 2)) {
            char tag[64];
            snprintf(tag, sizeof(tag), "__complex_%s", non_cplx.width == 16 ? "ldouble" : (non_cplx.width == 8 ? "double" : "float"));
            return type_make_struct(tag, non_cplx.width * 2);
        }
        return type_clone(cty);
    }
    Type res;
    /* Decimal float is a separate type domain (GNU): it does not mix with
     * binary float.  Among decimals the wider format wins; an integer is
     * converted to the decimal operand's type. */
    if (a.is_decimal || b.is_decimal) {
        if (a.is_decimal && b.is_decimal)
            res = a.width >= b.width ? a : b;
        else if (a.is_decimal && b.kind != TY_FLOAT)
            res = a;
        else if (b.is_decimal && a.kind != TY_FLOAT)
            res = b;
        else {
            /* Mixed decimal + binary float: keep the decimal type so later
             * sema on the operator can diagnose. */
            res = a.is_decimal ? a : b;
        }
    } else if (a.kind == TY_FLOAT && b.kind == TY_FLOAT)
        res = type_rank(a) >= type_rank(b) ? a : b;
    else if (a.kind == TY_FLOAT) res = a;
    else if (b.kind == TY_FLOAT) res = b;
    else if (a.width == b.width && a.is_unsigned == b.is_unsigned) res = a;
    else if (a.is_unsigned == b.is_unsigned) res = a.width > b.width ? a : b;
    else {
        Type u = a.is_unsigned ? a : b;
        Type s = a.is_unsigned ? b : a;
        res = (u.width >= s.width) ? u : s;
    }
    if (res.kind == TY_INT) {
        int type_bits = res.width * 8;
        res.bitfield_width = (prec > 0 && prec < type_bits) ? prec : 0;
    } else {
        res.bitfield_width = 0;
    }
    /* Return an owned copy: res is a shallow copy of one of the inputs and
     * shares any heap sub-types (e.g. a vector's elem_type). Callers free their
     * inputs immediately after, so the result must own its own sub-types or it
     * dangles.  For plain int/float scalars type_clone is effectively a no-op. */
    return type_clone(res);
}

/* Set an expr's type (frees previous). Convenience wrapper. */
static void set_type(Expr *e, Type t) { expr_set_type(e, t); }

/* Insert an implicit conversion of a call argument to its declared parameter
 * type ("as if by assignment", C11 6.5.2.2p7).  Without this, an argument is
 * lowered with its own width/signedness, which misbehaves when a value whose
 * representation is wider than the parameter reaches a narrower parameter —
 * e.g. a literal larger than INT_MAX passed to `unsigned int` gets sign
 * extended to 64 bits and participates in a 64-bit divide (see README known
 * defects).  Wrapping the argument in an EX_CAST makes IR lowering emit the
 * proper SEXT/ZEXT/TRUNC.  Only scalar arithmetic types are converted: pointers
 * are already 8 bytes, and structs/unions/arrays travel by reference in
 * fakecc's ABI and must not be reinterpreted here. */
/* Default argument promotions (C11 6.5.2.2p6): float→double, integer
 * promotions.  Applied to variadic extra args and to every argument of an
 * unprototyped call. */
static void apply_default_arg_promotions(Expr **argp) {
    if (!argp || !*argp) return;
    Expr *arg = *argp;
    if (arg->kind == EX_CALL && arg->u.call.callee && arg->u.call.callee->kind == EX_VAR) {
        const char *n = arg->u.call.callee->u.var.name;
        if (n && (strcmp(n, "__builtin_va_arg_pack") == 0
                  || strcmp(n, "__builtin_va_arg_pack_len") == 0))
            return;
    }
    Type t = arg->type;
    Type target;
    int need = 0;
    if (t.kind == TY_FLOAT && t.width == 4 && !t.is_decimal) {
        target = type_make_float(8);
        need = 1;
    } else if (t.kind == TY_INT && (t.width < 4 || t.bitfield_width > 0)) {
        Type p = integer_promote(t);
        if (p.kind != t.kind || p.width != t.width || p.is_unsigned != t.is_unsigned) {
            target = p;
            need = 1;
        }
    }
    if (!need) return;
    Expr *cast = expr_new_cast(target, arg, arg->loc);
    set_type(cast, type_clone(target));
    type_free(&target);
    *argp = cast;
}

/* Non-variadic calls must pass exactly nparams arguments.  Empty-parens
 * `foo()` is unprototyped in the grammar, but FakeCC still treats it as
 * taking zero arguments — extra or missing args are errors. */
static void check_call_arity(SourceLoc loc, const char *name,
                             int nparams, int is_variadic, size_t nargs) {
    if (fakecc_had_error()) return;
    if (is_variadic) {
        if ((int)nargs < nparams) {
            die_at(loc.file, loc.line, loc.col,
                   "function '%s' takes at least %d argument%s but %zu given",
                   name, nparams, nparams == 1 ? "" : "s", nargs);
            return;
        }
        return;
    }
    if ((int)nargs != nparams) {
        die_at(loc.file, loc.line, loc.col,
               "function '%s' takes %d argument%s but %zu given",
               name, nparams, nparams == 1 ? "" : "s", nargs);
        return;
    }
}

static void check_fnptr_arity(SourceLoc loc, int nparams, int is_variadic,
                              size_t nargs) {
    if (fakecc_had_error()) return;
    if (is_variadic) {
        if ((int)nargs < nparams) {
            die_at(loc.file, loc.line, loc.col,
                   "function pointer expects at least %d argument%s but %zu given",
                   nparams, nparams == 1 ? "" : "s", nargs);
            return;
        }
        return;
    }
    if ((int)nargs != nparams) {
        die_at(loc.file, loc.line, loc.col,
               "function pointer expects %d argument%s but %zu given",
               nparams, nparams == 1 ? "" : "s", nargs);
        return;
    }
}

static void coerce_arg_to_param(Expr **argp, const Type *ptype) {
    if (!argp || !*argp || !ptype) return;
    Expr *arg = *argp;
    const Type *at = &arg->type;
    int at_arith = (at->kind == TY_INT || at->kind == TY_FLOAT);
    int pt_arith = (ptype->kind == TY_INT || ptype->kind == TY_FLOAT);
    int at_cplx = (at->kind == TY_STRUCT && at->tag && strncmp(at->tag, "__complex_", 10) == 0);
    int pt_cplx = (ptype->kind == TY_STRUCT && ptype->tag && strncmp(ptype->tag, "__complex_", 10) == 0);
    if ((!at_arith && !at_cplx) || (!pt_arith && !pt_cplx)) return;
    /* No-op when the argument already has the parameter's representation. */
    if (at_cplx && pt_cplx) {
        if (at->tag && ptype->tag && strcmp(at->tag, ptype->tag) == 0)
            return;
    } else if (at->kind == ptype->kind && at->width == ptype->width
        && at->is_unsigned == ptype->is_unsigned && at_cplx == pt_cplx
        && at->is_decimal == ptype->is_decimal) {
        return;
    }
    Type target = type_clone(*ptype);
    Expr *cast = expr_new_cast(target, arg, arg->loc);  /* clones target */
    set_type(cast, target);                             /* takes ownership */
    *argp = cast;
}

/* Normalize a (possibly designated) initializer list (designator validation,
 * array-length inference, expansion to positional with zero-fill). */
static void normalize_init_list(Type *target, Expr *list, SourceLoc loc, int is_nested);
static void overlay_init_list(Type *target, Expr *dst, Expr *src, SourceLoc loc);

static SymTable *g_check_st;

/* Tiny accessors — do not take or return FunTable* from huge functions. */
static void ftab_cur_init(void) { ftab_init(&g_sema_ft); }
static void ftab_cur_free(void) { ftab_free(&g_sema_ft); }
static const FunSig *ftab_lookup(const char *name) {
    return ftab_find(&g_sema_ft, name);
}
/* Copy a FunSig out of the table.  Callers keep the returned value across
 * recursive check_expr_inner() calls, which can push new package exports and
 * realloc the table -- a raw &t->data[i] would dangle by the time it is read
 * back.  The nested param_types array is shared, not cloned: those Type
 * objects live until ftab_free, and the snapshot is only read. */
static int ftab_snapshot(const char *name, FunSig *out) {
    const FunSig *sig = ftab_find(&g_sema_ft, name);
    if (!sig) return 0;
    *out = *sig;
    return 1;
}

static void ftab_add(const FunctionDecl *fn) { ftab_push(&g_sema_ft, fn); }
static void ftab_add_export(const PkgFuncExport *ex) {
    ftab_push_export(&g_sema_ft, ex);
}
static void ftab_import_pkg(TranslationUnit *tu, Package *pkg) {
    import_pkg_funcs(tu, &g_sema_ft, pkg);
}
static void ftab_fill_extern(const FunSig *ex, const FunctionDecl *fn) {
    size_t idx = (size_t)(ex - g_sema_ft.data);
    g_sema_ft.data[idx].is_external = 0;
    g_sema_ft.data[idx].is_variadic = fn->is_variadic;
    g_sema_ft.data[idx].is_unprototyped = fn->is_unprototyped;
    g_sema_ft.data[idx].ret_type = fn->ret_type;
    g_sema_ft.data[idx].loc = fn->loc;
    int new_arity = (int)fn->params.len;
    if (g_sema_ft.data[idx].param_types) {
        for (int k = 0; k < g_sema_ft.data[idx].arity; k++)
            type_free(&g_sema_ft.data[idx].param_types[k]);
    }
    if (new_arity > g_sema_ft.data[idx].param_cap) {
        free(g_sema_ft.data[idx].param_types);
        g_sema_ft.data[idx].param_types = malloc(new_arity * sizeof(Type));
        if (!g_sema_ft.data[idx].param_types) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        g_sema_ft.data[idx].param_cap = new_arity;
    }
    g_sema_ft.data[idx].arity = new_arity;
    for (int k = 0; k < new_arity; k++)
        g_sema_ft.data[idx].param_types[k] = type_clone(fn->params.data[k].type);
}

/* Must stay tiny: Stage 0 miscompiles pointer stores inside huge frames. */
static void check_set_st(SymTable *st) {
    if (fakecc_had_error()) return;
    g_check_st = st;
}

static Type check_expr_inner(Expr *e);

/* Recurses with only `e`.  FunTable lives in g_sema_ft so huge callers never
 * pass a FunTable* (Stage 0 miscompiles those from large frames). */

static Type check_ternary_expr(Expr *e) {
    if (fakecc_had_error()) return type_make_void();
    Type ct = check_expr_inner(e->u.tern.cond);
    if (ct.kind != TY_INT && ct.kind != TY_FLOAT && ct.kind != TY_PTR)
        {
            die_at(e->loc.file, e->loc.line, e->loc.col,
               "ternary condition must be scalar");
            return type_make_void();
        }
    type_free(&ct);
    Expr *th = e->u.tern.then;
    if (!th) th = e->u.tern.cond;
    Type tt = check_expr_inner(th);
    Type et = check_expr_inner(e->u.tern.else_);
    if (tt.kind == TY_ARRAY) {
        Type d = type_decay(tt); type_free(&tt); tt = d;
    }
    if (et.kind == TY_ARRAY) {
        Type d = type_decay(et); type_free(&et); et = d;
    }
    int t_is_null_const = 0;
    int e_is_null_const = 0;
    {
        long long nv = 0;
        Expr *th_npc = e->u.tern.then ? e->u.tern.then : e->u.tern.cond;
        if (tt.kind == TY_INT && th_npc && fold_const_int(th_npc, &nv) && nv == 0)
            t_is_null_const = 1;
        if (et.kind == TY_INT && e->u.tern.else_
            && fold_const_int(e->u.tern.else_, &nv) && nv == 0)
            e_is_null_const = 1;
    }
    int tt_cplx = (tt.kind == TY_STRUCT && tt.tag && strncmp(tt.tag, "__complex_", 10) == 0);
    int et_cplx = (et.kind == TY_STRUCT && et.tag && strncmp(et.tag, "__complex_", 10) == 0);
    int tt_arith = (tt.kind == TY_INT || tt.kind == TY_FLOAT || tt_cplx);
    int et_arith = (et.kind == TY_INT || et.kind == TY_FLOAT || et_cplx);
    Type res;
    if (tt.kind == TY_VOID || et.kind == TY_VOID) {
        res = type_make_void();
    } else if (tt_arith && et_arith) {
        res = usual_arith_conv(tt, et);
    } else if (tt.kind == TY_PTR && et.kind == TY_PTR) {
        /* C99 6.5.15: if either pointer is void *, the result is void *. */
        int tvoid = tt.pointee && tt.pointee->kind == TY_VOID;
        int evoid = et.pointee && et.pointee->kind == TY_VOID;
        if (evoid && !tvoid)
            res = type_clone(et);
        else
            res = type_clone(tt);
    } else if (tt.kind == TY_PTR && e_is_null_const) {
        res = type_clone(tt);
    } else if (et.kind == TY_PTR && t_is_null_const) {
        res = type_clone(et);
    } else if (tt.kind == TY_STRUCT && et.kind == TY_STRUCT
               && strcmp(tt.tag, et.tag) == 0) {
        res = type_clone(tt);
    } else {
        die_at(e->loc.file, e->loc.line, e->loc.col,
               "ternary branches must both be arithmetic, both be pointer, "
               "or pointer with null constant");
        return type_make_void();
    }
    type_free(&tt); type_free(&et);
    set_type(e, res);
    return type_clone(e->type);
}

static Type check_expr_inner(Expr *e) {
    if (fakecc_had_error()) return type_make_void();
    if (!e) return type_default_int();
    const SymTable *st = g_check_st;
    switch (e->kind) {
    case EX_INT_LIT:
        /* Width/signedness were derived from the suffix and magnitude by the
         * parser; overwriting them with plain int here would truncate every
         * wide constant. */
        set_type(e, type_make_int(e->type.width ? e->type.width : 4,
                                  e->type.is_unsigned));
        return type_clone(e->type);
    case EX_FLOAT_LIT:
        /* Width was stashed in e->type by the parser (4 = float, 8 = double). */
        if (e->type.is_decimal)
            set_type(e, type_make_decimal(e->type.width ? e->type.width : 8));
        else
            set_type(e, type_make_float(e->type.width ? e->type.width : 8));
        return type_clone(e->type);
    case EX_BINOP: {
        Type lt = check_expr_inner(e->u.bin.l);
        Type rt = check_expr_inner(e->u.bin.r);
        /* Array-to-pointer decay for operands (except & / sizeof handled elsewhere). */
        if (lt.kind == TY_ARRAY) {
            Type d = type_decay(lt); type_free(&lt); lt = d;
        }
        if (rt.kind == TY_ARRAY) {
            Type d = type_decay(rt); type_free(&rt); rt = d;
        }
        BinOp op = e->u.bin.op;
        Type res;
        if (lt.is_vector || rt.is_vector) {
            res = type_clone(lt.is_vector ? lt : rt);
        } else if ((lt.kind == TY_STRUCT && lt.tag && strncmp(lt.tag, "__complex_", 10) == 0) ||
            (rt.kind == TY_STRUCT && rt.tag && strncmp(rt.tag, "__complex_", 10) == 0)) {
            if (op == BOP_EQ || op == BOP_NE) {
                res = type_make_int(4, 0);
            } else {
                res = type_clone(lt.kind == TY_STRUCT ? lt : rt);
            }
        } else if (op == BOP_AND || op == BOP_OR) {
            /* Logical && / ||: both operands must be scalar (int, float, or
             * pointer).  Result is always int 0 or 1. */
            if (lt.kind != TY_INT && lt.kind != TY_FLOAT && lt.kind != TY_PTR)
                {
                    die_at(e->loc.file, e->loc.line, e->loc.col,
                       "left operand of '%s' must be scalar",
                       op == BOP_AND ? "&&" : "||");
                    return type_make_void();
                }
            if (rt.kind != TY_INT && rt.kind != TY_FLOAT && rt.kind != TY_PTR)
                {
                    die_at(e->loc.file, e->loc.line, e->loc.col,
                       "right operand of '%s' must be scalar",
                       op == BOP_AND ? "&&" : "||");
                    return type_make_void();
                }
            res = type_make_int(4, 0);
        } else if (op >= BOP_EQ && op <= BOP_GE) {
            if (lt.kind == TY_STRUCT || rt.kind == TY_STRUCT)
                {
                    die_at(e->loc.file, e->loc.line, e->loc.col,
                       "invalid operands to binary comparison");
                    return type_make_void();
                }
            if (lt.kind == TY_VOID || rt.kind == TY_VOID)
                {
                    die_at(e->loc.file, e->loc.line, e->loc.col,
                       "invalid operands to binary comparison");
                    return type_make_void();
                }
            if ((lt.is_decimal && rt.kind == TY_FLOAT && !rt.is_decimal) ||
                (rt.is_decimal && lt.kind == TY_FLOAT && !lt.is_decimal))
                {
                    die_at(e->loc.file, e->loc.line, e->loc.col,
                       "cannot mix operands of decimal floating and other floating types");
                    return type_make_void();
                }
            /* Comparison operators: warn on distinct pointer types (GCC -Wcompare-distinct-pointer-types). */
            if (lt.kind == TY_PTR && rt.kind == TY_PTR) {
                int func_cmp = (lt.pointee && lt.pointee->kind == TY_FUNC) ||
                               (rt.pointee && rt.pointee->kind == TY_FUNC);
                int null_cmp = (lt.kind == TY_PTR && rt.kind == TY_INT && rt.width == 0) ||
                               (rt.kind == TY_PTR && lt.kind == TY_INT && lt.width == 0);
                if (!func_cmp && !null_cmp) {
                    int is_relational = (op == BOP_LT || op == BOP_LE || op == BOP_GT || op == BOP_GE);
                    int is_equality = (op == BOP_EQ || op == BOP_NE);
                    int lt_void = lt.pointee && lt.pointee->kind == TY_VOID;
                    int rt_void = rt.pointee && rt.pointee->kind == TY_VOID;
                    if (is_relational && !type_is_same(&lt, &rt)) {
                        fprintf(stderr, "%s:%d:%d: warning: comparison of distinct pointer types\n",
                                e->loc.file, e->loc.line, e->loc.col);
                    } else if (is_equality && !lt_void && !rt_void && !type_is_same(&lt, &rt)) {
                        fprintf(stderr, "%s:%d:%d: warning: comparison of distinct pointer types\n",
                                e->loc.file, e->loc.line, e->loc.col);
                    }
                }
            }
            res = type_make_int(4, 0);
        } else if ((op == BOP_ADD || op == BOP_SUB) && (lt.kind == TY_PTR || rt.kind == TY_PTR)) {
            /* Pointer arithmetic: p+int, int+p, p-int → pointer; p-q → int. */
            if (op == BOP_ADD) {
                if (lt.kind == TY_PTR && rt.kind == TY_PTR)
                    {
                        die_at(e->loc.file, e->loc.line, e->loc.col,
                           "invalid operands to binary + (pointer and pointer)");
                        return type_make_void();
                    }
                if (lt.kind == TY_PTR && rt.kind != TY_INT)
                    {
                        die_at(e->loc.file, e->loc.line, e->loc.col,
                           "invalid operands to binary +");
                        return type_make_void();
                    }
                if (rt.kind == TY_PTR && lt.kind != TY_INT)
                    {
                        die_at(e->loc.file, e->loc.line, e->loc.col,
                           "invalid operands to binary +");
                        return type_make_void();
                    }
                res = type_clone(lt.kind == TY_PTR ? lt : rt);
            } else { /* op == BOP_SUB */
                if (lt.kind == TY_PTR && rt.kind == TY_PTR) {
                    res = type_make_int(8, 0);   /* ptrdiff — use long */
                } else if (lt.kind == TY_PTR) {
                    if (rt.kind != TY_INT)
                        {
                            die_at(e->loc.file, e->loc.line, e->loc.col,
                               "invalid operands to binary -");
                            return type_make_void();
                        }
                    res = type_clone(lt);
                } else {
                    /* integer - pointer: constraint violation! */
                    die_at(e->loc.file, e->loc.line, e->loc.col,
                           "invalid operands to binary - (integer and pointer)");
                    return type_make_void();
                }
            }
        } else if (op == BOP_BITAND || op == BOP_BITOR || op == BOP_BITXOR) {
            /* Bitwise & | ^: both operands must be integer; result = UAC. */
            if (lt.kind != TY_INT)
                {
                    die_at(e->loc.file, e->loc.line, e->loc.col,
                       "left operand of '%s' must be integer",
                       op == BOP_BITAND ? "&" : op == BOP_BITOR ? "|" : "^");
                    return type_make_void();
                }
            if (rt.kind != TY_INT)
                {
                    die_at(e->loc.file, e->loc.line, e->loc.col,
                       "right operand of '%s' must be integer",
                       op == BOP_BITAND ? "&" : op == BOP_BITOR ? "|" : "^");
                    return type_make_void();
                }
            res = usual_arith_conv(lt, rt);
        } else if (op == BOP_SHL || op == BOP_SHR) {
            /* Shift << >>: both operands must be integer; result type is the
             * promoted type of the LEFT operand (C §6.5.7). */
            if (lt.kind != TY_INT)
                {
                    die_at(e->loc.file, e->loc.line, e->loc.col,
                       "left operand of '%s' must be integer",
                       op == BOP_SHL ? "<<" : ">>");
                    return type_make_void();
                }
            if (rt.kind != TY_INT)
                {
                    die_at(e->loc.file, e->loc.line, e->loc.col,
                       "right operand of '%s' must be integer",
                       op == BOP_SHL ? "<<" : ">>");
                    return type_make_void();
                }
            /* Warn on negative shift count (GCC -Wshift-count-negative). */
            if (rt.kind == TY_INT && rt.width <= 8) {
                long long shift_val;
                if (fold_const_int(e->u.bin.r, &shift_val) && shift_val < 0) {
                    fprintf(stderr, "%s:%d:%d: warning: shift count is negative\n",
                            e->loc.file, e->loc.line, e->loc.col);
                }
            }
            res = integer_promote(lt);
        } else {
            /* ADD / SUB / MUL / DIV / MOD.  MOD requires integer operands. */
            if (op == BOP_MOD &&
                (lt.kind != TY_INT || rt.kind != TY_INT))
                {
                    die_at(e->loc.file, e->loc.line, e->loc.col,
                       "operands of '%%' must be integer");
                    return type_make_void();
                }
            else if (op != BOP_MOD &&
                     (lt.kind != TY_INT && lt.kind != TY_FLOAT))
                {
                    die_at(e->loc.file, e->loc.line, e->loc.col,
                       "left operand of '%s' must be arithmetic",
                       op == BOP_ADD ? "+" : op == BOP_SUB ? "-"
                       : op == BOP_MUL ? "*" : "/");
                    return type_make_void();
                }
            else if (op != BOP_MOD &&
                     (rt.kind != TY_INT && rt.kind != TY_FLOAT))
                {
                    die_at(e->loc.file, e->loc.line, e->loc.col,
                       "right operand of '%s' must be arithmetic",
                       op == BOP_ADD ? "+" : op == BOP_SUB ? "-"
                       : op == BOP_MUL ? "*" : "/");
                    return type_make_void();
                }
            if ((lt.is_decimal && rt.kind == TY_FLOAT && !rt.is_decimal) ||
                (rt.is_decimal && lt.kind == TY_FLOAT && !lt.is_decimal))
                {
                    die_at(e->loc.file, e->loc.line, e->loc.col,
                       "cannot mix operands of decimal floating and other floating types");
                    return type_make_void();
                }
            res = usual_arith_conv(lt, rt);
        }
        type_free(&lt); type_free(&rt);
        set_type(e, res);
        return type_clone(e->type);
    }
    case EX_UNARY: {
        Type ot = check_expr_inner(e->u.un.operand);
        if (ot.kind == TY_ARRAY) {
            Type d = type_decay(ot); type_free(&ot); ot = d;
        }
        if (ot.is_vector) {
            set_type(e, ot);
            return type_clone(e->type);
        }
        if ((e->u.un.op == UOP_BITNOT || e->u.un.op == UOP_NEG || e->u.un.op == UOP_POS) &&
            ot.kind == TY_STRUCT && ot.tag && strncmp(ot.tag, "__complex_", 10) == 0) {
            set_type(e, ot);
            return type_clone(e->type);
        }
        if ((e->u.un.op == UOP_BITNOT) && ot.kind != TY_INT)
            {
                die_at(e->loc.file, e->loc.line, e->loc.col,
                   "bitwise NOT requires an integer operand");
                return type_make_void();
            }
        Type res;
        if (e->u.un.op == UOP_NOT) {
            /* Logical NOT: operand must be scalar (int, float, or pointer);
             * result is always int 0 or 1. */
            if (ot.kind != TY_INT && ot.kind != TY_FLOAT && ot.kind != TY_PTR)
                {
                    die_at(e->loc.file, e->loc.line, e->loc.col,
                       "logical NOT requires a scalar operand");
                    return type_make_void();
                }
            res = type_make_int(4, 0);
        } else {
            res = integer_promote(ot);
        }
        type_free(&ot);
        set_type(e, res);
        return type_clone(e->type);
    }
    case EX_VAR: {
        /* Qualified: pkg.sym.  The parser only sets `pkg` after a matching
         * `import`, so a missing-import diagnostic here is unreachable. */
        if (e->u.var.pkg) {
            const PkgFuncExport *pf = NULL;
            const PkgGlobalExport *pg = NULL;
            if (!pkg_resolve_sym(e->u.var.pkg, e->u.var.name, &pf, &pg)) {
                Package *pkg = pkg_find(g_sema_pkg, e->u.var.pkg);
                const EnumConstant *ec = pkg
                    ? pkg_find_enum_const(pkg, e->u.var.name) : NULL;
                if (ec) {
                    free(e->u.var.name);
                    free(e->u.var.pkg);
                    e->kind = EX_INT_LIT;
                    e->u.int_val = ec->value;
                    e->int_hi = 0;
                    set_type(e, type_default_int());
                    return type_clone(e->type);
                }
                die_at(e->loc.file, e->loc.line, e->loc.col,
                       "package '%s' has no exported symbol '%s'",
                       e->u.var.pkg, e->u.var.name);
                return type_make_void();
            }
            if (pf) {
                ftab_add_export(pf);
                const Type **ptys = NULL;
                if (pf->arity > 0) {
                    ptys = malloc(pf->arity * sizeof(Type *));
                    if (!ptys) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
                    for (int i = 0; i < pf->arity; i++)
                        ptys[i] = &pf->param_types[i];
                }
                Type fn = type_make_func_var(pf->ret_type, (Type **)ptys, pf->arity,
                                             pf->is_variadic);
                free(ptys);
                Type fp = type_make_ptr(fn);
                type_free(&fn);
                set_type(e, fp);
                return type_clone(e->type);
            }
            set_type(e, type_clone(pg->type));
            return type_clone(e->type);
        }
        const Sym *sym = symtable_find(st, e->u.var.name);
        if (sym) {
            set_type(e, type_clone(sym->type));
            return type_clone(e->type);
        }
        /* Not a variable — is it a function name?  A function lvalue decays
         * to a pointer to its type (so `add` and &add both yield `ptr(func)`). */
        const FunSig *sig = ftab_lookup(e->u.var.name);
        if (sig) {
            const Type **ptys = NULL;
            if (sig->arity > 0) {
                ptys = malloc(sig->arity * sizeof(Type *));
                if (!ptys) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
                for (int i = 0; i < sig->arity; i++)
                    ptys[i] = &sig->param_types[i];
            }
            Type fn = type_make_func_var(sig->ret_type, (Type **)ptys, sig->arity,
                                         sig->is_variadic);
            fn.func_is_unprototyped = sig->is_unprototyped;
            free(ptys);
            Type fp = type_make_ptr(fn);
            type_free(&fn);
            set_type(e, fp);
            return type_clone(e->type);
        }
        /* Same-package fallback: look up in the current package's exports. */
        if (g_sema_pkg && g_sema_tu && g_sema_tu->package.name) {
            const PkgFuncExport *pf = NULL;
            const PkgGlobalExport *pg = NULL;
            if (pkg_resolve_sym(g_sema_tu->package.name, e->u.var.name, &pf, &pg)) {
                if (pf) {
                    ftab_add_export(pf);
                    const Type **ptys = NULL;
                    if (pf->arity > 0) {
                        ptys = malloc(pf->arity * sizeof(Type *));
                        if (!ptys) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
                        for (int i = 0; i < pf->arity; i++)
                            ptys[i] = &pf->param_types[i];
                    }
                    Type fn = type_make_func_var(pf->ret_type, (Type **)ptys, pf->arity,
                                                 pf->is_variadic);
                    free(ptys);
                    Type fp = type_make_ptr(fn);
                    type_free(&fn);
                    set_type(e, fp);
                    return type_clone(e->type);
                }
                set_type(e, type_clone(pg->type));
                return type_clone(e->type);
            }
        }
        if (strcmp(e->u.var.name, "NULL") == 0) {
            Type vp = type_make_ptr(type_make_void());
            set_type(e, vp);
            return type_clone(e->type);
        }
        if (strcmp(e->u.var.name, "__CHAR_BIT__") == 0) {
            Type it = type_make_int(4, 0);
            set_type(e, it);
            return type_clone(e->type);
        }
        if (strcmp(e->u.var.name, "__INT_MAX__") == 0) {
            Type it = type_make_int(4, 0);
            set_type(e, it);
            return type_clone(e->type);
        }
        if (strcmp(e->u.var.name, "__FLT_MAX__") == 0) {
            /* GCC predefined: 3.40282346638528859812e+38F, type float. */
            Type ft = type_make_float(4);
            set_type(e, ft);
            return type_clone(e->type);
        }
        if (strncmp(e->u.var.name, "__builtin_", 10) == 0 || strcmp(e->u.var.name, "alloca") == 0) {
            const char *bname = e->u.var.name;
            Type ret = type_default_int();
            if (strcmp(bname, "__builtin_abort") == 0 || strcmp(bname, "__builtin_exit") == 0 || strcmp(bname, "__builtin_trap") == 0 || strcmp(bname, "__builtin_prefetch") == 0 || strcmp(bname, "__builtin_stack_restore") == 0 || strcmp(bname, "__builtin_longjmp") == 0 || strcmp(bname, "__builtin_return") == 0)
                ret = type_make_void();
            else if (strcmp(bname, "__builtin_memset") == 0 || strcmp(bname, "__builtin_memcpy") == 0 || strcmp(bname, "__builtin_memmove") == 0 || strcmp(bname, "__builtin_mempcpy") == 0 || strcmp(bname, "__builtin_alloca") == 0 || strcmp(bname, "alloca") == 0 || strcmp(bname, "__builtin_frame_address") == 0 || strcmp(bname, "__builtin_return_address") == 0 || strcmp(bname, "__builtin_stack_save") == 0 || strcmp(bname, "__builtin_apply_args") == 0 || strcmp(bname, "__builtin_apply") == 0 || strcmp(bname, "__builtin___memcpy_chk") == 0 || strcmp(bname, "__builtin___memmove_chk") == 0 || strcmp(bname, "__builtin___mempcpy_chk") == 0 || strcmp(bname, "__builtin___memset_chk") == 0)
                ret = type_make_ptr(type_make_void());
            else if (strcmp(bname, "__builtin_bcopy") == 0)
                ret = type_make_void();
            else if (strcmp(bname, "__builtin_strcat") == 0 || strcmp(bname, "__builtin___strcat_chk") == 0 ||
                     strcmp(bname, "__builtin_strcpy") == 0 || strcmp(bname, "__builtin___strcpy_chk") == 0 ||
                     strcmp(bname, "__builtin_stpcpy") == 0 || strcmp(bname, "__builtin___stpcpy_chk") == 0 ||
                     strcmp(bname, "__builtin_stpncpy") == 0 || strcmp(bname, "__builtin___stpncpy_chk") == 0 ||
                     strcmp(bname, "__builtin_strncat") == 0 || strcmp(bname, "__builtin___strncat_chk") == 0 ||
                     strcmp(bname, "__builtin_strncpy") == 0 || strcmp(bname, "__builtin___strncpy_chk") == 0)
                ret = type_make_ptr(type_make_int(1, 0));
            else if (strcmp(bname, "__builtin_strlen") == 0 || strcmp(bname, "__builtin_strspn") == 0 || strcmp(bname, "__builtin_object_size") == 0)
                ret = type_make_int(8, 1);
            else if (strcmp(bname, "__builtin_fabs") == 0)
                ret = type_make_float(8);
            else if (strcmp(bname, "__builtin_fabsf") == 0)
                ret = type_make_float(4);
            else if (strcmp(bname, "__builtin_fabsl") == 0)
                ret = type_make_float(16);
            else if (strcmp(bname, "__builtin_copysign") == 0 || strcmp(bname, "copysign") == 0)
                ret = type_make_float(8);
            else if (strcmp(bname, "__builtin_copysignf") == 0 || strcmp(bname, "copysignf") == 0)
                ret = type_make_float(4);
            else if (strcmp(bname, "__builtin_copysignl") == 0 || strcmp(bname, "copysignl") == 0)
                ret = type_make_float(16);
            else if (strcmp(bname, "__builtin_inf") == 0 || strcmp(bname, "__builtin_huge_val") == 0 || strcmp(bname, "__builtin_nan") == 0)
                ret = type_make_float(8);
            else if (strcmp(bname, "__builtin_inff") == 0 || strcmp(bname, "__builtin_huge_valf") == 0 || strcmp(bname, "__builtin_nanf") == 0)
                ret = type_make_float(4);
            else if (strcmp(bname, "__builtin_infl") == 0 || strcmp(bname, "__builtin_huge_vall") == 0 || strcmp(bname, "__builtin_nanl") == 0)
                ret = type_make_float(16);
            else if (strcmp(bname, "__builtin_bswap64") == 0)
                ret = type_make_int(8, 1);
            else if (strcmp(bname, "__builtin_bswap32") == 0)
                ret = type_make_int(4, 1);
            else if (strcmp(bname, "__builtin_bswap16") == 0)
                ret = type_make_int(2, 1);
            else if (strcmp(bname, "__builtin_classify_type") == 0)
                ret = type_make_int(4, 0);
            else if (strcmp(bname, "__builtin_signbit") == 0 || strcmp(bname, "__builtin_signbitf") == 0 || strcmp(bname, "__builtin_signbitl") == 0 || strcmp(bname, "signbit") == 0)
                ret = type_make_int(4, 0);
            else if (strcmp(bname, "__builtin_isnan") == 0 || strcmp(bname, "__builtin_isnanf") == 0 || strcmp(bname, "__builtin_isnanl") == 0 || strcmp(bname, "isnan") == 0)
                ret = type_make_int(4, 0);
            else if (strcmp(bname, "__builtin_isfinite") == 0 || strcmp(bname, "__builtin_isfinitef") == 0 || strcmp(bname, "__builtin_isfinitel") == 0 || strcmp(bname, "isfinite") == 0)
                ret = type_make_int(4, 0);
            else if (strcmp(bname, "__builtin_isinf") == 0 || strcmp(bname, "__builtin_isinff") == 0 || strcmp(bname, "__builtin_isinfl") == 0 || strcmp(bname, "isinf") == 0)
                ret = type_make_int(4, 0);
            else if (strcmp(bname, "__builtin_isgreater") == 0 || strcmp(bname, "__builtin_isgreaterequal") == 0 ||
                     strcmp(bname, "__builtin_isless") == 0 || strcmp(bname, "__builtin_islessequal") == 0 ||
                     strcmp(bname, "__builtin_islessgreater") == 0 || strcmp(bname, "__builtin_isunordered") == 0)
                ret = type_make_int(4, 0);
            else if (strcmp(bname, "__builtin_abs") == 0 || strcmp(bname, "abs") == 0)
                ret = type_make_int(4, 0);
            else if (strcmp(bname, "__builtin_labs") == 0 || strcmp(bname, "__builtin_llabs") == 0 || strcmp(bname, "__builtin_imaxabs") == 0 || strcmp(bname, "labs") == 0 || strcmp(bname, "llabs") == 0 || strcmp(bname, "imaxabs") == 0)
                ret = type_make_int(8, 0);
            else if (strcmp(bname, "__builtin_uabs") == 0)
                ret = type_make_int(4, 1);
            else if (strcmp(bname, "__builtin_ulabs") == 0 || strcmp(bname, "__builtin_ullabs") == 0 || strcmp(bname, "__builtin_umaxabs") == 0)
                ret = type_make_int(8, 1);
            else if (strcmp(bname, "__builtin_va_arg_pack") == 0 || strcmp(bname, "__builtin_va_arg_pack_len") == 0)
                ret = type_default_int();
            Type p0, p1;
            Type *params[2];
            int num_params = 0;
            if (strcmp(bname, "__builtin_copysignf") == 0 || strcmp(bname, "copysignf") == 0) {
                p0 = type_make_float(4);
                p1 = type_make_float(4);
                params[0] = &p0; params[1] = &p1;
                num_params = 2;
            } else if (strcmp(bname, "__builtin_copysign") == 0 || strcmp(bname, "copysign") == 0) {
                p0 = type_make_float(8);
                p1 = type_make_float(8);
                params[0] = &p0; params[1] = &p1;
                num_params = 2;
            } else if (strcmp(bname, "__builtin_copysignl") == 0 || strcmp(bname, "copysignl") == 0) {
                p0 = type_make_float(16);
                p1 = type_make_float(16);
                params[0] = &p0; params[1] = &p1;
                num_params = 2;
            } else if (strcmp(bname, "__builtin_fabsf") == 0) {
                p0 = type_make_float(4);
                params[0] = &p0;
                num_params = 1;
            } else if (strcmp(bname, "__builtin_fabs") == 0) {
                p0 = type_make_float(8);
                params[0] = &p0;
                num_params = 1;
            } else if (strcmp(bname, "__builtin_fabsl") == 0) {
                p0 = type_make_float(16);
                params[0] = &p0;
                num_params = 1;
            }
            Type fn = type_make_func(ret, num_params > 0 ? (Type * const *)params : NULL, num_params);
            /* Builtins without a recorded prototype (memcpy, snprintf_chk, …)
             * used to look like `T (*)()`.  Mark them variadic so the strict
             * function-pointer arity check does not reject real builtin calls. */
            if (num_params == 0)
                fn.func_is_variadic = 1;
            Type fp = type_make_ptr(fn);
            type_free(&fn);
            for (int i = 0; i < num_params; i++) type_free(params[i]);
            set_type(e, fp);
            return type_clone(e->type);
        }
        {
            const char *hint = g_sema_pkg
                ? pkg_suggest_export(g_sema_pkg, e->u.var.name) : NULL;
            if (hint) {
                if (g_sema_tu && tu_imports(g_sema_tu, hint)) {
                    die_at(e->loc.file, e->loc.line, e->loc.col,
                           "use of undeclared '%s'; did you mean '%s.%s'?",
                           e->u.var.name, hint, e->u.var.name);
                    return type_make_void();
                }
                die_at(e->loc.file, e->loc.line, e->loc.col,
                       "use of undeclared '%s'; did you mean '%s.%s'? "
                       "(add 'import %s;')",
                       e->u.var.name, hint, e->u.var.name, hint);
                return type_make_void();
            }
            die_at(e->loc.file, e->loc.line, e->loc.col,
                   "use of undeclared variable '%s'", e->u.var.name);
            return type_make_void();
        }
    }
    case EX_ASSIGN: {
        if (e->u.assign.lvalue->kind != EX_VAR &&
            e->u.assign.lvalue->kind != EX_DEREF &&
            e->u.assign.lvalue->kind != EX_INDEX &&
            e->u.assign.lvalue->kind != EX_MEMBER) {
            die_at(e->u.assign.lvalue->loc.file,
                   e->u.assign.lvalue->loc.line,
                   e->u.assign.lvalue->loc.col,
                   "expression is not assignable");
            return type_make_void();
        }
        Type lt = check_expr_inner(e->u.assign.lvalue);
        if (lt.is_const)
            {
                die_at(e->u.assign.lvalue->loc.file,
                   e->u.assign.lvalue->loc.line,
                   e->u.assign.lvalue->loc.col,
                   "assignment of read-only variable");
                return type_make_void();
            }
        if (lt.kind == TY_ARRAY)
            {
                die_at(e->u.assign.lvalue->loc.file,
                   e->u.assign.lvalue->loc.line,
                   e->u.assign.lvalue->loc.col,
                   "assignment to expression with array type");
                return type_make_void();
            }
        Type rt = check_expr_inner(e->u.assign.rvalue);
        if (rt.kind == TY_VOID)
            {
                die_at(e->u.assign.rvalue->loc.file,
                   e->u.assign.rvalue->loc.line,
                   e->u.assign.rvalue->loc.col,
                   "void value not ignored as it ought to be");
                return type_make_void();
            }
        int is_lt_cplx = (lt.kind == TY_STRUCT && lt.tag && strncmp(lt.tag, "__complex_", 10) == 0);
        int is_rt_cplx = (rt.kind == TY_STRUCT && rt.tag && strncmp(rt.tag, "__complex_", 10) == 0);
        if ((lt.kind == TY_STRUCT && !is_lt_cplx) ||
            (rt.kind == TY_STRUCT && !is_rt_cplx)) {
            if (!type_is_same(&lt, &rt)) {
                die_at(e->loc.file, e->loc.line, e->loc.col,
                       "incompatible struct assignment");
                return type_make_void();
            }
        }
        coerce_arg_to_param(&e->u.assign.rvalue, &lt);
        type_free(&rt);
        set_type(e, lt);
        return type_clone(e->type);
    }
    case EX_COMPOUND_ASSIGN: {
        /* lvalue op= rvalue. The lvalue must be assignable. Type rules follow
         * the underlying binary op: +=/-= allow pointer += int; the rest
         * require integer operands. Result type is the lvalue's type. */
        Expr *lv = e->u.comp.lvalue;
        if (lv->kind != EX_VAR && lv->kind != EX_DEREF &&
            lv->kind != EX_INDEX && lv->kind != EX_MEMBER)
            {
                die_at(lv->loc.file, lv->loc.line, lv->loc.col,
                   "left operand of '%s' must be an lvalue",
                   "compound assign");
                return type_make_void();
            }
        Type lt = check_expr_inner(lv);
        if (lt.is_const)
            {
                die_at(lv->loc.file, lv->loc.line, lv->loc.col,
                   "compound assignment of read-only variable");
                return type_make_void();
            }
        Type rt = check_expr_inner(e->u.comp.rvalue);
        BinOp op = e->u.comp.op;
        int arith_float = (op == BOP_ADD || op == BOP_SUB || op == BOP_MUL
                           || op == BOP_DIV);
        int is_complex = (lt.kind == TY_STRUCT && lt.tag && strncmp(lt.tag, "__complex_", 10) == 0);
        int is_vector = (lt.is_vector || rt.is_vector);
        if (op == BOP_ADD || op == BOP_SUB) {
            /* Pointer arithmetic: p += n, p -= n (n must be int). */
            if (lt.kind == TY_PTR && rt.kind != TY_INT)
                {
                    die_at(lv->loc.file, lv->loc.line, lv->loc.col,
                       "pointer %s requires an integer right operand",
                       op == BOP_ADD ? "+=" : "-=");
                    return type_make_void();
                }
        }
        if (lt.kind != TY_PTR && !is_complex && !is_vector
            && !(arith_float && (lt.kind == TY_FLOAT || rt.kind == TY_FLOAT))) {
            if (lt.kind != TY_INT)
                {
                    die_at(lv->loc.file, lv->loc.line, lv->loc.col,
                       "left operand of '%s' must be integer",
                       "compound assign");
                    return type_make_void();
                }
            if (rt.kind != TY_INT)
                {
                    die_at(lv->loc.file, lv->loc.line, lv->loc.col,
                       "right operand of '%s' must be integer",
                       "compound assign");
                    return type_make_void();
                }
        }
        if (lt.kind == TY_FLOAT && !arith_float)
            {
                die_at(lv->loc.file, lv->loc.line, lv->loc.col,
                   "left operand of '%s' must be integer", "compound assign");
                return type_make_void();
            }
        type_free(&rt);
        set_type(e, lt);
        return type_clone(e->type);
    }
    case EX_CALL: {
        /* Recognize the __syscall intrinsic FIRST — before type-checking the
         * callee, because `__syscall` is not a real variable/function and would
         * otherwise fail the EX_VAR lookup.  It takes 1..7 int args and returns
         * long. */
        if (e->u.call.callee->kind == EX_VAR
            && strcmp(e->u.call.callee->u.var.name, "__syscall") == 0) {
            if (e->u.call.args.len < 1 || e->u.call.args.len > 7) {
                die_at(e->loc.file, e->loc.line, e->loc.col,
                       "__syscall takes 1 to 7 arguments (syscall num + up to 6 args)");
                return type_make_void();
            }
            for (size_t i = 0; i < e->u.call.args.len; i++) {
                Type at = check_expr_inner(e->u.call.args.data[i]);
                type_free(&at);
            }
            set_type(e, type_make_int(8, 0));
            return type_clone(e->type);
        }
        if (e->u.call.callee->kind == EX_VAR
            && strcmp(e->u.call.callee->u.var.name, "__clone") == 0) {
            if (e->u.call.args.len != 6) {
                die_at(e->loc.file, e->loc.line, e->loc.col,
                       "__clone takes 6 arguments (fn, child_stack, flags, arg, tcb, ctid)");
                return type_make_void();
            }
            for (size_t i = 0; i < e->u.call.args.len; i++) {
                Type at = check_expr_inner(e->u.call.args.data[i]);
                type_free(&at);
            }
            set_type(e, type_make_int(8, 0));
            return type_clone(e->type);
        }
        if (e->u.call.callee->kind == EX_VAR) {
            const char *cn = e->u.call.callee->u.var.name;
            int is_conj = (strcmp(cn, "__builtin_conjf") == 0 || strcmp(cn, "__builtin_conj") == 0 ||
                           strcmp(cn, "__builtin_conjl") == 0);
            int is_crealf = (strcmp(cn, "__builtin_crealf") == 0 || strcmp(cn, "__builtin_cimagf") == 0);
            int is_creal = (strcmp(cn, "__builtin_creal") == 0 || strcmp(cn, "__builtin_cimag") == 0);
            int is_creall = (strcmp(cn, "__builtin_creall") == 0 || strcmp(cn, "__builtin_cimagl") == 0);
            if (is_conj || is_crealf || is_creal || is_creall) {
                if (e->u.call.args.len != 1) {
                    die_at(e->loc.file, e->loc.line, e->loc.col,
                           "complex builtin takes exactly 1 argument");
                    return type_make_void();
                }
                Type at = check_expr_inner(e->u.call.args.data[0]);
                if (is_conj) {
                    set_type(e, at);
                    return type_clone(e->type);
                }
                type_free(&at);
                if (is_crealf) set_type(e, type_make_float(4));
                else if (is_creall) set_type(e, type_make_float(16));
                else set_type(e, type_make_float(8));
                return type_clone(e->type);
            }
        }
        if (e->u.call.callee->kind == EX_VAR && strcmp(e->u.call.callee->u.var.name, "__builtin_shuffle") == 0) {
            if (e->u.call.args.len < 2 || e->u.call.args.len > 3) {
                die_at(e->loc.file, e->loc.line, e->loc.col,
                       "__builtin_shuffle takes 2 or 3 arguments");
                return type_make_void();
            }
            Type t0 = check_expr_inner(e->u.call.args.data[0]);
            for (size_t i = 1; i < e->u.call.args.len; i++) {
                Type ti = check_expr_inner(e->u.call.args.data[i]);
                type_free(&ti);
            }
            set_type(e, t0);
            return type_clone(e->type);
        }
        /* The float classification builtins are expanded inline too (ir.c reads
         * their arguments positionally), so check the count here rather than
         * let the lowering index past the end of the argument array.  The
         * names are matched exactly, the way ir.c dispatches on them: a
         * program that defines its own isunordered() keeps working. */
        if (e->u.call.callee->kind == EX_VAR) {
            static const char *const one_arg[] = {
                "__builtin_isnan", "__builtin_isnanf", "__builtin_isnanl", "isnan",
                "__builtin_isinf", "__builtin_isinff", "__builtin_isinfl", "isinf",
                "__builtin_isfinite", "__builtin_isfinitef", "__builtin_isfinitel",
                "isfinite", NULL };
            static const char *const two_arg[] = {
                "__builtin_isgreater", "__builtin_isgreaterequal",
                "__builtin_isless", "__builtin_islessequal",
                "__builtin_islessgreater", "__builtin_isunordered", NULL };
            const char *bn = e->u.call.callee->u.var.name;
            for (int k = 0; one_arg[k]; k++)
                if (strcmp(bn, one_arg[k]) == 0 && e->u.call.args.len != 1) {
                    die_at(e->loc.file, e->loc.line, e->loc.col,
                           "'%s' takes exactly 1 argument", bn);
                    return type_make_void();
                }
            for (int k = 0; two_arg[k]; k++)
                if (strcmp(bn, two_arg[k]) == 0 && e->u.call.args.len != 2) {
                    die_at(e->loc.file, e->loc.line, e->loc.col,
                           "'%s' takes exactly 2 arguments", bn);
                    return type_make_void();
                }
        }
        /* GCC integer bit builtins — codegen expands these inline (lzcnt /
         * tzcnt / popcnt / bswap) and reads their single argument directly, so
         * the argument count has to be right before we get there. */        if (e->u.call.callee->kind == EX_VAR) {
            const char *bn = e->u.call.callee->u.var.name;
            if (strncmp(bn, "__builtin_", 10) == 0 && e->u.call.args.len != 1
                && (strstr(bn, "clz") || strstr(bn, "ctz") || strstr(bn, "ffs")
                    || strstr(bn, "popcount") || strstr(bn, "parity")
                    || strstr(bn, "clrsb") || strstr(bn, "bswap"))) {
                die_at(e->loc.file, e->loc.line, e->loc.col,
                       "'%s' takes exactly 1 argument", bn);
                return type_make_void();
            }
        }
        /* __builtin_ctzll(x) — count trailing zeros of a nonzero uint64.  The
         * surrounding code guarantees the argument is nonzero (`_w &&`), so
         * the result is well-defined (1..64).  Returns int. */
        if (e->u.call.callee->kind == EX_VAR
            && strcmp(e->u.call.callee->u.var.name, "__builtin_ctzll") == 0) {
            if (e->u.call.args.len != 1) {
                die_at(e->loc.file, e->loc.line, e->loc.col,
                       "__builtin_ctzll takes exactly 1 argument");
                return type_make_void();
            }
            Type at = check_expr_inner(e->u.call.args.data[0]);
            type_free(&at);
            set_type(e, type_make_int(4, 0));
            return type_clone(e->type);
        }
        if (e->u.call.callee->kind == EX_VAR
            && strncmp(e->u.call.callee->u.var.name, "__sync_", 7) == 0) {
            const char *sname = e->u.call.callee->u.var.name;
            if (strcmp(sname, "__sync_synchronize") == 0) {
                set_type(e, type_make_void());
                return type_clone(e->type);
            }
            if (e->u.call.args.len < 1) {
                die_at(e->loc.file, e->loc.line, e->loc.col,
                       "atomic builtin takes at least 1 argument");
                return type_make_void();
            }
            Type ptr_ty = check_expr_inner(e->u.call.args.data[0]);
            if (ptr_ty.kind != TY_PTR || !ptr_ty.pointee) {
                die_at(e->loc.file, e->loc.line, e->loc.col,
                       "atomic builtin first argument must be a pointer");
                return type_make_void();
            }
            Type val_ty = type_clone(*ptr_ty.pointee);
            type_free(&ptr_ty);
            for (size_t i = 1; i < e->u.call.args.len; i++) {
                Type at = check_expr_inner(e->u.call.args.data[i]);
                type_free(&at);
            }
            if (strcmp(sname, "__sync_lock_release") == 0) {
                type_free(&val_ty);
                set_type(e, type_make_void());
            } else if (strcmp(sname, "__sync_bool_compare_and_swap") == 0) {
                type_free(&val_ty);
                set_type(e, type_make_int(4, 0));
            } else {
                set_type(e, val_ty);
            }
            return type_clone(e->type);
        }
        /* C11 __atomic_* family — accept the call and lower it as an
         * ordinary (non-atomic) load/store.  fakecc does not implement
         * atomic memory ordering; this is enough to compile code that uses
         * these builtins for portability (e.g. GCC torture tests that
         * exercise __thread + atomic load).  The IR layer treats the call
         * as a normal function call — the symbol is left undefined and the
         * linker resolves it from libc's libatomic when present, or the
         * harness's compile-only test passes regardless. */
        if (e->u.call.callee->kind == EX_VAR
            && strncmp(e->u.call.callee->u.var.name, "__atomic_", 9) == 0) {
            const char *sname = e->u.call.callee->u.var.name;
            if (e->u.call.args.len < 1) {
                die_at(e->loc.file, e->loc.line, e->loc.col,
                       "__atomic builtin takes at least 1 argument");
                return type_make_void();
            }
            Type ptr_ty = check_expr_inner(e->u.call.args.data[0]);
            if (ptr_ty.kind != TY_PTR || !ptr_ty.pointee) {
                die_at(e->loc.file, e->loc.line, e->loc.col,
                       "__atomic builtin first argument must be a pointer");
                return type_make_void();
            }
            Type val_ty = type_clone(*ptr_ty.pointee);
            type_free(&ptr_ty);
            for (size_t i = 1; i < e->u.call.args.len; i++) {
                Type at = check_expr_inner(e->u.call.args.data[i]);
                type_free(&at);
            }
            /* __atomic_load_n returns the loaded value; __atomic_store_n
             * returns void; __atomic_*_fetch / fetch_* return the prior
             * value (same type as the pointee). */
            if (strcmp(sname, "__atomic_store_n") == 0
                || strcmp(sname, "__atomic_clear") == 0) {
                type_free(&val_ty);
                set_type(e, type_make_void());
            } else {
                set_type(e, val_ty);
            }
            return type_clone(e->type);
        }
        if (e->u.call.callee->kind == EX_VAR
            && strcmp(e->u.call.callee->u.var.name, "__builtin_apply_args") == 0) {
            if (e->u.call.args.len != 0) {
                die_at(e->loc.file, e->loc.line, e->loc.col,
                       "__builtin_apply_args takes no arguments");
                return type_make_void();
            }
            set_type(e, type_make_ptr(type_make_void()));
            return type_clone(e->type);
        }
        if (e->u.call.callee->kind == EX_VAR
            && strcmp(e->u.call.callee->u.var.name, "__builtin_apply") == 0) {
            if (e->u.call.args.len != 3) {
                die_at(e->loc.file, e->loc.line, e->loc.col,
                       "__builtin_apply takes 3 arguments");
                return type_make_void();
            }
            for (size_t i = 0; i < e->u.call.args.len; i++) {
                Type at = check_expr_inner(e->u.call.args.data[i]);
                type_free(&at);
            }
            set_type(e, type_make_ptr(type_make_void()));
            return type_clone(e->type);
        }
        if (e->u.call.callee->kind == EX_VAR
            && strcmp(e->u.call.callee->u.var.name, "__builtin_return") == 0) {
            if (e->u.call.args.len != 1) {
                die_at(e->loc.file, e->loc.line, e->loc.col,
                       "__builtin_return takes 1 argument");
                return type_make_void();
            }
            Type at = check_expr_inner(e->u.call.args.data[0]);
            type_free(&at);
            set_type(e, type_make_void());
            return type_clone(e->type);
        }
        if (e->u.call.callee->kind == EX_VAR
            && strcmp(e->u.call.callee->u.var.name, "__builtin_va_arg_pack") == 0) {
            /* Placeholder expanded during always-inline lowering. */
            set_type(e, type_default_int());
            return type_clone(e->type);
        }
        if (e->u.call.callee->kind == EX_VAR
            && strcmp(e->u.call.callee->u.var.name, "__builtin_va_arg_pack_len") == 0) {
            set_type(e, type_default_int());
            return type_clone(e->type);
        }
        /* Recognize the va_start / va_arg / va_end builtins BEFORE the callee
         * lookup, for the same reason as __syscall: they are not real
         * variables/functions. They operate on a `va_list` lvalue (the struct
         * is passed as its address, matching fakecc's struct-as-pointer value
         * representation). */
        if (e->u.call.callee->kind == EX_VAR
            && (strcmp(e->u.call.callee->u.var.name, "va_start") == 0 ||
                strcmp(e->u.call.callee->u.var.name, "__builtin_va_start") == 0 ||
                strcmp(e->u.call.callee->u.var.name, "__builtin_c23_va_start") == 0)) {
            if (e->u.call.args.len < 1 || e->u.call.args.len > 2) {
                die_at(e->loc.file, e->loc.line, e->loc.col,
                       "va_start takes 1 or 2 arguments");
                return type_make_void();
            }
            Type list_ty = check_expr_inner(e->u.call.args.data[0]);
            if (!is_va_list_type(&list_ty)) {
                type_free(&list_ty);
                die_at(e->loc.file, e->loc.line, e->loc.col,
                       "va_start first argument must be a va_list");
                return type_make_void();
            }
            type_free(&list_ty);
            if (e->u.call.args.len == 2) {
                Type at = check_expr_inner(e->u.call.args.data[1]);
                type_free(&at);
            }
            set_type(e, type_make_void());
            return type_clone(e->type);
        }
        if (e->u.call.callee->kind == EX_VAR
            && (strcmp(e->u.call.callee->u.var.name, "va_arg") == 0 ||
                strcmp(e->u.call.callee->u.var.name, "__builtin_va_arg") == 0)) {
            /* va_arg's second argument is a type, parsed into va_arg_type
             * (not args), so args holds only the va_list expression. */
            if (e->u.call.args.len != 1) {
                die_at(e->loc.file, e->loc.line, e->loc.col,
                       "va_arg takes exactly 2 arguments (va_list, type)");
                return type_make_void();
            }
            Type list_ty = check_expr_inner(e->u.call.args.data[0]);
            if (!is_va_list_type(&list_ty)) {
                type_free(&list_ty);
                die_at(e->loc.file, e->loc.line, e->loc.col,
                       "va_arg first argument must be a va_list");
                return type_make_void();
            }
            type_free(&list_ty);
            if (e->va_arg_type.kind == TY_VOID && e->va_arg_type.width == 0
                && !e->va_arg_type.tag) {
                /* Matches GCC: "second argument to 'va_arg' is of incomplete
                 * type 'void'", so the dg-error pattern in the GCC torture
                 * test pr48767.c matches this diagnostic. */
                die_at(e->loc.file, e->loc.line, e->loc.col,
                       "second argument to 'va_arg' is of incomplete type 'void'");
                return type_make_void();
            }
            set_type(e, type_clone(e->va_arg_type));
            return type_clone(e->type);
        }
        if (e->u.call.callee->kind == EX_VAR
            && (strcmp(e->u.call.callee->u.var.name, "va_end") == 0 ||
                strcmp(e->u.call.callee->u.var.name, "__builtin_va_end") == 0)) {
            if (e->u.call.args.len != 1) {
                die_at(e->loc.file, e->loc.line, e->loc.col,
                       "va_end takes exactly 1 argument (va_list)");
                return type_make_void();
            }
            Type list_ty = check_expr_inner(e->u.call.args.data[0]);
            if (!is_va_list_type(&list_ty)) {
                type_free(&list_ty);
                die_at(e->loc.file, e->loc.line, e->loc.col,
                       "va_end argument must be a va_list");
                return type_make_void();
            }
            type_free(&list_ty);
            set_type(e, type_make_void());
            return type_clone(e->type);
        }
        if (e->u.call.callee->kind == EX_VAR
            && (strcmp(e->u.call.callee->u.var.name, "va_copy") == 0 ||
                strcmp(e->u.call.callee->u.var.name, "__builtin_va_copy") == 0)) {
            if (e->u.call.args.len != 2) {
                die_at(e->loc.file, e->loc.line, e->loc.col,
                       "va_copy takes exactly 2 arguments (dst, src)");
                return type_make_void();
            }
            for (int i = 0; i < 2; i++) {
                Type list_ty = check_expr_inner(e->u.call.args.data[i]);
                if (!is_va_list_type(&list_ty)) {
                    type_free(&list_ty);
                    die_at(e->loc.file, e->loc.line, e->loc.col,
                           "va_copy arguments must be va_list");
                    return type_make_void();
                }
                type_free(&list_ty);
            }
            set_type(e, type_make_void());
            return type_clone(e->type);
        }
        /* Type-check the callee expression. */
        Type callee_ty = check_expr_inner(e->u.call.callee);
        /* Direct call: callee is `EX_VAR` naming a known function. */
        if (e->u.call.callee->kind == EX_VAR) {
            const Sym *local_fn_sym = symtable_find(st, e->u.call.callee->u.var.name);
            /* If a local *variable* (including a function pointer parameter)
             * has this name, it shadows any same-named function.  A block-scope
             * `extern` / unprototyped function *declaration* does not: C
             * composite types prefer an existing prototype in the same TU
             * (`float fx(float);` then `float fx();` still calls as prototyped). */
            int local_is_fn_decl = local_fn_sym && local_fn_sym->type.kind == TY_FUNC;
            int local_is_fnptr = local_fn_sym && local_fn_sym->type.kind == TY_PTR
                                 && local_fn_sym->type.pointee
                                 && local_fn_sym->type.pointee->kind == TY_FUNC;
            int have_local = local_is_fnptr || (local_is_fn_decl && !local_fn_sym->type.func_is_unprototyped);
            FunSig sig_snapshot;
            const FunSig *sig = (have_local ||
                                 !ftab_snapshot(e->u.call.callee->u.var.name, &sig_snapshot))
                                ? NULL : &sig_snapshot;
            if (local_fn_sym && local_fn_sym->type.kind == TY_FUNC
                && !(local_fn_sym->type.func_is_unprototyped && sig && !sig->is_unprototyped)) {
                const Type *fty = &local_fn_sym->type;
                type_free(&callee_ty);
                check_call_arity(e->loc, e->u.call.callee->u.var.name,
                                 fty->func_nparams, fty->func_is_variadic,
                                 e->u.call.args.len);
                for (size_t i = 0; i < e->u.call.args.len; i++) {
                    Type at = check_expr_inner(e->u.call.args.data[i]);
                    type_free(&at);
                    if (fty->func_params && (int)i < fty->func_nparams
                        && !fty->func_is_unprototyped)
                        coerce_arg_to_param(&e->u.call.args.data[i],
                                            &fty->func_params[i]);
                    else if (fty->func_is_variadic && (int)i >= fty->func_nparams)
                        apply_default_arg_promotions(&e->u.call.args.data[i]);
                    else if (fty->func_is_unprototyped || !fty->func_params)
                        apply_default_arg_promotions(&e->u.call.args.data[i]);
                }
                set_type(e, type_clone(*fty->func_ret));
                return type_clone(e->type);
            }
            if (sig) {
                type_free(&callee_ty);
                check_call_arity(e->loc, e->u.call.callee->u.var.name,
                                 sig->arity, sig->is_variadic,
                                 e->u.call.args.len);
                for (size_t i = 0; i < e->u.call.args.len; i++) {
                    Type at = check_expr_inner(e->u.call.args.data[i]);
                    type_free(&at);
                    if ((int)i < sig->arity && !sig->is_unprototyped)
                        coerce_arg_to_param(&e->u.call.args.data[i],
                                            &sig->param_types[i]);
                    else if (sig->is_variadic)
                        apply_default_arg_promotions(&e->u.call.args.data[i]);
                    else if (sig->is_unprototyped)
                        apply_default_arg_promotions(&e->u.call.args.data[i]);
                }
                set_type(e, type_clone(sig->ret_type));
                return type_clone(e->type);
            }
            /* Not in function table — check the symbol table for an extern
             * function declaration (block-scope `extern void foo();`).  Such
             * declarations are registered as function-typed symbols so that
             * the call resolves as a direct call rather than indirect through
             * an uninitialized function-pointer variable (which would crash). */
            const Sym *extern_sym = symtable_find(st, e->u.call.callee->u.var.name);
            if (extern_sym && (extern_sym->type.kind == TY_FUNC ||
                               (extern_sym->type.kind == TY_PTR &&
                                extern_sym->type.pointee &&
                                extern_sym->type.pointee->kind == TY_FUNC))) {
                const Type *fty = (extern_sym->type.kind == TY_FUNC)
                                  ? &extern_sym->type : extern_sym->type.pointee;
                type_free(&callee_ty);
                check_call_arity(e->loc, e->u.call.callee->u.var.name,
                                 fty->func_nparams, fty->func_is_variadic,
                                 e->u.call.args.len);
                for (size_t i = 0; i < e->u.call.args.len; i++) {
                    Type at = check_expr_inner(e->u.call.args.data[i]);
                    type_free(&at);
                    if (fty->func_params && (int)i < fty->func_nparams
                        && !fty->func_is_unprototyped)
                        coerce_arg_to_param(&e->u.call.args.data[i],
                                            &fty->func_params[i]);
                    else if (fty->func_is_variadic && (int)i >= fty->func_nparams)
                        apply_default_arg_promotions(&e->u.call.args.data[i]);
                    else if (fty->func_is_unprototyped || !fty->func_params)
                        apply_default_arg_promotions(&e->u.call.args.data[i]);
                }
                Type ret = fty->func_ret ? *fty->func_ret : type_make_void();
                set_type(e, ret);
                return type_clone(e->type);
            }
        }
        /* Indirect call: callee type must be pointer-to-function or bare
         * function (a function lvalue such as `*fp` decays to a pointer). */
        Type fn_ty = callee_ty;
        if (fn_ty.kind == TY_PTR && fn_ty.pointee && fn_ty.pointee->kind == TY_FUNC) {
            fn_ty = type_clone(*fn_ty.pointee);
            type_free(&callee_ty);
        } else if (fn_ty.kind == TY_FUNC) {
            /* Bare function lvalue — keep as-is. */
            fn_ty = type_clone(callee_ty);
            type_free(&callee_ty);
        } else {
            die_at(e->loc.file, e->loc.line, e->loc.col,
                   "call to non-function (callee type must be a function or function pointer)");
            return type_make_void();
        }
        /* Empty-parens `T (*)()` is unprototyped in the grammar but FakeCC
         * treats it as taking zero arguments, same as a named `T foo()`. */
        check_fnptr_arity(e->loc, fn_ty.func_nparams, fn_ty.func_is_variadic,
                          e->u.call.args.len);
        for (size_t i = 0; i < e->u.call.args.len; i++) {
            Type at = check_expr_inner(e->u.call.args.data[i]);
            type_free(&at);
            if (fn_ty.func_params && (int)i < fn_ty.func_nparams)
                coerce_arg_to_param(&e->u.call.args.data[i],
                                    &fn_ty.func_params[i]);
            else if (fn_ty.func_is_variadic && (int)i >= fn_ty.func_nparams)
                apply_default_arg_promotions(&e->u.call.args.data[i]);
            else if (!fn_ty.func_params)
                apply_default_arg_promotions(&e->u.call.args.data[i]);
        }
        set_type(e, type_clone(*fn_ty.func_ret));
        type_free(&fn_ty);
        return type_clone(e->type);
    }
    case EX_STR: {
        /* Type: char[len+1] — array of char with length = len+1.
         * The array-to-pointer decay happens at expression use sites
         * (BINOP, UNARY, DEREF, INDEX, ASSIGN, CALL), preserving the
         * array type for sizeof("literal") which must return N, not 8. */
        Type ct = type_make_int(1, 0);   /* char */
        set_type(e, type_make_array(ct, e->u.str.len + 1));
        type_free(&ct);
        return type_clone(e->type);
    }
    case EX_ADDR: {
        Type ot = check_expr_inner(e->u.addr.operand);
        /* operand must be lvalue */
        ExprKind ok = e->u.addr.operand->kind;
        if (ok != EX_VAR && ok != EX_DEREF && ok != EX_INDEX && ok != EX_MEMBER
            && ok != EX_COMPOUND_LITERAL) {
            die_at(e->loc.file, e->loc.line, e->loc.col,
                   "cannot take address of rvalue");
            return type_make_void();
        }
        if (e->u.addr.operand->kind == EX_MEMBER && ot.bitfield_width > 0) {
            die_at(e->loc.file, e->loc.line, e->loc.col,
                   "cannot take address of bitfield member");
            return type_make_void();
        }
        if (e->u.addr.operand->kind == EX_VAR) {
            const Sym *sy = symtable_find(st, e->u.addr.operand->u.var.name);
            if (sy && sy->is_register) {
                die_at(e->loc.file, e->loc.line, e->loc.col,
                       "address of register variable requested");
                return type_make_void();
            }
        }
        /* `&function` is a no-op in C -- the result is the same function
         * pointer that the function lvalue already decayed to.  If the operand
         * is already a function pointer (from decay), just return it. */
        if (ot.kind == TY_PTR && ot.pointee && ot.pointee->kind == TY_FUNC) {
            set_type(e, ot);  /* takes ownership of ot */
            return type_clone(e->type);
        }
        Type res = type_make_ptr(ot);
        type_free(&ot);
        set_type(e, res);
        return type_clone(e->type);
    }
    case EX_DEREF: {
        Type ot = check_expr_inner(e->u.deref.operand);
        if (ot.kind == TY_ARRAY) {
            Type d = type_decay(ot); type_free(&ot); ot = d;
        }
        if (ot.kind != TY_PTR) {
            die_at(e->loc.file, e->loc.line, e->loc.col,
                   "cannot dereference non-pointer");
            return type_make_void();
        }
        Type res = type_clone(*ot.pointee);
        type_free(&ot);
        set_type(e, res);
        return type_clone(e->type);
    }
    case EX_INDEX: {
        /* a[i] — a must be ptr or array; result = pointee/elem. */
        Type at = check_expr_inner(e->u.idx.array);
        Type it = check_expr_inner(e->u.idx.index);
        /* `a[i]` is defined as `*(a + i)`, and addition commutes, so the
         * pointer is allowed on either side: `3[p]` means `p[3]`.  Normalize
         * by swapping the operands so everything downstream sees the usual
         * order. */
        if (at.kind == TY_INT && (it.kind == TY_PTR || it.kind == TY_ARRAY)) {
            Expr *tmp = e->u.idx.array;
            e->u.idx.array = e->u.idx.index;
            e->u.idx.index = tmp;
            Type swap = at; at = it; it = swap;
        }
        if (it.kind != TY_INT) {
            die_at(e->loc.file, e->loc.line, e->loc.col,
                   "array subscript is not an integer");
            return type_make_void();
        }
        type_free(&it);
        if (at.is_vector && at.elem_type) {
            Type res = type_clone(*at.elem_type);
            type_free(&at);
            set_type(e, res);
            return type_clone(e->type);
        }
        Type base = at;
        if (base.kind == TY_ARRAY) {
            /* Decay to pointer *locally* for typing.  Do NOT overwrite the
             * subexpression's type — IR-gen needs to see the original array
             * type to decide whether to return an address (for nested a[i][j]
             * where inner a[i] is another array) or to load a value. */
            Type d = type_decay(base); type_free(&base); base = d;
        }
        if (base.kind != TY_PTR) {
            die_at(e->loc.file, e->loc.line, e->loc.col,
                   "subscript on non-pointer/non-array");
            return type_make_void();
        }
        Type res = type_clone(*base.pointee);
        type_free(&base);
        set_type(e, res);
        return type_clone(e->type);
    }
    case EX_MEMBER: {
        /* Package-qualified name: `runtime.printf` parsed as member access.
         * Prefer a real local/global struct variable of that name; otherwise
         * rewrite to a qualified EX_VAR so IR sees a direct-call EX_VAR. */
        if (e->u.member.obj->kind == EX_VAR
            && e->u.member.obj->u.var.pkg == NULL
            && g_sema_tu
            && tu_imports(g_sema_tu, e->u.member.obj->u.var.name)
            && !symtable_find(st, e->u.member.obj->u.var.name)) {
            char *pkg = xstrdup(e->u.member.obj->u.var.name);
            char *name = xstrdup(e->u.member.name);
            expr_free(e->u.member.obj);
            free(e->u.member.name);
            e->kind = EX_VAR;
            e->u.var.name = name;
            e->u.var.pkg = pkg;
            return check_expr_inner(e);
        }
        Type ot = check_expr_inner(e->u.member.obj);
        if (ot.kind != TY_STRUCT) {
            die_at(e->loc.file, e->loc.line, e->loc.col,
                   "member access '.%s' on non-struct", e->u.member.name);
            return type_make_void();
        }
        const StructDef *sd = struct_registry_find_c(g_sema_structs, ot.tag);
        if (!sd) {
            die_at(e->loc.file, e->loc.line, e->loc.col,
                   "unknown struct 'struct %s'", ot.tag);
            return type_make_void();
        }
        const StructMember *m = struct_lookup_member(g_sema_structs, sd,
                                                     e->u.member.name, NULL);
        if (!m) {
            die_at(e->loc.file, e->loc.line, e->loc.col,
                   "struct '%s' has no member '%s'", ot.tag, e->u.member.name);
            return type_make_void();
        }
        type_free(&ot);
        Type mt = type_clone(m->type);
        if (m->bit_width > 0) {
            mt.bitfield_width = m->bit_width;
            if (m->bit_width < 32 || (m->bit_width == 32 && !m->type.is_unsigned)) {
                mt.width = 4;
                mt.is_unsigned = 0;
            } else if (m->bit_width == 32 && m->type.is_unsigned) {
                mt.width = 4;
                mt.is_unsigned = 1;
            }
        }
        set_type(e, mt);
        return type_clone(e->type);
    }
    case EX_INC_DEC: {
        /* ++lvalue / --lvalue (prefix or postfix). */
        Expr *op = e->u.incdec.operand;
        if (op->kind != EX_VAR && op->kind != EX_DEREF &&
            op->kind != EX_INDEX && op->kind != EX_MEMBER)
            {
                die_at(op->loc.file, op->loc.line, op->loc.col,
                   "operand of '%s' must be an lvalue",
                   e->u.incdec.is_inc ? "++" : "--");
                return type_make_void();
            }
        Type ot = check_expr_inner(op);
        if (ot.is_const)
            {
                die_at(op->loc.file, op->loc.line, op->loc.col,
                   "cannot increment/decrement a read-only variable");
                return type_make_void();
            }
        if (ot.kind != TY_INT && ot.kind != TY_FLOAT && ot.kind != TY_PTR)
            {
                die_at(op->loc.file, op->loc.line, op->loc.col,
                   "operand of '%s' must be arithmetic or pointer",
                   e->u.incdec.is_inc ? "++" : "--");
                return type_make_void();
            }
        type_free(&ot);
        /* Result type is the operand type (before increment). For both prefix
         * and postfix, e->type is the same as the operand's declared type. */
        set_type(e, type_clone(op->type));
        return type_clone(e->type);
    }
    case EX_COMMA: {
        /* a, b: evaluate a (discard result), result is b after lvalue
         * conversion (array-to-pointer and function-to-pointer decay). */
        Type lt = check_expr_inner(e->u.comma.lhs);
        type_free(&lt);
        Type rt = check_expr_inner(e->u.comma.rhs);
        if (rt.kind == TY_ARRAY) {
            Type d = type_decay(rt); type_free(&rt); rt = d;
        } else if (rt.kind == TY_FUNC) {
            Type d = type_make_ptr(rt); type_free(&rt); rt = d;
        }
        set_type(e, rt);
        return type_clone(e->type);
    }
    case EX_TERNARY:
        return check_ternary_expr(e);
    case EX_CAST: {
        Type ot = check_expr_inner(e->u.cast.operand);
        type_free(&ot);
        /* Result type is the cast target already stored on the expr;
         * we also mirror it on e->type. */
        set_type(e, type_clone(e->u.cast.target));
        return type_clone(e->type);
    }
    case EX_SIZEOF_TYPE: {
        Type *t = &e->u.sizeof_t.target;
        if (t->kind == TY_STRUCT && t->tag) {
            int is_cplx = (strncmp(t->tag, "__complex_", 10) == 0);
            if (!is_cplx && !struct_registry_find_c(g_sema_structs, t->tag)) {
                die_at(e->loc.file, e->loc.line, e->loc.col,
                       "invalid application of 'sizeof' to incomplete type 'struct %s'", t->tag);
                return type_make_void();
            }
        }
        while (t && t->kind == TY_ARRAY) {
            if (t->vla_dim) {
                Type dt = check_expr_inner(t->vla_dim);
                type_free(&dt);
            }
            t = t->elem_type;
        }
        set_type(e, type_make_int(8, 1));   /* size_t == unsigned long */
        return type_clone(e->type);
    }
    case EX_SIZEOF_EXPR: {
        Type ot = check_expr_inner(e->u.sizeof_e.operand);
        if (ot.bitfield_width > 0) {
            die_at(e->loc.file, e->loc.line, e->loc.col,
                   "invalid application of 'sizeof' to bit-field");
            return type_make_void();
        }
        if (ot.kind == TY_STRUCT && ot.tag) {
            int is_cplx = (strncmp(ot.tag, "__complex_", 10) == 0);
            if (!is_cplx && !struct_registry_find_c(g_sema_structs, ot.tag)) {
                die_at(e->loc.file, e->loc.line, e->loc.col,
                       "invalid application of 'sizeof' to incomplete type 'struct %s'", ot.tag);
                return type_make_void();
            }
        }
        type_free(&ot);
        set_type(e, type_make_int(8, 1));   /* size_t == unsigned long */
        return type_clone(e->type);
    }
    case EX_ALIGNOF_TYPE: {
        Type *t = &e->u.alignof_t.target;
        if (t->kind == TY_STRUCT && t->tag) {
            int is_cplx = (strncmp(t->tag, "__complex_", 10) == 0);
            if (!is_cplx && !struct_registry_find_c(g_sema_structs, t->tag)) {
                die_at(e->loc.file, e->loc.line, e->loc.col,
                       "invalid application of 'alignof' to incomplete type 'struct %s'", t->tag);
                return type_make_void();
            }
        }
        set_type(e, type_make_int(8, 1));   /* size_t == unsigned long */
        return type_clone(e->type);
    }
    case EX_ALIGNOF_EXPR: {
        Type ot = check_expr_inner(e->u.alignof_e.operand);
        long long al = type_align(ot);
        type_free(&ot);
        if (e->u.alignof_e.operand->kind == EX_VAR) {
            const char *nm = e->u.alignof_e.operand->u.var.name;
            const Sym *sy = symtable_find(st, nm);
            if (sy && sy->align > al) al = sy->align;
            if (g_sema_tu) {
                for (size_t i = 0; i < g_sema_tu->functions.len; i++) {
                    if (strcmp(g_sema_tu->functions.data[i].name, nm) == 0
                        && g_sema_tu->functions.data[i].align > al)
                        al = g_sema_tu->functions.data[i].align;
                }
            }
        }
        expr_free(e->u.alignof_e.operand);
        e->kind = EX_INT_LIT;
        e->u.int_val = al;
        e->int_hi = 0;
        set_type(e, type_make_int(8, 1));
        return type_clone(e->type);
    }
    case EX_INIT_LIST: {
        /* An initializer list is valid only as a declarator initializer.  We
         * type-check every element; the count / layout validation against the
         * target type happens in check_stmt(ST_DECL).  The list's own type is
         * the target type, which the caller knows — here we just return a
         * benign int so a stray use in an expression context degrades gracefully
         * (it is rejected upstream anyway). */
        for (int i = 0; i < e->u.init_list.num_elements; i++) {
            Type et = check_expr_inner(e->u.init_list.elements[i]);
            type_free(&et);
        }
        set_type(e, type_default_int());
        return type_clone(e->type);
    }
    case EX_COMPOUND_LITERAL: {
        /* (Type){ ... }: validate the init list against the target type and
         * type-check every element.  The list's own type is the target type. */
        Expr *init = e->u.compound.init;
        if (init->kind == EX_INIT_LIST)
            normalize_init_list(&e->u.compound.target_type, init, e->loc, 0);
        for (int i = 0; i < init->u.init_list.num_elements; i++) {
            Type et = check_expr_inner(init->u.init_list.elements[i]);
            type_free(&et);
        }
        set_type(e, type_clone(e->u.compound.target_type));
        return type_clone(e->type);
    }
    case EX_STMT_EXPR: {
        int has_ret = 0;
        check_stmt_list(e->u.stmt_expr.stmts, &has_ret);
        Type res_type = type_make_void();
        if (e->u.stmt_expr.stmts && e->u.stmt_expr.stmts->len > 0) {
            Stmt *last = &e->u.stmt_expr.stmts->data[e->u.stmt_expr.stmts->len - 1];
            if (last->kind == ST_EXPR && last->u.expr) {
                type_free(&res_type);
                res_type = type_clone(last->u.expr->type);
                if (res_type.kind == TY_VOID) {
                    sema_report_error(&last->u.expr->loc,
                            "void value not ignored as it ought to be");
                }
            }
        }
        set_type(e, res_type);
        return type_clone(e->type);
    }
    case EX_LABEL_ADDR: {
        /* &&label yields a void* */
        Type t; memset(&t, 0, sizeof(t));
        t.kind = TY_PTR;
        t.width = 8;
        t.pointee = malloc(sizeof(Type));
        *t.pointee = type_make_void();
        set_type(e, t);
        return type_clone(e->type);
    }
    }
    return type_default_int();
}

static void check_stmt(Stmt *s, size_t scope_mark, int *has_return);

static int g_sema_loop_depth = 0;

static void check_stmt_list(StmtArray *body, int *has_return) {
    if (fakecc_had_error()) return;
    SymTable *st = g_check_st;
    size_t mark = symtable_enter_scope(st);
    for (size_t i = 0; i < body->len; i++)
        check_stmt(&body->data[i], mark, has_return);
    symtable_leave_scope(st, mark);
}

/* Is `e` a compile-time constant suitable for initializing a global?  Integer
 * literals and nested initializer lists of constants qualify. */
static int is_const_init(const Expr *e, const SymTable *globals) {
    if (!e) return 1;
    if (e->kind == EX_INT_LIT) return 1;
    if (e->kind == EX_FLOAT_LIT) return 1;
    if (e->kind == EX_STR) return 1;
    if (e->kind == EX_LABEL_ADDR) return 1;
    if (e->kind == EX_COMPOUND_LITERAL)
        return is_const_init(e->u.compound.init, globals);
    if (e->kind == EX_BINOP)
        return is_const_init(e->u.bin.l, globals) && is_const_init(e->u.bin.r, globals);
    if (e->kind == EX_CAST)
        return is_const_init(e->u.cast.operand, globals);
    if (e->kind == EX_CALL && e->u.call.callee && e->u.call.callee->kind == EX_VAR) {
        const char *name = e->u.call.callee->u.var.name;
        if (strcmp(name, "__builtin_inf") == 0 || strcmp(name, "__builtin_inff") == 0 ||
            strcmp(name, "__builtin_infl") == 0 || strcmp(name, "__builtin_huge_val") == 0 ||
            strcmp(name, "__builtin_huge_valf") == 0 || strcmp(name, "__builtin_huge_vall") == 0 ||
            strcmp(name, "__builtin_nan") == 0 || strcmp(name, "__builtin_nanf") == 0 ||
            strcmp(name, "__builtin_nanl") == 0)
            return 1;
    }
    if (e->kind == EX_UNARY
        && (e->u.un.op == UOP_NEG || e->u.un.op == UOP_POS)
        && is_const_init(e->u.un.operand, globals))
        return 1;
    /* A constant integer expression: `(1u << 14) - 1u`, `-1`, etc. */
    long long _fold_tmp;
    if (fold_const_int(e, &_fold_tmp)) return 1;
    if (e->kind == EX_INIT_LIST) {
        for (int i = 0; i < e->u.init_list.num_elements; i++)
            if (!is_const_init(e->u.init_list.elements[i], globals)) return 0;
        return 1;
    }
    if (e->kind == EX_VAR) return 1;
    /* A member access on a global struct variable: `s.f` decays (if an array
     * member) to &s.f[0], a link-time constant.  The parser leaves e->type as
     * the default int, so we can't check the member's declared type here —
     * but any member access on a file-scope object is a constant address. */
    if (e->kind == EX_MEMBER && e->u.member.obj && e->u.member.obj->kind == EX_VAR) {
        if (!e->u.member.obj->u.var.pkg) {
            /* Unqualified name — check if it's a known global */
            if (symtable_find(globals, e->u.member.obj->u.var.name))
                return 1;
        }
    }
    /* Address of a file-scope object: `&g`, `&g.member`, `&g[i]`, `&(*ptr).member`, etc.
     * Also `&((T){...})` — GCC gives file-scope compound literals static storage, so
     * their address is a link-time constant.  The `.member` form (`&((T){...}).f`)
     * is handled below by the EX_MEMBER-on-compound-literal case. */
    if (e->kind == EX_ADDR) {
        const Expr *sub = e->u.addr.operand;
        while (sub && sub->kind == EX_CAST) sub = sub->u.cast.operand;
        if (sub && (sub->kind == EX_VAR || sub->kind == EX_MEMBER || sub->kind == EX_INDEX || sub->kind == EX_DEREF))
            return 1;
        if (sub && sub->kind == EX_COMPOUND_LITERAL)
            return is_const_init(sub->u.compound.init, globals);
    }
    /* Member access on a file-scope compound literal: `&((T){...}).f`.  The address
     * is a link-time constant (static storage + member offset). */
    if (e->kind == EX_MEMBER) {
        const Expr *obj = e->u.member.obj;
        while (obj && obj->kind == EX_CAST) obj = obj->u.cast.operand;
        if (obj && obj->kind == EX_COMPOUND_LITERAL)
            return is_const_init(obj->u.compound.init, globals);
    }
    return 0;
}

/* Number of scalar slots an aggregate swallows when its braces are elided. */
static int init_leaf_count(Type t) {
    if (t.kind == TY_ARRAY && t.elem_type) {
        if (t.length <= 0) return 0;
        return t.length * init_leaf_count(*t.elem_type);
    }
    if (t.kind == TY_STRUCT) {
        const StructDef *sd = struct_registry_find_c(g_sema_structs, t.tag);
        if (!sd || sd->num_members == 0) return 1;
        if (sd->is_union) return init_leaf_count(sd->members[0].type);
        int total = 0;
        for (int i = 0; i < sd->num_members; i++) {
            if (sd->members[i].bit_width == 0) continue;
            total += init_leaf_count(sd->members[i].type);
        }
        return total;
    }
    return 1;
}

/* Could this element denote a whole aggregate value?  Initializer lists are
 * normalized before the elements are type-checked, so the decision to regroup
 * flat elements into an elided sub-aggregate has to be made syntactically:
 * `struct P a[2] = {p, q}` initializes one slot per expression and must be
 * left alone, while `struct P a[2] = {1, 2, 3, 4}` is regrouped. */
static int init_elem_may_be_aggregate(const Expr *e) {
    switch (e->kind) {
    case EX_VAR: case EX_CALL: case EX_MEMBER: case EX_INDEX: case EX_DEREF:
    case EX_COMPOUND_LITERAL: case EX_ASSIGN: case EX_TERNARY: case EX_COMMA:
    case EX_CAST: case EX_BINOP:
        return 1;
    default:
        return 0;
    }
}

/* Normalize a (possibly designated) initializer list against `target`:
 *  1. infer an array length of 0 from the element count / designators,
 *  2. validate every designator (array index in range, struct member exists,
 *     no mixing of array designators with non-array targets or member
 *     designators with non-struct targets),
 *  3. expand to a fully positional list, filling gaps with int 0.
 * Mutates `list` in place (its desig arrays are consumed and NULLed).  For
 * nested init lists, recurses against the element / member type so their own
 * designators resolve too.  C99 designated-initializer semantics: a designator
 * sets the current position, and subsequent positional elements continue from
 * the slot after it (`[1]=10, 20` → a[1]=10, a[2]=20). */
/* Top-level member index that owns `name`, walking unnamed nested
 * struct/union members the same way as struct_lookup_member.  Named nested
 * structs do not export their fields.  Returns -1 if not found. */
static int designator_member_index(const StructDef *sd, const char *name) {
    if (!sd || !name) return -1;
    for (int j = 0; j < sd->num_members; j++) {
        const StructMember *m = &sd->members[j];
        if (m->name && m->name[0] && strcmp(m->name, name) == 0)
            return j;
        if ((!m->name || !m->name[0]) && m->type.kind == TY_STRUCT
            && m->type.tag && g_sema_structs) {
            const StructDef *nested =
                struct_registry_find_c(g_sema_structs, m->type.tag);
            if (nested && designator_member_index(nested, name) >= 0)
                return j;
        }
    }
    return -1;
}

static int designator_is_direct_member(const StructDef *sd, int idx,
                                       const char *name) {
    if (!sd || !name || idx < 0 || idx >= sd->num_members) return 0;
    const char *mn = sd->members[idx].name;
    return mn && mn[0] && strcmp(mn, name) == 0;
}

/* Wrap `elem` as `{ .name = elem }` so a designator that named a field of an
 * anonymous nested struct/union can be applied to that nested type. */
static Expr *wrap_anon_designator(const char *name, Expr *elem, SourceLoc loc) {
    Expr **els = malloc(sizeof(Expr *));
    if (!els) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
    els[0] = elem;
    Expr *list = expr_new_init_list(els, 1, loc);
    list->u.init_list.desig_kind[0] = 1;
    list->u.init_list.desig_index[0] = -1;
    list->u.init_list.desig_member[0] = xstrdup(name);
    return list;
}

static int init_list_slot_pos(const Type *target, const StructDef *sd,
                              const Expr *list, int i, int *cursor,
                              int *member_idx_out, int N) {
    int pos;
    int member_idx = 0;
    if (list->u.init_list.desig_kind[i] == 0) {
        pos = list->u.init_list.desig_index[i];
        member_idx = pos;
        if (sd && sd->is_union) pos = 0;
    } else if (list->u.init_list.desig_kind[i] == 1) {
        const char *name = list->u.init_list.desig_member[i];
        pos = designator_member_index(sd, name);
        member_idx = pos;
        if (sd && sd->is_union) pos = 0;
    } else {
        if (sd && !sd->is_union) {
            while (*cursor < sd->num_members
                   && (sd->members[*cursor].name == NULL
                       || sd->members[*cursor].name[0] == '\0')
                   && sd->members[*cursor].bit_width >= 0) {
                (*cursor)++;
            }
        }
        pos = *cursor;
        member_idx = (sd && sd->is_union) ? 0 : pos;
    }
    if (member_idx_out) *member_idx_out = member_idx;
    (void)target;
    (void)N;
    return pos;
}

/* Overlay `src` onto an already-expanded positional `dst` so a later nested
 * designator (`.i.b = 7` after `.i = {1,2}`) updates only the named subobject. */
static void overlay_init_list(Type *target, Expr *dst, Expr *src, SourceLoc loc) {
    if (!dst || !src || dst->kind != EX_INIT_LIST || src->kind != EX_INIT_LIST)
        return;
    int n = src->u.init_list.num_elements;
    int N = dst->u.init_list.num_elements;
    const StructDef *sd = NULL;
    if (target->kind == TY_STRUCT)
        sd = struct_registry_find_c(g_sema_structs, target->tag);
    int cursor = 0;
    for (int i = 0; i < n; i++) {
        int member_idx = 0;
        int pos = init_list_slot_pos(target, sd, src, i, &cursor, &member_idx, N);
        if (pos < 0 || pos >= N) continue;
        Expr *elem = src->u.init_list.elements[i];
        src->u.init_list.elements[i] = NULL;
        if (src->u.init_list.desig_kind[i] == 1 && sd && member_idx >= 0) {
            const char *nm = src->u.init_list.desig_member[i];
            if (nm && !designator_is_direct_member(sd, member_idx, nm))
                elem = wrap_anon_designator(nm, elem, loc);
        }
        Type *slot_type = NULL;
        if (target->kind == TY_ARRAY || target->is_vector) slot_type = target->elem_type;
        else if (target->kind == TY_STRUCT && sd && member_idx >= 0
                 && member_idx < sd->num_members)
            slot_type = &sd->members[member_idx].type;
        Expr *dst_el = dst->u.init_list.elements[pos];
        if (elem && elem->kind == EX_INIT_LIST && slot_type
            && dst_el && dst_el->kind == EX_INIT_LIST) {
            Type sub = type_clone(*slot_type);
            overlay_init_list(&sub, dst_el, elem, loc);
            type_free(&sub);
            expr_free(elem);
        } else if (elem && elem->kind == EX_INIT_LIST && slot_type) {
            Type sub = type_clone(*slot_type);
            normalize_init_list(&sub, elem, loc, 1);
            type_free(&sub);
            expr_free(dst->u.init_list.elements[pos]);
            dst->u.init_list.elements[pos] = elem;
        } else {
            expr_free(dst->u.init_list.elements[pos]);
            dst->u.init_list.elements[pos] = elem;
        }
        cursor = pos + 1;
        if (sd && !sd->is_union) {
            while (cursor < sd->num_members
                   && (sd->members[cursor].name == NULL
                       || sd->members[cursor].name[0] == '\0')
                   && sd->members[cursor].bit_width >= 0) {
                cursor++;
            }
        }
    }
}

static void normalize_init_list(Type *target, Expr *list, SourceLoc loc, int is_nested) {
    if (fakecc_had_error()) return;
    int n = list->u.init_list.num_elements;
    /* 1. Infer array length for an empty `[]` declarator. */
    if (target->kind == TY_ARRAY && target->length < 0 && !target->vla_dim) {
        int len = n;
        /* With elided braces each slot swallows several flat elements, so
         * `int m[][3] = {1,2,3,4,5,6}` has length 2, not 6. */
        if (target->elem_type
            && (target->elem_type->kind == TY_ARRAY
                || target->elem_type->kind == TY_STRUCT)
            && n > 0 && list->u.init_list.elements[0]->kind != EX_INIT_LIST
            && !init_elem_may_be_aggregate(list->u.init_list.elements[0])
            && !(list->u.init_list.elements[0]->kind == EX_STR
                 && target->elem_type->kind == TY_ARRAY)) {
            int per = init_leaf_count(*target->elem_type);
            if (per > 0) len = (n + per - 1) / per;
        }
        for (int i = 0; i < n; i++)
            if (list->u.init_list.desig_kind[i] == 0
                && list->u.init_list.desig_index[i] + 1 > len)
                len = list->u.init_list.desig_index[i] + 1;
        if (n == 1 && list->u.init_list.elements[0]
            && list->u.init_list.elements[0]->kind == EX_STR
            && target->elem_type && target->elem_type->kind == TY_INT
            && target->elem_type->width == 1) {
            int sl = list->u.init_list.elements[0]->u.str.len + 1;
            if (sl > len) len = sl;
        }
        target->length = len;
    }
    /* Determine the output slot count */
    int N;
    const StructDef *sd = NULL;
    switch (target->kind) {
    case TY_ARRAY: N = target->length; break;
    case TY_STRUCT:
        sd = struct_registry_find_c(g_sema_structs, target->tag);
        if (!sd)
            {
                die_at(loc.file, loc.line, loc.col,
                   "unknown struct 'struct %s'", target->tag);
                return;
            }
        N = sd->is_union ? 1 : sd->num_members;
        break;
    default:
        if (target->is_vector) N = target->length;
        else N = 1;
        break;  /* scalar: `int x = {5}` */
    }
    /* C99 6.7.8p14: `char s[] = { "hi" }` / `char s[4] = { "hi" }` copies
     * the string into the array.  Leave the list as a single EX_STR. */
    if (target->kind == TY_ARRAY && target->elem_type
        && target->elem_type->kind == TY_INT && target->elem_type->width == 1
        && n == 1 && list->u.init_list.elements[0]
        && list->u.init_list.elements[0]->kind == EX_STR) {
        if (target->length >= 0 && list->u.init_list.elements[0]->u.str.len > target->length) {
            die_at(loc.file, loc.line, loc.col,
                   "initializer-string for array of chars is too long");
            return;
        }
        return;
    }
    /* 2. Validate designators. */
    for (int i = 0; i < n; i++) {
        int kind = list->u.init_list.desig_kind[i];
        if (kind == 0) {  /* [index] */
            if (target->kind != TY_ARRAY && !target->is_vector)
                {
                    die_at(loc.file, loc.line, loc.col,
                       "array index designator used on a non-array type");
                    return;
                }
            int idx = list->u.init_list.desig_index[i];
            if (idx < 0 || idx >= N)
                {
                    die_at(loc.file, loc.line, loc.col,
                       "designator index %d out of range for array of length %d",
                       idx, N);
                    return;
                }
        } else if (kind == 1) {  /* .member */
            if (target->kind != TY_STRUCT)
                {
                    die_at(loc.file, loc.line, loc.col,
                       "member designator used on a non-struct type");
                    return;
                }
        }
    }
    /* 3. Build the dense array of N initialized elements.
     *
     * Arrays/vectors leave unmentioned slots as NULL rather than synthesizing
     * an integer 0 for every gap.  `char buf[32753] = "A"` would otherwise
     * allocate ~32k AST nodes and, after lowering, tens of thousands of
     * stores — enough to time out an unoptimized host compiler.  Implicit
     * zeros are the object's background (emit_zero_bytes / pack_init calloc).
     * Structs still materialize a 0 per member: N is small and overlay /
     * pack_init index members by position. */
    /* An incomplete or flexible array has length -1; there is no dense array
     * to build (and calloc((size_t)-1, ...) would overflow). */
    if (N < 0) {
        die_at(loc.file, loc.line, loc.col,
               "initialization of an incomplete array");
        return;
    }
    Expr **out = calloc((size_t)N, sizeof(Expr *));
    if (!out) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }    if (target->kind != TY_ARRAY && !target->is_vector) {
        for (int i = 0; i < N; i++)
            out[i] = expr_new_int(0, loc);
    }

    int cursor = 0;
    int last_union_member_idx = 0;
    for (int i = 0; i < n; i++) {
        int pos;
        int member_idx = 0;
        if (list->u.init_list.desig_kind[i] == 0) {
            pos = list->u.init_list.desig_index[i];
            member_idx = pos;
            if (sd && sd->is_union) pos = 0;
        } else if (list->u.init_list.desig_kind[i] == 1) {
            const char *name = list->u.init_list.desig_member[i];
            pos = designator_member_index(sd, name);
            if (pos < 0)
                {
                    die_at(loc.file, loc.line, loc.col,
                       "struct '%s' has no member named '%s'",
                       target->tag, name);
                    return;
                }
            member_idx = pos;
            if (sd && sd->is_union) pos = 0;
        } else {
            if (sd && !sd->is_union) {
                while (cursor < sd->num_members
                       && (sd->members[cursor].name == NULL
                           || sd->members[cursor].name[0] == '\0')
                       && sd->members[cursor].bit_width >= 0) {
                    cursor++;
                }
            }
            pos = cursor;
            member_idx = (sd && sd->is_union) ? 0 : pos;
        }
        if (pos < 0 || pos >= N) {
            die_at(loc.file, loc.line, loc.col,
                   "excess elements in initializer");
            return;
        }
        Expr *elem = list->u.init_list.elements[i];
        if (list->u.init_list.desig_kind[i] == 1 && sd && member_idx >= 0) {
            const char *nm = list->u.init_list.desig_member[i];
            if (nm && !designator_is_direct_member(sd, member_idx, nm))
                elem = wrap_anon_designator(nm, elem, loc);
        }

        /* Reject initialization of flexible array member in a nested context (e.g. within an array).
         * GCC: "initialization of flexible array member in a nested context".
         * Empty initializer list `{}` is allowed (no elements initialized). */
        if (is_nested && target->kind == TY_STRUCT && sd && member_idx == sd->num_members - 1) {
            const Type *mtype = &sd->members[member_idx].type;
            if (mtype->kind == TY_ARRAY && mtype->length <= 0 && mtype->elem_type) {
                int is_empty = (elem->kind == EX_INIT_LIST && elem->u.init_list.num_elements == 0);
                if (!is_empty) {
                    sema_report_error(&elem->loc,
                                      "initialization of flexible array member in a nested context");
                }
            }
        }

        /* Bracket elision: if this slot expects an aggregate (array or struct)
         * and the source element is a scalar (not an EX_INIT_LIST), collect as
         * many flat elements as that aggregate has leaf scalars and wrap them
         * in a synthetic EX_INIT_LIST.  Done before recursion so that the
         * rest of the pipeline only ever sees fully braced initializers. */
        Type *slot_type = NULL;
        if (target->kind == TY_ARRAY || target->is_vector) slot_type = target->elem_type;
        else if (target->kind == TY_STRUCT && sd && member_idx < sd->num_members)
            slot_type = &sd->members[member_idx].type;
        if (slot_type && elem->kind != EX_INIT_LIST
            && (slot_type->kind == TY_ARRAY || slot_type->kind == TY_STRUCT)
            && !init_elem_may_be_aggregate(elem)
            && !(elem->kind == EX_STR && slot_type->kind == TY_ARRAY)) {
            int want = init_leaf_count(*slot_type);
            int take = 1;
            while (take < want && i + take < n
                   && list->u.init_list.desig_kind[i + take] == -1)
                take++;
            Expr **sub = malloc(take * sizeof(Expr *));
            for (int k = 0; k < take; k++)
                sub[k] = list->u.init_list.elements[i + k];
            elem = expr_new_init_list(sub, take, elem->loc);
            i += take - 1;
        }

        /* Recurse into a nested init list to resolve its designators against
         * the sub-type.  Array element types are mutated in place (so an
         * inferred inner length propagates to the parent type); struct member
         * types carry explicit lengths and need no propagation (clone to avoid
         * mutating the shared struct definition). */
        if (elem->kind == EX_INIT_LIST) {
            int overlaid = 0;
            if (out[pos] && out[pos]->kind == EX_INIT_LIST) {
                if (target->kind == TY_ARRAY || target->is_vector) {
                    overlay_init_list(target->elem_type, out[pos], elem, elem->loc);
                    expr_free(elem);
                    overlaid = 1;
                } else if (sd && member_idx < sd->num_members) {
                    Type sub_type = type_clone(sd->members[member_idx].type);
                    overlay_init_list(&sub_type, out[pos], elem, elem->loc);
                    type_free(&sub_type);
                    expr_free(elem);
                    overlaid = 1;
                }
            }
            if (!overlaid) {
                if (target->kind == TY_ARRAY || target->is_vector)
                    normalize_init_list(target->elem_type, elem, elem->loc, 1);
                else if (sd && member_idx < sd->num_members) {
                    Type sub_type = type_clone(sd->members[member_idx].type);
                    normalize_init_list(&sub_type, elem, elem->loc, 1);
                    type_free(&sub_type);
                }
                expr_free(out[pos]);
                out[pos] = elem;
            }
        } else {
            expr_free(out[pos]); /* drop the gap-fill zero (or previous element) */
            out[pos] = elem;
        }
        /* C99 6.7.8p17: after a designator the next current object is the one
         * following the designated slot, so the cursor always advances to
         * pos+1 (for positional inits pos == cursor, so this is just +1). */
        cursor = pos + 1;
        last_union_member_idx = member_idx;
        if (sd && !sd->is_union) {
            while (cursor < sd->num_members
                   && (sd->members[cursor].name == NULL
                       || sd->members[cursor].name[0] == '\0')
                   && sd->members[cursor].bit_width >= 0) {
                cursor++;
            }
        }
    }
    /* Consume the now-obsolete desig arrays and the old element buffer.
     * Unions keep a single slot but must remember which member was named
     * (pack_init / lower_init_list look up desig_member). */
    int keep_union_desig = (sd && sd->is_union);
    char *union_member_name = NULL;
    int union_desig_kind = 1;
    int union_desig_index = 0;
    if (keep_union_desig) {
        if (last_union_member_idx < 0 || last_union_member_idx >= sd->num_members)
            last_union_member_idx = 0;
        const char *mn = sd->num_members > 0
            ? sd->members[last_union_member_idx].name : NULL;
        if (mn && mn[0]) {
            union_member_name = xstrdup(mn);
            union_desig_kind = 1;
            union_desig_index = 0;
        } else {
            union_desig_kind = 0;
            union_desig_index = last_union_member_idx;
        }
    }
    for (int i = 0; i < n; i++)
        free(list->u.init_list.desig_member[i]);
    free(list->u.init_list.desig_kind);
    free(list->u.init_list.desig_index);
    free(list->u.init_list.desig_member);
    free(list->u.init_list.elements);
    list->u.init_list.elements = out;
    list->u.init_list.num_elements = N;
    if (keep_union_desig) {
        list->u.init_list.desig_kind = malloc(sizeof(int));
        list->u.init_list.desig_index = malloc(sizeof(int));
        list->u.init_list.desig_member = calloc(1, sizeof(char *));
        if (!list->u.init_list.desig_kind || !list->u.init_list.desig_index
            || !list->u.init_list.desig_member) {
            fprintf(stderr, "fakecc: OOM\n");
            exit(1);
        }
        list->u.init_list.desig_kind[0] = union_desig_kind;
        list->u.init_list.desig_index[0] = union_desig_index;
        list->u.init_list.desig_member[0] = union_member_name;
    } else {
        list->u.init_list.desig_kind = NULL;
        list->u.init_list.desig_index = NULL;
        list->u.init_list.desig_member = NULL;
    }
}

/* Parser-time folding cannot see object types, so `T a[sizeof g / sizeof *g]`
 * is parsed as a VLA.  After the dimension is type-checked it is often an
 * ICE — complete it back to a fixed array so sizeof/object_size see a known
 * bound and the local is not a DYN_ALLOCA. */
static void try_fold_vla_type(Type *t) {
    while (t && t->kind == TY_ARRAY) {
        if (t->vla_dim) {
            Type dt = check_expr_inner(t->vla_dim);
            type_free(&dt);
            long long n = 0;
            if (fold_const_int(t->vla_dim, &n) && n >= 0) {
                expr_free(t->vla_dim);
                t->vla_dim = NULL;
                t->length = n;
            }
        }
        t = t->elem_type;
    }
}

static void check_decl_type(Stmt *s) {
    if (fakecc_had_error()) return;
    if (s->kind != ST_DECL) return;
    try_fold_vla_type(&s->u.decl.type);
    const Type *t = &s->u.decl.type;
    if (t->kind == TY_ARRAY) {
        const Type *elem = t;
        while (elem && elem->kind == TY_ARRAY) {
            if (s->u.decl.storage_class == 1 && elem->vla_dim) {
                die_at(s->loc.file, s->loc.line, s->loc.col,
                       "variable length array declaration cannot have 'static' storage duration");
                return;
            }
            elem = elem->elem_type;
        }
        if (elem) {
            if (elem->kind == TY_VOID) {
                die_at(s->loc.file, s->loc.line, s->loc.col,
                       "array type has incomplete element type 'void'");
                return;
            }
            if (elem->kind == TY_FUNC) {
                die_at(s->loc.file, s->loc.line, s->loc.col,
                       "'%s' declared as array of functions", s->u.decl.name);
                return;
            }
            if (elem->kind == TY_STRUCT && elem->tag) {
                int is_cplx = (strncmp(elem->tag, "__complex_", 10) == 0);
                if (!is_cplx && !struct_registry_find_c(g_sema_structs, elem->tag)) {
                    die_at(s->loc.file, s->loc.line, s->loc.col,
                           "array type has incomplete element type 'struct %s'", elem->tag);
                    return;
                }
            }
        }
    }
    if (s->u.decl.storage_class != 2 /* extern */) {
        if (t->kind == TY_STRUCT && t->tag) {
            int is_cplx = (strncmp(t->tag, "__complex_", 10) == 0);
            if (!is_cplx && !struct_registry_find_c(g_sema_structs, t->tag)) {
                die_at(s->loc.file, s->loc.line, s->loc.col,
                       "variable '%s' has incomplete type 'struct %s'",
                       s->u.decl.name, t->tag);
                return;
            }
        }
    }
}

static void check_stmt(Stmt *s, size_t scope_mark, int *has_return) {
    if (fakecc_had_error()) return;
    SymTable *st = g_check_st;
    Type discard;
    switch (s->kind) {
    case ST_DECL: {
        if (symtable_has_since(st, s->u.decl.name, scope_mark)) {
            die_at(s->loc.file, s->loc.line, s->loc.col,
                   "redeclaration of '%s'", s->u.decl.name);
            return;
        }
        /* A variable may not have type `void` (but `void*` is fine — that is a
         * pointer, TY_PTR). */
        if (s->u.decl.type.kind == TY_VOID)
            {
                die_at(s->loc.file, s->loc.line, s->loc.col,
                   "cannot declare variable '%s' of type void",
                   s->u.decl.name ? s->u.decl.name : "(null)");
                return;
            }
        check_decl_type(s);
        /* Block-scope `extern` (e.g. `extern void foo();`) or block-scope function
         * declaration (e.g. `float fx();`) is a declaration referring to a file-scope
         * symbol. It carries no local storage, so register the name in scope and break. */
        if (s->u.decl.type.kind == TY_FUNC || s->u.decl.storage_class == 2) {
            symtable_push(st, s->u.decl.name, s->u.decl.type, s->loc, s->u.decl.align, 0);
            break;
        }
        try_fold_vla_type(&s->u.decl.type);
        if (s->u.decl.init && s->u.decl.init->kind == EX_COMPOUND_LITERAL
            && s->u.decl.type.kind == TY_ARRAY) {
            if (s->u.decl.type.length < 0)
                s->u.decl.type.length = s->u.decl.init->u.compound.target_type.length;
            Expr *cl = s->u.decl.init;
            s->u.decl.init = cl->u.compound.init;
            cl->u.compound.init = NULL;
            expr_free(cl);
        }
        /* Infer array length from an empty `[]` declarator when initialized by
         * a string literal: `char s[] = "hi"` → length strlen+1.  (Array length
         * from an init list is inferred later by normalize_init_list.) */
        if (s->u.decl.type.kind == TY_ARRAY && s->u.decl.type.length < 0
            && s->u.decl.init && s->u.decl.init->kind == EX_STR
            && s->u.decl.type.elem_type
            && s->u.decl.type.elem_type->width == 1) {
            s->u.decl.type.length = s->u.decl.init->u.str.len + 1;
        }
        /* A string literal initializing a `char[]` is lowered as a per-byte
         * copy, not a pointer store — convert it to an init list of char
         * literals so it flows through the array-init path. */
        if (s->u.decl.init && s->u.decl.init->kind == EX_STR
            && s->u.decl.type.kind == TY_ARRAY && s->u.decl.type.elem_type
            && s->u.decl.type.elem_type->width == 1) {
            Expr *str = s->u.decl.init;
            int str_len = str->u.str.len;
            int target_len = (int)s->u.decl.type.length;
            if (target_len >= 0 && str_len > target_len) {
                die_at(str->loc.file, str->loc.line, str->loc.col,
                       "initializer-string for array of chars is too long");
                return;
            }
            int n = str_len + 1;
            if (target_len >= 0 && n > target_len) {
                n = target_len;
            }
            Expr **elems = malloc(n * sizeof(Expr *));
            for (int i = 0; i < n; i++) {
                if (i < str_len)
                    elems[i] = expr_new_int((unsigned char)str->u.str.bytes[i],
                                            str->loc);
                else
                    elems[i] = expr_new_int(0, str->loc);
            }
            Expr *list = expr_new_init_list(elems, n, str->loc);
            expr_free(str);
            s->u.decl.init = list;
        }
        /* Normalize a (possibly designated) init list: infer array length,
         * validate designators, expand to positional with zero-fill. */
        if (s->u.decl.init && s->u.decl.init->kind == EX_INIT_LIST)
            normalize_init_list(&s->u.decl.type, s->u.decl.init, s->loc, 0);
        symtable_push(st, s->u.decl.name, s->u.decl.type, s->loc, s->u.decl.align, s->u.decl.storage_class == 3);
        if (s->u.decl.init) {
            discard = check_expr_inner(s->u.decl.init);
            if (discard.kind == TY_VOID) {
                die_at(s->u.decl.init->loc.file, s->u.decl.init->loc.line, s->u.decl.init->loc.col,
                       "void value not ignored as it ought to be");
                return;
            }
            type_free(&discard);
            if (s->u.decl.init->kind != EX_INIT_LIST)
                coerce_arg_to_param(&s->u.decl.init, &s->u.decl.type);
        }
        break;
    }
    case ST_EXPR:
        if (s->u.expr && s->u.expr->kind == EX_CALL) {
            *has_return = 1;
        }
        discard = check_expr_inner(s->u.expr); type_free(&discard);
        break;
    case ST_RETURN:
        /* Bare `return;` is allowed only in a void function; `return expr;`
         * is forbidden in a void function unless the expression itself is
         * void (e.g. `return f();` where f returns void). */
        if (s->u.value == NULL) {
            if (g_sema_ret_type.kind != TY_VOID)
                {
                    die_at(s->loc.file, s->loc.line, s->loc.col,
                       "non-void function must return a value");
                    return;
                }
        } else {
            discard = check_expr_inner(s->u.value);
            if (g_sema_ret_type.kind == TY_VOID) {
                if (discard.kind != TY_VOID)
                    {
                        die_at(s->loc.file, s->loc.line, s->loc.col,
                           "void function cannot return a value");
                        return;
                    }
            } else {
                if (!type_is_same(&g_sema_ret_type, &discard)) {
                    int is_ret_cplx = (g_sema_ret_type.kind == TY_STRUCT && g_sema_ret_type.tag && strncmp(g_sema_ret_type.tag, "__complex_", 10) == 0);
                    int is_val_cplx = (discard.kind == TY_STRUCT && discard.tag && strncmp(discard.tag, "__complex_", 10) == 0);
                    if (discard.kind == TY_VOID) {
                        die_at(s->loc.file, s->loc.line, s->loc.col,
                               "void value not ignored as it ought to be");
                        return;
                    }
                    if ((g_sema_ret_type.kind == TY_STRUCT && !is_ret_cplx) ||
                        (discard.kind == TY_STRUCT && !is_val_cplx)) {
                        die_at(s->loc.file, s->loc.line, s->loc.col,
                               "returning incompatible struct type");
                        return;
                    }
                    if ((g_sema_ret_type.kind != TY_STRUCT && discard.kind != TY_STRUCT) ||
                        is_ret_cplx || is_val_cplx ||
                        (g_sema_ret_type.kind == TY_INT && g_sema_ret_type.width == 16) ||
                        (discard.kind == TY_INT && discard.width == 16)) {
                        Expr *c = expr_new_cast(type_clone(g_sema_ret_type), s->u.value, s->loc);
                        set_type(c, type_clone(g_sema_ret_type));
                        s->u.value = c;
                    }
                }
            }
            type_free(&discard);
        }
        *has_return = 1;
        break;
    case ST_IF:
        discard = check_expr_inner(s->u.if_s.cond);
        if (discard.kind == TY_VOID) {
            sema_report_error(&s->u.if_s.cond->loc,
                    "void value not ignored as it ought to be");
        } else if (!type_is_scalar(discard)) {
            die_at(s->u.if_s.cond->loc.file, s->u.if_s.cond->loc.line, s->u.if_s.cond->loc.col,
                   "statement requires expression of scalar type");
            return;
        }
        type_free(&discard);
        check_stmt(s->u.if_s.then_s, scope_mark, has_return);
        if (s->u.if_s.else_s) check_stmt(s->u.if_s.else_s, scope_mark, has_return);
        break;
    case ST_WHILE:
        discard = check_expr_inner(s->u.while_s.cond);
        if (!type_is_scalar(discard)) {
            die_at(s->u.while_s.cond->loc.file, s->u.while_s.cond->loc.line, s->u.while_s.cond->loc.col,
                   "statement requires expression of scalar type");
            return;
        }
        type_free(&discard);
        g_sema_loop_depth++;
        check_stmt(s->u.while_s.body, scope_mark, has_return);
        g_sema_loop_depth--;
        break;
    case ST_DO_WHILE:
        /* Condition must be scalar (int, pointer, or float). */
        discard = check_expr_inner(s->u.do_s.cond);
        if (!type_is_scalar(discard))
            {
                die_at(s->loc.file, s->loc.line, s->loc.col,
                   "statement requires expression of scalar type");
                return;
            }
        type_free(&discard);
        g_sema_loop_depth++;
        check_stmt(s->u.do_s.body, scope_mark, has_return);
        g_sema_loop_depth--;
        break;
    case ST_GOTO:
        if (s->u.goto_s.target_expr) {
            /* computed goto: goto *ptr_expr; — check the expression */
            Type t = check_expr_inner(s->u.goto_s.target_expr);
            type_free(&t);
        } else if (!labelset_has(g_sema_labels, s->u.goto_s.target)) {
            die_at(s->loc.file, s->loc.line, s->loc.col,
                   "use of undeclared label '%s'", s->u.goto_s.target);
            return;
        }
        break;
    case ST_LABEL:
        /* Validate the wrapped statement; the label name itself was
         * registered during the collect_labels pre-pass. */
        check_stmt(s->u.label_s.stmt, scope_mark, has_return);
        break;
    case ST_SWITCH: {
        /* Condition must be integer. */
        Type ct = check_expr_inner(s->u.switch_s.cond);
        if (ct.kind != TY_INT)
            {
                die_at(s->u.switch_s.cond->loc.file,
                   s->u.switch_s.cond->loc.line,
                   s->u.switch_s.cond->loc.col,
                   "switch condition must be integer");
                return;
            }
        type_free(&ct);
        /* Switch introduces a breakable scope. */
        g_sema_loop_depth++;
        if (s->u.switch_s.body)
            check_stmt(s->u.switch_s.body, scope_mark, has_return);
        for (int i = 0; i < s->u.switch_s.num_cases; i++) {
            SwitchCase *arm = &s->u.switch_s.cases[i];
            for (size_t j = 0; j < arm->stmts.len; j++)
                check_stmt(&arm->stmts.data[j], scope_mark, has_return);
        }
        g_sema_loop_depth--;
        break;
    }
    case ST_FOR: {
        /* for-loop introduces its own scope for the init decl (if any). */
        size_t mark = symtable_enter_scope(st);
        if (s->u.for_s.init) {
            if (s->u.for_s.init->kind == ST_BLOCK) {
                /* Multi-declarator init (`for (T a, b; ...)`) is wrapped in a
                 * block by the parser.  Check its stmts directly in the for
                 * scope so the declarations are visible in cond/step/body
                 * (check_stmt_list would add a nested scope, hiding them). */
                StmtArray *blk = &s->u.for_s.init->u.block;
                for (size_t i = 0; i < blk->len; i++)
                    check_stmt(&blk->data[i], mark, has_return);
            } else {
                check_stmt(s->u.for_s.init, mark, has_return);
            }
        }
        if (s->u.for_s.cond) {
            discard = check_expr_inner(s->u.for_s.cond);
            if (!type_is_scalar(discard)) {
                die_at(s->u.for_s.cond->loc.file, s->u.for_s.cond->loc.line, s->u.for_s.cond->loc.col,
                       "statement requires expression of scalar type");
                return;
            }
            type_free(&discard);
        }
        if (s->u.for_s.step) {
            discard = check_expr_inner(s->u.for_s.step); type_free(&discard);
        }
        g_sema_loop_depth++;
        check_stmt(s->u.for_s.body, mark, has_return);
        g_sema_loop_depth--;
        symtable_leave_scope(st, mark);
        if (!s->u.for_s.cond) {
            /* Infinite loop without condition never falls through. */
            *has_return = 1;
        }
        break;
    }
    case ST_BREAK:
    case ST_CONTINUE:
        if (g_sema_loop_depth == 0) {
            die_at(s->loc.file, s->loc.line, s->loc.col,
                   "'%s' outside of loop", s->kind == ST_BREAK ? "break" : "continue");
            return;
        }
        break;
    case ST_BLOCK:
        check_stmt_list(&s->u.block, has_return);
        break;
    }
}

static int sema_check_in_pkg_impl(const TranslationUnit *tu_const, int require_main,
                                  PkgContext *ctx) {
    fakecc_clear_error();
    g_sema_error_count = 0;
    TranslationUnit *tu = (TranslationUnit *)tu_const;
    g_sema_structs = &tu->structs;
    g_sema_pkg = ctx;
    g_sema_tu = tu_const;

    /* Package name is unrestricted; the linker still requires a `main`
     * symbol for executables. */

    ftab_cur_init();
    int has_main = 0;
    for (size_t i = 0; i < tu->functions.len; i++) {
        FunctionDecl *fn = &tu->functions.data[i];
        const FunSig *ex = ftab_lookup(fn->name);
        if (ex) {
            if (fn->is_extern) {
                /* A new `extern` decl never overrides what we already have
                 * (whether another decl or a real definition).  Silently
                 * drop the redundant declaration. */
                continue;
            }
            if (ex->is_external) {
                /* We had only an `extern` decl; this definition replaces
                 * it in place so the slot keeps its position. */
                ftab_fill_extern(ex, fn);
                if (strcmp(fn->name, "main") == 0) has_main = 1;
                continue;
            }
            return die_at(fn->loc.file, fn->loc.line, fn->loc.col,
                   "redefinition of function '%s'", fn->name);
        }
        ftab_add(fn);
        if (strcmp(fn->name, "main") == 0 && !fn->is_extern) has_main = 1;
    }
    if (!has_main && require_main) {
        return die_at(tu->package.loc.file, tu->package.loc.line, tu->package.loc.col,
               "no 'main' function defined");
    }

    /* Import every function from imported packages and from the current
     * package (sibling files) before checking bodies — see tu_ensure_extern_func. */
    /* Same-package sibling exports are visible unqualified.  Imported
     * packages are NOT — those require pkg.sym (see EX_VAR / EX_MEMBER). */
    if (g_sema_pkg && tu->package.name) {
        Package *cur = pkg_find(g_sema_pkg, tu->package.name);
        ftab_import_pkg(tu, cur);
        import_pkg_globals(tu, cur);
    }
    /* Imported packages: extern globals (runtime.stdout) and extern function
     * decls so IR can FADDR `runtime.printf` used as a value.  Do not push
     * imported funcs into FunTable — that would make `printf` work bare. */
    if (g_sema_pkg) {
        for (size_t i = 0; i < tu->imports.len; i++) {
            Package *ip = pkg_find(g_sema_pkg, tu->imports.data[i].name);
            import_pkg_globals(tu, ip);
            if (!ip) continue;
            for (size_t f = 0; f < ip->nfuncs; f++)
                tu_ensure_extern_func(tu, &ip->funcs[f]);
        }
    }

    /* Type-check global variable declarations.  Store their names in a
     * long-lived symbol table so functions can see them.  Enforce no
     * duplicates against each other or against function names. */
    SymTable globals;
    symtable_init(&globals);
    check_set_st(&globals);
    for (size_t i = 0; i < tu->globals.len; i++) {
        Stmt *s = &tu->globals.data[i];
        if (s->kind != ST_DECL) continue;
        check_decl_type(s);
        if (symtable_has_since(&globals, s->u.decl.name, 0)) {
            Stmt *prev = NULL;
            for (size_t k = 0; k < i; k++) {
                if (tu->globals.data[k].kind == ST_DECL &&
                    strcmp(tu->globals.data[k].u.decl.name, s->u.decl.name) == 0) {
                    prev = &tu->globals.data[k];
                    break;
                }
            }
            if (prev) {
                if (prev->u.decl.init && s->u.decl.init) {
                    return die_at(s->loc.file, s->loc.line, s->loc.col,
                           "redefinition of global '%s'", s->u.decl.name);
                }
                if (!types_compatible_globals(&prev->u.decl.type, &s->u.decl.type)) {
                    return die_at(s->loc.file, s->loc.line, s->loc.col,
                           "conflicting types for '%s'", s->u.decl.name);
                }
                /* C99 6.2.2: a file-scope `static` after a non-static
                 * declaration (or the reverse, except `extern` restating
                 * internal linkage) is a constraint violation. */
                {
                    int prev_sc = prev->u.decl.storage_class;
                    int new_sc = s->u.decl.storage_class;
                    if (new_sc == 1 && prev_sc != 1) {
                        return die_at(s->loc.file, s->loc.line, s->loc.col,
                               "static declaration of '%s' follows non-static declaration",
                               s->u.decl.name);
                    }
                    if (prev_sc == 1 && new_sc == 0) {
                        return die_at(s->loc.file, s->loc.line, s->loc.col,
                               "non-static declaration of '%s' follows static declaration",
                               s->u.decl.name);
                    }
                }
                /* `extern int x; int x;` — the tentative definition gives the
                 * object storage.  Keep the first Stmt and drop the duplicate. */
                if (!s->u.decl.init && prev->u.decl.storage_class == 2
                    && s->u.decl.storage_class != 2)
                    prev->u.decl.storage_class = s->u.decl.storage_class;
                /* C99 composite type: complete a tentative incomplete array. */
                if (prev->u.decl.type.kind == TY_ARRAY
                    && s->u.decl.type.kind == TY_ARRAY) {
                    if (prev->u.decl.type.length <= 0 && s->u.decl.type.length > 0)
                        prev->u.decl.type.length = s->u.decl.type.length;
                    else if (s->u.decl.type.length <= 0 && prev->u.decl.type.length > 0)
                        s->u.decl.type.length = prev->u.decl.type.length;
                    if (prev->u.decl.type.length > 0)
                        symtable_push(&globals, prev->u.decl.name, prev->u.decl.type,
                                      prev->loc, prev->u.decl.align, 0);
                }
                if (!prev->u.decl.init && s->u.decl.init) {
                    prev->u.decl.init = s->u.decl.init;
                    prev->u.decl.type = s->u.decl.type;
                    if (s->u.decl.storage_class != 2)
                        prev->u.decl.storage_class = s->u.decl.storage_class;
                    /* Update symbol table with completed type */
                    symtable_push(&globals, prev->u.decl.name, prev->u.decl.type, prev->loc, prev->u.decl.align, 0);
                    if (prev->u.decl.type.kind == TY_ARRAY && prev->u.decl.type.length < 0
                        && prev->u.decl.init && prev->u.decl.init->kind == EX_STR
                        && prev->u.decl.type.elem_type
                        && prev->u.decl.type.elem_type->width == 1) {
                        prev->u.decl.type.length = prev->u.decl.init->u.str.len + 1;
                    }
                    if (prev->u.decl.init && prev->u.decl.init->kind == EX_INIT_LIST)
                        normalize_init_list(&prev->u.decl.type, prev->u.decl.init, prev->loc, 0);
                    if (!is_const_init(prev->u.decl.init, &globals))
                        {
                            return die_at(prev->loc.file, prev->loc.line, prev->loc.col,
                               "global '%s' initializer must be a compile-time constant",
                               prev->u.decl.name);
                        }
                }
                s->kind = ST_EXPR;
                s->u.expr = NULL;
                continue;
            }
        }
        if (ftab_lookup(s->u.decl.name)) {
            if (s->u.decl.type.kind == TY_FUNC) {
                /* GCC: `extern __typeof (f) f __asm__ ("name")` restates f
                 * with an assembler name.  Apply the rename; do not treat
                 * it as a conflicting object. */
                if (s->u.decl.alias_target) {
                    for (size_t fi = 0; fi < tu->functions.len; fi++) {
                        if (strcmp(tu->functions.data[fi].name, s->u.decl.name) == 0) {
                            if (!tu->functions.data[fi].alias_target)
                                tu->functions.data[fi].alias_target =
                                    xstrdup(s->u.decl.alias_target);
                            break;
                        }
                    }
                }
                s->kind = ST_EXPR;
                s->u.expr = NULL;
                continue;
            }
            return die_at(s->loc.file, s->loc.line, s->loc.col,
                   "global '%s' conflicts with a function of the same name",
                   s->u.decl.name);
        }
        if (s->u.decl.init && s->u.decl.init->kind == EX_COMPOUND_LITERAL
            && s->u.decl.type.kind == TY_ARRAY) {
            if (s->u.decl.type.length < 0)
                s->u.decl.type.length = s->u.decl.init->u.compound.target_type.length;
            Expr *cl = s->u.decl.init;
            s->u.decl.init = cl->u.compound.init;
            cl->u.compound.init = NULL;
            expr_free(cl);
        }
        /* Infer array length from an empty `[]` declarator when initialized by
         * a string literal: `char s[] = "hi"` → length strlen+1.  (Array length
         * from an init list is inferred later by normalize_init_list.) */
        if (s->u.decl.type.kind == TY_ARRAY && s->u.decl.init && s->u.decl.init->kind == EX_STR
            && s->u.decl.type.elem_type
            && s->u.decl.type.elem_type->width == 1) {
            if (s->u.decl.type.length < 0) {
                s->u.decl.type.length = s->u.decl.init->u.str.len + 1;
            } else if (s->u.decl.init->u.str.len > s->u.decl.type.length) {
                return die_at(s->u.decl.init->loc.file, s->u.decl.init->loc.line, s->u.decl.init->loc.col,
                       "initializer-string for array of chars is too long");
            }
        }
        try_fold_vla_type(&s->u.decl.type);
        /* Normalize a (possibly designated) init list: infer array length,
         * validate designators, expand to positional with zero-fill. */
        if (s->u.decl.init && s->u.decl.init->kind == EX_INIT_LIST)
            normalize_init_list(&s->u.decl.type, s->u.decl.init, s->loc, 0);
        symtable_push(&globals, s->u.decl.name, s->u.decl.type, s->loc, s->u.decl.align, s->u.decl.storage_class == 3);
        if (s->u.decl.init) {
            /* A global's initializer must be a compile-time constant. */
            if (!is_const_init(s->u.decl.init, &globals))
                {
                    return die_at(s->loc.file, s->loc.line, s->loc.col,
                       "global initializer must be a constant");
                }
            Type dt = check_expr_inner(s->u.decl.init);
            type_free(&dt);
            if (s->u.decl.init->kind != EX_INIT_LIST)
                coerce_arg_to_param(&s->u.decl.init, &s->u.decl.type);
        }
    }

    for (size_t i = 0; i < tu->functions.len; i++) {
        FunctionDecl *fn = &tu->functions.data[i];

        /* Prototype-only `extern` decls have no body.  GNU89 `extern inline`
         * definitions keep is_extern (so they are not emitted) but still have
         * a body that must be type-checked. */
        if (fn->is_extern && fn->body.len == 0) continue;

        SymTable st;
        symtable_init(&st);
        /* Import globals (as a fixed lower scope). */
        for (size_t g = 0; g < globals.len; g++) {
            symtable_push(&st, globals.data[g].name, globals.data[g].type,
                          globals.data[g].loc, globals.data[g].align, globals.data[g].is_register);
        }

        size_t mark = symtable_enter_scope(&st);
        check_set_st(&st);
        for (size_t j = 0; j < fn->params.len; j++) {
            if (fn->params.data[j].name && fn->params.data[j].name[0] != '\0') {
                if (symtable_has_since(&st, fn->params.data[j].name, mark)) {
                    return die_at(fn->params.data[j].loc.file,
                           fn->params.data[j].loc.line,
                           fn->params.data[j].loc.col,
                           "duplicate parameter name '%s'",
                           fn->params.data[j].name);
                }
            }
            /* Normalize unsized array parameters (`int a[]`) to pointers,
             * matching standard C parameter adjustment.  `own_ptr` is true
             * only when we allocate a fresh ptr type here — freeing `pty`
             * otherwise would double-free heap pointers shared with
             * fn->params.data[j].type (e.g. a function-pointer parameter,
             * whose pointee is freed later by param_array_free in tu_free). */
            Type pty = fn->params.data[j].type;
            if (pty.kind == TY_STRUCT && pty.tag) {
                int is_cplx = (strncmp(pty.tag, "__complex_", 10) == 0);
                if (!is_cplx && !struct_registry_find_c(g_sema_structs, pty.tag)) {
                    return die_at(fn->params.data[j].loc.file,
                           fn->params.data[j].loc.line,
                           fn->params.data[j].loc.col,
                           "parameter has incomplete type 'struct %s'", pty.tag);
                }
            }
            if (pty.vla_dim) {
                Type dt = check_expr_inner(pty.vla_dim);
                type_free(&dt);
            }
            int own_ptr = 0;
            if (pty.kind == TY_ARRAY && pty.length <= 0) {
                pty = type_make_ptr(*pty.elem_type);
                own_ptr = 1;
            }
            if (fn->params.data[j].name && fn->params.data[j].name[0] != '\0') {
                symtable_push(&st, fn->params.data[j].name,
                              pty, fn->params.data[j].loc, 0, 0);
            }
            if (own_ptr) type_free(&pty);
        }

        /* Pre-pass: collect every label in the function so forward gotos
         * resolve. */
        LabelSet ls;
        labelset_init(&ls);
        g_sema_labels = &ls;
        for (size_t j = 0; j < fn->body.len; j++)
            collect_labels(&ls, &fn->body.data[j]);

        g_sema_ret_type = fn->ret_type;
        int has_return = 0;
        check_stmt_list(&fn->body, &has_return);
        symtable_leave_scope(&st, mark);
        symtable_free(&st);

        g_sema_labels = NULL;
        labelset_free(&ls);

        /* Non-void `main` must return a value (FakeCC is stricter than
         * C99 §5.1.2.2.3, which treats falling off `main` as `return 0`).
         * Other non-void functions may still fall off; IR appends a typed
         * zero if the function does not end in IR_RETURN. */
        if (!has_return && strcmp(fn->name, "main") == 0
            && fn->ret_type.kind != TY_VOID) {
            return die_at(fn->loc.file, fn->loc.line, fn->loc.col,
                   "non-void function must return a value");
        }
    }

    ftab_cur_free();
    symtable_free(&globals);
    if (fakecc_had_error() || g_sema_error_count > 0) return FAKECC_ERR;
    return FAKECC_OK;
}

int sema_check_in_pkg(const TranslationUnit *tu_const, int require_main,
                       PkgContext *ctx) {
    int rc = sema_check_in_pkg_impl(tu_const, require_main, ctx);
    /* g_sema_structs/g_sema_tu point into *tu_const: drop them on the error
     * paths too (they return early), so a later compile in the same process
     * cannot consult a TranslationUnit that has gone away. */
    g_sema_pkg = NULL;
    g_sema_tu = NULL;
    g_sema_structs = NULL;
    return rc;
}

int sema_check(const TranslationUnit *tu, int require_main) {
    return sema_check_in_pkg(tu, require_main, NULL);
}
