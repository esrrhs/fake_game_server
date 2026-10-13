#ifndef FAKECC_EMIT_H
#define FAKECC_EMIT_H

#include "fakecc/common.h"
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Section indices (also used as ELF st_shndx for defined symbols)      */
/* ------------------------------------------------------------------ */

#define SECT_UNDEF  0
#define SHN_COMMON  0xfff2 /* tentative def: st_value is alignment, st_size is size */
#define SECT_TEXT   1
#define SECT_RODATA 2
#define SECT_DATA   3
#define SECT_BSS    4
#define SECT_TDATA  5  /* initialized __thread variables (ELF SHF_TLS) */
#define SECT_TBSS   6  /* zero-initialized __thread variables (SHF_TLS) */
#define SECT_INIT_ARRAY 7 /* .init_array constructor pointers */
#define SECT_FINI_ARRAY 8 /* .fini_array destructor pointers */
#define SECT_TVARS   9  /* Mach-O tlv_descriptor table (reader-internal:
                        *   never carries bytes into an image) */
#define INIT_PRIO_DEFAULT 65535

/* ------------------------------------------------------------------ */
/* Symbol table entry                                                  */
/* ------------------------------------------------------------------ */

/* arm64 Mach-O internal rebase.  `slot` and `target` are offsets from
 * the mach header.  dyld's chained fixups add the load slide so the
 * slot holds the runtime address.  The ELF path leaves this empty. */
typedef struct {
    uint64_t slot;
    uint64_t target;
} EmitRebase;

/* arm64 Mach-O import.  Where a rebase names an address inside the image,
 * a bind names one outside it: `slot` is a pointer-sized cell in __DATA
 * (an offset from the mach header) that dyld overwrites with the address
 * of `name` as it loads the dylibs.  The cell is what makes a call to a
 * library function possible at all -- there is no displacement a direct
 * branch could hold, because the distance is not known until load time. */
typedef struct {
    uint64_t slot;
    char    *name;     /* Mach-O spelling, i.e. already "_"-prefixed */
} EmitBind;

typedef struct {
    char *name;        /* symbol name (NULL for section symbols) */
    uint8_t binding;   /* STB_LOCAL (0) / STB_GLOBAL (1) */
    uint8_t type;      /* STT_NOTYPE(0) / STT_OBJECT(1) / STT_FUNC(2) / STT_SECTION(3) / STT_TLS(6) */
    uint16_t shndx;    /* section index, or SHN_UNDEF(0) for undefined */
    size_t value;      /* offset within section (defined symbols) */
    size_t size;       /* byte size (0 for undefined symbols) */
    uint16_t macho_desc; /* Mach-O n_desc bits the ELF writer ignores */
} EmitSymbol;

/* ------------------------------------------------------------------ */
/* Relocation entry                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    size_t   offset;   /* offset within the section being relocated */
    uint32_t type;     /* R_X86_64_64(1), R_X86_64_PC32(2), etc. */
    uint32_t sym;      /* target symbol index into syms[] */
    int32_t  addend;   /* addend (rip-relative uses -4) */
    uint16_t shndx;    /* site section: SECT_DATA, SECT_TDATA, or SECT_RODATA */
} EmitReloc;

/* ------------------------------------------------------------------ */
/* Debug records (populated by codegen when -g; serialized in .o)      */
/* ------------------------------------------------------------------ */

typedef enum {
    DBG_VAR_PARAM  = 0,
    DBG_VAR_LOCAL  = 1,
    DBG_VAR_GLOBAL = 2
} DebugVarKind;

typedef enum {
    DBG_TY_VOID   = 0,
    DBG_TY_INT    = 1,
    DBG_TY_FLOAT  = 2,
    DBG_TY_PTR    = 3,
    DBG_TY_ARRAY  = 4,
    DBG_TY_STRUCT = 5,
    DBG_TY_BOOL   = 6
} DebugTypeTag;

typedef enum {
    DBG_LOC_NONE  = 0,
    DBG_LOC_FBREG = 1,  /* rbp + rbp_offset */
    DBG_LOC_REG   = 2,  /* DWARF register number in dwarf_reg */
    DBG_LOC_ADDR  = 3,  /* absolute address via sym_name (link resolves) */
    /* Value of the ABI arrival register as it was on entry to this function.
     * Resolved by the debugger via DW_TAG_call_site_parameter. */
    DBG_LOC_ENTRY_VALUE = 4
} DebugLocKind;

/* One entry of a DWARF location list: over [pc_start, pc_end) within the
 * module's .text, the variable lives at the described location.  Optimized
 * variables move between registers and spill slots, so a single location
 * expression is not enough — this mirrors gcc's .debug_loc behaviour. */
typedef struct {
    size_t pc_start;
    size_t pc_end;
    DebugLocKind loc_kind;
    int rbp_offset;
    int dwarf_reg;
} DebugLocRange;

/* One field of a DBG_TY_STRUCT, used to emit DW_TAG_member.  Nested structs
 * are represented as DBG_TY_STRUCT with only width (no further members here);
 * that is enough for sizeof / offsetof of the outer type and for `print p.x`
 * when the member itself is a scalar. */
typedef struct {
    char *name;
    int offset;
    int bit_width;      /* 0 = normal member */
    int bit_offset;
    DebugTypeTag type_tag;
    int width;
    int is_unsigned;
} DebugMember;

typedef struct {
    char *name;
    char *file;
    int line;
    DebugVarKind kind;
    DebugTypeTag type_tag;
    int width;          /* byte size of the object */
    int is_unsigned;
    int array_len;      /* DBG_TY_ARRAY element count; else 0 */
    /* Struct tag when type_tag==DBG_TY_STRUCT, or pointee struct tag when
     * type_tag==DBG_TY_PTR and the pointee is a named struct. */
    char *type_name;
    int struct_size;        /* byte size of that struct; 0 if unknown */
    DebugMember *members;
    size_t num_members;
    /* Single-location form: used when the variable has one home for its
     * whole scope (stack slot, or a global's address). */
    DebugLocKind loc_kind;
    int rbp_offset;     /* DBG_LOC_FBREG */
    int dwarf_reg;      /* DBG_LOC_REG (x86-64 DWARF numbering) */
    char *sym_name;     /* DBG_LOC_ADDR: ELF symbol name */
    /* Location-list form: non-empty when the variable moves between homes.
     * Takes precedence over loc_kind when num_ranges > 0. */
    DebugLocRange *ranges;
    size_t num_ranges, cap_ranges;
    /* Scratch for codegen before locations are finalized: */
    int alloca_ssa;     /* local/param alloca SSA id, or -1 */
    int param_idx;      /* SysV param index, or -1 */
    /* For DBG_VAR_PARAM: DWARF register the argument arrived in (SysV), or
     * -1 if it arrived on the stack.  Used to emit DW_OP_entry_value so outer
     * frames can recover the parameter after caller-saved homes are clobbered. */
    int entry_dwarf_reg;
} DebugVar;

/* One register argument at a call site (DW_TAG_call_site_parameter). */
typedef struct {
    int dwarf_reg;              /* DW_AT_location: DW_OP_regN */
    unsigned char *value_expr;  /* DW_AT_call_value expression bytes */
    size_t value_expr_len;
} DebugCallSiteParam;

typedef struct {
    size_t call_pc;             /* offset of the CALL instruction */
    size_t return_pc;           /* offset of the instruction after CALL */
    char *callee_name;          /* direct callee; NULL if indirect */
    DebugCallSiteParam *params;
    size_t num_params;
} DebugCallSite;

typedef struct {
    char *file;
    int line;
    int col;
    size_t pc_off;      /* offset within this module's .text */
} DebugLineEntry;

typedef struct {
    char *name;
    char *file;
    int line;
    size_t start_pc;
    size_t end_pc;
    size_t prologue_end_pc;
    /* Where the frame is established, so call-frame info can describe the
     * prologue step by step.  gdb routinely stops mid-prologue (`break f`
     * skips `push %rbp; mov %rsp,%rbp` on its own), and a single CFI rule for
     * the whole prologue makes it unwind into a bogus frame there. */
    size_t after_push_rbp_pc;   /* just past `push %rbp` */
    size_t after_mov_rbp_pc;    /* just past `mov %rsp,%rbp` */
    DebugVar *vars;
    size_t num_vars, cap_vars;
    DebugCallSite *call_sites;
    size_t num_call_sites, cap_call_sites;
} DebugFunc;

/* ------------------------------------------------------------------ */
/* Object module — the in-memory representation of one compiled TU     */
/* ------------------------------------------------------------------ */

typedef struct EmitModule {
    /* Set for a translation unit the user wrote, as opposed to one loaded
     * from a package.  A user definition overrides a package's definition of
     * the same name -- that is how a program replaces fputs or memcpy, and
     * the C library is expected to lose.  */
    int      is_user_tu;
    Buffer   text;     /* .text */
    Buffer   rodata;   /* .rodata (string literals, long double constants) */
    Buffer   data;     /* .data (mutable globals) */
    size_t   bss_size; /* .bss total bytes (zero-initialized globals) */
    Buffer   tdata;    /* .tdata — initialized __thread variables */
    size_t   tbss_size;/* .tbss total bytes (zero-init __thread variables) */
    /* Darwin thread-local variables.  tdata/tbss_size above hold the same
     * storage for the ELF path; this is the descriptor table the Mach-O
     * loader needs on top of it -- one 24-byte tlv_descriptor per
     * __thread object, whose thunk field dyld rebinds to _tlv_bootstrap.
     * Written as __DATA,__thread_vars. */
    Buffer   thread_vars;
    size_t   thread_vars_align;
    /* Max sh_addralign of each output section (gcc objects often use 16). */
    size_t   text_align;
    size_t   rodata_align;
    size_t   data_align;
    size_t   bss_align;
    size_t   tdata_align;
    size_t   tbss_align;
    Buffer   init_array; /* .init_array — constructor function pointers */
    size_t   init_array_align;
    int     *init_prio;  /* one priority per 8-byte init_array slot */
    Buffer   fini_array; /* .fini_array — destructor function pointers */
    size_t   fini_array_align;
    int     *fini_prio;  /* one priority per 8-byte fini_array slot */

    EmitSymbol *syms;  /* unified symbol table (section + defined + undefined) */
    size_t num_syms, cap_syms;

    EmitReloc  *relocs; /* relocations (all within .text) */
    size_t num_relocs, cap_relocs;
    EmitReloc  *data_relocs; /* relocations within .data (pointer fixups) */
    size_t num_data_relocs, cap_data_relocs;
    /* Pointer slots dyld must rebase (arm64 Mach-O PIE). */
    EmitRebase *rebases;
    size_t num_rebases, cap_rebases;
    /* Pointer slots dyld must bind to a dylib symbol (arm64 Mach-O). */
    EmitBind *binds;
    size_t num_binds, cap_binds;

    /* Debug info (-g).  Empty when compiled without -g. */
    char *dbg_tu_name;           /* primary source file name */
    DebugLineEntry *dbg_lines;
    size_t num_dbg_lines, cap_dbg_lines;
    DebugFunc *dbg_funcs;
    size_t num_dbg_funcs, cap_dbg_funcs;
    DebugVar *dbg_globals;
    size_t num_dbg_globals, cap_dbg_globals;
} EmitModule;

/* ------------------------------------------------------------------ */
/* Lifetime                                                            */
/* ------------------------------------------------------------------ */

void emit_module_init(EmitModule *m);
void emit_module_free(EmitModule *m);

/* ------------------------------------------------------------------ */
/* Symbol table                                                        */
/* ------------------------------------------------------------------ */

/* Add a defined symbol; returns its index. */
int  emit_module_add_symbol(EmitModule *m, const char *name,
                            uint8_t binding, uint8_t type,
                            uint16_t shndx, size_t value, size_t size);
/* Find a defined symbol by name; returns index or -1. */
int  emit_module_find_symbol(EmitModule *m, const char *name);
/* Add (or reuse) an undefined symbol; returns its index. */
int  emit_module_add_undefined(EmitModule *m, const char *name);
/* Like emit_module_add_undefined, with an explicit ELF st_type
 * (STT_NOTYPE / STT_TLS).  Reusing an existing UND upgrades NOTYPE. */
int  emit_module_add_undefined_type(EmitModule *m, const char *name,
                                   uint8_t st_type);

/* ------------------------------------------------------------------ */
/* Relocations                                                         */
/* ------------------------------------------------------------------ */

void emit_module_add_reloc(EmitModule *m, size_t offset, uint32_t type,
                           int sym, int32_t addend);
void emit_module_add_data_reloc(EmitModule *m, size_t offset, uint32_t type,
                                int sym, int32_t addend);
/* Record one arm64 image-relative pointer rebase (see EmitRebase). */
void emit_module_add_rebase(EmitModule *m, uint64_t slot, uint64_t target);
/* Record one arm64 dylib import (see EmitBind).  `name` is the C spelling;
 * the Mach-O "_" prefix is added here, so callers pass "pthread_create". */
void emit_module_add_bind(EmitModule *m, uint64_t slot, const char *name);

/* True when `name` is a function the arm64 backend may import from
 * libSystem instead of requiring a definition in the program.  The list is
 * deliberately explicit: an undefined call that is not on it still fails at
 * compile time, which is what keeps a misspelled name a diagnostic rather
 * than a launch-time "symbol not found" from dyld. */
int emit_is_darwin_import(const char *name);

/* ------------------------------------------------------------------ */
/* Output                                                              */
/* ------------------------------------------------------------------ */

/* Write a relocatable object file (ET_REL or Mach-O MH_OBJECT) to path. */
void emit_obj(const EmitModule *m, const char *path);
/* When set, arm64 codegen emits a relocatable object body (no LC_MAIN
 * stub) instead of a freestanding executable image. */
void emit_set_object_mode(int on);
int  emit_object_mode(void);
void emit_set_multi_tu(int on);
int  emit_multi_tu(void);
/* Read a relocatable object file into an EmitModule.  Returns 0 on success. */
int  emit_obj_read(const char *path, EmitModule *m);

#define ET_DYN             3

/* Link one or more object modules into an executable or shared library.
 *
 * If any module references undefined symbols, or if `is_shared` is true,
 * a dynamically-linked ELF (or ET_DYN shared library) with a PLT/GOT is produced;
 * otherwise a static executable.
 *
 * `needed` / `num_needed` are DT_NEEDED sonames (e.g. "libc.so.6", "libfoo.so"),
 * typically derived from `-l` flags.  Defaults never auto-add libc — the driver
 * links builtin `runtime/` instead (Go-like freestanding).  Pass `-l` for optional
 * dynamic interop.  `nodefaultlibs` is retained as a no-op for compatibility.
 * `needed` may be NULL when `num_needed` is 0.
 *
 * `lib_paths` / `num_lib_paths` come from `-L` (and directories of explicit `.so`
 * inputs).  When non-empty on a dynamic link they are joined with ':' into
 * DT_RUNPATH so the dynamic linker can find non-system libraries without
 * LD_LIBRARY_PATH. */
void emit_link(EmitModule **mods, size_t n, const char *path,
               const char **needed, size_t num_needed, int nodefaultlibs,
               const char **lib_paths, size_t num_lib_paths,
               int want_debug, int is_shared);

/* Legacy entry point — compile a single TU directly into an executable.
 * Equivalent to emit_link(&m, 1, path, NULL, 0, 0, NULL, 0, 0, 0). */
void emit_elf(const EmitModule *m, const char *path);

/* ELF relocation type constants (SysV AMD64 ABI). */
#define R_X86_64_64        1  /* absolute 64-bit (pointer fixups in .data) */
#define R_X86_64_PC32      2
#define R_X86_64_PLT32     4
#define R_X86_64_COPY          5  /* executable copy of a DSO data object */
#define R_X86_64_GLOB_DAT  6
#define R_X86_64_GOTPCREL  9
#define R_X86_64_32       10
#define R_X86_64_32S      11
#define R_X86_64_DTPMOD64 16  /* TLS GD GOT pair: module ID */
#define R_X86_64_DTPOFF64 17  /* TLS GD GOT pair: offset in TLS block */
#define R_X86_64_TPOFF64  18  /* TLS IE GOT fill (64-bit): dynamic linker writes
                               * l_tls_offset + st_value + addend into the GOT slot.
                               * Used in DSOs; executables fill the same slot statically. */
#define R_X86_64_TLSGD    19  /* General-Dynamic: 16-byte lea+call __tls_get_addr.
                               * The linker relaxes this to Initial-Exec. */
#define R_X86_64_TLSLD    20  /* Local-Dynamic 12-byte lea+call __tls_get_addr.
                               * Executables relax to LE (`movq %fs:0`). */
#define R_X86_64_DTPOFF32 21  /* Local-Dynamic offset from the TLS block base.
                               * After LD→LE this is filled as TPOFF32 (S+A−tp). */
#define R_X86_64_GOTTPOFF 22  /* TLS Initial-Exec: RIP-relative disp to a GOT slot
                               * holding the TPOFF of a __thread symbol.
                               * Code: movq %fs:0, %reg; addq x@gottpoff(%rip), %reg */
#define R_X86_64_TPOFF32  23  /* TLS Local-Exec offset (32-bit signed): S + A - tp_end.
                               * Patches the imm32 of `addq $imm32, %reg` that follows
                               * a `movq %fs:0, %reg` to produce the thread-local address.
                               * Kept for legacy objects and static executables; DSOs
                               * must use GOTTPOFF + TPOFF64 (ld.so rejects type 0x17). */
#define R_X86_64_PC64          24 /* S + A - P, 64-bit (`.quad sym - .`) */
#define R_X86_64_GOTPCRELX     41 /* relaxable GOTPCREL; same S as GOTPCREL */
#define R_X86_64_REX_GOTPCRELX 42 /* REX-prefixed relaxable GOTPCREL */
#define R_X86_64_IRELATIVE     37 /* STT_GNU_IFUNC: r_addend = resolver, GOT filled at startup */
#define STT_GNU_IFUNC          10

#endif /* FAKECC_EMIT_H */
