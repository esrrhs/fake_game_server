#include "fakecc/ast.h"
#include "fakecc/target.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Type — recursive helpers                                            */
/* ------------------------------------------------------------------ */

Type type_clone(Type t) {
    Type r = t;
    if (t.kind == TY_PTR && t.pointee) {
        /* struct types are shared — deep-copying them freezes the struct's
         * width at the time of the clone, which breaks self-referential structs
         * where the struct is still growing.  share the pointee instead. */
        if (t.pointee->kind == TY_STRUCT) {
            r.pointee = t.pointee;
        } else {
            r.pointee = malloc(sizeof(Type));
            if (!r.pointee) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
            *r.pointee = type_clone(*t.pointee);
        }
    } else {
        r.pointee = NULL;
    }
    if ((t.kind == TY_ARRAY || t.is_vector) && t.elem_type) {
        if (t.elem_type->kind == TY_STRUCT) {
            r.elem_type = t.elem_type;
        } else {
            r.elem_type = malloc(sizeof(Type));
            if (!r.elem_type) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
            *r.elem_type = type_clone(*t.elem_type);
        }
    } else {
        r.elem_type = NULL;
    }
    r.vla_dim = t.vla_dim ? expr_clone(t.vla_dim) : NULL;
    r.tag = t.tag ? xstrdup(t.tag) : NULL;
    if (t.kind == TY_FUNC && t.func_ret) {
        if (t.func_ret->kind == TY_STRUCT) {
            r.func_ret = t.func_ret;
        } else {
            r.func_ret = malloc(sizeof(Type));
            if (!r.func_ret) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
            *r.func_ret = type_clone(*t.func_ret);
        }
    } else {
        r.func_ret = NULL;
    }
    if (t.kind == TY_FUNC && t.func_nparams > 0 && t.func_params) {
        r.func_params = malloc(t.func_nparams * sizeof(Type));
        if (!r.func_params) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        for (int i = 0; i < t.func_nparams; i++)
            r.func_params[i] = type_clone(t.func_params[i]);
    } else {
        r.func_params = NULL;
    }
    r.func_is_variadic = t.func_is_variadic;
    return r;
}

void type_free(Type *t) {
    if (!t) return;
    /* struct types are shared (not owned by the pointer they're embedded in).
     * only free non-struct pointees / elem_types / func_rets. */
    if (t->pointee) {
        if (t->pointee->kind != TY_STRUCT) {
            type_free(t->pointee); free(t->pointee);
        }
        t->pointee = NULL;
    }
    if (t->elem_type) {
        if (t->elem_type->kind != TY_STRUCT) {
            type_free(t->elem_type); free(t->elem_type);
        }
        t->elem_type = NULL;
    }
    if (t->vla_dim) {
        expr_free(t->vla_dim);
        t->vla_dim = NULL;
    }
    if (t->tag) { free(t->tag); t->tag = NULL; }
    if (t->func_ret) {
        if (t->func_ret->kind != TY_STRUCT) {
            type_free(t->func_ret); free(t->func_ret);
        }
        t->func_ret = NULL;
    }
    if (t->func_params) {
        for (int i = 0; i < t->func_nparams; i++) type_free(&t->func_params[i]);
        free(t->func_params); t->func_params = NULL;
    }
}

extern const StructRegistry *get_ir_structs(void);
extern const StructRegistry *get_sema_structs(void);
extern const StructRegistry *get_parser_structs(void);

/* The struct registry to consult for layout.  Every phase publishes the
 * TranslationUnit it is working on and unpublishes it when it is done — except
 * ir_generate(), whose registry codegen still needs afterwards.  Ask the
 * earliest phase that is live: a leftover IR registry points at a
 * TranslationUnit that may already be gone (second compile in the same
 * process, next file of a package build). */
static const StructRegistry *current_structs(void) {
    const StructRegistry *reg = get_parser_structs();
    if (!reg) reg = get_sema_structs();
    if (!reg) reg = get_ir_structs();
    return reg;
}
extern const TranslationUnit *get_sema_tu(void);
extern const TranslationUnit *get_ir_tu(void);

long long type_size(Type t) {
    if (t.is_vector) return t.width;
    /* Walk array nesting through a pointer instead of recursing on the
     * by-value parameter.  Self-recursion here is rewritten by clang into a
     * loop that overwrites its own argument slot; because that slot is not
     * given a private copy, both this function and its caller then read a
     * mutated Type.  Never writing to `t` keeps the parameter intact. */
    const Type *p = &t;
    long long count = 1;
    while (p->kind == TY_ARRAY && p->elem_type) {
        if (p->vla_dim || p->length < 0) return -1;
        count *= p->length;
        p = p->elem_type;
    }
    if (p->is_vector) return count * p->width;
    switch (p->kind) {
    case TY_VOID:   return 0;
    case TY_INT:    return count * p->width;
    case TY_FLOAT:  return count * p->width;  /* 4 for float, 8 for double */
    case TY_PTR:    return count * 8;
    case TY_ARRAY:  return 0;   /* array without an element type — malformed */
    case TY_STRUCT: {
        if (p->tag) {
            const StructRegistry *reg = current_structs();
            if (reg) {
                const StructDef *sd = struct_registry_find_c(reg, p->tag);
                if (sd && sd->size > 0) return count * sd->size;
            }
        }
        return count * p->width;
    }
    case TY_FUNC:   return 0;        /* sizeof a function is undefined */
    }
    return 0;
}

int type_is_complex_ldouble(Type t) {
    return t.kind == TY_STRUCT && t.tag
        && strcmp(t.tag, "__complex_ldouble") == 0;
}

int type_is_empty_struct(Type t) {
    /* GNU empty structs and size-0 types (only a flexible/`[0]` member)
     * consume no argument slots.  gcc named args and va_arg agree. */
    if (t.kind != TY_STRUCT) return 0;
    const StructRegistry *reg = current_structs();
    if (t.tag && reg) {
        const StructDef *sd = struct_registry_find_c(reg, t.tag);
        if (sd) return sd->size <= 0 || sd->num_members == 0;
    }
    return type_size(t) <= 0;
}

int type_needs_stack_align16(Type t) {
    if (t.kind == TY_FLOAT && t.width == 16 && !t.is_vector) return 1;
    if (t.kind == TY_INT && t.width == 16 && !t.is_vector) return 1;
    if (type_is_complex_ldouble(t)) return 1;
    return type_align(t) >= 16;
}

int type_stack_align(Type t) {
    long long a = type_align(t);
    if (t.kind == TY_FLOAT && t.width == 16 && !t.is_vector) return 16;
    if (t.kind == TY_INT && t.width == 16 && !t.is_vector) return 16;
    if (type_is_complex_ldouble(t)) return 16;
    if (a >= 64) return 64;
    if (a >= 32) return 32;
    if (a >= 16) return 16;
    return 0;
}

Type type_make_vector(Type elem, long long vec_size) {
    Type t = elem;
    t.is_vector = 1;
    t.width = vec_size;
    t.elem_type = malloc(sizeof(Type));
    if (!t.elem_type) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
    *t.elem_type = type_clone(elem);
    long long esz = type_size(elem);
    t.length = esz > 0 ? vec_size / esz : 1;
    return t;
}

Type type_make_ptr(Type pointee) {
    Type t; t.kind = TY_PTR; t.width = 8; t.is_unsigned = 1;
    t.is_const = 0; t.is_volatile = 0; t.is_restrict = 0; t.is_bool = 0;
    t.elem_type = NULL; t.length = 0; t.vla_dim = NULL; t.tag = NULL;
    t.func_ret = NULL; t.func_params = NULL; t.func_nparams = 0; t.func_is_variadic = 0;
    t.func_is_unprototyped = 0; t.enum_id = 0;
    t.bitfield_width = 0; t.is_vector = 0; t.is_decimal = 0;
    t.pointee = malloc(sizeof(Type));
    if (!t.pointee) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
    *t.pointee = type_clone(pointee);
    return t;
}

/* fixup all TY_STRUCT widths in a type tree that match the given tag.
 * this is called after a struct definition is complete so that self-referential
 * pointer members use the final struct size instead of a stale snapshot. */
static void type_fixup_struct_width(Type *t, const char *tag, long long final_width) {
    if (!t || !tag) return;
    if (t->kind == TY_STRUCT && t->tag && strcmp(t->tag, tag) == 0) {
        t->width = final_width;
    }
    if (t->pointee) type_fixup_struct_width(t->pointee, tag, final_width);
    if (t->elem_type) type_fixup_struct_width(t->elem_type, tag, final_width);
    if (t->func_ret) type_fixup_struct_width(t->func_ret, tag, final_width);
    if (t->func_params) {
        for (int i = 0; i < t->func_nparams; i++)
            type_fixup_struct_width(&t->func_params[i], tag, final_width);
    }
}

const StructMember *struct_lookup_member(const StructRegistry *reg,
                                         const StructDef *sd,
                                         const char *name,
                                         long long *offset_out) {
    if (!sd || !name) return NULL;
    for (int i = 0; i < sd->num_members; i++) {
        const StructMember *m = &sd->members[i];
        if (m->name && m->name[0] && strcmp(m->name, name) == 0) {
            if (offset_out) *offset_out = m->offset;
            return m;
        }
        if ((!m->name || !m->name[0]) && m->type.kind == TY_STRUCT
            && m->type.tag && reg) {
            const StructDef *nested = struct_registry_find_c(reg, m->type.tag);
            long long inner = 0;
            const StructMember *found =
                struct_lookup_member(reg, nested, name, &inner);
            if (found) {
                if (offset_out) *offset_out = m->offset + inner;
                return found;
            }
        }
    }
    return NULL;
}

void struct_def_fixup_self_types(StructDef *sd) {
    if (!sd || !sd->tag) return;
    for (int i = 0; i < sd->num_members; i++) {
        type_fixup_struct_width(&sd->members[i].type, sd->tag, sd->size);
    }
}

Type type_make_array(Type elem, long long length) {
    Type t; t.kind = TY_ARRAY; t.width = elem.width;
    t.is_unsigned = elem.is_unsigned;
    t.is_const = elem.is_const; t.is_volatile = elem.is_volatile; t.is_restrict = elem.is_restrict;
    t.is_bool = 0; t.length = length; t.vla_dim = NULL;
    t.pointee = NULL; t.tag = NULL;
    t.func_ret = NULL; t.func_params = NULL; t.func_nparams = 0; t.func_is_variadic = 0;
    t.func_is_unprototyped = 0; t.enum_id = 0;
    t.bitfield_width = 0; t.is_vector = 0; t.is_decimal = 0;
    t.elem_type = malloc(sizeof(Type));
    if (!t.elem_type) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
    *t.elem_type = type_clone(elem);
    return t;
}

Type type_make_vla(Type elem, Expr *dim) {
    Type t = type_make_array(elem, -1);
    t.vla_dim = dim;
    return t;
}

Type type_make_struct(const char *tag, long long size) {
    Type t; t.kind = TY_STRUCT; t.width = size; t.is_unsigned = 0;
    t.is_const = 0; t.is_volatile = 0; t.is_restrict = 0; t.is_bool = 0;
    t.pointee = NULL; t.elem_type = NULL; t.length = 0; t.vla_dim = NULL;
    t.func_ret = NULL; t.func_params = NULL; t.func_nparams = 0; t.func_is_variadic = 0;
    t.func_is_unprototyped = 0; t.enum_id = 0;
    t.bitfield_width = 0; t.is_vector = 0; t.is_decimal = 0;
    t.tag = xstrdup(tag);
    return t;
}

Type type_make_func_var(Type ret, Type * const *params, int nparams, int is_variadic) {
    Type t; t.kind = TY_FUNC; t.width = 0; t.is_unsigned = 0; t.is_const = 0; t.is_volatile = 0; t.is_restrict = 0; t.is_bool = 0;
    t.pointee = NULL; t.elem_type = NULL; t.length = 0; t.vla_dim = NULL; t.tag = NULL; t.enum_id = 0;
    t.bitfield_width = 0; t.is_vector = 0; t.is_decimal = 0;
    t.func_is_variadic = is_variadic;
    t.func_is_unprototyped = 0;
    t.func_ret = malloc(sizeof(Type));
    if (!t.func_ret) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
    *t.func_ret = type_clone(ret);
    t.func_nparams = nparams;
    if (nparams > 0) {
        t.func_params = malloc(nparams * sizeof(Type));
        if (!t.func_params) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        for (int i = 0; i < nparams; i++)
            t.func_params[i] = type_clone(*params[i]);
    } else {
        t.func_params = NULL;
    }
    return t;
}

Type type_make_func(Type ret, Type * const *params, int nparams) {
    return type_make_func_var(ret, params, nparams, 0);
}

static int types_equal(Type a, Type b);  /* forward */

int type_funcs_equal(Type a, Type b) {
    if (a.kind != TY_FUNC || b.kind != TY_FUNC) return 0;
    if (!types_equal(*a.func_ret, *b.func_ret)) return 0;
    if (a.func_nparams != b.func_nparams) return 0;
    for (int i = 0; i < a.func_nparams; i++)
        if (!types_equal(a.func_params[i], b.func_params[i])) return 0;
    return 1;
}

static int types_equal(Type a, Type b) {
    if (a.kind != b.kind) return 0;
    switch (a.kind) {
    case TY_VOID: return 1;
    case TY_INT: return a.width == b.width && a.is_unsigned == b.is_unsigned;
    case TY_FLOAT: return a.width == b.width;  /* float vs double */
    case TY_PTR: return types_equal(*a.pointee, *b.pointee);
    case TY_ARRAY: return a.length == b.length && types_equal(*a.elem_type, *b.elem_type);
    case TY_STRUCT: return a.tag && b.tag && strcmp(a.tag, b.tag) == 0;
    case TY_FUNC: return type_funcs_equal(a, b);
    }
    return 0;
}

int type_same_typedef(Type a, Type b) {
    if (a.is_const != b.is_const || a.is_volatile != b.is_volatile
        || a.is_restrict != b.is_restrict)
        return 0;
    if (a.is_bool != b.is_bool) return 0;
    if (a.is_vector != b.is_vector) return 0;
    if (a.is_vector) {
        if (a.width != b.width) return 0;
        if (!a.elem_type || !b.elem_type) return a.elem_type == b.elem_type;
        return type_same_typedef(*a.elem_type, *b.elem_type);
    }
    if (a.kind != b.kind) return 0;
    switch (a.kind) {
    case TY_VOID:
        return 1;
    case TY_INT:
        if (a.enum_id != b.enum_id) return 0;
        return a.width == b.width && a.is_unsigned == b.is_unsigned;
    case TY_FLOAT:
        return a.width == b.width && a.is_decimal == b.is_decimal;
    case TY_PTR:
        if (!a.pointee || !b.pointee) return a.pointee == b.pointee;
        return type_same_typedef(*a.pointee, *b.pointee);
    case TY_ARRAY:
        /* GCC: restating a typedef requires the same type, not merely a
         * compatible one, so `int[]` and `int[3]` do not match. */
        if (a.length != b.length)
            return 0;
        if (!a.elem_type || !b.elem_type) return a.elem_type == b.elem_type;
        return type_same_typedef(*a.elem_type, *b.elem_type);
    case TY_STRUCT:
        if (!a.tag || !b.tag) return 0;
        return strcmp(a.tag, b.tag) == 0;
    case TY_FUNC:
        if (a.func_is_variadic != b.func_is_variadic) return 0;
        if (a.func_nparams != b.func_nparams) return 0;
        if (a.func_ret && b.func_ret) {
            if (!type_same_typedef(*a.func_ret, *b.func_ret)) return 0;
        } else if (a.func_ret || b.func_ret) {
            return 0;
        }
        for (int i = 0; i < a.func_nparams; i++) {
            if (!type_same_typedef(a.func_params[i], b.func_params[i]))
                return 0;
        }
        return 1;
    }
    return 0;
}

Type type_decay(Type t) {
    if (t.kind == TY_ARRAY) {
        Type r = type_make_ptr(*t.elem_type);
        return r;
    }
    return type_clone(t);
}

int type_is_ptr_or_array(Type t) {
    return t.kind == TY_PTR || t.kind == TY_ARRAY;
}

Type type_pointee_or_elem(Type t) {
    if (t.kind == TY_PTR)   return type_clone(*t.pointee);
    if (t.kind == TY_ARRAY) return type_clone(*t.elem_type);
    return type_default_int();  /* caller should have checked */
}

void expr_set_type(Expr *e, Type t) {
    if (!e) { type_free(&t); return; }
    type_free(&e->type);
    e->type = t;
}

/* ------------------------------------------------------------------ */
/* Struct registry                                                     */
/* ------------------------------------------------------------------ */

void struct_registry_init(StructRegistry *r) {
    r->data = NULL; r->len = 0; r->cap = 0;
}

void struct_registry_free(StructRegistry *r) {
    for (size_t i = 0; i < r->len; i++) {
        StructDef *sd = &r->data[i];
        free(sd->tag);
        for (int j = 0; j < sd->num_members; j++) {
            free(sd->members[j].name);
            type_free(&sd->members[j].type);
        }
        free(sd->members);
    }
    free(r->data);
    r->data = NULL; r->len = 0; r->cap = 0;
}

StructDef *struct_registry_add(StructRegistry *r, const char *tag, SourceLoc loc) {
    if (r->len >= r->cap) {
        size_t nc = r->cap ? r->cap * 2 : 4;
        r->data = realloc(r->data, nc * sizeof(StructDef));
        if (!r->data) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        r->cap = nc;
    }
    StructDef *sd = &r->data[r->len++];
    memset(sd, 0, sizeof(StructDef));
    sd->tag = xstrdup(tag);
    sd->align = 1;
    sd->loc = loc;
    return sd;
}

StructDef *struct_registry_find(StructRegistry *r, const char *tag) {
    for (size_t i = 0; i < r->len; i++)
        if (strcmp(r->data[i].tag, tag) == 0) return &r->data[i];
    return NULL;
}

const StructDef *struct_registry_find_c(const StructRegistry *r, const char *tag) {
    return struct_registry_find((StructRegistry *)r, tag);
}

/* Round up x to a multiple of align. */
static long long align_up(long long x, long long align) {
    if (align <= 1) return x;
    return (x + align - 1) & ~(align - 1);
}

/* Natural alignment of a type: 1/2/4/8 for scalars, the vector width for
 * GCC vector_size (8/16/32/64), elem's alignment for arrays, max member
 * alignment for structs.  SysV: __m256 aligns to 32, __m512 to 64. */
long long type_align(Type t) {
    if (t.is_vector) return t.width > 0 ? t.width : 1;
    /* Pointer walk rather than self-recursion — see type_size(). */
    const Type *p = &t;
    while (p->kind == TY_ARRAY && p->elem_type) p = p->elem_type;
    if (p->is_vector) return p->width > 0 ? p->width : 1;
    switch (p->kind) {
    case TY_VOID:  return 1;    /* void has no size; alignment is a no-op */
    case TY_INT:   return p->width;
    case TY_FLOAT: return p->width;  /* float aligns to 4, double to 8 */
    case TY_PTR:   return 8;
    case TY_ARRAY: return 1;    /* array without an element type — malformed */
    case TY_STRUCT: {
        if (p->tag) {
            const StructRegistry *reg = current_structs();
            if (reg) {
                const StructDef *sd = struct_registry_find_c(reg, p->tag);
                if (sd && sd->align > 0) return sd->align;
            }
        }
        return 8;   /* fallback if incomplete/unknown */
    }
    case TY_FUNC:  return 1;    /* bare function has no size */
    }
    return 1;
}

int type_is_vla(Type t) {
    const Type *p = &t;
    while (p->kind == TY_ARRAY && p->elem_type) {
        if (p->vla_dim) return 1;
        p = p->elem_type;
    }
    return 0;
}

/* Field class for SysV eightbyte merging (NO_CLASS = 0).
 * SSEUP is the upper half of a 16-byte vector (same XMM as the preceding SSE). */
enum { SV_NO = 0, SV_INT = 1, SV_SSE = 2, SV_MEM = 3, SV_SSEUP = 4 };

static int sysv_merge(int a, int b) {
    if (a == b) return a;
    if (a == SV_NO) return b;
    if (b == SV_NO) return a;
    if (a == SV_MEM || b == SV_MEM) return SV_MEM;
    if (a == SV_INT || b == SV_INT) return SV_INT;
    /* SSE and SSEUP in one eightbyte collapse to SSE. */
    return SV_SSE;
}

static int sysv_field_class(Type t) {
    switch (t.kind) {
    case TY_INT:
    case TY_PTR:
        return SV_INT;
    case TY_FLOAT:
        if (t.width == 16 && !t.is_vector) return SV_MEM; /* long double / X87 → MEMORY */
        return SV_SSE;
    default:
        return SV_MEM;
    }
}

/* Paint type `t` starting at byte `offset` onto the parent object's
 * eightbyte classes.  Nested structs and arrays are walked member-by-member
 * (SysV AMD64) so a `{double; long}` inner struct stays SSE+INTEGER rather
 * than collapsing to INTEGER,INTEGER.  Returns 1 if the whole object must
 * use the MEMORY class. */
static int sysv_paint(Type t, int offset, int eight[8]) {
    if (t.is_vector) {
        /* SysV: 8-byte vectors are SSE; 16-byte are SSE+SSEUP (one XMM);
         * 32-byte are SSE+SSEUP×3 (one YMM).  Integer vector_size(8) is
         * SSE, not GP. */
        int end = offset + (int)t.width;
        for (int eb = 0; eb < 8; eb++) {
            int lo = eb * 8, hi = lo + 8;
            if (end <= lo || offset >= hi) continue;
            int fc = (t.width >= 16 && lo >= offset + 8) ? SV_SSEUP : SV_SSE;
            eight[eb] = sysv_merge(eight[eb], fc);
            if (eight[eb] == SV_MEM) return 1;
        }
        return 0;
    }
    if (t.kind == TY_ARRAY && t.elem_type) {
        /* FAM / `[0]`: no allocated eightbytes.  Only the fixed prefix
         * of the enclosing struct is classified (gcc/SysV). */
        if (t.length <= 0) return 0;
        int esz = type_size(*t.elem_type);
        if (esz <= 0) return 0;
        for (long long i = 0; i < t.length; i++) {
            int eoff = offset + (int)(i * esz);
            if (eoff >= 64) break;
            if (sysv_paint(*t.elem_type, eoff, eight)) return 1;
        }
        return 0;
    }
    if (t.kind == TY_STRUCT && t.tag) {
        const StructRegistry *reg = current_structs();
        const StructDef *sd = reg ? struct_registry_find_c(reg, t.tag) : NULL;
        if (!sd) {
            int sz = type_size(t);
            int end = offset + (sz > 0 ? sz : 0);
            for (int eb = 0; eb < 8; eb++) {
                int lo = eb * 8, hi = lo + 8;
                if (end <= lo || offset >= hi) continue;
                eight[eb] = sysv_merge(eight[eb], SV_INT);
            }
            return 0;
        }
        for (int mi = 0; mi < sd->num_members; mi++) {
            const StructMember *m = &sd->members[mi];
            /* Zero-width `: 0` only forces alignment; it is not an ABI object. */
            if (m->bit_width == 0) continue;
            /* A union carrying a true flexible array member is passed in
             * memory -- but only under SysV/x86-64, which is where that rule
             * comes from.  AAPCS64 has no such clause: clang hands
             * `union U { int n; char d[]; }` to a callee in a general
             * register, and a fixed 4-byte union classifies like any other
             * small aggregate.  Applying the SysV rule on arm64 puts the
             * argument on the stack where the callee is not looking, and the
             * callee reads whatever the register happened to hold. */
            if (sd->is_union && m->type.kind == TY_ARRAY && m->type.length < 0
                && !m->type.vla_dim
                && target_current()->arch != TARGET_ARCH_ARM64)
                return 1;
            int moff = offset + m->offset;
            int msz = m->bit_width > 0 ? 0 : type_size(m->type);
            if (msz <= 0 && m->bit_width <= 0) {
                if (m->type.kind == TY_ARRAY || m->type.kind == TY_STRUCT
                    || m->type.is_vector) {
                    if (sysv_paint(m->type, moff, eight)) return 1;
                }
                continue;
            }
            if (m->bit_width <= 0) {
                long long ma = type_align(m->type);
                if (ma > 1 && (m->offset % ma) != 0) return 1;
            }
            if (m->bit_width > 0) {
                /* Occupy every byte that holds any bit of the field.
                 * Rounding width to 1/2/4/8 from the container byte misses
                 * a packed field that starts mid-byte and spills into the
                 * next eightbyte (`unsigned long :60` then `unsigned :8`). */
                int start_bit = moff * 8 + m->bit_offset;
                int last_bit = start_bit + m->bit_width - 1;
                int start_byte = start_bit / 8;
                int last_byte = last_bit / 8;
                for (int eb = 0; eb < 8; eb++) {
                    int lo = eb * 8, hi = lo + 8;
                    if (last_byte + 1 <= lo || start_byte >= hi) continue;
                    eight[eb] = sysv_merge(eight[eb], SV_INT);
                    if (eight[eb] == SV_MEM) return 1;
                }
            } else if (m->type.kind == TY_ARRAY || m->type.kind == TY_STRUCT
                       || m->type.is_vector) {
                if (sysv_paint(m->type, moff, eight)) return 1;
            } else {
                int fc = sysv_field_class(m->type);
                if (fc == SV_MEM) return 1;
                int end = moff + msz;
                for (int eb = 0; eb < 8; eb++) {
                    int lo = eb * 8, hi = lo + 8;
                    if (end <= lo || moff >= hi) continue;
                    eight[eb] = sysv_merge(eight[eb], fc);
                    if (eight[eb] == SV_MEM) return 1;
                }
            }
        }
        return 0;
    }
    int fc = sysv_field_class(t);
    if (fc == SV_MEM) return 1;
    int sz = type_size(t);
    if (sz <= 0) sz = (int)t.width;
    int end = offset + sz;
    for (int eb = 0; eb < 8; eb++) {
        int lo = eb * 8, hi = lo + 8;
        if (end <= lo || offset >= hi) continue;
        eight[eb] = sysv_merge(eight[eb], fc);
        if (eight[eb] == SV_MEM) return 1;
    }
    return 0;
}

int sysv_classify_agg(Type t, SysVRegClass cls[2]) {
    cls[0] = SYSV_CLS_INTEGER;
    cls[1] = SYSV_CLS_INTEGER;
    if (t.is_vector) {
        if (t.width == 32) {
            /* SysV: __m256 / vector_size(32) is SSE+SSEUP×3 → one YMM.
             * gcc -mno-avx cannot use YMM, so the same type is MEMORY. */
            if (g_no_avx) return 0;
            cls[0] = SYSV_CLS_SSE;
            return 1;
        }
        if (t.width == 16) {
            /* SysV: __m128 / vector_size(16) is SSE + SSEUP → one XMM. */
            cls[0] = SYSV_CLS_SSE;
            return 1;
        }
        if (t.width == 8) {
            cls[0] = SYSV_CLS_SSE;
            return 1;
        }
        if (t.width <= 4) {
            cls[0] = (t.kind == TY_FLOAT) ? SYSV_CLS_SSE : SYSV_CLS_INTEGER;
            return 1;
        }
        if (t.width == 64 && host_has_avx512f()) {
            /* SysV: __m512 / vector_size(64) is one ZMM under -mavx512f. */
            cls[0] = SYSV_CLS_SSE;
            return 1;
        }
        /* vector_size(64) without AVX-512: MEMORY, 64-byte-aligned stack. */
        return 0;
    }
    if (t.kind != TY_STRUCT || !t.tag) return 0;
    int sz = type_size(t);
    if (sz <= 0 || sz > 64) return 0;
    const StructRegistry *reg = current_structs();
    const StructDef *sd = NULL;
    if (reg) sd = struct_registry_find_c(reg, t.tag);
    if (!sd) {
        if (sz > 16) return 0;
        int n = (sz + 7) / 8;
        cls[0] = SYSV_CLS_INTEGER;
        if (n > 1) cls[1] = SYSV_CLS_INTEGER;
        return n;
    }
    int eight[8] = { SV_NO, SV_NO, SV_NO, SV_NO, SV_NO, SV_NO, SV_NO, SV_NO };
    if (sysv_paint(t, 0, eight)) return 0;
    int n = (sz + 7) / 8;
    if (n < 1) n = 1;
    if (n > 8) return 0;
    /* SSEUP without a preceding SSE eightbyte is just SSE. */
    if (n > 1 && eight[1] == SV_SSEUP && eight[0] != SV_SSE)
        eight[1] = SV_SSE;
    /* Size > two eightbytes: SysV says MEMORY unless the first eightbyte
     * is SSE and every later eightbyte is SSEUP — i.e. one YMM/ZMM.
     * Padding eightbytes are NO_CLASS, not SSEUP, so
     * `aligned(32) { __m128 }` is MEMORY (not a YMM).
     * 32-byte → one YMM (nreg=1).  64-byte → one ZMM if AVX-512F. */
    if (n > 2) {
        if (g_no_avx && sz >= 32) return 0;
        if (eight[0] != SV_SSE) return 0;
        for (int i = 1; i < n; i++) {
            if (eight[i] != SV_SSEUP) return 0;
        }
        if (sz == 32 || (sz == 64 && host_has_avx512f())) {
            cls[0] = SYSV_CLS_SSE;
            return 1;
        }
        return 0;
    }
    /* SSE + SSEUP share one XMM (`struct { __m128 x; }`, not `{double;double}`). */
    if (n == 2 && eight[0] == SV_SSE && eight[1] == SV_SSEUP) {
        cls[0] = SYSV_CLS_SSE;
        return 1;
    }
    /* Trailing padding eightbytes are NO_CLASS: they do not consume a
     * register.  `aligned(16) { long x; }` is 16 bytes but only RDI;
     * converting the pad to INTEGER would steal RSI from the next arg. */
    while (n > 1 && eight[n - 1] == SV_NO) n--;
    for (int i = 0; i < n; i++) {
        int c = eight[i] == SV_NO ? SV_INT : eight[i];
        if (c == SV_MEM) return 0;
        cls[i] = (c == SV_SSE || c == SV_SSEUP) ? SYSV_CLS_SSE : SYSV_CLS_INTEGER;
    }
    return n;
}

static int type_is_pure_x87(Type t) {
    if (t.is_vector) return 0;
    if (t.kind == TY_FLOAT && t.width == 16) return 1;
    if (t.kind == TY_ARRAY && t.elem_type) {
        if (t.length <= 0) return 1;
        return type_is_pure_x87(*t.elem_type);
    }
    if (t.kind != TY_STRUCT || !t.tag) return 0;
    const StructRegistry *reg = current_structs();
    const StructDef *sd = reg ? struct_registry_find_c(reg, t.tag) : NULL;
    if (!sd) return 0;
    int any = 0;
    for (int i = 0; i < sd->num_members; i++) {
        if (sd->members[i].bit_width == 0) continue;
        Type mt = sd->members[i].type;
        if (mt.kind == TY_ARRAY && mt.length <= 0 && !mt.vla_dim) continue;
        if (!type_is_pure_x87(mt)) return 0;
        any = 1;
    }
    return any;
}

int sysv_agg_ret_x87(Type t) {
    /* SysV: one X87+X87UP object (16 bytes) returns in st0.  A 32-byte
     * `{ long double a, b }` or `_Complex long double` is not this case.
     * A union that also has INTEGER/SSE members is MEMORY and uses sret. */
    if (type_is_complex_ldouble(t)) return 0;
    if (t.kind != TY_STRUCT) return 0;
    if (type_size(t) != 16) return 0;
    SysVRegClass cls[2];
    if (sysv_classify_agg(t, cls) != 0) return 0;
    return type_is_pure_x87(t);
}

static int abi_is_arm64(void);

int sysv_memory_pass_as_pointer(Type t) {
    /* SysV MEMORY arguments are copied onto the outgoing stack.  GCC's
     * `__va_list_tag` (array-of-1) is the exception: it decays to a
     * pointer, matching libc.  Darwin's va_list is an 8-byte cursor and
     * travels in a GP register like any other small aggregate. */
    if (abi_is_arm64()) return 0;
    return t.kind == TY_STRUCT && t.tag
        && strcmp(t.tag, "__va_list_tag") == 0;
}

static int abi_is_arm64(void) {
    const TargetDesc *t = target_current();
    return t && t->arch == TARGET_ARCH_ARM64;
}

int type_long_double_width(void) {
    const TargetDesc *t = target_current();
    if (t && t->arch == TARGET_ARCH_ARM64) return 8;
    return 16;
}

/* On Darwin arm64 this is plain `double` in every respect that matters to
 * codegen -- same width, same ABI class, same instructions -- but C keeps it
 * a distinct type, and so does clang:
 *
 *   $ cc -x c - <<< '__builtin_types_compatible_p(long double, double)'
 *   0
 *
 * The width alone cannot answer that, hence the flag.
 */
Type type_make_long_double(void) {
    Type t = type_make_float(type_long_double_width());
    t.is_long_double = 1;
    return t;
}

int abi_gp_nregs(void) {
    const TargetDesc *t = target_current();
    if (t && t->gp_arg_regs > 0) return t->gp_arg_regs;
    return 6;
}

int abi_sret_uses_gp(void) {
    const TargetDesc *t = target_current();
    if (!t) return 1;
    return t->sret_uses_gp;
}

/* Element count of a homogeneous float/double aggregate, or -1.
 * `kind` is 0 on entry and becomes 4 (float) or 8 (double). */
static int hfa_elems(Type t, int *kind) {
    if (t.is_vector || t.is_decimal) return -1;
    if (t.kind == TY_FLOAT) {
        if (t.width != 4 && t.width != 8) return -1;
        if (*kind == 0) *kind = t.width;
        if (*kind != t.width) return -1;
        return 1;
    }
    if (t.kind == TY_ARRAY && t.elem_type && t.length > 0) {
        int n = hfa_elems(*t.elem_type, kind);
        if (n <= 0) return -1;
        long long tot = (long long)n * t.length;
        if (tot > 4) return -1;
        return (int)tot;
    }
    if (t.kind != TY_STRUCT || !t.tag) return -1;
    const StructRegistry *reg = get_ir_structs();
    if (!reg) reg = get_sema_structs();
    if (!reg) reg = get_parser_structs();
    const StructDef *sd = reg ? struct_registry_find_c(reg, t.tag) : NULL;
    if (!sd || sd->is_union) return -1;
    int total = 0, any = 0;
    for (int i = 0; i < sd->num_members; i++) {
        if (sd->members[i].bit_width == 0) continue;
        if (sd->members[i].bit_width > 0) return -1;
        Type mt = sd->members[i].type;
        if (mt.kind == TY_ARRAY && mt.length <= 0 && !mt.vla_dim) continue;
        int n = hfa_elems(mt, kind);
        if (n <= 0) return -1;
        total += n;
        any = 1;
        if (total > 4) return -1;
    }
    return any ? total : -1;
}

int abi_hfa_elem_count(Type t) {
    if (!abi_is_arm64()) return -1;
    int kind = 0;
    int n = hfa_elems(t, &kind);
    return (n >= 1 && n <= 4) ? n : -1;
}

int abi_is_hfa(Type t) {
    return abi_hfa_elem_count(t) > 0;
}

/* Width of one element of a homogeneous float/double aggregate, or -1.
 * An HFA's eightbytes hold one element each, which is not the aggregate's
 * size: `struct __attribute__((aligned(16))) { double d; }` is 16 bytes
 * carrying a single 8-byte element, and sizing its parameter from
 * type_size made the incoming V->GP move 32-bit, so the high half of the
 * double was lost and `(int)s.d` read 0. */
int abi_hfa_elem_size(Type t) {
    if (!abi_is_arm64()) return -1;
    int kind = 0;
    int n = hfa_elems(t, &kind);
    if (n < 1 || n > 4) return -1;
    return kind ? kind : -1;
}

int abi_indirect_agg(Type t) {
    /* Darwin: a non-HFA aggregate larger than 16 bytes is passed and
     * returned through a pointer.  HFAs use V registers instead. */
    if (!abi_is_arm64()) return 0;
    /* Darwin has no YMM/ZMM.  A vector wider than 16 bytes is a pointer. */
    if (t.is_vector) return type_size(t) > 16;
    if (t.kind != TY_STRUCT) return 0;
    if (abi_is_hfa(t)) return 0;
    return type_size(t) > 16;
}

int g_no_avx = 0;
int g_avx512f = 0;

int host_has_avx512f(void) {
    /* Target flag only — never CPUID.  Host probing made XMM spill size
     * (and thus runtime tan() rbp offsets) depend on the build CPU, so
     * Stage 0 and fakecc-1 disagreed on AVX-512 runners. */
    return g_avx512f && !g_no_avx;
}

static void close_bitfield_run(StructDef *sd) {
    if (!sd || sd->bf_unit_type == 0) return;
    long long next_bit = sd->bf_unit_offset * 8 + sd->bf_unit_used;
    long long bytes = (next_bit + 7) / 8;
    if (bytes > sd->size) sd->size = bytes;
    sd->bf_unit_type = 0;
    sd->bf_unit_used = 0;
    sd->bf_unit_offset = 0;
}

void struct_def_push_member(StructDef *sd, const char *name, Type ty, int bit_width) {
    struct_def_push_member_aligned(sd, name, ty, bit_width, 0);
}

void struct_def_push_member_aligned(StructDef *sd, const char *name, Type ty, int bit_width, int align) {
    if (fakecc_had_error()) return;
    if (name && name[0] != '\0') {
        for (int i = 0; i < sd->num_members; i++) {
            if (sd->members[i].name && strcmp(sd->members[i].name, name) == 0) {
                die_at(sd->loc.file, sd->loc.line, sd->loc.col,
                       "duplicate member '%s'", name);
                return;
            }
        }
    }
    if (sd->num_members > 0 && !sd->is_union) {
        const StructMember *prev = &sd->members[sd->num_members - 1];
        if (prev->type.kind == TY_ARRAY && prev->type.length < 0 && !prev->type.vla_dim && prev->type.elem_type) {
            die_at(sd->loc.file, sd->loc.line, sd->loc.col,
                   "flexible array member not at end of struct");
            return;
        }
    }
    if (sd->num_members >= sd->cap_members) {
        int nc = sd->cap_members ? sd->cap_members * 2 : 4;
        sd->members = realloc(sd->members, nc * sizeof(StructMember));
        if (!sd->members) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        sd->cap_members = nc;
    }
    long long a = sd->is_packed ? 1 : type_align(ty);
    if (align > a) a = align;
    long long sz = type_size(ty);
    /* FAM / incomplete array: no allocated bytes; `[0]` is already 0. */
    if (sz < 0) sz = 0;
    /* Track the max member alignment for final struct alignment. */
    if (a > sd->align) sd->align = a;
    long long off;
    if (sd->is_union) {
        /* Union members all start at offset 0; total size is the max. */
        off = 0;
    } else if (bit_width >= 0 && ty.kind == TY_INT) {
        /* Zero-width bit-field: close the current run and align the next
         * allocation unit.  Unnamed `: 0` is not a real member. */
        if (bit_width == 0) {
            close_bitfield_run(sd);
            sd->size = align_up(sd->size, a > 0 ? a : 1);
            sd->members[sd->num_members].name = xstrdup(name ? name : "");
            sd->members[sd->num_members].type = type_clone(ty);
            sd->members[sd->num_members].offset = sd->size;
            sd->members[sd->num_members].bit_width = 0;
            sd->members[sd->num_members].bit_offset = 0;
            sd->members[sd->num_members].align = align;
            sd->num_members++;
            return;
        }
        /* GCC/SysV: pack adjacent bitfields by bit position.  A field of
         * declared type T must sit in one T-sized, T-aligned cell unless
         * the struct is packed (then bits concatenate with no padding). */
        long long unit = sz > 0 ? sz : 4;
        long long unit_bits = unit * 8;
        long long start_bit = (sd->bf_unit_type != 0)
            ? (sd->bf_unit_offset * 8 + sd->bf_unit_used)
            : (sd->size * 8);
        if (!sd->is_packed) {
            long long in_unit = start_bit % unit_bits;
            if (in_unit + bit_width > unit_bits)
                start_bit += unit_bits - in_unit;
            long long align_bits = (a > 0 ? a : 1) * 8;
            long long unit_start = start_bit - (start_bit % unit_bits);
            if (align_bits > 0 && (unit_start % align_bits) != 0) {
                start_bit = ((start_bit + align_bits - 1) / align_bits) * align_bits;
                in_unit = start_bit % unit_bits;
                if (in_unit + bit_width > unit_bits)
                    start_bit += unit_bits - in_unit;
            }
        }
        long long container;
        int bit_off;
        if (sd->is_packed) {
            /* Packed: bits concatenate.  The containing cell starts at the
             * first byte of the field so a later load of T (or a widened
             * 8-byte load if T cannot cover bit_offset+width) can extract it. */
            container = start_bit / 8;
            bit_off = (int)(start_bit % 8);
        } else {
            container = (start_bit / unit_bits) * unit;
            bit_off = (int)(start_bit - container * 8);
        }
        long long end_bit = start_bit + bit_width;
        long long end_bytes = (end_bit + 7) / 8;
        if (end_bytes > sd->size) sd->size = end_bytes;
        sd->bf_unit_type = 1;
        sd->bf_unit_offset = end_bit / 8;
        sd->bf_unit_used = (int)(end_bit % 8);
        sd->members[sd->num_members].bit_offset = bit_off;
        sd->members[sd->num_members].offset = container;
        sd->members[sd->num_members].bit_width = bit_width;
        sd->members[sd->num_members].name = xstrdup(name);
        sd->members[sd->num_members].type = type_clone(ty);
        sd->members[sd->num_members].align = align;
        sd->num_members++;
        return;
    } else {
        /* Normal (non-bitfield) member: close any open bitfield run first.
         * Alignment padding is deferred to struct_def_finish() so trailing
         * members can pack into leftover bytes of a bitfield unit. */
        close_bitfield_run(sd);
        off = align_up(sd->size, a);
    }
    sd->members[sd->num_members].name = xstrdup(name);
    sd->members[sd->num_members].type = type_clone(ty);
    sd->members[sd->num_members].offset = off;
    sd->members[sd->num_members].bit_width = bit_width;
    sd->members[sd->num_members].bit_offset = 0;
    sd->members[sd->num_members].align = align;
    sd->num_members++;
    if (sd->is_union) {
        /* Size grows to the largest member.  Final alignment is applied once
         * in struct_def_finish(), not here — padding after every member would
         * prevent later members from packing into the trailing gap. */
        if (sz > sd->size) sd->size = sz;
    } else {
        /* Track the raw byte end of the member.  Alignment padding to the
         * struct's natural boundary is deferred to struct_def_finish() so
         * that a following member can still pack into any trailing gap
         * (e.g. `struct { void *p; int a; int b; }` packs a,b at 8,12). */
        sd->size = off + sz;
    }
}

/* Finalize a struct/union definition: round the total size up to the struct's
 * natural alignment (max member alignment).  Call once after the last member
 * is pushed.  Applying this per-member would prematurely pad the struct and
 * break trailing-member packing. */
void struct_def_finish(StructDef *sd) {
    if (sd->is_packed && sd->align < 1) sd->align = 1;
    sd->size = align_up(sd->size, sd->align);
}

void struct_def_apply_sso(StructDef *sd, int is_big_endian) {
    if (!sd) return;
    if (sd->is_big_endian == is_big_endian) return;
    sd->is_big_endian = is_big_endian;
    if (is_big_endian) {
        for (int i = 0; i < sd->num_members; i++) {
            StructMember *m = &sd->members[i];
            if (m->bit_width > 0) {
                int uw = type_size(m->type);
                if (uw > 8) uw = 8;
                if (uw < 1) uw = 1;
                int unit_bits = uw * 8;
                m->bit_offset = unit_bits - m->bit_offset - m->bit_width;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Switch case helper                                                    */
/* ------------------------------------------------------------------ */

void switch_push_case_range(Stmt *s, int is_default, long long value, long long high_value, int is_range, const char *label_name) {
    if (fakecc_had_error()) return;
    if (is_default) {
        for (int i = 0; i < s->u.switch_s.num_cases; i++) {
            if (s->u.switch_s.cases[i].is_default) {
                die_at(s->loc.file, s->loc.line, s->loc.col,
                       "multiple default labels in one switch");
                return;
            }
        }
    } else {
        long long lo = value;
        long long hi = is_range ? high_value : value;
        for (int i = 0; i < s->u.switch_s.num_cases; i++) {
            const SwitchCase *sc = &s->u.switch_s.cases[i];
            if (sc->is_default) continue;
            long long slo = sc->value;
            long long shi = sc->is_range ? sc->high_value : sc->value;
            if (lo <= shi && hi >= slo) {
                die_at(s->loc.file, s->loc.line, s->loc.col,
                       "duplicate case value");
                return;
            }
        }
    }
    if (s->u.switch_s.num_cases >= s->u.switch_s.cap_cases) {
        int nc = s->u.switch_s.cap_cases ? s->u.switch_s.cap_cases * 2 : 4;
        s->u.switch_s.cases = realloc(s->u.switch_s.cases,
                                      nc * sizeof(SwitchCase));
        if (!s->u.switch_s.cases) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        s->u.switch_s.cap_cases = nc;
    }
    SwitchCase *c = &s->u.switch_s.cases[s->u.switch_s.num_cases++];
    c->is_default = is_default;
    c->value = value;
    c->high_value = high_value;
    c->is_range = is_range;
    c->label_name = label_name ? xstrdup(label_name) : NULL;
    stmt_array_init(&c->stmts);
}

void switch_push_case(Stmt *s, int is_default, long long value, const char *label_name) {
    switch_push_case_range(s, is_default, value, value, 0, label_name);
}

/* ------------------------------------------------------------------ */
/* Enum registry                                                        */
/* ------------------------------------------------------------------ */

void enum_registry_init(EnumRegistry *r) {
    r->data = NULL; r->len = 0; r->cap = 0;
}

void enum_registry_free(EnumRegistry *r) {
    for (size_t i = 0; i < r->len; i++) {
        EnumDef *ed = &r->data[i];
        free(ed->tag);
        for (int j = 0; j < ed->num_constants; j++)
            free(ed->constants[j].name);
        free(ed->constants);
    }
    free(r->data);
    r->data = NULL; r->len = 0; r->cap = 0;
}

EnumDef *enum_registry_add(EnumRegistry *r, const char *tag, SourceLoc loc) {
    if (r->len >= r->cap) {
        size_t nc = r->cap ? r->cap * 2 : 4;
        r->data = realloc(r->data, nc * sizeof(EnumDef));
        if (!r->data) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        r->cap = nc;
    }
    EnumDef *ed = &r->data[r->len++];
    ed->tag = tag ? xstrdup(tag) : NULL;
    ed->constants = NULL; ed->num_constants = 0; ed->cap_constants = 0;
    ed->loc = loc;
    ed->has_underlying_type = 0;
    ed->underlying_type = type_default_int();
    return ed;
}

Type enum_def_as_type(const EnumDef *ed, int enum_id) {
    Type t = (ed && ed->has_underlying_type)
        ? type_clone(ed->underlying_type) : type_default_int();
    t.enum_id = enum_id;
    return t;
}

EnumDef *enum_registry_find(EnumRegistry *r, const char *tag) {
    if (!tag) return NULL;
    for (size_t i = 0; i < r->len; i++)
        if (r->data[i].tag && strcmp(r->data[i].tag, tag) == 0) return &r->data[i];
    return NULL;
}

int enum_def_push_constant(EnumDef *ed, const char *name, int has_value,
                           int value, SourceLoc loc) {
    (void)loc;
    if (ed->num_constants >= ed->cap_constants) {
        int nc = ed->cap_constants ? ed->cap_constants * 2 : 4;
        ed->constants = realloc(ed->constants, nc * sizeof(EnumConstant));
        if (!ed->constants) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        ed->cap_constants = nc;
    }
    int assigned;
    if (has_value) {
        assigned = value;
    } else {
        assigned = (ed->num_constants > 0)
            ? ed->constants[ed->num_constants - 1].value + 1 : 0;
    }
    ed->constants[ed->num_constants].name = xstrdup(name);
    ed->constants[ed->num_constants].value = assigned;
    ed->num_constants++;
    return assigned;
}

const EnumConstant *enum_registry_find_constant(const EnumRegistry *r,
                                                const char *name) {
    for (size_t i = 0; i < r->len; i++) {
        const EnumDef *ed = &r->data[i];
        for (int j = 0; j < ed->num_constants; j++)
            if (strcmp(ed->constants[j].name, name) == 0)
                return &ed->constants[j];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Typedef registry                                                     */
/* ------------------------------------------------------------------ */

void typedef_registry_init(TypedefRegistry *r) {
    r->data = NULL; r->len = 0; r->cap = 0;
}

void typedef_registry_free(TypedefRegistry *r) {
    for (size_t i = 0; i < r->len; i++) {
        free(r->data[i].name);
        type_free(&r->data[i].type);
    }
    free(r->data);
    r->data = NULL; r->len = 0; r->cap = 0;
}

TypedefEntry *typedef_registry_add(TypedefRegistry *r, const char *name, Type type) {
    if (r->len >= r->cap) {
        size_t nc = r->cap ? r->cap * 2 : 4;
        r->data = realloc(r->data, nc * sizeof(TypedefEntry));
        if (!r->data) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        r->cap = nc;
    }
    TypedefEntry *e = &r->data[r->len++];
    e->name = xstrdup(name);
    e->type = type; /* takes ownership */
    return e;
}

const Type *typedef_registry_find(const TypedefRegistry *r, const char *name) {
    size_t i = r->len;
    while (i > 0) {
        i--;
        if (strcmp(r->data[i].name, name) == 0) return &r->data[i].type;
    }
    return NULL;
}

TypedefEntry *typedef_registry_get(TypedefRegistry *r, const char *name) {
    size_t i = r->len;
    while (i > 0) {
        i--;
        if (strcmp(r->data[i].name, name) == 0) return &r->data[i];
    }
    return NULL;
}

void typedef_registry_truncate(TypedefRegistry *r, size_t len) {
    while (r->len > len) {
        r->len--;
        free(r->data[r->len].name);
        type_free(&r->data[r->len].type);
    }
}

/* ------------------------------------------------------------------ */
/* Expr constructors & destructor                                       */
/* ------------------------------------------------------------------ */

static Expr *expr_alloc(ExprKind k, SourceLoc loc) {
    Expr *e = malloc(sizeof(Expr));
    if (!e) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
    memset(e, 0, sizeof(Expr));
    e->kind = k;
    e->loc = loc;
    e->type = type_default_int();
    return e;
}

Expr *expr_new_int(long long v, SourceLoc loc) {
    Expr *e = expr_alloc(EX_INT_LIT, loc);
    e->u.int_val = v;
    return e;
}

Expr *expr_new_int_typed(long long v, int width, int is_unsigned, SourceLoc loc) {
    Expr *e = expr_new_int(v, loc);
    e->type = type_make_int(width, is_unsigned);
    return e;
}

Expr *expr_new_int_bits(unsigned long long lo, unsigned long long hi,
                       int width, int is_unsigned, SourceLoc loc) {
    Expr *e = expr_new_int((long long)lo, loc);
    e->int_hi = hi;
    e->type = type_make_int(width, is_unsigned);
    return e;
}

Expr *expr_new_binop(BinOp op, Expr *l, Expr *r, SourceLoc loc) {
    Expr *e = expr_alloc(EX_BINOP, loc);
    e->u.bin.op = op;
    e->u.bin.l = l;
    e->u.bin.r = r;
    return e;
}

Expr *expr_new_unary(UnaryOp op, Expr *operand, SourceLoc loc) {
    Expr *e = expr_alloc(EX_UNARY, loc);
    e->u.un.op = op;
    e->u.un.operand = operand;
    return e;
}

Expr *expr_new_var(const char *name, SourceLoc loc) {
    Expr *e = expr_alloc(EX_VAR, loc);
    e->u.var.name = xstrdup(name);
    e->u.var.pkg = NULL;
    return e;
}

Expr *expr_new_var_qual(const char *pkg, const char *name, SourceLoc loc) {
    Expr *e = expr_new_var(name, loc);
    e->u.var.pkg = xstrdup(pkg);
    return e;
}

Expr *expr_new_assign(Expr *lvalue, Expr *rvalue, SourceLoc loc) {
    Expr *e = expr_alloc(EX_ASSIGN, loc);
    e->u.assign.lvalue = lvalue;
    e->u.assign.rvalue = rvalue;
    return e;
}

Expr *expr_new_call(Expr *callee, SourceLoc loc) {
    Expr *e = expr_alloc(EX_CALL, loc);
    e->u.call.callee = callee;   /* takes ownership */
    e->u.call.args.data = NULL;
    e->u.call.args.len = 0;
    e->u.call.args.cap = 0;
    return e;
}

void expr_call_set_callee(Expr *e, Expr *callee) {
    if (!e || e->kind != EX_CALL) return;
    if (e->u.call.callee) expr_free(e->u.call.callee);
    e->u.call.callee = callee;   /* takes ownership */
}

Expr *expr_new_str(const char *bytes, int len, SourceLoc loc) {
    Expr *e = expr_alloc(EX_STR, loc);
    /* type is set by sema: char[len+1] initially, decays to char* on use */
    e->u.str.bytes = malloc(len + 1);
    if (!e->u.str.bytes) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
    memcpy(e->u.str.bytes, bytes, len);
    e->u.str.bytes[len] = '\0';
    e->u.str.len = len;
    return e;
}

Expr *expr_new_float_lit(const char *text, int width, SourceLoc loc) {
    Expr *e = expr_alloc(EX_FLOAT_LIT, loc);
    e->u.float_text = xstrdup(text);
    /* type is set by sema, but stash width now so parser-level consumers work */
    e->type = type_make_float(width);
    return e;
}

void expr_call_push_arg(Expr *e, Expr *arg) {
    ExprArray *a = &e->u.call.args;
    if (a->len >= a->cap) {
        a->cap = a->cap ? a->cap * 2 : 4;
        a->data = realloc(a->data, a->cap * sizeof(Expr *));
        if (!a->data) { fprintf(stderr, "fakecc: out of memory\n"); exit(1); }
    }
    a->data[a->len++] = arg;
}

Expr *expr_new_addr(Expr *operand, SourceLoc loc) {
    Expr *e = expr_alloc(EX_ADDR, loc); e->u.addr.operand = operand; return e;
}
Expr *expr_new_deref(Expr *operand, SourceLoc loc) {
    Expr *e = expr_alloc(EX_DEREF, loc); e->u.deref.operand = operand; return e;
}
Expr *expr_new_index(Expr *array, Expr *index, SourceLoc loc) {
    Expr *e = expr_alloc(EX_INDEX, loc);
    e->u.idx.array = array; e->u.idx.index = index; return e;
}
Expr *expr_new_member(Expr *obj, const char *name, SourceLoc loc) {
    Expr *e = expr_alloc(EX_MEMBER, loc);
    e->u.member.obj = obj;
    e->u.member.name = xstrdup(name);
    return e;
}
Expr *expr_new_cast(Type target, Expr *operand, SourceLoc loc) {
    Expr *e = expr_alloc(EX_CAST, loc);
    e->u.cast.target = type_clone(target); e->u.cast.operand = operand; return e;
}
Expr *expr_new_sizeof_type(Type t, SourceLoc loc) {
    Expr *e = expr_alloc(EX_SIZEOF_TYPE, loc);
    e->u.sizeof_t.target = type_clone(t); return e;
}
Expr *expr_new_sizeof_expr(Expr *operand, SourceLoc loc) {
    Expr *e = expr_alloc(EX_SIZEOF_EXPR, loc);
    e->u.sizeof_e.operand = operand; return e;
}
Expr *expr_new_alignof_type(Type t, SourceLoc loc) {
    Expr *e = expr_alloc(EX_ALIGNOF_TYPE, loc);
    e->u.alignof_t.target = type_clone(t); return e;
}
Expr *expr_new_alignof_expr(Expr *operand, SourceLoc loc) {
    Expr *e = expr_alloc(EX_ALIGNOF_EXPR, loc);
    e->u.alignof_e.operand = operand; return e;
}
Expr *expr_new_ternary(Expr *cond, Expr *then, Expr *else_, SourceLoc loc) {
    Expr *e = expr_alloc(EX_TERNARY, loc);
    e->u.tern.cond = cond; e->u.tern.then = then; e->u.tern.else_ = else_; return e;
}
Expr *expr_new_inc_dec(Expr *operand, int is_inc, int is_prefix, SourceLoc loc) {
    Expr *e = expr_alloc(EX_INC_DEC, loc);
    e->u.incdec.operand = operand; e->u.incdec.is_inc = is_inc;
    e->u.incdec.is_prefix = is_prefix; return e;
}
Expr *expr_new_compound_assign(Expr *lvalue, Expr *rvalue, BinOp op, SourceLoc loc) {
    Expr *e = expr_alloc(EX_COMPOUND_ASSIGN, loc);
    e->u.comp.lvalue = lvalue; e->u.comp.rvalue = rvalue; e->u.comp.op = op; return e;
}
Expr *expr_new_comma(Expr *l, Expr *r, SourceLoc loc) {
    Expr *e = expr_alloc(EX_COMMA, loc);
    e->u.comma.lhs = l; e->u.comma.rhs = r; return e;
}
Expr *expr_new_init_list(Expr **elements, int num_elements, SourceLoc loc) {
    Expr *e = expr_alloc(EX_INIT_LIST, loc);
    e->u.init_list.elements = elements;
    e->u.init_list.num_elements = num_elements;
    e->u.init_list.desig_kind = malloc(num_elements * sizeof(int));
    e->u.init_list.desig_index = malloc(num_elements * sizeof(int));
    e->u.init_list.desig_member = calloc(num_elements, sizeof(char *));
    for (int i = 0; i < num_elements; i++) {
        e->u.init_list.desig_kind[i] = -1;
        e->u.init_list.desig_index[i] = -1;
    }
    return e;
}

Expr *expr_new_compound_literal(Type target_type, Expr *init, SourceLoc loc) {
    Expr *e = expr_alloc(EX_COMPOUND_LITERAL, loc);
    e->u.compound.target_type = type_clone(target_type); /* own a copy so the caller may type_free(&ty) */
    e->u.compound.init = init;
    return e;
}

Expr *expr_new_stmt_expr(StmtArray *stmts, SourceLoc loc) {
    Expr *e = expr_alloc(EX_STMT_EXPR, loc);
    e->u.stmt_expr.stmts = malloc(sizeof(StmtArray));
    if (!e->u.stmt_expr.stmts) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
    *e->u.stmt_expr.stmts = *stmts;
    return e;
}

Expr *expr_new_label_addr(const char *label, SourceLoc loc) {
    Expr *e = expr_alloc(EX_LABEL_ADDR, loc);
    e->u.label_addr.label = xstrdup(label);
    return e;
}

void expr_free(Expr *e) {
    if (!e) return;
    switch (e->kind) {
    case EX_INT_LIT:
        break;
    case EX_BINOP:
        expr_free(e->u.bin.l);
        expr_free(e->u.bin.r);
        break;
    case EX_UNARY:
        expr_free(e->u.un.operand);
        break;
    case EX_VAR:
        free(e->u.var.name);
        free(e->u.var.pkg);
        break;
    case EX_ASSIGN:
        expr_free(e->u.assign.lvalue);
        expr_free(e->u.assign.rvalue);
        break;
    case EX_CALL:
        expr_free(e->u.call.callee);
        for (size_t i = 0; i < e->u.call.args.len; i++)
            expr_free(e->u.call.args.data[i]);
        free(e->u.call.args.data);
        type_free(&e->va_arg_type);
        break;
    case EX_STR:
        free(e->u.str.bytes);
        break;
    case EX_FLOAT_LIT:
        free(e->u.float_text);
        break;
    case EX_ADDR:  expr_free(e->u.addr.operand); break;
    case EX_DEREF: expr_free(e->u.deref.operand); break;
    case EX_INDEX:
        expr_free(e->u.idx.array); expr_free(e->u.idx.index); break;
    case EX_MEMBER:
        expr_free(e->u.member.obj);
        free(e->u.member.name);
        break;
    case EX_CAST:
        type_free(&e->u.cast.target);
        expr_free(e->u.cast.operand);
        break;
    case EX_SIZEOF_TYPE:
        type_free(&e->u.sizeof_t.target); break;
    case EX_SIZEOF_EXPR:
        expr_free(e->u.sizeof_e.operand); break;
    case EX_TERNARY:
        expr_free(e->u.tern.cond);
        expr_free(e->u.tern.then);
        expr_free(e->u.tern.else_);
        break;
    case EX_INC_DEC:
        expr_free(e->u.incdec.operand);
        break;
    case EX_COMPOUND_ASSIGN:
        expr_free(e->u.comp.lvalue);
        expr_free(e->u.comp.rvalue);
        break;
    case EX_COMMA:
        expr_free(e->u.comma.lhs);
        expr_free(e->u.comma.rhs);
        break;
    case EX_INIT_LIST: {
        for (int i = 0; i < e->u.init_list.num_elements; i++)
            expr_free(e->u.init_list.elements[i]);
        free(e->u.init_list.elements);
        if (e->u.init_list.desig_member) {
            for (int i = 0; i < e->u.init_list.num_elements; i++)
                free(e->u.init_list.desig_member[i]);
        }
        free(e->u.init_list.desig_kind);
        free(e->u.init_list.desig_index);
        free(e->u.init_list.desig_member);
        break;
    }
    case EX_COMPOUND_LITERAL:
        type_free(&e->u.compound.target_type);
        expr_free(e->u.compound.init);
        break;
    case EX_ALIGNOF_TYPE:
        type_free(&e->u.alignof_t.target);
        break;
    case EX_ALIGNOF_EXPR:
        expr_free(e->u.alignof_e.operand);
        break;
    case EX_STMT_EXPR:
        if (e->u.stmt_expr.stmts) {
            stmt_array_free(e->u.stmt_expr.stmts);
            free(e->u.stmt_expr.stmts);
        }
        break;
    case EX_LABEL_ADDR:
        free(e->u.label_addr.label);
        break;
    }
    type_free(&e->type);
    free(e);
}

/* ------------------------------------------------------------------ */
/* Stmt lifetime                                                       */
/* ------------------------------------------------------------------ */

void stmt_free(Stmt *s) {
    if (!s) return;
    switch (s->kind) {
    case ST_DECL:
        free(s->u.decl.name);
        type_free(&s->u.decl.type);
        expr_free(s->u.decl.init);
        free(s->u.decl.alias_target);
        break;
    case ST_EXPR:
        expr_free(s->u.expr);
        break;
    case ST_RETURN:
        expr_free(s->u.value);
        break;
    case ST_IF:
        expr_free(s->u.if_s.cond);
        stmt_free_ptr(s->u.if_s.then_s);
        stmt_free_ptr(s->u.if_s.else_s);
        break;
    case ST_WHILE:
        expr_free(s->u.while_s.cond);
        stmt_free_ptr(s->u.while_s.body);
        break;
    case ST_DO_WHILE:
        expr_free(s->u.do_s.cond);
        stmt_free_ptr(s->u.do_s.body);
        break;
    case ST_GOTO:
        free(s->u.goto_s.target);
        expr_free(s->u.goto_s.target_expr);
        break;
    case ST_LABEL:
        free(s->u.label_s.name);
        stmt_free_ptr(s->u.label_s.stmt);
        break;
    case ST_SWITCH:
        expr_free(s->u.switch_s.cond);
        stmt_free_ptr(s->u.switch_s.body);
        for (int i = 0; i < s->u.switch_s.num_cases; i++) {
            free(s->u.switch_s.cases[i].label_name);
            stmt_array_free(&s->u.switch_s.cases[i].stmts);
        }
        free(s->u.switch_s.cases);
        break;
    case ST_FOR:
        stmt_free_ptr(s->u.for_s.init);
        expr_free(s->u.for_s.cond);
        expr_free(s->u.for_s.step);
        stmt_free_ptr(s->u.for_s.body);
        break;
    case ST_BREAK:
    case ST_CONTINUE:
        break;
    case ST_BLOCK:
        stmt_array_free(&s->u.block);
        break;
    }
}

static Stmt *stmt_clone_ptr(const Stmt *s) {
    if (!s) return NULL;
    Stmt *r = malloc(sizeof(Stmt));
    if (!r) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
    *r = stmt_clone(s);
    return r;
}

Stmt stmt_clone(const Stmt *s) {
    Stmt r;
    memset(&r, 0, sizeof(r));
    if (!s) return r;
    r.kind = s->kind;
    r.loc = s->loc;
    switch (s->kind) {
    case ST_DECL:
        r.u.decl.name = s->u.decl.name ? xstrdup(s->u.decl.name) : NULL;
        r.u.decl.type = type_clone(s->u.decl.type);
        r.u.decl.init = expr_clone(s->u.decl.init);
        r.u.decl.storage_class = s->u.decl.storage_class;
        r.u.decl.alias_target = s->u.decl.alias_target ? xstrdup(s->u.decl.alias_target) : NULL;
        r.u.decl.align = s->u.decl.align;
        break;
    case ST_EXPR:
        r.u.expr = expr_clone(s->u.expr);
        break;
    case ST_RETURN:
        r.u.value = expr_clone(s->u.value);
        break;
    case ST_IF:
        r.u.if_s.cond = expr_clone(s->u.if_s.cond);
        r.u.if_s.then_s = stmt_clone_ptr(s->u.if_s.then_s);
        r.u.if_s.else_s = stmt_clone_ptr(s->u.if_s.else_s);
        break;
    case ST_WHILE:
        r.u.while_s.cond = expr_clone(s->u.while_s.cond);
        r.u.while_s.body = stmt_clone_ptr(s->u.while_s.body);
        break;
    case ST_DO_WHILE:
        r.u.do_s.cond = expr_clone(s->u.do_s.cond);
        r.u.do_s.body = stmt_clone_ptr(s->u.do_s.body);
        break;
    case ST_GOTO:
        r.u.goto_s.target = s->u.goto_s.target ? xstrdup(s->u.goto_s.target) : NULL;
        r.u.goto_s.target_expr = expr_clone(s->u.goto_s.target_expr);
        break;
    case ST_LABEL:
        r.u.label_s.name = s->u.label_s.name ? xstrdup(s->u.label_s.name) : NULL;
        r.u.label_s.stmt = stmt_clone_ptr(s->u.label_s.stmt);
        break;
    case ST_SWITCH:
        r.u.switch_s.cond = expr_clone(s->u.switch_s.cond);
        r.u.switch_s.body = stmt_clone_ptr(s->u.switch_s.body);
        r.u.switch_s.num_cases = s->u.switch_s.num_cases;
        r.u.switch_s.cap_cases = s->u.switch_s.num_cases;
        r.u.switch_s.cases = NULL;
        if (s->u.switch_s.num_cases > 0) {
            r.u.switch_s.cases = calloc((size_t)s->u.switch_s.num_cases, sizeof(SwitchCase));
            for (int i = 0; i < s->u.switch_s.num_cases; i++) {
                r.u.switch_s.cases[i].is_default = s->u.switch_s.cases[i].is_default;
                r.u.switch_s.cases[i].value = s->u.switch_s.cases[i].value;
                r.u.switch_s.cases[i].high_value = s->u.switch_s.cases[i].high_value;
                r.u.switch_s.cases[i].is_range = s->u.switch_s.cases[i].is_range;
                r.u.switch_s.cases[i].label_name = s->u.switch_s.cases[i].label_name
                    ? xstrdup(s->u.switch_s.cases[i].label_name) : NULL;
                stmt_array_init(&r.u.switch_s.cases[i].stmts);
                for (size_t j = 0; j < s->u.switch_s.cases[i].stmts.len; j++)
                    stmt_array_push(&r.u.switch_s.cases[i].stmts,
                                    stmt_clone(&s->u.switch_s.cases[i].stmts.data[j]));
            }
        }
        break;
    case ST_FOR:
        r.u.for_s.init = stmt_clone_ptr(s->u.for_s.init);
        r.u.for_s.cond = expr_clone(s->u.for_s.cond);
        r.u.for_s.step = expr_clone(s->u.for_s.step);
        r.u.for_s.body = stmt_clone_ptr(s->u.for_s.body);
        break;
    case ST_BREAK:
    case ST_CONTINUE:
        break;
    case ST_BLOCK:
        stmt_array_init(&r.u.block);
        for (size_t i = 0; i < s->u.block.len; i++)
            stmt_array_push(&r.u.block, stmt_clone(&s->u.block.data[i]));
        break;
    }
    return r;
}

Stmt *stmt_alloc(void) {
    Stmt *s = malloc(sizeof(Stmt));
    if (!s) {
        fprintf(stderr, "fakecc: out of memory\n");
        exit(1);
    }
    memset(s, 0, sizeof(Stmt));
    return s;
}

void stmt_free_ptr(Stmt *s) {
    if (!s) return;
    stmt_free(s);
    free(s);
}

void stmt_array_init(StmtArray *a) {
    a->data = NULL;
    a->len = 0;
    a->cap = 0;
}

void stmt_array_push(StmtArray *a, Stmt s) {
    if (a->len >= a->cap) {
        size_t new_cap = a->cap ? a->cap * 2 : 8;
        a->data = realloc(a->data, new_cap * sizeof(Stmt));
        if (!a->data) {
            fprintf(stderr, "fakecc: out of memory\n");
            exit(1);
        }
        a->cap = new_cap;
    }
    a->data[a->len++] = s;
}

void stmt_array_free(StmtArray *a) {
    for (size_t i = 0; i < a->len; i++) {
        stmt_free(&a->data[i]);
    }
    free(a->data);
    a->data = NULL;
    a->len = 0;
    a->cap = 0;
}

/* ------------------------------------------------------------------ */
/* TranslationUnit lifetime                                            */
/* ------------------------------------------------------------------ */

void import_array_init(ImportArray *a) {
    a->data = NULL;
    a->len = 0;
    a->cap = 0;
}

void import_array_push(ImportArray *a, const char *name, SourceLoc loc) {
    if (a->len >= a->cap) {
        a->cap = a->cap ? a->cap * 2 : 4;
        a->data = realloc(a->data, a->cap * sizeof(ImportDecl));
        if (!a->data) { fprintf(stderr, "fakecc: out of memory\n"); exit(1); }
    }
    a->data[a->len].name = xstrdup(name);
    a->data[a->len].loc = loc;
    a->len++;
}

void import_array_free(ImportArray *a) {
    for (size_t i = 0; i < a->len; i++)
        free(a->data[i].name);
    free(a->data);
    a->data = NULL;
    a->len = 0;
    a->cap = 0;
}

void tu_init(TranslationUnit *tu) {
    tu->package.name = NULL;
    tu->package.loc.file = NULL;
    tu->package.loc.line = 0;
    tu->package.loc.col = 0;
    import_array_init(&tu->imports);
    stmt_array_init(&tu->globals);
    tu->functions.data = NULL;
    tu->functions.len = 0;
    tu->functions.cap = 0;
    struct_registry_init(&tu->structs);
    enum_registry_init(&tu->enums);
    typedef_registry_init(&tu->typedefs);

    /* Predeclare the `va_list` type used by the va_start/va_arg/va_end
     * builtins.  SysV AMD64 is the 24-byte four-field struct.  Darwin
     * arm64 (clang) is an 8-byte cursor: anonymous arguments live on the
     * stack, and va_list is just the pointer to the next one. */
    SourceLoc vloc = {0};
    StructDef *va = struct_registry_add(&tu->structs, "__va_list_tag", vloc);
    if (abi_is_arm64()) {
        struct_def_push_member(va, "overflow_arg_area",
                               type_make_ptr(type_make_void()), -1);
    } else {
        struct_def_push_member(va, "gp_offset", type_make_int(4, 1), -1);
        struct_def_push_member(va, "fp_offset", type_make_int(4, 1), -1);
        struct_def_push_member(va, "overflow_arg_area",
                               type_make_ptr(type_make_void()), -1);
        struct_def_push_member(va, "reg_save_area",
                               type_make_ptr(type_make_void()), -1);
    }
    struct_def_finish(va);
    Type va_type = type_make_struct("__va_list_tag", va->size);
    typedef_registry_add(&tu->typedefs, "va_list", va_type);
    typedef_registry_add(&tu->typedefs, "__builtin_va_list", type_clone(va_type));
    typedef_registry_add(&tu->typedefs, "__INT8_TYPE__", type_make_int(1, 0));
    typedef_registry_add(&tu->typedefs, "__UINT8_TYPE__", type_make_int(1, 1));
    typedef_registry_add(&tu->typedefs, "__INT16_TYPE__", type_make_int(2, 0));
    typedef_registry_add(&tu->typedefs, "__UINT16_TYPE__", type_make_int(2, 1));
    typedef_registry_add(&tu->typedefs, "__INT32_TYPE__", type_make_int(4, 0));
    typedef_registry_add(&tu->typedefs, "__UINT32_TYPE__", type_make_int(4, 1));
    typedef_registry_add(&tu->typedefs, "__INT64_TYPE__", type_make_int(8, 0));
    typedef_registry_add(&tu->typedefs, "__UINT64_TYPE__", type_make_int(8, 1));
    typedef_registry_add(&tu->typedefs, "__INTMAX_TYPE__", type_make_int(8, 0));
    typedef_registry_add(&tu->typedefs, "__UINTMAX_TYPE__", type_make_int(8, 1));
    typedef_registry_add(&tu->typedefs, "__SIZE_TYPE__", type_make_int(8, 1));
    typedef_registry_add(&tu->typedefs, "__PTRDIFF_TYPE__", type_make_int(8, 0));
    typedef_registry_add(&tu->typedefs, "__INTPTR_TYPE__", type_make_int(8, 0));
    typedef_registry_add(&tu->typedefs, "__UINTPTR_TYPE__", type_make_int(8, 1));
    typedef_registry_add(&tu->typedefs, "__WCHAR_TYPE__", type_make_int(4, 0));
    typedef_registry_add(&tu->typedefs, "wchar_t", type_make_int(4, 0));
    typedef_registry_add(&tu->typedefs, "__WINT_TYPE__", type_make_int(4, 0));
}

void tu_free(TranslationUnit *tu) {
    free(tu->package.name);
    import_array_free(&tu->imports);
    stmt_array_free(&tu->globals);
    for (size_t i = 0; i < tu->functions.len; i++) {
        free(tu->functions.data[i].name);
        type_free(&tu->functions.data[i].ret_type);
        param_array_free(&tu->functions.data[i].params);
        stmt_array_free(&tu->functions.data[i].body);
        free(tu->functions.data[i].alias_target);
    }
    free(tu->functions.data);
    struct_registry_free(&tu->structs);
    enum_registry_free(&tu->enums);
    typedef_registry_free(&tu->typedefs);
}

void param_array_init(ParamArray *a) {
    a->data = NULL; a->len = 0; a->cap = 0;
}

void param_array_push(ParamArray *a, const char *name, Type type, SourceLoc loc) {
    if (a->len >= a->cap) {
        a->cap = a->cap ? a->cap * 2 : 4;
        a->data = realloc(a->data, a->cap * sizeof(Param));
        if (!a->data) { fprintf(stderr, "fakecc: out of memory\n"); exit(1); }
    }
    a->data[a->len].name = xstrdup(name);
    a->data[a->len].type = type;
    a->data[a->len].loc = loc;
    a->len++;
}

void param_array_free(ParamArray *a) {
    for (size_t i = 0; i < a->len; i++) {
        free(a->data[i].name);
        type_free(&a->data[i].type);
    }
    free(a->data);
    a->data = NULL; a->len = 0; a->cap = 0;
}

/* ------------------------------------------------------------------ */
/* Compile-time integer constant folding                               */
/* ------------------------------------------------------------------ */

/* EX_VAR / *p / a[i] are born with type_default_int().  Folding sizeof of
 * that dummy type yields 4 and mis-sizes `T a[sizeof g / sizeof *g]`.
 * Trust those operand types only after sema (or IR) has filled them in. */
static int sizeof_operand_needs_sema(const Expr *op) {
    if (!op) return 0;
    switch (op->kind) {
    case EX_VAR:
    case EX_DEREF:
    case EX_INDEX:
    case EX_MEMBER:
    case EX_ADDR:
    case EX_CALL:
    case EX_ASSIGN:
    case EX_COMPOUND_ASSIGN:
    case EX_INC_DEC:
    case EX_STMT_EXPR:
        return 1;
    case EX_UNARY:
        return sizeof_operand_needs_sema(op->u.un.operand);
    case EX_BINOP:
        return sizeof_operand_needs_sema(op->u.bin.l)
            || sizeof_operand_needs_sema(op->u.bin.r);
    case EX_TERNARY:
        return sizeof_operand_needs_sema(op->u.tern.then)
            || sizeof_operand_needs_sema(op->u.tern.else_);
    case EX_COMMA:
        return sizeof_operand_needs_sema(op->u.comma.rhs);
    case EX_CAST:
        return 0;
    default:
        return 0;
    }
}

static int fold_sizeof_types_ready(void) {
    if (fakecc_had_error()) return 0;
    return get_sema_tu() != NULL || get_ir_tu() != NULL;
}

/* Fold e to a single integer constant.  Returns 1 and writes the value to
 * *out if e is an integer literal, a cast of one, or a unary/binary
 * operation on constant integer operands (e.g. (1u << 14) - 1u).  Returns 0
 * otherwise (non-constant, non-integer, or involving pointer values).
 * Used by sema (global-init constness check) and ir (pack_init) so that
 * constant expressions are accepted and emitted exactly like literals. */
int fold_const_int128(const Expr *e, unsigned long long *lo, unsigned long long *hi) {
    if (fakecc_had_error()) return FAKECC_ERR;
    if (!e) return 0;
    if (e->kind == EX_INT_LIT) {
        *lo = (unsigned long long)e->u.int_val;
        *hi = (e->type.width == 16) ? e->int_hi : ((e->u.int_val < 0) ? ~0ULL : 0ULL);
        return 1;
    }
    if (e->kind == EX_VAR && strcmp(e->u.var.name, "__CHAR_BIT__") == 0) {
        *lo = 8;
        *hi = 0;
        return 1;
    }
    if (e->kind == EX_VAR && strcmp(e->u.var.name, "__INT_MAX__") == 0) {
        *lo = 0x7fffffff;
        *hi = 0;
        return 1;
    }
    if (e->kind == EX_CAST) {
        Type t = e->u.cast.target;
        unsigned long long vlo, vhi;
        if (!fold_const_int128(e->u.cast.operand, &vlo, &vhi)) return 0;
        if (t.kind == TY_INT) {
            int w = t.width ? (int)t.width : 4;
            if (t.is_bool) {
                vlo = (vlo != 0 || vhi != 0) ? 1ULL : 0ULL;
                vhi = 0;
            } else if (w < 16) {
                int bits = w * 8;
                unsigned long long mask = (w >= 8) ? ~0ULL : ((1ULL << bits) - 1ULL);
                vlo &= mask;
                if (w < 8) {
                    if (!t.is_unsigned && (vlo & (1ULL << (bits - 1)))) {
                        vlo |= ~mask;
                        vhi = ~0ULL;
                    } else {
                        vhi = 0;
                    }
                } else {
                    /* width 8: low 64 bits; sign-extend into hi if signed. */
                    vhi = (!t.is_unsigned && (vlo >> 63)) ? ~0ULL : 0ULL;
                }
            }
        }
        *lo = vlo;
        *hi = vhi;
        return 1;
    }
    if (e->kind == EX_UNARY) {
        unsigned long long vlo, vhi;
        if (!fold_const_int128(e->u.un.operand, &vlo, &vhi)) return 0;
        switch (e->u.un.op) {
        case UOP_NEG: *lo = 0ULL - vlo; *hi = 0ULL - vhi - (vlo != 0ULL ? 1ULL : 0ULL); return 1;
        case UOP_POS: *lo = vlo; *hi = vhi; return 1;
        case UOP_BITNOT: *lo = ~vlo; *hi = ~vhi; return 1;
        case UOP_NOT: *lo = (vlo == 0ULL && vhi == 0ULL) ? 1ULL : 0ULL; *hi = 0ULL; return 1;
        default: return 0;
        }
    }
    if (e->kind == EX_BINOP) {
        unsigned long long llo, lhi, rlo, rhi;
        if (!fold_const_int128(e->u.bin.l, &llo, &lhi)) return 0;
        if (!fold_const_int128(e->u.bin.r, &rlo, &rhi)) return 0;
        switch (e->u.bin.op) {
        case BOP_ADD: {
            *lo = llo + rlo;
            *hi = lhi + rhi + (*lo < llo ? 1ULL : 0ULL);
            return 1;
        }
        case BOP_SUB: {
            *lo = llo - rlo;
            *hi = lhi - rhi - (llo < rlo ? 1ULL : 0ULL);
            return 1;
        }
        case BOP_SHL: {
            unsigned long long n = rlo;
            if (n == 0) { *lo = llo; *hi = lhi; return 1; }
            if (n >= 128) { *lo = 0; *hi = 0; return 1; }
            if (n >= 64) { *lo = 0; *hi = llo << (n - 64); return 1; }
            *lo = llo << n;
            *hi = (lhi << n) | (llo >> (64 - n));
            return 1;
        }
        case BOP_SHR: {
            unsigned long long n = rlo;
            int arith = (e->type.kind == TY_INT && !e->type.is_unsigned)
                     || (e->u.bin.l->type.kind == TY_INT && !e->u.bin.l->type.is_unsigned);
            if (n == 0) { *lo = llo; *hi = lhi; return 1; }
            if (n >= 128) {
                if (arith && (lhi >> 63)) { *lo = ~0ULL; *hi = ~0ULL; }
                else { *lo = 0; *hi = 0; }
                return 1;
            }
            if (n >= 64) {
                unsigned long long shifted = arith
                    ? (unsigned long long)((long long)lhi >> (int)(n - 64))
                    : (lhi >> (n - 64));
                *lo = shifted;
                *hi = (arith && (lhi >> 63)) ? ~0ULL : 0ULL;
                return 1;
            }
            *lo = (llo >> n) | (lhi << (64 - n));
            *hi = arith ? (unsigned long long)((long long)lhi >> (int)n)
                        : (lhi >> n);
            return 1;
        }
        case BOP_BITAND: *lo = llo & rlo; *hi = lhi & rhi; return 1;
        case BOP_BITOR: *lo = llo | rlo; *hi = lhi | rhi; return 1;
        case BOP_BITXOR: *lo = llo ^ rlo; *hi = lhi ^ rhi; return 1;
        default: return 0;
        }
    }
    return 0;
}

/* Integer-promote a type for UAC: rank below int becomes signed int. */
static void fold_int_promote(int *width, int *is_unsigned) {
    if (fakecc_had_error()) return;
    if (*width < 4) { *width = 4; *is_unsigned = 0; }
    if (*width <= 0) *width = 4;
}

static unsigned long long fold_width_mask(int width) {
    if (fakecc_had_error()) return FAKECC_ERR;
    if (width >= 8) return ~0ULL;
    if (width <= 0) width = 4;
    return (1ULL << (width * 8)) - 1ULL;
}

static long long fold_trunc_int(long long v, int width, int is_unsigned) {
    if (fakecc_had_error()) return FAKECC_ERR;
    if (width >= 8) return v;
    unsigned long long mask = fold_width_mask(width);
    unsigned long long u = (unsigned long long)v & mask;
    if (is_unsigned) return (long long)u;
    int bits = width * 8;
    if (u & (1ULL << (bits - 1)))
        return (long long)(u | ~mask);
    return (long long)u;
}

/* Usual arithmetic conversions on two integer operands: 1 if the common
 * type is unsigned. */
static int fold_uac_unsigned(const Expr *l, const Expr *r) {
    if (fakecc_had_error()) return 0;
    int lw = (l && l->type.kind == TY_INT && l->type.width) ? (int)l->type.width : 4;
    int rw = (r && r->type.kind == TY_INT && r->type.width) ? (int)r->type.width : 4;
    int lu = (l && l->type.kind == TY_INT) ? l->type.is_unsigned : 0;
    int ru = (r && r->type.kind == TY_INT) ? r->type.is_unsigned : 0;
    fold_int_promote(&lw, &lu);
    fold_int_promote(&rw, &ru);
    if (lu == ru) return lu;
    if (lu && lw >= rw) return 1;
    if (ru && rw >= lw) return 1;
    if (!lu && lw > rw) return 0;
    if (!ru && rw > lw) return 0;
    return 1;
}

static int fold_binop_unsigned(const Expr *e) {
    if (fakecc_had_error()) return 0;
    BinOp op = e->u.bin.op;
    if (op == BOP_SHL || op == BOP_SHR) {
        const Expr *l = e->u.bin.l;
        if (l && l->type.kind == TY_INT) return l->type.is_unsigned;
        return e->type.kind == TY_INT && e->type.is_unsigned;
    }
    if (op >= BOP_EQ && op <= BOP_GE)
        return fold_uac_unsigned(e->u.bin.l, e->u.bin.r);
    return e->type.kind == TY_INT && e->type.is_unsigned;
}

static int fold_binop_width(const Expr *e) {
    if (fakecc_had_error()) return 0;
    if (e->u.bin.op >= BOP_EQ && e->u.bin.op <= BOP_GE) {
        int lw = (e->u.bin.l && e->u.bin.l->type.width) ? (int)e->u.bin.l->type.width : 4;
        int rw = (e->u.bin.r && e->u.bin.r->type.width) ? (int)e->u.bin.r->type.width : 4;
        int lu = e->u.bin.l ? e->u.bin.l->type.is_unsigned : 0;
        int ru = e->u.bin.r ? e->u.bin.r->type.is_unsigned : 0;
        fold_int_promote(&lw, &lu);
        fold_int_promote(&rw, &ru);
        return lw >= rw ? lw : rw;
    }
    if (e->type.kind == TY_INT && e->type.width)
        return (int)e->type.width;
    if (e->u.bin.l && e->u.bin.l->type.kind == TY_INT && e->u.bin.l->type.width)
        return (int)e->u.bin.l->type.width;
    return 4;
}

int fold_const_int(const Expr *e, long long *out) {
    if (fakecc_had_error()) return FAKECC_ERR;
    if (!e) return 0;
    if (e->kind == EX_FLOAT_LIT && e->u.float_text) {
        /* Integer conversion of a floating constant: truncate toward zero.
         * Out of long long range the conversion is undefined in C, so clamp
         * instead of letting the compiler itself overflow. */
        long double d = strtold(e->u.float_text, NULL);
        if (d >= 9223372036854775808.0L) *out = 9223372036854775807LL;
        else if (d < -9223372036854775807.0L - 1.0L) *out = (-9223372036854775807LL - 1);
        else *out = (long long)d;
        return 1;
    }
    if (e->kind == EX_INT_LIT) {
        /* int128 literals cannot be folded to a single long long: the high
         * half would be lost.  Return 0 so the expression is evaluated at
         * runtime (where i128_alloc/i128_store2 handle both halves). */
        if (e->type.width == 16)
            return 0;
        *out = e->u.int_val;
        return 1;
    }
    if (e->kind == EX_VAR && strcmp(e->u.var.name, "__CHAR_BIT__") == 0) {
        *out = 8;
        return 1;
    }
    if (e->kind == EX_VAR && strcmp(e->u.var.name, "__INT_MAX__") == 0) {
        *out = 0x7fffffff;
        return 1;
    }
    if (e->kind == EX_CAST) {
        /* Apply the cast's target representation (truncation / sign-extend).
         * Pointer casts of integer constants keep the operand value. */
        Type t = e->u.cast.target;
        long long v;
        if (!fold_const_int(e->u.cast.operand, &v)) return 0;
        if (t.kind == TY_INT) {
            if (t.is_bool) {
                *out = v != 0 ? 1 : 0;
                return 1;
            }
            int w = t.width ? (int)t.width : 4;
            if (w >= 8) {
                *out = v;
                return 1;
            }
            int bits = w * 8;
            unsigned long long mask = (1ULL << bits) - 1ULL;
            unsigned long long u = (unsigned long long)v & mask;
            if (t.is_unsigned) {
                *out = (long long)u;
            } else if (u & (1ULL << (bits - 1))) {
                *out = (long long)(u | ~mask);
            } else {
                *out = (long long)u;
            }
            return 1;
        }
        *out = v;
        return 1;
    }
    if (e->kind == EX_UNARY) {
        long long v;
        if (!fold_const_int(e->u.un.operand, &v)) return 0;
        switch (e->u.un.op) {
        case UOP_NEG: *out = (long long)(0ull - (unsigned long long)v); return 1;
        case UOP_POS: *out = v; return 1;
        case UOP_BITNOT: *out = ~v; return 1;
        case UOP_NOT: *out = !v ? 1 : 0; return 1;
        default: return 0;
        }
    }
    if (e->kind == EX_BINOP) {
        long long l, r;
        if (!fold_const_int(e->u.bin.l, &l)) return 0;
        if (!fold_const_int(e->u.bin.r, &r)) return 0;
        int is_u = fold_binop_unsigned(e);
        int w = fold_binop_width(e);
        if (w <= 0) w = 4;
        if (e->u.bin.op != BOP_AND && e->u.bin.op != BOP_OR
            && e->u.bin.op != BOP_SHL && e->u.bin.op != BOP_SHR
            && e->u.bin.op != BOP_ADD && e->u.bin.op != BOP_SUB
            && e->u.bin.op != BOP_MUL) {
            l = fold_trunc_int(l, w, is_u);
            r = fold_trunc_int(r, w, is_u);
        } else if (e->u.bin.op == BOP_SHL || e->u.bin.op == BOP_SHR) {
            int lw = (e->u.bin.l && e->u.bin.l->type.width)
                     ? (int)e->u.bin.l->type.width : w;
            int lu = (e->u.bin.l && e->u.bin.l->type.kind == TY_INT)
                     ? e->u.bin.l->type.is_unsigned : is_u;
            fold_int_promote(&lw, &lu);
            l = fold_trunc_int(l, lw, lu);
            w = lw;
            is_u = lu;
        }
        unsigned long long ul = (unsigned long long)l;
        unsigned long long ur = (unsigned long long)r;
        if (w < 8) {
            unsigned long long mask = fold_width_mask(w);
            ul &= mask;
            ur &= mask;
        }
        long long res;
        int trunc_result = 1;
        switch (e->u.bin.op) {
        case BOP_ADD:
            /* Unsigned arithmetic: the fold must not overflow in the
             * compiler's own signed type (-fwrapv semantics, which is what the
             * oracle runs with anyway). */
            res = (long long)((unsigned long long)l + (unsigned long long)r);
            /* Signed ADD/SUB/MUL of constants keep the full 64-bit value so
             * parse-time folds (when e->type is still default int) do not
             * truncate `1L << 62` to 32 bits before a later subtract. */
            if (!is_u) trunc_result = 0;
            break;
        case BOP_SUB:
            res = (long long)((unsigned long long)l - (unsigned long long)r);
            if (!is_u) trunc_result = 0;
            break;
        case BOP_MUL:
            res = (long long)((unsigned long long)l * (unsigned long long)r);
            if (!is_u) trunc_result = 0;
            break;
        case BOP_DIV:
            if (r == 0) { *out = 0; return 1; }
            res = (long long)((unsigned long long)l / (unsigned long long)r);
            break;
        case BOP_MOD:
            if (r == 0) { *out = 0; return 1; }
            res = (long long)((unsigned long long)l % (unsigned long long)r);
            break;
        case BOP_BITAND: res = (long long)(ul & ur); break;
        case BOP_BITOR:  res = (long long)(ul | ur); break;
        case BOP_BITXOR: res = (long long)(ul ^ ur); break;
        case BOP_SHL:
            res = (long long)((unsigned long long)l << (r & 63));
            break;
        case BOP_SHR:
            if (is_u) res = (long long)(ul >> (r & 63));
            else res = l >> (r & 63);
            break;
        case BOP_EQ:     res = is_u ? (ul == ur) : (l == r); break;
        case BOP_NE:     res = is_u ? (ul != ur) : (l != r); break;
        case BOP_LT:     res = is_u ? (ul < ur)  : (l < r);  break;
        case BOP_LE:     res = is_u ? (ul <= ur) : (l <= r); break;
        case BOP_GT:     res = is_u ? (ul > ur)  : (l > r);  break;
        case BOP_GE:     res = is_u ? (ul >= ur) : (l >= r); break;
        case BOP_AND:    res = (l && r) ? 1 : 0; break;
        case BOP_OR:     res = (l || r) ? 1 : 0; break;
        default: return 0;
        }
        if (e->u.bin.op >= BOP_EQ && e->u.bin.op <= BOP_GE)
            *out = res ? 1 : 0;
        else if (e->u.bin.op == BOP_AND || e->u.bin.op == BOP_OR)
            *out = res ? 1 : 0;
        else if (trunc_result)
            *out = fold_trunc_int(res, w, is_u);
        else
            *out = res;
        return 1;
    }
    if (e->kind == EX_SIZEOF_TYPE) {
        Type t = e->u.sizeof_t.target;
        if (type_is_vla(t)) return 0;
        long long sz = type_size(t);
        if (sz < 0) return 0;
        *out = sz;
        return 1;
    }
    if (e->kind == EX_SIZEOF_EXPR) {
        const Expr *op = e->u.sizeof_e.operand;
        Type t;
        if (!op) return 0;
        if (op->kind == EX_STR) {
            *out = (long long)op->u.str.len + 1;
            return 1;
        }
        if (op->kind == EX_COMPOUND_LITERAL)
            t = op->u.compound.target_type;
        else if (op->kind == EX_CAST)
            t = op->u.cast.target;
        else if (sizeof_operand_needs_sema(op) && !fold_sizeof_types_ready())
            return 0;
        else
            t = op->type;
        if (t.kind == TY_VOID && t.width == 0 && !t.tag)
            return 0;
        if (t.kind == TY_ARRAY && t.length <= 0)
            return 0;
        long long sz = type_size(t);
        if (sz < 0) return 0;
        *out = sz;
        return 1;
    }
    if (e->kind == EX_TERNARY) {
        long long c;
        if (!fold_const_int(e->u.tern.cond, &c)) return 0;
        if (c) {
            if (!e->u.tern.then) {
                *out = c;
                return 1;
            }
            return fold_const_int(e->u.tern.then, out);
        }
        return fold_const_int(e->u.tern.else_, out);
    }
    if (e->kind == EX_ALIGNOF_TYPE) {
        *out = type_align(e->u.alignof_t.target);
        return 1;
    }
    return 0;
}

Expr *expr_clone(const Expr *e) {
    if (!e) return NULL;
    Expr *r = malloc(sizeof(Expr));
    if (!r) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
    memset(r, 0, sizeof(Expr));
    *r = *e;
    r->type = type_clone(e->type);
    r->va_arg_type = type_clone(e->va_arg_type);
    switch (e->kind) {
    case EX_BINOP:
        r->u.bin.l = expr_clone(e->u.bin.l);
        r->u.bin.r = expr_clone(e->u.bin.r);
        break;
    case EX_UNARY:
        r->u.un.operand = expr_clone(e->u.un.operand);
        break;
    case EX_VAR:
        r->u.var.name = e->u.var.name ? xstrdup(e->u.var.name) : NULL;
        r->u.var.pkg = e->u.var.pkg ? xstrdup(e->u.var.pkg) : NULL;
        break;
    case EX_ASSIGN:
        r->u.assign.lvalue = expr_clone(e->u.assign.lvalue);
        r->u.assign.rvalue = expr_clone(e->u.assign.rvalue);
        break;
    case EX_CALL:
        r->u.call.callee = expr_clone(e->u.call.callee);
        r->u.call.args.len = e->u.call.args.len;
        r->u.call.args.cap = e->u.call.args.len;
        if (e->u.call.args.len > 0) {
            r->u.call.args.data = malloc(e->u.call.args.len * sizeof(Expr*));
            for (size_t i = 0; i < e->u.call.args.len; i++)
                r->u.call.args.data[i] = expr_clone(e->u.call.args.data[i]);
        } else {
            r->u.call.args.data = NULL;
        }
        break;
    case EX_STR:
        r->u.str.bytes = e->u.str.bytes ? malloc(e->u.str.len + 1) : NULL;
        if (r->u.str.bytes) {
            memcpy(r->u.str.bytes, e->u.str.bytes, e->u.str.len);
            r->u.str.bytes[e->u.str.len] = '\0';
        }
        break;
    case EX_ADDR:
        r->u.addr.operand = expr_clone(e->u.addr.operand);
        break;
    case EX_DEREF:
        r->u.deref.operand = expr_clone(e->u.deref.operand);
        break;
    case EX_INDEX:
        r->u.idx.array = expr_clone(e->u.idx.array);
        r->u.idx.index = expr_clone(e->u.idx.index);
        break;
    case EX_MEMBER:
        r->u.member.obj = expr_clone(e->u.member.obj);
        r->u.member.name = e->u.member.name ? xstrdup(e->u.member.name) : NULL;
        break;
    case EX_CAST:
        r->u.cast.target = type_clone(e->u.cast.target);
        r->u.cast.operand = expr_clone(e->u.cast.operand);
        break;
    case EX_SIZEOF_TYPE:
        r->u.sizeof_t.target = type_clone(e->u.sizeof_t.target);
        break;
    case EX_SIZEOF_EXPR:
        r->u.sizeof_e.operand = expr_clone(e->u.sizeof_e.operand);
        break;
    case EX_ALIGNOF_TYPE:
        r->u.alignof_t.target = type_clone(e->u.alignof_t.target);
        break;
    case EX_ALIGNOF_EXPR:
        r->u.alignof_e.operand = expr_clone(e->u.alignof_e.operand);
        break;
    case EX_TERNARY:
        r->u.tern.cond = expr_clone(e->u.tern.cond);
        r->u.tern.then = expr_clone(e->u.tern.then);
        r->u.tern.else_ = expr_clone(e->u.tern.else_);
        break;
    case EX_INC_DEC:
        r->u.incdec.operand = expr_clone(e->u.incdec.operand);
        break;
    case EX_COMPOUND_ASSIGN:
        r->u.comp.lvalue = expr_clone(e->u.comp.lvalue);
        r->u.comp.rvalue = expr_clone(e->u.comp.rvalue);
        break;
    case EX_COMMA:
        r->u.comma.lhs = expr_clone(e->u.comma.lhs);
        r->u.comma.rhs = expr_clone(e->u.comma.rhs);
        break;
    case EX_FLOAT_LIT:
        r->u.float_text = e->u.float_text ? xstrdup(e->u.float_text) : NULL;
        break;
    case EX_LABEL_ADDR:
        r->u.label_addr.label = e->u.label_addr.label ? xstrdup(e->u.label_addr.label) : NULL;
        break;
    case EX_INIT_LIST: {
        int n = e->u.init_list.num_elements;
        r->u.init_list.num_elements = n;
        r->u.init_list.elements = n ? malloc((size_t)n * sizeof(Expr *)) : NULL;
        r->u.init_list.desig_kind = NULL;
        r->u.init_list.desig_index = NULL;
        r->u.init_list.desig_member = NULL;
        for (int i = 0; i < n; i++)
            r->u.init_list.elements[i] = expr_clone(e->u.init_list.elements[i]);
        if (e->u.init_list.desig_kind) {
            r->u.init_list.desig_kind = malloc((size_t)n * sizeof(int));
            memcpy(r->u.init_list.desig_kind, e->u.init_list.desig_kind,
                   (size_t)n * sizeof(int));
        }
        if (e->u.init_list.desig_index) {
            r->u.init_list.desig_index = malloc((size_t)n * sizeof(int));
            memcpy(r->u.init_list.desig_index, e->u.init_list.desig_index,
                   (size_t)n * sizeof(int));
        }
        if (e->u.init_list.desig_member) {
            r->u.init_list.desig_member = calloc((size_t)n, sizeof(char *));
            for (int i = 0; i < n; i++)
                r->u.init_list.desig_member[i] = e->u.init_list.desig_member[i]
                    ? xstrdup(e->u.init_list.desig_member[i]) : NULL;
        }
        break;
    }
    case EX_COMPOUND_LITERAL:
        r->u.compound.target_type = type_clone(e->u.compound.target_type);
        r->u.compound.init = expr_clone(e->u.compound.init);
        break;
    case EX_STMT_EXPR:
        r->u.stmt_expr.stmts = NULL;
        if (e->u.stmt_expr.stmts) {
            r->u.stmt_expr.stmts = malloc(sizeof(StmtArray));
            if (!r->u.stmt_expr.stmts) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
            stmt_array_init(r->u.stmt_expr.stmts);
            for (size_t i = 0; i < e->u.stmt_expr.stmts->len; i++)
                stmt_array_push(r->u.stmt_expr.stmts,
                                stmt_clone(&e->u.stmt_expr.stmts->data[i]));
        }
        break;
    default:
        break;
    }
    return r;
}
