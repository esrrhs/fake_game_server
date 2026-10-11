#ifndef FAKECC_IR_H
#define FAKECC_IR_H

#include "fakecc/common.h"
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* SSA virtual register — grows monotonically per function             */
/* ------------------------------------------------------------------ */

typedef int IRValue;   /* SSA virtual register id, incrementing from 0 */

/* ------------------------------------------------------------------ */
/* IR instruction opcodes                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    IR_CONST,       /* dst = imm */
    IR_ADD,         /* dst = a + b */
    IR_SUB,         /* dst = a - b */
    IR_MUL,         /* dst = a * b */
    IR_DIV,         /* dst = a / b  (signed) */
    IR_MOD,         /* dst = a % b  (signed) */
    IR_NEG,         /* dst = -a */
    IR_BAND,        /* dst = a & b  (bitwise AND) */
    IR_BOR,         /* dst = a | b  (bitwise OR) */
    IR_BXOR,        /* dst = a ^ b  (bitwise XOR) */
    IR_BNOT,        /* dst = ~a    (bitwise NOT) */
    IR_SHL,         /* dst = a << b (left shift) */
    IR_SHR,         /* dst = a >> b (arithmetic if signed, logical if unsigned) */
    IR_ROL,         /* dst = rotate_left(a, b); width is the rotate size in bytes */
    IR_EQ,          /* dst = (a == b) ? 1 : 0 */
    IR_NE,          /* dst = (a != b) ? 1 : 0 */
    IR_FADD,        /* dst = a + b (float; width 4=float 8=double) */
    IR_FSUB,        /* dst = a - b (float) */
    IR_FMUL,        /* dst = a * b (float) */
    IR_FDIV,        /* dst = a / b (float) */
    IR_FCMP,        /* dst = (a op b) ? 1 : 0 (float comparison; signedness
                       encodes the ordered comparison: 0 = LT,1=LE,2=GT,3=GE,
                       4 = EQ, 5 = NE).  Result width 4 (int). */
    IR_VADD,        /* dst = a + b (vector SIMD; width=vec_sz, imm=elem_sz, is_float) */
    IR_VSUB,        /* dst = a - b (vector SIMD) */
    IR_VMUL,        /* dst = a * b (vector SIMD) */
    IR_VDIV,        /* dst = a / b (vector SIMD) */
    IR_VBAND,       /* dst = a & b (vector SIMD) */
    IR_VBOR,        /* dst = a | b (vector SIMD) */
    IR_VBXOR,       /* dst = a ^ b (vector SIMD) */
    IR_SITOFP,      /* dst = (float)a — signed int → float; width = target */
    IR_FPTOSI,      /* dst = (int)a — float → signed int; width = target */
    IR_FPEXT,       /* dst = (double)(float)a — float → double */
    IR_FPTRUNC,     /* dst = (float)(double)a — double → float */
    IR_LT,          /* dst = (a <  b) ? 1 : 0 (signed) */
    IR_LE,          /* dst = (a <= b) ? 1 : 0 (signed) */
    IR_GT,          /* dst = (a >  b) ? 1 : 0 (signed) */
    IR_GE,          /* dst = (a >= b) ? 1 : 0 (signed) */
    IR_ALLOCA,      /* dst = stack slot for a variable (codegen no-op unless pinned) */
    IR_LOAD,        /* dst = [a]   — read variable slot a → dst (mem2reg-promotable) */
    IR_STORE,       /* [a] = b     — write b into variable slot a; dst unused */
    IR_ADDR,        /* dst = &alloca_a — a is an alloca-value id; result is 8-byte pointer */
    IR_LOAD_PTR,    /* dst = *a    — a is a pointer-valued SSA; not mem2reg-promoted */
    IR_STORE_PTR,   /* *a = b      — a is a pointer-valued SSA */
    IR_COPY,        /* dst = a    — simple move (mem2reg / φ resolution product) */
    IR_LABEL,       /* imm = label_id — basic-block marker */
    IR_BR,          /* imm = target_label — unconditional branch */
    IR_CBR,         /* a = cond, imm = true_label, b = false_label — conditional branch */
    IR_PARAM,       /* dst = incoming param; imm = param_index (0..5) */
    IR_CALL,        /* dst = callee(call.args[0..nargs-1]); callee name in call.name */
    IR_RETURN,      /* return a */
    IR_SEXT,        /* dst = signext(a) — a is smaller width; result is `width` */
    IR_ZEXT,        /* dst = zeroext(a) — a is smaller width; result is `width` */
    IR_TRUNC,       /* dst = trunc(a) to `width` — no-op at register level */
    IR_GADDR,       /* dst = &global; global name in call_name.  Result is 8-byte ptr. */
    IR_GADDR_TLS,   /* dst = &__thread global; global name in call_name.
                     * Result is 8-byte ptr to a thread-local variable; codegen
                     * emits Initial-Exec: `movq %fs:0; addq x@gottpoff(%rip)`.
                     * The linker fills the GOT slot with TPOFF (static exe) or
                     * a dynamic R_X86_64_TPOFF64 (DSO). */
    IR_FADDR,       /* dst = &function; function name in call_name.  Result is 8-byte ptr. */
    IR_LADDR,       /* dst = &label; imm = label_id.  Result is 8-byte ptr. */
    IR_JMP_PTR,     /* jmp *a — indirect jump to pointer value in a */
    IR_FRAME_ADDR,  /* dst = %rbp — frame pointer (at level imm) */
    IR_RETURN_ADDR, /* dst = return address (at level imm) */
    IR_DYN_ALLOCA,  /* dst = alloca(a) — dynamic stack allocation of size a */
    IR_STACK_SAVE,  /* dst = %rsp — save stack pointer */
    IR_STACK_RESTORE, /* %rsp = a — restore stack pointer */
    IR_LONGJMP,     /* a = buf ptr — restore %rbp, %rsp and jump to buf[1] */
    /* Debug-only marker (emitted by mem2reg under -g): from here on, source
     * variable `imm` (index into fn->dbg_vars) lives in SSA value `a`.
     *
     * Codegen emits no machine code for it.  Every optimization pass must
     * treat it as invisible — it is NOT a use for DCE/liveness and takes no
     * part in value renumbering — so that -g cannot change generated code.
     * If `a`'s definition is optimized away, renumber sets `a` to -1 and the
     * variable is reported as unavailable over that range. */
    IR_DBG_VALUE,
} IROpcode;

/* Maximum arguments to IR_CALL.  SysV packs small aggregates into 1–2
 * register args; MEMORY-class aggregates are one stack blob (not one IR
 * arg per eightbyte).  Kept in sync with MAX_PARAMS (common.h).  Args are
 * heap-allocated on the IR_CALL itself — do NOT embed [IR_CALL_MAX_ARGS]
 * in IRInst (that made every instruction ~5KB and ballooned compile time). */
#define IR_CALL_MAX_ARGS 1024

/* call_arg_on_stack bits: 0 = stack, 1 = align≥16, 2 = ≥32, 3 = ≥64,
 * 4 = MEMORY blob (SSA is a pointer; copy call_arg_nbytes bytes). */
#define CALL_ARG_STACK   1
#define CALL_ARG_ALIGN16 2
#define CALL_ARG_ALIGN32 4
#define CALL_ARG_ALIGN64 8
#define CALL_ARG_BLOB    16
/* Darwin HFA eightbyte: the SSA value is an integer bit pattern, but the
 * call boundary moves it through the next V register (not a GP). */
#define CALL_ARG_HFA     32
/* Stack argument that is an aggregate or vector: the SSA value is a pointer
 * to it and call_arg_nbytes[] says how many bytes to copy there.  Used for
 * anonymous variadic arguments, which take no register and so never went
 * through the classification that used to size them. */
#define CALL_ARG_AGG     64
/* IR_CALL / IR_RETURN align16 on arm64.  x86 leaves these 0.
 * HFA: result eightbytes travel in v0/v1.  SRET: call_args[0] is the
 * indirect-result pointer and belongs in x8, not x0. */
#define A64_MARK_HFA     4
#define A64_MARK_SRET    8

typedef struct {
    IROpcode op;
    IRValue  dst;      /* meaningless for IR_RETURN/IR_BR/IR_LABEL, fill -1 */
    IRValue  a, b;     /* source operands; IR_CONST only uses imm */
    /* Third and fourth source operands.  Only IR_RETURN reads both, and
     * IR_CALL defines them as its third/fourth result eightbytes, and only
     * when the callee returns a three- or four-element HFA -- AAPCS64 brings
     * it back in v0-v3.  Everything leaves these at -1 (set by the emit
     * helpers), so no other opcode needs to look at them. */
    IRValue  c, d;
    int64_t  imm;      /* CONST value, LABEL id, BR/CBR target, PARAM index */
    SourceLoc loc;
    /* IR_CALL only: callee name + argument SSA values.  NULL for other ops. */
    char    *call_name;
    /* IR_CALL only: heap array of length call_nargs (NULL if call_nargs==0). */
    IRValue *call_args;
    int      call_nargs;
    /* IR_CALL only: for indirect calls, the SSA value holding the function
     * pointer (lowered from the callee expression).  -1 for named calls. */
    IRValue  call_callee;
    /* Slice 7a: width of the result (or the storage for LOAD/STORE/ALLOCA).
     *   1, 2, 4, or 8 bytes.  Meaningful for all operand-producing ops and
     *   for LOAD/STORE/ALLOCA/PARAM.  0 for control-flow (BR/CBR/LABEL/RETURN). */
    int      width;
    int      is_unsigned;   /* signedness — governs sext vs zext, sdiv vs udiv */
    /* For IR_CONST of a float value: the IEEE-754 bit pattern, cast to
     * int64.  For all other ops (and int CONSTs) this is 0 / unused. */
    int64_t  float_imm;
    /* True if this instruction produces a float value (TY_FLOAT).  Lets
     * codegen pick the XMM register file instead of the GP file. */
    int      is_float;
    /* IR_PARAM / IR_CALL arg: force SysV MEMORY-class stack passing (do not
     * assign a GP/XMM register even if one is free).  For IR_CALL, see also
     * call_arg_on_stack[]. */
    int      force_stack;
    /* IR_PARAM / IR_CALL: stack-slot alignment in bytes (16/32/64), or 1
     * for legacy 16-byte.  SysV long double / __int128 / AVX vectors /
     * over-aligned MEMORY.  call_arg_on_stack bit 1 = ≥16, bit 2 = ≥32,
     * bit 3 = ≥64, bit 4 = MEMORY blob. */
    int      align16;
    /* IR_CALL / IR_RETURN: X87 aggregate in st0(/st1).  imm is 16 (one
     * long double in st0) or 32 (`_Complex long double` in st0/st1). */
    int      x87_pair;
    /* IR_CALL only: per-arg force_stack; heap array length call_nargs, or NULL.
     * See CALL_ARG_* bits. */
    unsigned char *call_arg_on_stack;
    /* IR_CALL only: MEMORY-blob byte counts, length call_nargs, or NULL. */
    int     *call_arg_nbytes;
    /* Slice 7b/c: for IR_ALLOCA, total bytes reserved on the stack when
     * the alloca is pinned.  For IR_PARAM, a MEMORY blob's incoming size
     * (codegen LEAs [rbp+off] when alloca_bytes > 8). */
    int      alloca_bytes;
    /* IR_LOAD / IR_LOAD_PTR: the access is volatile and must not be DCE'd. */
    int      is_volatile;
} IRInst;

/* ------------------------------------------------------------------ */
/* IR function & module                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    IRInst *data;
    size_t len;
    size_t cap;
} IRInstArray;

/* Debug variable retained from lowering for DWARF emission under -g. */
typedef enum {
    IR_DBG_PARAM  = 0,
    IR_DBG_LOCAL  = 1
} IRDebugVarKind;

/* Member layout for a TY_STRUCT debug variable (feeds DW_TAG_member). */
typedef struct {
    char *name;           /* xstrdup'd */
    int offset;
    int bit_width;        /* 0 = normal */
    int bit_offset;
    int type_kind;        /* TypeKind of the member */
    int width;
    int is_unsigned;
    int is_bool;
} IRDebugMember;

typedef struct {
    char *name;           /* xstrdup'd */
    SourceLoc loc;
    IRDebugVarKind kind;
    int type_kind;        /* TypeKind value */
    int width;
    int is_unsigned;
    int is_bool;
    int array_len;        /* >0 for TY_ARRAY */
    char *struct_tag;     /* TY_STRUCT tag, or pointee tag for ptr-to-struct */
    int struct_size;      /* byte size of that struct (for DIE AT_byte_size) */
    IRDebugMember *members;
    int num_members;
    int alloca_ssa;       /* IR_ALLOCA dst for locals/pinned params; -1 else */
    int param_idx;        /* SysV param index for IR_DBG_PARAM; -1 else */
} IRDebugVar;

/* Debug marker held outside fn->insts during register allocation.  Says "from
 * here on, dbg_var `var` lives in SSA value `value`"; `pos` is the number of
 * real (non-marker) instructions that precede it in the post-cleanup array, so
 * opt() can put it back at the exact same spot after regalloc.  `value` is
 * already remapped to compacted ids by scalar_cleanup's scalar_renumber. */
typedef struct {
    int var;     /* dbg_var index */
    int value;   /* SSA value ref (post-renumbering) */
    int pos;     /* number of real instructions before this marker */
} ExtractedMarker;

typedef struct {
    char *name;       /* function name, xstrdup'd */
    IRInstArray insts;
    int next_value_id; /* SSA id counter, incremented by lower_expr */
    int next_label_id; /* label id counter, used by control-flow lowering */
    SourceLoc loc;
    void *ra;         /* RAResult* for GP values, set by reg_alloc. */
    void *ra_xmm;     /* RAResult* for float values, set by reg_alloc_xmm.
                         NULL until register allocation runs, and NULL for
                         functions with no float values. */
    /* Slice 7a: per-value width and signedness (indexed by SSA id).
     * Populated by IR-gen; consulted by codegen when a use needs to know
     * the defining op's type. `NULL` on functions built by test helpers
     * that don't need type info. */
    int  *value_width;
    int  *value_is_unsigned;
    int   value_meta_cap;
    /* Float support: per-value flag — 1 if the value is TY_FLOAT. */
    int  *value_is_float;
    /* Slice 7a: declared return type (width & signedness). */
    int   ret_width;
    int   ret_is_unsigned;
    /* Float support: 1 if the function returns TY_FLOAT. */
    int   ret_is_float;
    /* Slice 13: 1 if the function returns a struct by value. */
    int   ret_is_struct;
    /* SysV: how many eightbytes of a struct return travel in registers
     * (1 or 2).  0 means MEMORY class — hidden sret pointer in RDI.
     * ret_reg_cls[i] holds SysVRegClass (INTEGER/SSE) for each eightbyte. */
    int   ret_reg_n;
    int   ret_reg_cls[4];
    /* 1 if the function returns _Bool (normalize the value to 0/1). */
    int   ret_is_bool;
    /* 1 if the function returns `_Complex long double` in st0/st1. */
    int   ret_is_complex_ld;
    /* SysV X87 return size: 16 = one long double in st0, 32 = complex
     * pair in st0/st1, 0 = not an X87 return. */
    int   ret_x87_bytes;
    /* Variadic: 1 if the function was defined with a `...` tail.  The prologue
     * emits a register-save area and the va_* builtins read/write it. */
    int   is_variadic;
    int   is_static;  /* 1 = `static` function — LOCAL linkage */
    int   is_weak;    /* 1 = __attribute__((weak)); a strong def wins at link */
    int   is_hidden;  /* 1 = Mach-O private-external (hidden) */
    int   is_used;    /* 1 = __attribute__((used)); Mach-O N_NO_DEAD_STRIP */
    int   is_constructor; /* 1 = emit a .init_array pointer to this function */
    int   is_destructor;  /* 1 = emit a .fini_array pointer to this function */
    int   ctor_prio;
    int   dtor_prio;
    int   has_dyn_alloca; /* 1 = function uses dynamic alloca / VLA */
    /* GNU __builtin_apply_args: save incoming arg regs at prologue (GCC
     * migrates the save to function entry so later calls cannot clobber
     * them).  Layout matches GCC x86-64 SysV apply_args_size(). */
    int   needs_apply_args;
    /* GNU __builtin_apply: reserve a result block and allow a dynamic
     * outgoing-arg allocation (has_dyn_alloca is also set). */
    int   needs_apply;
    /* Slice 13: SSA value of the hidden sret pointer param (param index 0)
     * when ret_is_struct && ret_reg_n == 0.  The return statement copies
     * struct bytes into *sret_value and returns the pointer in RAX. */
    IRValue sret_value;
    /* Debug variables (params + locals). Always populated; codegen uses
     * them only when -g is set. */
    IRDebugVar *dbg_vars;
    size_t num_dbg_vars, cap_dbg_vars;
} IRFunction;

typedef struct {
    IRFunction *data;
    size_t len;
    size_t cap;
} IRFunctionArray;

/* A pointer slot inside a global's init_bytes that must hold the *address*
 * of another global (e.g. `.regs = ALLOCATABLE_REGS`: array decays to a
 * pointer).  Codegen emits a R_X86_64_64 relocation for each fixup; the
 * linker patches the slot with the target symbol's link-time address.
 * Without this, pack_init would copy the target's *bytes* into the pointer
 * slot — wrong, and the root cause of the self-bootstrap crash. */
typedef struct {
    int   offset;       /* byte offset within the global's init_bytes */
    char *sym;          /* xstrdup'd target symbol name */
    char *sub;          /* if set, the slot is sym - sub + addend */
    int   addend;       /* addend for relocation (e.g. &g + addend) */
} GlobalFixup;

/* Module-level global variable.  Codegen places these in the .data section
 * (or .bss when init_bytes == NULL).  `init_bytes` owns `size` bytes. */
typedef struct {
    char *name;         /* xstrdup'd */
    int   size;         /* bytes in .data/.bss */
    char *init_bytes;   /* NULL → zero-init (bss).  Otherwise owns `size` bytes. */
    int   is_readonly;  /* 1 = string literal → rodata; 0 = mutable → data */
    int   is_const_obj; /* 1 = C const object.  arm64 may place it in __const
                         * when it has no pointer fixup.  x86 ignores this. */
    int   is_static;    /* 1 = `static` global — LOCAL linkage */
    int   is_weak;      /* 1 = __attribute__((weak)) */
    int   is_hidden;    /* 1 = visibility("hidden"); x86 ignores this */
    int   is_used;      /* 1 = __attribute__((used)); x86 ignores this */
    int   is_tls;       /* 1 = `__thread` / `_Thread_local` global — .tdata/.tbss */
    int   align;        /* byte alignment (`aligned(N)` / natural); ≥1 */
    SourceLoc loc;
    GlobalFixup *fixups;/* pointer slots needing link-time address patching */
    int   num_fixups, cap_fixups;
} IRGlobal;

typedef struct {
    IRGlobal *data;
    size_t len;
    size_t cap;
} IRGlobalArray;

typedef struct {
    char *name;         /* xstrdup'd */
    char *target;       /* xstrdup'd */
    int   is_static;
    int   is_weak;      /* 1 = weak alias; x86 ignores this */
    SourceLoc loc;
} IRAlias;

typedef struct {
    IRAlias *data;
    size_t len;
    size_t cap;
} IRAliasArray;

typedef struct {
    IRFunctionArray functions;
    IRGlobalArray   globals;
    IRAliasArray    aliases;
    /* Names declared `__attribute__((weak))` with no definition in this TU.
     * arm64 records them as N_WEAK_REF; x86 ignores the list. */
    char **weak_refs;
    size_t n_weak_refs, cap_weak_refs;
} IRModule;

void ir_module_init(IRModule *m);
void ir_module_free(IRModule *m);
void ir_module_push_alias(IRModule *m, const char *name, const char *target,
                          int is_static, SourceLoc loc);

#include "fakecc/ast.h"
/* Lower AST to IR.  When `pin_locals` is set (-O0), scalar locals and params
 * keep a real stack slot instead of relying on mem2reg promotion.
 * Returns FAKECC_OK on success, FAKECC_ERR on error (message recorded). */
int ir_generate(const TranslationUnit *tu, IRModule *ir, int pin_locals);

extern int g_instrument_functions;
extern int g_sanitize_address;
extern int g_no_builtin;

/* GCC -fno-builtin / -fno-builtin-NAME: do not expand the library name as a
 * builtin. Explicit __builtin_* names are never disabled. */
void ir_disable_builtin(const char *name);
int ir_builtin_disabled(const char *name);

/* Return the live struct registry during lowering (NULL outside it).
 * type_size() uses this to refresh stale cached struct widths. */
const StructRegistry *get_ir_structs(void);

#endif /* FAKECC_IR_H */
