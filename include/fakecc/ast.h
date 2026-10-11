#ifndef FAKECC_AST_H
#define FAKECC_AST_H

#include "fakecc/token.h"
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Type — 7a: integer types; 7b/c: pointers + arrays                    */
/* ------------------------------------------------------------------ */

typedef enum {
    TY_VOID,    /* void — only valid as a function return type */
    TY_INT,     /* char/short/int/long × signed/unsigned */
    TY_FLOAT,   /* float (width 4) / double (width 8) */
    TY_PTR,     /* T*  — pointee owned via heap allocation */
    TY_ARRAY,   /* T[N] — elem_type owned via heap; length is a positive int */
    TY_STRUCT,  /* struct Name — layout looked up in module StructRegistry */
    TY_FUNC,    /* function type: ret(params...). Carried as pointee of a TY_PTR. */
} TypeKind;

typedef struct Type Type;
struct Type {
    TypeKind kind;
    long long width;         /* TY_INT: 1/2/4/8. TY_PTR: always 8. TY_ARRAY: elem width. TY_STRUCT: total size. TY_FUNC: 0. */
    int is_unsigned;   /* TY_INT only */
    unsigned is_const : 1; /* const-qualified (assignment forbidden) */
    unsigned is_volatile : 1; /* volatile-qualified (no-op without an optimizer) */
    unsigned is_restrict : 1; /* restrict-qualified (no-op without an optimizer) */
    unsigned is_bool : 1;  /* _Bool (width-1 unsigned that normalizes to 0/1) */
    Type *pointee;     /* TY_PTR only: malloc'd */
    Type *elem_type;   /* TY_ARRAY only: malloc'd */
    long long length;  /* TY_ARRAY only */
    struct Expr *vla_dim; /* TY_ARRAY only: dynamic dimension expr if length == -1 */
    char *tag;         /* TY_STRUCT only: xstrdup'd tag name */
    Type *func_ret;    /* TY_FUNC only: malloc'd return type */
    Type *func_params; /* TY_FUNC only: malloc'd array of param types (nparams long) */
    int   func_nparams;/* TY_FUNC only */
    int   func_is_variadic; /* TY_FUNC only: non-zero if variadic */
    int   func_is_unprototyped; /* TY_FUNC only: 1 = empty identifier list `foo()` */
    int   enum_id;     /* TY_INT only: non-zero unique ID for enum types */
    int   bitfield_width; /* 0 = not a bit-field; else width in bits (promotions) */
    int   is_vector;   /* 1 = GCC vector extension (__attribute__((vector_size(N)))) */
    unsigned is_decimal : 1; /* TY_FLOAT: IEEE 754 decimal (_Decimal32/64/128) */
    unsigned is_long_double : 1; /* TY_FLOAT: the `long double` specifier, even
                                  * where it shares double's size and
                                  * representation (Darwin arm64 gives it
                                  * width 8).  It is still a distinct type. */
};

static inline Type type_make_int(long long width, int is_unsigned) {
    Type t; t.kind = TY_INT; t.width = width; t.is_unsigned = is_unsigned;
    t.is_const = 0; t.is_volatile = 0; t.is_restrict = 0; t.is_bool = 0;
    t.pointee = NULL; t.elem_type = NULL; t.length = 0; t.vla_dim = NULL; t.tag = NULL;
    t.func_ret = NULL; t.func_params = NULL; t.func_nparams = 0; t.func_is_variadic = 0;
    t.func_is_unprototyped = 0; t.enum_id = 0;
    t.bitfield_width = 0; t.is_vector = 0; t.is_decimal = 0;
    t.is_long_double = 0; return t;
}
static inline Type type_make_bool(void) {
    Type t = type_make_int(1, 1);
    t.is_bool = 1;
    return t;
}
static inline Type type_default_int(void) { return type_make_int(4, 0); }
static inline Type type_make_float(long long width) {
    Type t; t.kind = TY_FLOAT; t.width = width; t.is_unsigned = 0;
    t.is_const = 0; t.is_volatile = 0; t.is_restrict = 0; t.is_bool = 0;
    t.pointee = NULL; t.elem_type = NULL; t.length = 0; t.vla_dim = NULL; t.tag = NULL;
    t.func_ret = NULL; t.func_params = NULL; t.func_nparams = 0; t.func_is_variadic = 0;
    t.func_is_unprototyped = 0; t.enum_id = 0;
    t.bitfield_width = 0; t.is_vector = 0; t.is_decimal = 0;
    t.is_long_double = 0; return t;
}
/* The `long double` specifier.  Darwin arm64 hands it double's width and
 * representation, but it stays a type of its own: clang there answers 0 for
 * __builtin_types_compatible_p(long double, double), and so must we. */
Type type_make_long_double(void);
static inline Type type_make_decimal(long long width) {
    Type t = type_make_float(width);
    t.is_decimal = 1;
    return t;
}
static inline int type_is_decimal(Type t) {
    return t.kind == TY_FLOAT && t.is_decimal && !t.is_vector;
}
static inline int type_is_decimal128(Type t) {
    return type_is_decimal(t) && t.width == 16;
}
static inline Type type_make_void(void) {
    Type t; t.kind = TY_VOID; t.width = 0; t.is_unsigned = 0;
    t.is_const = 0; t.is_volatile = 0; t.is_restrict = 0; t.is_bool = 0;
    t.pointee = NULL; t.elem_type = NULL; t.length = 0; t.vla_dim = NULL; t.tag = NULL;
    t.func_ret = NULL; t.func_params = NULL; t.func_nparams = 0; t.func_is_variadic = 0;
    t.func_is_unprototyped = 0; t.enum_id = 0;
    t.bitfield_width = 0; t.is_vector = 0; t.is_decimal = 0;
    t.is_long_double = 0; return t;
}

/* Deep-clone a Type (recursing into pointee/elem_type). */
Type type_clone(Type t);
/* Free heap-owned sub-types (no-op for TY_INT). Does NOT free `t` itself. */
void type_free(Type *t);
/* Total byte size of a Type: sizeof for scalars, N*elem for arrays,
 * width for structs (which is stashed at parse-time via layout). */
long long  type_size(Type t);
long long  type_align(Type t); /* natural alignment of a type */
int type_is_complex_ldouble(Type t); /* `_Complex long double` (X87 pair) */
int type_is_empty_struct(Type t);    /* GNU empty or size-0 (FAM / [0]) */
int type_needs_stack_align16(Type t); /* long double / __int128 / align≥16 */
/* SysV overflow-arg alignment: 16, 32, or 64, else 0. */
int type_stack_align(Type t);

/* Target AVX-512F from `-mavx512f` (not host CPUID).  0 after `-mno-avx`. */
int host_has_avx512f(void);
/* 1 after `-mno-avx`: 32-byte vectors are MEMORY (no YMM), matching gcc. */
extern int g_no_avx;
/* 1 after `-mavx512f`: 64-byte vectors are one ZMM (EVEX).  Default 0. */
extern int g_avx512f;

/* SysV AMD64 aggregate classification for ≤64-byte structs/unions.
 * Returns the number of register slots (1 or 2), writing INTEGER/SSE class
 * per eightbyte into `cls`.  A 16-byte SSE+SSEUP vector is one XMM; a
 * 32-byte SSE+SSEUP×3 vector is one YMM (nreg=1).  A 64-byte SSE+SSEUP
 * vector is one ZMM when `-mavx512f` is set, else MEMORY.  Returns 0
 * for MEMORY: size > 64, unaligned fields, X87 members, or mixed
 * aggregates larger than two eightbytes that are not a single vector. */
typedef enum {
    SYSV_CLS_INTEGER = 1,
    SYSV_CLS_SSE     = 2
} SysVRegClass;
int sysv_classify_agg(Type t, SysVRegClass cls[2]);
/* True if a 16-byte aggregate that classify reports as MEMORY is a
 * single X87+X87UP object (`struct { long double }`, nested the same,
 * or `union { long double }`).  SysV returns that in st0, not sret.
 * Arguments of this class still go on the stack. */
int sysv_agg_ret_x87(Type t);
/* MEMORY-class passing: 0 = copy the object onto the outgoing stack
 * (SysV); 1 = pass a pointer to a copy.  Pointer is used only for
 * `__va_list_tag` (matches libc/GCC array decay). */
int sysv_memory_pass_as_pointer(Type t);
/* Integer argument registers for the current target (6 SysV, 8 AAPCS64). */
int abi_gp_nregs(void);
/* 16 on SysV x86 (80-bit x87 in a 16-byte slot).  8 on Darwin arm64,
 * where long double is IEEE double. */
int type_long_double_width(void);
/* 1 when a hidden struct-return pointer consumes a GP argument register. */
int abi_sret_uses_gp(void);
/* 1 when a MEMORY aggregate is passed as a pointer to a copy (Darwin
 * aggregates larger than 16 bytes that are not HFAs). */
int abi_indirect_agg(Type t);
/* 1 when `t` is an AAPCS64 homogeneous float/double aggregate of 1–4
 * elements.  Always 0 on SysV x86. */
int abi_is_hfa(Type t);
/* Element count of that aggregate (1-4), or -1 when `t` is not one.
 * An HFA of three or four elements is 24 / 32 bytes, which AAPCS64 still
 * passes one V register per element. */
int abi_hfa_elem_count(Type t);
/* Width of one element of that aggregate (4 or 8), or -1.  Not the size of
 * `t`: an aligned(16) `struct { double d; }` is 16 bytes with one 8-byte
 * element.  -1 on SysV x86. */
int abi_hfa_elem_size(Type t);

Type type_make_ptr(Type pointee);
Type type_make_array(Type elem, long long length);
Type type_make_vector(Type elem, long long vec_size);
Type type_make_vla(Type elem, struct Expr *dim);
Type type_make_struct(const char *tag, long long size);
Type type_make_func(Type ret, Type * const *params, int nparams);
Type type_make_func_var(Type ret, Type * const *params, int nparams, int is_variadic);
Type type_decay(Type t);
int  type_is_ptr_or_array(Type t);
Type type_pointee_or_elem(Type t);
int  type_funcs_equal(Type a, Type b);  /* true if ret + all params match */
int  type_is_vla(Type t);               /* any dimension is a VLA */
/* GCC: a repeated typedef is valid only when it denotes the same type. */
int  type_same_typedef(Type a, Type b);

struct Expr *expr_clone(const struct Expr *e);

/* ------------------------------------------------------------------ */
/* Expression — Slice 2 introduces arithmetic expressions              */
/* ------------------------------------------------------------------ */

typedef enum {
    EX_INT_LIT,
    EX_BINOP,
    EX_UNARY,
    EX_VAR,     /* variable reference: name */
    EX_ASSIGN,  /* lvalue = rvalue; result is the assigned value */
    EX_CALL,    /* callee(arg1, arg2, ...) */
    EX_STR,     /* "hello" — anonymous global char array; value is const char* */
    EX_ADDR,    /* &lvalue */
    EX_DEREF,   /* *ptr — lvalue */
    EX_INDEX,   /* a[i]  — lvalue; desugars to *(a+i) in IR */
    EX_MEMBER,  /* s.x   — lvalue; requires s.type to be TY_STRUCT */
    EX_CAST,    /* (T)expr */
    EX_SIZEOF_TYPE,  /* sizeof(T)     — compile-time integer */
    EX_SIZEOF_EXPR,  /* sizeof(expr)  — compile-time integer */
    EX_ALIGNOF_TYPE, /* _Alignof(T)  — compile-time integer (alignment) */
    EX_ALIGNOF_EXPR, /* __alignof__(expr) — GCC; alignment of the object/type */
    EX_TERNARY, /* cond ? then : else — right associative, lower than || */
    EX_INC_DEC, /* ++lvalue / --lvalue (prefix or postfix) */
    EX_COMPOUND_ASSIGN, /* lvalue op= rvalue */
    EX_COMMA, /* a, b — evaluate a (discard), result is b */
    EX_INIT_LIST, /* { e1, e2, ... } — array/struct initializer; valid only as decl.init */
    EX_FLOAT_LIT, /* 1.5, 1e10, .5 — double value (width selects float vs double) */
    EX_COMPOUND_LITERAL, /* (Type){ ... } — compound literal; yields a value of the named type */
    EX_STMT_EXPR, /* ({ ... }) — GNU statement expression */
    EX_LABEL_ADDR, /* &&label — GNU C address of label */
} ExprKind;

typedef enum {
    BOP_ADD,     /* + */
    BOP_SUB,     /* - */
    BOP_MUL,     /* * */
    BOP_DIV,     /* / */
    BOP_MOD,     /* % */
    BOP_EQ,      /* == */
    BOP_NE,      /* != */
    BOP_LT,      /* <  */
    BOP_LE,      /* <= */
    BOP_GT,      /* >  */
    BOP_GE,      /* >= */
    BOP_AND,     /* && — logical AND, short-circuit */
    BOP_OR,      /* || — logical OR, short-circuit */
    BOP_BITAND,  /* &  — bitwise AND */
    BOP_BITOR,   /* |  — bitwise OR */
    BOP_BITXOR,  /* ^  — bitwise XOR */
    BOP_SHL,     /* << — left shift */
    BOP_SHR,     /* >> — right shift (arithmetic if signed, logical if unsigned) */
} BinOp;

typedef enum {
    UOP_NEG,     /* -x */
    UOP_POS,     /* +x — no-op */
    UOP_BITNOT,  /* ~x — bitwise NOT */
    UOP_NOT,     /* !x — logical NOT */
} UnaryOp;

typedef struct StmtArray StmtArray;
typedef struct Expr Expr;
int fold_const_int(const Expr *e, long long *out); /* compile-time int fold */
int fold_const_int128(const Expr *e, unsigned long long *lo, unsigned long long *hi);

/* Argument-list holder used by EX_CALL — grows as arguments are parsed. */
typedef struct {
    Expr **data;    /* Expr* per argument; owns each pointer */
    size_t len;
    size_t cap;
} ExprArray;

struct Expr {
    ExprKind kind;
    SourceLoc loc;
    Type type;   /* populated by sema; default-initialized to int */
    /* EX_CALL only, when callee is the `va_arg` builtin: the requested
     * type T (va_arg's second argument is a type, not an expression).
     * Kept OUT of the union so it never aliases with EX_CALL::args/callee. */
    Type va_arg_type;
    union {
        long long int_val;                             /* EX_INT_LIT — low 64 bits (or the whole value if width ≤ 8) */
        struct { BinOp op; Expr *l, *r; } bin;        /* EX_BINOP */
        struct { UnaryOp op; Expr *operand; } un;     /* EX_UNARY */
        struct { char *name; char *pkg; } var;         /* EX_VAR — pkg NULL = unqualified; else pkg.name */
        struct { Expr *lvalue; Expr *rvalue; } assign;/* EX_ASSIGN */
        struct { Expr *callee; ExprArray args; } call;/* EX_CALL — owns callee expr + args */
        struct { char *bytes; int len; } str;         /* EX_STR — bytes owns strdup'd data, len excludes trailing NUL */
        struct { Expr *operand; } addr;                /* EX_ADDR */
        struct { Expr *operand; } deref;               /* EX_DEREF */
        struct { Expr *array;  Expr *index; } idx;    /* EX_INDEX */
        struct { Expr *obj; char *name; } member;     /* EX_MEMBER — s.name (or (*p).name after arrow rewrite) */
        struct { Type target; Expr *operand; } cast;  /* EX_CAST — owns target sub-types */
        struct { Type target; } sizeof_t;              /* EX_SIZEOF_TYPE */
        struct { Expr *operand; } sizeof_e;            /* EX_SIZEOF_EXPR */
        struct { Type target; } alignof_t;             /* EX_ALIGNOF_TYPE */
        struct { Expr *operand; } alignof_e;           /* EX_ALIGNOF_EXPR */
        struct { Expr *cond; Expr *then; Expr *else_; } tern; /* EX_TERNARY */
        struct { Expr *operand; int is_inc; int is_prefix; } incdec; /* EX_INC_DEC */
        struct { Expr *lvalue; Expr *rvalue; BinOp op; } comp; /* EX_COMPOUND_ASSIGN */
        struct { Expr *lhs; Expr *rhs; } comma; /* EX_COMMA */
        struct { Expr **elements; int num_elements; int *desig_kind; int *desig_index; char **desig_member; } init_list; /* EX_INIT_LIST — owns each element; desig_kind[i]: -1=positional, 0=[index], 1=.member (name in desig_member[i], index in desig_index[i]) */
        char   *float_text;                                    /* EX_FLOAT_LIT — source text (strdup'd), parsed at IR time for precision */
        struct { Type target_type; Expr *init; } compound; /* EX_COMPOUND_LITERAL — target_type owns sub-types; init is an EX_INIT_LIST */
        struct { StmtArray *stmts; } stmt_expr;            /* EX_STMT_EXPR — GNU statement expression ({ ... }) */
        struct { char *label; } label_addr;                /* EX_LABEL_ADDR — &&label address of label */
    } u;
    /* EX_INT_LIT only: high 64 bits of a width-16 (`__int128`) constant.
     * Kept after the union so existing Expr field offsets stay GCC-identical. */
    unsigned long long int_hi;
};

/* Ownership: Expr uses malloc; tu_free recurses */
Expr *expr_new_int(long long v, SourceLoc loc);
/* Integer literal with an explicit type, as C §6.4.4.1 assigns one from the
 * suffix and the value's magnitude: 0x8000000000000000 is unsigned long even
 * with no suffix.  Getting this wrong silently truncates wide constants. */
Expr *expr_new_int_typed(long long v, int width, int is_unsigned, SourceLoc loc);
/* Integer literal, including GCC's `__int128` rank for decimal constants
 * larger than `long long` but still within 64-bit unsigned magnitude. */
Expr *expr_new_int_bits(unsigned long long lo, unsigned long long hi,
                       int width, int is_unsigned, SourceLoc loc);
Expr *expr_new_binop(BinOp op, Expr *l, Expr *r, SourceLoc loc);
Expr *expr_new_unary(UnaryOp op, Expr *operand, SourceLoc loc);
Expr *expr_new_var(const char *name, SourceLoc loc);
/* Qualified name: pkg.name (e.g. runtime.printf). Both strings are strdup'd. */
Expr *expr_new_var_qual(const char *pkg, const char *name, SourceLoc loc);
Expr *expr_new_assign(Expr *lvalue, Expr *rvalue, SourceLoc loc);
Expr *expr_new_call(Expr *callee, SourceLoc loc);
Expr *expr_new_str(const char *bytes, int len, SourceLoc loc);
Expr *expr_new_addr(Expr *operand, SourceLoc loc);
Expr *expr_new_deref(Expr *operand, SourceLoc loc);
Expr *expr_new_index(Expr *array, Expr *index, SourceLoc loc);
Expr *expr_new_member(Expr *obj, const char *name, SourceLoc loc);
Expr *expr_new_cast(Type target, Expr *operand, SourceLoc loc);
Expr *expr_new_sizeof_type(Type t, SourceLoc loc);
Expr *expr_new_sizeof_expr(Expr *operand, SourceLoc loc);
Expr *expr_new_alignof_type(Type t, SourceLoc loc);
Expr *expr_new_alignof_expr(Expr *operand, SourceLoc loc);
Expr *expr_new_ternary(Expr *cond, Expr *then, Expr *else_, SourceLoc loc);
Expr *expr_new_inc_dec(Expr *operand, int is_inc, int is_prefix, SourceLoc loc);
Expr *expr_new_compound_assign(Expr *lvalue, Expr *rvalue, BinOp op, SourceLoc loc);
Expr *expr_new_comma(Expr *l, Expr *r, SourceLoc loc);
Expr *expr_new_init_list(Expr **elements, int num_elements, SourceLoc loc);
Expr *expr_new_float_lit(const char *text, int width, SourceLoc loc);
Expr *expr_new_compound_literal(Type target_type, Expr *init, SourceLoc loc);
Expr *expr_new_stmt_expr(StmtArray *stmts, SourceLoc loc);
Expr *expr_new_label_addr(const char *label, SourceLoc loc);
void  expr_call_push_arg(Expr *e, Expr *arg);   /* takes ownership of arg */
void  expr_call_set_callee(Expr *e, Expr *callee); /* takes ownership, frees old */
void  expr_free(Expr *e);
/* Set an Expr's type, freeing the old (owning) type first; takes ownership of t. */
void  expr_set_type(Expr *e, Type t);

/* ------------------------------------------------------------------ */
/* Statement                                                           */
/* ------------------------------------------------------------------ */

typedef enum {
    ST_DECL,     /* int x;  or  int x = expr;  */
    ST_EXPR,     /* expr;  (typical: x = 5;) */
    ST_RETURN,   /* return expr; */
    ST_IF,       /* if (cond) then_stmt [else else_stmt] */
    ST_WHILE,    /* while (cond) body */
    ST_FOR,      /* for (init? ; cond? ; step?) body — init/step are Stmt* / Expr* */
    ST_BREAK,    /* break; */
    ST_CONTINUE, /* continue; */
    ST_BLOCK,    /* { stmt* } — introduces a new scope */
    ST_DO_WHILE, /* do body while (cond); — body executes at least once */
    ST_GOTO,     /* goto label; */
    ST_LABEL,    /* label: stmt */
    ST_SWITCH,   /* switch (expr) { case/default ... } */
} StmtKind;

typedef struct Stmt Stmt;
struct StmtArray {
    Stmt *data;
    size_t len;
    size_t cap;
};

/* One case arm of a switch.  `is_default` marks the default arm; otherwise
 * `value` is the constant case value (or range low value).  `stmts` owns the arm's statements. */
typedef struct {
    int is_default;
    long long value;          /* case value (or range low value) */
    long long high_value;     /* range high value when is_range */
    int is_range;             /* 1 for case low ... high: */
    char *label_name;   /* synthetic label name */
    StmtArray stmts;    /* statements in this arm */
} SwitchCase;

struct Stmt {
    StmtKind kind;
    SourceLoc loc;
    union {
        struct { char *name; Type type; Expr *init; int storage_class; char *alias_target; int align; int is_tls; int is_weak; int is_hidden; int is_used; } decl;   /* ST_DECL */
        Expr *expr;                                 /* ST_EXPR */
        Expr *value;                                /* ST_RETURN */
        struct { Expr *cond; Stmt *then_s; Stmt *else_s; } if_s; /* ST_IF: else_s may be NULL */
        struct { Expr *cond; Stmt *body; } while_s;              /* ST_WHILE */
        struct { Expr *cond; Stmt *body; } do_s;                 /* ST_DO_WHILE */
        /* ST_FOR: init (may be Stmt or NULL), cond (may be NULL), step (may be NULL). */
        struct { Stmt *init; Expr *cond; Expr *step; Stmt *body; } for_s;
        StmtArray block;                             /* ST_BLOCK — owns its statements */
        struct { char *target; Expr *target_expr; } goto_s; /* ST_GOTO — target_expr != NULL for indirect goto *expr */
        struct { char *name; Stmt *stmt; } label_s;   /* ST_LABEL — owns stmt */
        struct { Expr *cond; Stmt *body; SwitchCase *cases; int num_cases; int cap_cases; } switch_s; /* ST_SWITCH */
    } u;
};

void stmt_array_init(StmtArray *a);
void stmt_array_push(StmtArray *a, Stmt s);
void stmt_array_free(StmtArray *a);
void stmt_free(Stmt *s);
Stmt stmt_clone(const Stmt *s);

/* Heap-allocated statement helpers used by if/while (which own sub-stmts) */
Stmt *stmt_alloc(void);
void  stmt_free_ptr(Stmt *s);

void switch_push_case(Stmt *s, int is_default, long long value, const char *label_name);
void switch_push_case_range(Stmt *s, int is_default, long long value, long long high_value, int is_range, const char *label_name);

/* ------------------------------------------------------------------ */
/* Function & package declarations                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    char *name;         /* strdup'd */
    Type type;          /* declared parameter type */
    SourceLoc loc;
} Param;

typedef struct {
    Param *data;
    size_t len;
    size_t cap;
} ParamArray;

void param_array_init(ParamArray *a);
void param_array_push(ParamArray *a, const char *name, Type type, SourceLoc loc);
void param_array_free(ParamArray *a);

typedef struct {
    char *name;         /* strdup'd */
    Type ret_type;      /* declared return type */
    ParamArray params;
    StmtArray body;
    SourceLoc loc;
    int    is_variadic; /* 1 = ends with `...` (variadic function) */
    int    is_unprototyped; /* 1 = K&R `foo()` empty identifier list */
    int    is_extern;   /* 1 = no standalone copy (`extern int f();`, or GNU89 extern inline) */
    int    is_static;   /* 1 = `static` function — LOCAL linkage */
    int    is_weak;     /* 1 = __attribute__((weak)) */
    int    is_hidden;   /* 1 = visibility("hidden") or "internal" */
    int    is_used;     /* 1 = __attribute__((used)) */
    char  *alias_target; /* __attribute__((alias("..."))) or NULL */
    int    align;       /* alignment attribute */
    int    no_instrument; /* 1 = __attribute__((no_instrument_function)) */
    int    is_constructor; /* 1 = __attribute__((constructor)) → .init_array */
    int    is_destructor;  /* 1 = __attribute__((destructor)) → .fini_array */
    int    ctor_prio;      /* constructor priority (smaller runs first; 65535 = default) */
    int    dtor_prio;
} FunctionDecl;

typedef struct {
    char *name;         /* strdup'd */
    SourceLoc loc;
} PackageDecl;

typedef struct {
    char *name;         /* strdup'd package name from `import IDENT;` */
    SourceLoc loc;
} ImportDecl;

typedef struct {
    ImportDecl *data;
    size_t len;
    size_t cap;
} ImportArray;

void import_array_init(ImportArray *a);
void import_array_push(ImportArray *a, const char *name, SourceLoc loc);
void import_array_free(ImportArray *a);

/* Struct definition: tag + ordered member list.  Each member has a name,
 * a Type (owned), a computed byte offset within the struct, and a size.
 * `bit_width` is -1 for a normal member, 0 for a zero-width bit-field
 * (`int : 0`, which starts a new allocation unit), or N (1..64) for a
 * bitfield `unsigned x : N;`. */
typedef struct {
    char *name;
    Type type;
    long long offset;
    int  bit_width;     /* -1 = normal member, 0 = zero-width bitfield, else bits */
    int  bit_offset;    /* bit position within the unit (0 = LSB); valid when
                         * bit_width > 0. */
    int  align;         /* explicit member alignment, or 0 */
} StructMember;

typedef struct {
    char *tag;            /* xstrdup'd */
    int   is_union;       /* 1 = union (members overlap at offset 0) */
    int   is_big_endian;  /* 1 = scalar_storage_order("big-endian") */
    StructMember *members;
    int num_members;
    int cap_members;
    long long size;       /* total size in bytes (already aligned) */
    long long align;      /* max member alignment seen so far */
    int is_packed;        /* 1 if __attribute__((packed)) */
    SourceLoc loc;
    /* Canonical TY_STRUCT Type for this struct's tag, if one has been created
     * via type_make_struct().  struct_def_finish() updates its width so that
     * clones made while the struct was still incomplete see the final size. */
    Type *canonical_type;
    /* Bitfield layout state (valid while members are being added).
     * `bf_unit_type != 0` means a bitfield run is open; the next free bit is
     * `bf_unit_offset * 8 + bf_unit_used`.  Adjacent bitfields pack by bit
     * position (GCC/SysV), including mixed declared types and packed structs. */
    long long bf_unit_type;   /* non-zero = open bitfield run */
    int   bf_unit_used;   /* bit remainder 0..7 of the next free bit */
    long long bf_unit_offset; /* byte of the next free bit */
} StructDef;

typedef struct {
    StructDef *data;
    size_t len;
    size_t cap;
} StructRegistry;

void struct_registry_init(StructRegistry *r);
void struct_registry_free(StructRegistry *r);
/* Register a new struct; returns pointer into registry.  Fails/duplicates
 * are caller's problem — sema checks before calling. */
StructDef *struct_registry_add(StructRegistry *r, const char *tag, SourceLoc loc);
/* Find a struct by tag; NULL if absent. */
StructDef *struct_registry_find(StructRegistry *r, const char *tag);
/* Const variant. */
const StructDef *struct_registry_find_c(const StructRegistry *r, const char *tag);
/* Append a member to a struct definition; computes offset (naturally aligned).
 * `bit_width` is -1 for a normal member, 0 for `: 0`, or N for `x : N`. */
void struct_def_push_member(StructDef *sd, const char *name, Type ty, int bit_width);
void struct_def_push_member_aligned(StructDef *sd, const char *name, Type ty, int bit_width, int align);
void struct_def_finish(StructDef *sd);
void struct_def_apply_sso(StructDef *sd, int is_big_endian);
void struct_def_fixup_self_types(StructDef *sd);
/* Find a member by name, walking C11 anonymous struct/union members.
 * On success, *offset_out (if non-NULL) is the byte offset from `sd`'s start. */
const StructMember *struct_lookup_member(const StructRegistry *reg,
                                         const StructDef *sd,
                                         const char *name,
                                         long long *offset_out);

typedef struct {
    FunctionDecl *data;
    size_t len;
    size_t cap;
} FunctionArray;

/* Enum constant: a name → int value.  Owned by an EnumDef. */
typedef struct {
    char *name;     /* xstrdup'd */
    int  value;
} EnumConstant;

/* Enum definition: tag + ordered constant list.  `tag` is NULL for anonymous
 * enums (allowed but cannot be used as a type). */
typedef struct {
    char *tag;              /* xstrdup'd, or NULL */
    EnumConstant *constants;
    int num_constants;
    int cap_constants;
    SourceLoc loc;
    int has_underlying_type; /* C23: enum Tag : type */
    Type underlying_type;    /* valid when has_underlying_type */
} EnumDef;

typedef struct {
    EnumDef *data;
    size_t len;
    size_t cap;
} EnumRegistry;

void enum_registry_init(EnumRegistry *r);
void enum_registry_free(EnumRegistry *r);
/* Register a new enum; returns pointer into registry. */
EnumDef *enum_registry_add(EnumRegistry *r, const char *tag, SourceLoc loc);
/* Find an enum by tag; NULL if absent. */
EnumDef *enum_registry_find(EnumRegistry *r, const char *tag);
/* Append a constant to an enum, computing its value: explicit if `has_value`
 * (value given), else prev+1 (or 0 if first). Returns the assigned value. */
int  enum_def_push_constant(EnumDef *ed, const char *name, int has_value,
                            int value, SourceLoc loc);
/* Look up an enum constant by name across ALL enums; returns pointer to the
 * EnumConstant, or NULL if not found. */
const EnumConstant *enum_registry_find_constant(const EnumRegistry *r,
                                                const char *name);
/* Build the Type for an enum definition (underlying type or int + enum_id). */
Type enum_def_as_type(const EnumDef *ed, int enum_id);

/* Typedef: a name → Type alias.  The aliased Type is fully owned (heap
 * pointee/elem types and strdup'd tag are cloned on insert). */
typedef struct {
    char *name;     /* xstrdup'd */
    Type type;      /* the aliased type, owned */
} TypedefEntry;

typedef struct {
    TypedefEntry *data;
    size_t len;
    size_t cap;
} TypedefRegistry;

void typedef_registry_init(TypedefRegistry *r);
void typedef_registry_free(TypedefRegistry *r);
/* Register a typedef; returns pointer into registry.  Caller must not already
 * have an entry with the same name (sema checks first). */
TypedefEntry *typedef_registry_add(TypedefRegistry *r, const char *name, Type type);
/* Find a typedef by name; NULL if absent. */
const Type *typedef_registry_find(const TypedefRegistry *r, const char *name);
TypedefEntry *typedef_registry_get(TypedefRegistry *r, const char *name);
void typedef_registry_truncate(TypedefRegistry *r, size_t len);

typedef struct {
    PackageDecl package;
    ImportArray imports; /* `import IDENT;` after the package decl */
    StmtArray globals;   /* ST_DECL at file scope (globals) */
    FunctionArray functions;
    StructRegistry structs;
    EnumRegistry enums;
    TypedefRegistry typedefs;
} TranslationUnit;

void tu_init(TranslationUnit *tu);
void tu_free(TranslationUnit *tu);

#endif /* FAKECC_AST_H */
