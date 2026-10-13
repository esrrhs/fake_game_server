#include "fakecc/emit.h"
#include "fakecc/common.h"
#include "fakecc/debug.h"
#include "fakecc/macho.h"
#include "fakecc/target.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/* EmitModule lifetime                                                 */
/* ------------------------------------------------------------------ */

void emit_module_init(EmitModule *m) {
    /* Only a TU the driver built from a command-line source is a user TU
     * (main.c promotes it after lowering); modules codegen'd for imported
     * packages must start at 0.  The driver's module array is malloc'd, so
     * without this the field kept stale heap bytes and a package definition
     * (memcpy, strlen, ...) was occasionally mistaken for a user one, which
     * made the replaceable-symbol rule report a spurious duplicate symbol. */
    m->is_user_tu = 0;
    buffer_init(&m->text);
    buffer_init(&m->rodata);
    buffer_init(&m->data);
    m->bss_size = 0;
    buffer_init(&m->tdata);
    m->tbss_size = 0;
    buffer_init(&m->thread_vars);
    m->thread_vars_align = 8;
    m->text_align = 16;
    m->rodata_align = 8;
    m->data_align = 8;
    m->bss_align = 8;
    m->tdata_align = 8;
    m->tbss_align = 8;
    buffer_init(&m->init_array);
    m->init_array_align = 8;
    m->init_prio = NULL;
    buffer_init(&m->fini_array);
    m->fini_array_align = 8;
    m->fini_prio = NULL;
    m->syms = NULL; m->num_syms = 0; m->cap_syms = 0;
    m->relocs = NULL; m->num_relocs = 0; m->cap_relocs = 0;
    m->data_relocs = NULL; m->num_data_relocs = 0; m->cap_data_relocs = 0;
    m->rebases = NULL; m->num_rebases = 0; m->cap_rebases = 0;
    m->binds = NULL; m->num_binds = 0; m->cap_binds = 0;
    m->dbg_tu_name = NULL;
    m->dbg_lines = NULL; m->num_dbg_lines = 0; m->cap_dbg_lines = 0;
    m->dbg_funcs = NULL; m->num_dbg_funcs = 0; m->cap_dbg_funcs = 0;
    m->dbg_globals = NULL; m->num_dbg_globals = 0; m->cap_dbg_globals = 0;
}

static void free_debug_var(DebugVar *v) {
    debug_var_release(v);
}

void emit_module_free(EmitModule *m) {
    for (size_t i = 0; i < m->num_syms; i++) free(m->syms[i].name);
    free(m->syms);
    free(m->relocs);
    free(m->data_relocs);
    free(m->rebases);
    for (size_t i = 0; i < m->num_binds; i++) free(m->binds[i].name);
    free(m->binds);
    buffer_free(&m->text);
    buffer_free(&m->rodata);
    buffer_free(&m->data);
    buffer_free(&m->tdata);
    buffer_free(&m->thread_vars);
    buffer_free(&m->init_array);
    free(m->init_prio);
    buffer_free(&m->fini_array);
    free(m->fini_prio);
    free(m->dbg_tu_name);
    for (size_t i = 0; i < m->num_dbg_lines; i++) free(m->dbg_lines[i].file);
    free(m->dbg_lines);
    for (size_t i = 0; i < m->num_dbg_funcs; i++) {
        free(m->dbg_funcs[i].name);
        free(m->dbg_funcs[i].file);
        for (size_t j = 0; j < m->dbg_funcs[i].num_vars; j++)
            free_debug_var(&m->dbg_funcs[i].vars[j]);
        free(m->dbg_funcs[i].vars);
        for (size_t j = 0; j < m->dbg_funcs[i].num_call_sites; j++)
            debug_call_site_release(&m->dbg_funcs[i].call_sites[j]);
        free(m->dbg_funcs[i].call_sites);
    }
    free(m->dbg_funcs);
    for (size_t i = 0; i < m->num_dbg_globals; i++)
        free_debug_var(&m->dbg_globals[i]);
    free(m->dbg_globals);
}

/* ------------------------------------------------------------------ */
/* Symbol table                                                        */
/* ------------------------------------------------------------------ */

int emit_module_add_symbol(EmitModule *m, const char *name, uint8_t binding,
                           uint8_t type, uint16_t shndx, size_t value,
                           size_t size) {
    if (name) {
        for (size_t i = 0; i < m->num_syms; i++) {
            if (m->syms[i].name && strcmp(m->syms[i].name, name) == 0 &&
                m->syms[i].shndx == SECT_UNDEF) {
                m->syms[i].binding = binding;
                m->syms[i].type = type;
                m->syms[i].shndx = shndx;
                m->syms[i].value = value;
                m->syms[i].size = size;
                m->syms[i].macho_desc = 0;
                return (int)i;
            }
        }
    }
    if (m->num_syms >= m->cap_syms) {
        size_t nc = m->cap_syms ? m->cap_syms * 2 : 8;
        m->syms = realloc(m->syms, nc * sizeof(EmitSymbol));
        if (!m->syms) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        m->cap_syms = nc;
    }
    EmitSymbol *s = &m->syms[m->num_syms];
    s->name = name ? xstrdup(name) : NULL;
    s->binding = binding;
    s->type = type;
    s->shndx = shndx;
    s->value = value;
    s->size = size;
    s->macho_desc = 0;
    return (int)m->num_syms++;
}

int emit_module_find_symbol(EmitModule *m, const char *name) {
    if (!name) return -1;
    for (size_t i = 0; i < m->num_syms; i++) {
        if (m->syms[i].name && strcmp(m->syms[i].name, name) == 0)
            return (int)i;
    }
    return -1;
}

int emit_module_add_undefined_type(EmitModule *m, const char *name,
                                  uint8_t st_type) {
    if (!name) {
        /* No relocatable name (e.g. a long-double IR_CONST whose call_name
         * was not set): emit an unnamed absolute undefined symbol rather than
         * dereference NULL in strcmp.  It will not be referenced by name. */
        return emit_module_add_symbol(m, NULL, 1 /* STB_GLOBAL */,
                                      st_type, SECT_UNDEF, 0, 0);
    }
    /* Reuse an existing undefined symbol of the same name if present.
     * Upgrade STT_NOTYPE when a later reference knows the real type
     * (`extern __thread` → STT_TLS). */
    for (size_t i = 0; i < m->num_syms; i++) {
        if (m->syms[i].name && strcmp(m->syms[i].name, name) == 0 &&
            m->syms[i].shndx == SECT_UNDEF) {
            if (st_type != 0 && m->syms[i].type == 0)
                m->syms[i].type = st_type;
            return (int)i;
        }
    }
    return emit_module_add_symbol(m, name, 1 /* STB_GLOBAL */, st_type,
                                  SECT_UNDEF, 0, 0);
}

int emit_module_add_undefined(EmitModule *m, const char *name) {
    return emit_module_add_undefined_type(m, name, 0 /* STT_NOTYPE */);
}

/* ------------------------------------------------------------------ */
/* Relocations                                                         */
/* ------------------------------------------------------------------ */

void emit_module_add_reloc(EmitModule *m, size_t offset, uint32_t type,
                           int sym, int32_t addend) {
    if (m->num_relocs >= m->cap_relocs) {
        size_t nc = m->cap_relocs ? m->cap_relocs * 2 : 8;
        m->relocs = realloc(m->relocs, nc * sizeof(EmitReloc));
        if (!m->relocs) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        m->cap_relocs = nc;
    }
    EmitReloc *r = &m->relocs[m->num_relocs++];
    r->offset = offset;
    r->type = type;
    r->sym = (uint32_t)sym;
    r->addend = addend;
    r->shndx = SECT_TEXT;
}

/* Add a relocation within .data (for pointer fixups in global initializers).
 * Default site is SECT_DATA; callers that patch TLS pointer initializers
 * overwrite shndx so emit_obj can split them into .rela.tdata. */
void emit_module_add_data_reloc(EmitModule *m, size_t offset, uint32_t type,
                                int sym, int32_t addend) {
    if (m->num_data_relocs >= m->cap_data_relocs) {
        size_t nc = m->cap_data_relocs ? m->cap_data_relocs * 2 : 8;
        m->data_relocs = realloc(m->data_relocs, nc * sizeof(EmitReloc));
        if (!m->data_relocs) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        m->cap_data_relocs = nc;
    }
    EmitReloc *r = &m->data_relocs[m->num_data_relocs++];
    r->offset = offset;
    r->type = type;
    r->sym = (uint32_t)sym;
    r->addend = addend;
    r->shndx = SECT_DATA;
}

void emit_module_add_rebase(EmitModule *m, uint64_t slot, uint64_t target) {
    if (m->num_rebases >= m->cap_rebases) {
        size_t nc = m->cap_rebases ? m->cap_rebases * 2 : 8;
        m->rebases = realloc(m->rebases, nc * sizeof(EmitRebase));
        if (!m->rebases) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        m->cap_rebases = nc;
    }
    m->rebases[m->num_rebases].slot = slot;
    m->rebases[m->num_rebases].target = target;
    m->num_rebases++;
}

void emit_module_add_bind(EmitModule *m, uint64_t slot, const char *name) {
    if (!name) return;
    if (m->num_binds >= m->cap_binds) {
        size_t nc = m->cap_binds ? m->cap_binds * 2 : 8;
        m->binds = realloc(m->binds, nc * sizeof(EmitBind));
        if (!m->binds) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        m->cap_binds = nc;
    }
    size_t nlen = strlen(name);
    char *mach = malloc(nlen + 2);
    if (!mach) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
    mach[0] = '_';
    memcpy(mach + 1, name, nlen + 1);
    m->binds[m->num_binds].slot = slot;
    m->binds[m->num_binds].name = mach;
    m->num_binds++;
}

/* Functions a freestanding image may reach for in libSystem.  Every
 * executable already carries LC_LOAD_DYLIB for it, so dyld has the library
 * mapped; what was missing was any way to name one of its symbols.
 *
 * Only what the runtime actually needs is listed.  fakecc has its own
 * printf, malloc and string functions, so adding those would only widen
 * the set of typos that compile.  Threads are the one thing that cannot be
 * built from syscalls here: bsdthread_create and bsdthread_register both
 * return EINVAL from any thread libpthread did not create, including one
 * that already called pthread_create successfully -- measured, not assumed. */
static const char *const emit_darwin_imports[] = {
    "pthread_create",
    "pthread_join",
    "pthread_self",
    "pthread_exit",
    "pthread_detach",
    "pthread_equal",
    "pthread_once",
    "pthread_mutex_init",
    "pthread_mutex_lock",
    "pthread_mutex_unlock",
    "pthread_mutex_destroy",
    "pthread_cond_init",
    "pthread_cond_wait",
    "pthread_cond_signal",
    "pthread_cond_broadcast",
    "pthread_cond_destroy",
    "sched_yield",
    /* Clang's stack protector.  -fstack-protector is the distribution
     * default, so every object it produces references these two; without
     * them in the list a link against a stock clang .o fails with
     * "undefined symbol". */
    "__stack_chk_fail",
    "__stack_chk_guard",
    /* ld64's static-init stub registers destructors through this, so every
     * object compiled with a destructor or a constructor that needs cleanup
     * references it.  libSystem exports it (__dso_handle, which the stub
     * also names, does not -- the linker synthesises that one instead). */
    "__cxa_atexit",
};

int emit_is_darwin_import(const char *name) {
    if (!name) return 0;
    for (size_t i = 0; i < sizeof emit_darwin_imports
                            / sizeof *emit_darwin_imports; i++)
        if (strcmp(emit_darwin_imports[i], name) == 0) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* ELF primitives for object-file writing                              */
/* ------------------------------------------------------------------ */

#define ELFCLASS64      2
#define ELFDATA2LSB     1
#define EV_CURRENT      1
#define ELFOSABI_NONE   0
#define ET_REL          1
#define EM_X86_64       62

#define SHT_NULL        0
#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_STRTAB      3
#define SHT_RELA        4
#define SHT_NOBITS      8
#define SHT_INIT_ARRAY 14
#define SHT_FINI_ARRAY 15

#define SHF_WRITE       0x1
#define SHF_ALLOC       0x2
#define SHF_EXECINSTR   0x4
#define SHF_TLS         0x400

#define STB_LOCAL       0
#define STB_GLOBAL      1
#define STT_NOTYPE      0
#define STT_OBJECT      1
#define STT_FUNC        2
#define STT_SECTION     3

#define SHN_UNDEF       0
#define SHN_ABS         0xfff1
#define SHN_COMMON      0xfff2

#define ELF64_EHDR_SIZE 64
#define ELF64_PHDR_SIZE 56
#define ELF64_SYM_SIZE  24
#define ELF64_RELA_SIZE 24
#define ELF64_SHDR_SIZE 64

static void buf_u8(Buffer *b, uint8_t v) { buffer_append(b, (const char *)&v, 1); }
static void buf_bytes(Buffer *b, const char *d, size_t n) { buffer_append(b, d, n); }
static void buf_u16(Buffer *b, uint16_t v) { buffer_append(b, (const char *)&v, 2); }
static void buf_u32(Buffer *b, uint32_t v) { buffer_append(b, (const char *)&v, 4); }
static void buf_u64(Buffer *b, uint64_t v) { buffer_append(b, (const char *)&v, 8); }
static void buf_pad(Buffer *b, size_t n) { while (n--) buf_u8(b, 0); }

static void write_ehdr(Buffer *b, uint16_t type, uint16_t shnum) {
    buffer_append(b, "\x7f" "ELF", 4);
    char ident[12];
    memset(ident, 0, 12);
    ident[0] = ELFCLASS64;
    ident[1] = ELFDATA2LSB;
    ident[2] = EV_CURRENT;
    ident[3] = ELFOSABI_NONE;
    buffer_append(b, ident, 12);

    buf_u16(b, type);          /* e_type */
    buf_u16(b, EM_X86_64);     /* e_machine */
    buf_u32(b, EV_CURRENT);    /* e_version */
    buf_u64(b, 0);             /* e_entry (none for relocatable) */
    buf_u64(b, 0);             /* e_phoff (no program headers) */
    buf_u64(b, 0);             /* e_shoff (patched later) */
    buf_u32(b, 0);             /* e_flags */
    buf_u16(b, ELF64_EHDR_SIZE); /* e_ehsize */
    buf_u16(b, ELF64_PHDR_SIZE); /* e_phentsize */
    buf_u16(b, 0);             /* e_phnum */
    buf_u16(b, ELF64_SHDR_SIZE); /* e_shentsize */
    buf_u16(b, shnum);         /* e_shnum */
    buf_u16(b, 0);             /* e_shstrndx (patched later) */
}

static void write_shdr(Buffer *b, uint32_t name, uint32_t type, uint64_t flags,
                       uint64_t addr, uint64_t offset, uint64_t size,
                       uint32_t link, uint32_t info, uint64_t addralign,
                       uint64_t entsize) {
    buf_u32(b, name);          /* sh_name */
    buf_u32(b, type);          /* sh_type */
    buf_u64(b, flags);         /* sh_flags */
    buf_u64(b, addr);          /* sh_addr */
    buf_u64(b, offset);        /* sh_offset */
    buf_u64(b, size);          /* sh_size */
    buf_u32(b, link);          /* sh_link */
    buf_u32(b, info);          /* sh_info */
    buf_u64(b, addralign);     /* sh_addralign */
    buf_u64(b, entsize);       /* sh_entsize */
}

static uint32_t append_array_shname(Buffer *shstrtab, const char *base,
                                    const int *prio, size_t nbytes) {
    int p = INIT_PRIO_DEFAULT;
    int all = 1;
    size_t n = nbytes / 8;
    if (prio && n > 0) {
        p = prio[0];
        for (size_t i = 1; i < n; i++) {
            if (prio[i] != p) { all = 0; break; }
        }
    }
    char nm[40];
    if (!all || p == INIT_PRIO_DEFAULT)
        snprintf(nm, sizeof nm, "%s", base);
    else
        snprintf(nm, sizeof nm, "%s.%05d", base, p);
    uint32_t off = (uint32_t)shstrtab->len;
    buf_bytes(shstrtab, nm, strlen(nm) + 1);
    return off;
}

/* ------------------------------------------------------------------ */
/* emit_obj — write a relocatable object file (ET_REL)                 */
/* ------------------------------------------------------------------ */

static int g_object_mode;

void emit_set_object_mode(int on) { g_object_mode = on ? 1 : 0; }
int emit_object_mode(void) { return g_object_mode; }

/* Multi-TU link: more than one translation unit contributes to the output.
 * The arm64 backend emits its LC_MAIN entry stub inside the TU that defines
 * main, so it needs to know a TU without main is legitimate rather than an
 * error -- which is only true once another TU can supply it. */
static int g_multi_tu;

void emit_set_multi_tu(int on) { g_multi_tu = on ? 1 : 0; }
int emit_multi_tu(void) { return g_multi_tu; }

void emit_obj(const EmitModule *m, const char *path) {
    if (target_current()->objfmt == TARGET_OBJFMT_MACHO) {
        if (macho_write_object(m, path) != 0) exit(1);
        return;
    }
    if (target_current()->objfmt != TARGET_OBJFMT_ELF) {
        fprintf(stderr,
                "fakecc: object emission for target '%s' is not implemented yet\n",
                target_current()->triple);
        exit(1);
    }
    /* Section layout: .text, .rodata, .data, .bss, .symtab, .strtab,
     * .shstrtab, .rela.text.  We build the section data first, then the
     * section headers, then patch e_shoff + e_shstrndx into the ehdr. */

    /* --- section name string table --- */
    Buffer shstrtab;
    buffer_init(&shstrtab);
    buf_u8(&shstrtab, 0); /* empty string at index 0 */

    /* name offsets: index by section id */
    uint32_t shname_text = (uint32_t)shstrtab.len;
    buf_bytes(&shstrtab, ".text", sizeof(".text"));
    uint32_t shname_rodata = (uint32_t)shstrtab.len;
    buf_bytes(&shstrtab, ".rodata", sizeof(".rodata"));
    uint32_t shname_data = (uint32_t)shstrtab.len;
    buf_bytes(&shstrtab, ".data", sizeof(".data"));
    uint32_t shname_bss = (uint32_t)shstrtab.len;
    buf_bytes(&shstrtab, ".bss", sizeof(".bss"));
    uint32_t shname_tdata = (uint32_t)shstrtab.len;
    buf_bytes(&shstrtab, ".tdata", sizeof(".tdata"));
    uint32_t shname_tbss = (uint32_t)shstrtab.len;
    buf_bytes(&shstrtab, ".tbss", sizeof(".tbss"));
    uint32_t shname_symtab = (uint32_t)shstrtab.len;
    buf_bytes(&shstrtab, ".symtab", sizeof(".symtab"));
    uint32_t shname_strtab = (uint32_t)shstrtab.len;
    buf_bytes(&shstrtab, ".strtab", sizeof(".strtab"));
    uint32_t shname_rela_text = (uint32_t)shstrtab.len;
    buf_bytes(&shstrtab, ".rela.text", sizeof(".rela.text"));
    uint32_t shname_rela_data = (uint32_t)shstrtab.len;
    buf_bytes(&shstrtab, ".rela.data", sizeof(".rela.data"));
    uint32_t shname_rela_tdata = (uint32_t)shstrtab.len;
    buf_bytes(&shstrtab, ".rela.tdata", sizeof(".rela.tdata"));
    uint32_t shname_fakecc_dbg = (uint32_t)shstrtab.len;
    buf_bytes(&shstrtab, ".fakecc_dbg", sizeof(".fakecc_dbg"));
    uint32_t shname_gnu_stack = (uint32_t)shstrtab.len;
    buf_bytes(&shstrtab, ".note.GNU-stack", sizeof(".note.GNU-stack"));
    uint32_t shname_init_array = append_array_shname(&shstrtab, ".init_array",
                                                    m->init_prio, m->init_array.len);
    uint32_t shname_rela_init = (uint32_t)shstrtab.len;
    buf_bytes(&shstrtab, ".rela.init_array", sizeof(".rela.init_array"));
    uint32_t shname_fini_array = append_array_shname(&shstrtab, ".fini_array",
                                                    m->fini_prio, m->fini_array.len);
    uint32_t shname_rela_fini = (uint32_t)shstrtab.len;
    buf_bytes(&shstrtab, ".rela.fini_array", sizeof(".rela.fini_array"));
    uint32_t shname_shstrtab = (uint32_t)shstrtab.len;
    buf_bytes(&shstrtab, ".shstrtab", sizeof(".shstrtab"));

    /* --- symbol name string table --- */
    Buffer strtab;
    buffer_init(&strtab);
    buf_u8(&strtab, 0); /* empty string at index 0 */

    /* --- symbol table --- */
    Buffer symtab;
    buffer_init(&symtab);
    /* [0] NULL symbol */
    buf_pad(&symtab, ELF64_SYM_SIZE);

    /* section symbols (one per real section, in section-index order) */
    uint32_t sym_text = (uint32_t)(symtab.len / ELF64_SYM_SIZE);
    buf_pad(&symtab, ELF64_SYM_SIZE); /* STT_SECTION for .text */
    uint32_t sym_rodata = (uint32_t)(symtab.len / ELF64_SYM_SIZE);
    buf_pad(&symtab, ELF64_SYM_SIZE);
    uint32_t sym_data = (uint32_t)(symtab.len / ELF64_SYM_SIZE);
    buf_pad(&symtab, ELF64_SYM_SIZE);
    uint32_t sym_bss = (uint32_t)(symtab.len / ELF64_SYM_SIZE);
    buf_pad(&symtab, ELF64_SYM_SIZE);
    uint32_t sym_tdata = (uint32_t)(symtab.len / ELF64_SYM_SIZE);
    buf_pad(&symtab, ELF64_SYM_SIZE);
    uint32_t sym_tbss = (uint32_t)(symtab.len / ELF64_SYM_SIZE);
    buf_pad(&symtab, ELF64_SYM_SIZE);

    /* Emit defined + undefined symbols in ELF-required order: all LOCAL
     * symbols first, then all GLOBAL/WEAK symbols.  This groups the
     * .symtab correctly so sh_info (index of first non-local) is valid.
     * We also remap old m->syms[] indices → new .symtab indices so
     * relocation entries (which reference symbols by index) stay correct. */
    int *sym_remap = malloc(m->num_syms * sizeof(int));
    if (!sym_remap) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
    for (size_t i = 0; i < m->num_syms; i++) sym_remap[i] = -1;

    /* Pass 1: LOCAL symbols */
    for (size_t i = 0; i < m->num_syms; i++) {
        const EmitSymbol *s = &m->syms[i];
        if (s->binding != STB_LOCAL) continue;
        sym_remap[i] = (int)(symtab.len / ELF64_SYM_SIZE);
        uint32_t name_off = (uint32_t)strtab.len;
        if (s->name) buf_bytes(&strtab, s->name, strlen(s->name) + 1);
        else buf_u8(&strtab, 0);
        buf_u32(&symtab, name_off);
        buf_u8(&symtab, (s->binding << 4) | (s->type & 0xf));
        buf_u8(&symtab, 0);
        buf_u16(&symtab, s->shndx);
        buf_u64(&symtab, s->value);
        buf_u64(&symtab, s->size);
    }
    /* Remember the first non-local index for sh_info. */
    unsigned first_global = (unsigned)(symtab.len / ELF64_SYM_SIZE);

    /* Pass 2: GLOBAL / WEAK symbols */
    for (size_t i = 0; i < m->num_syms; i++) {
        const EmitSymbol *s = &m->syms[i];
        if (s->binding == STB_LOCAL) continue;
        sym_remap[i] = (int)(symtab.len / ELF64_SYM_SIZE);
        uint32_t name_off = (uint32_t)strtab.len;
        if (s->name) buf_bytes(&strtab, s->name, strlen(s->name) + 1);
        else buf_u8(&strtab, 0);
        buf_u32(&symtab, name_off);
        buf_u8(&symtab, (s->binding << 4) | (s->type & 0xf));
        buf_u8(&symtab, 0);
        buf_u16(&symtab, s->shndx);
        buf_u64(&symtab, s->value);
        buf_u64(&symtab, s->size);
    }
    /* Fill in the section symbols' st_info now that the table exists. */
    uint8_t sec_info = (STB_LOCAL << 4) | STT_SECTION;
    memcpy(symtab.data + sym_text * ELF64_SYM_SIZE + 4, &sec_info, 1);
    memcpy(symtab.data + sym_rodata * ELF64_SYM_SIZE + 4, &sec_info, 1);
    memcpy(symtab.data + sym_data * ELF64_SYM_SIZE + 4, &sec_info, 1);
    memcpy(symtab.data + sym_bss * ELF64_SYM_SIZE + 4, &sec_info, 1);
    memcpy(symtab.data + sym_tdata * ELF64_SYM_SIZE + 4, &sec_info, 1);
    memcpy(symtab.data + sym_tbss * ELF64_SYM_SIZE + 4, &sec_info, 1);

    /* --- relocation tables (.rela.text and .rela.data) --- */
    /* Relocations reference symbols by their old m->syms[] index.
     * sym_remap[] maps old index → new .symtab index (which already
     * includes the 5 section symbols at positions 0-4), so we use
     * the remapped value directly. */
    Buffer rela_text;
    buffer_init(&rela_text);
    for (size_t i = 0; i < m->num_relocs; i++) {
        const EmitReloc *r = &m->relocs[i];
        int old_sym = r->sym;
        int new_sym = (old_sym >= 0 && old_sym < (int)m->num_syms)
                      ? sym_remap[old_sym] : old_sym;
        if (new_sym < 0) new_sym = old_sym; /* safety: unmapped stays as-is */
        buf_u64(&rela_text, r->offset);                        /* r_offset */
        buf_u64(&rela_text, ((uint64_t)new_sym << 32) | r->type);
        buf_u64(&rela_text, (uint64_t)(int64_t)r->addend);     /* r_addend */
    }
    Buffer rela_data, rela_tdata, rela_init, rela_fini;
    buffer_init(&rela_data);
    buffer_init(&rela_tdata);
    buffer_init(&rela_init);
    buffer_init(&rela_fini);
    for (size_t i = 0; i < m->num_data_relocs; i++) {
        const EmitReloc *r = &m->data_relocs[i];
        int old_sym = r->sym;
        int new_sym = (old_sym >= 0 && old_sym < (int)m->num_syms)
                      ? sym_remap[old_sym] : old_sym;
        if (new_sym < 0) new_sym = old_sym;
        Buffer *dst = (r->shndx == SECT_TDATA) ? &rela_tdata
                    : (r->shndx == SECT_INIT_ARRAY) ? &rela_init
                    : (r->shndx == SECT_FINI_ARRAY) ? &rela_fini
                    : &rela_data;
        buf_u64(dst, r->offset);                               /* r_offset */
        buf_u64(dst, ((uint64_t)new_sym << 32) | r->type);
        buf_u64(dst, (uint64_t)(int64_t)r->addend);            /* r_addend */
    }

    /* --- assemble section data in order --- */
    /* We write: .text, .rodata, .data, .bss(implicit size), then metadata. */
    size_t hdr_size = ELF64_EHDR_SIZE;

    Buffer body;
    buffer_init(&body);
    size_t off_text = body.len;
    buf_bytes(&body, m->text.data, m->text.len);
    size_t off_rodata = body.len;
    buf_bytes(&body, m->rodata.data, m->rodata.len);
    size_t off_data = body.len;
    buf_bytes(&body, m->data.data, m->data.len);
    /* .bss contributes size but no bytes. */
    size_t off_tdata = body.len;
    buf_bytes(&body, m->tdata.data, m->tdata.len);
    /* .tbss contributes size but no bytes. */
    size_t off_symtab = body.len;
    buf_bytes(&body, symtab.data, symtab.len);
    size_t off_strtab = body.len;
    buf_bytes(&body, strtab.data, strtab.len);
    size_t off_shstrtab = body.len;
    buf_bytes(&body, shstrtab.data, shstrtab.len);
    size_t off_rela_text = body.len;
    buf_bytes(&body, rela_text.data, rela_text.len);
    size_t off_rela_data = body.len;
    buf_bytes(&body, rela_data.data, rela_data.len);
    size_t off_rela_tdata = body.len;
    buf_bytes(&body, rela_tdata.data, rela_tdata.len);
    int have_init = m->init_array.len > 0;
    size_t off_init_array = body.len;
    if (have_init)
        buf_bytes(&body, m->init_array.data, m->init_array.len);
    size_t off_rela_init = body.len;
    if (have_init)
        buf_bytes(&body, rela_init.data, rela_init.len);
    int have_fini = m->fini_array.len > 0;
    size_t off_fini_array = body.len;
    if (have_fini)
        buf_bytes(&body, m->fini_array.data, m->fini_array.len);
    size_t off_rela_fini = body.len;
    if (have_fini)
        buf_bytes(&body, rela_fini.data, rela_fini.len);

    Buffer fakecc_dbg;
    buffer_init(&fakecc_dbg);
    int have_dbg = (m->num_dbg_lines > 0 || m->num_dbg_funcs > 0 ||
                    m->num_dbg_globals > 0 || m->dbg_tu_name != NULL);
    if (have_dbg)
        debug_serialize(m, &fakecc_dbg);
    size_t off_fakecc_dbg = body.len;
    buf_bytes(&body, fakecc_dbg.data, fakecc_dbg.len);

    size_t shoff = hdr_size + body.len;
    /* null + 6 data/tls + symtab + strtab + shstrtab + rela.text + rela.data
     * + rela.tdata + .note.GNU-stack + [.fakecc_dbg] */
    unsigned shnum = have_dbg ? 15 : 14;
    if (have_init) shnum += 2;
    if (have_fini) shnum += 2;
    unsigned shstrndx = 9; /* index of .shstrtab */

    /* --- ELF header --- */
    Buffer elf;
    buffer_init(&elf);
    write_ehdr(&elf, ET_REL, shnum);
    /* patch e_shoff (offset 40) + e_shnum (offset 60) + e_shstrndx (offset 62) */
    uint64_t shoff_val = shoff;
    memcpy(elf.data + 40, &shoff_val, 8);
    uint16_t shnum_val = shnum;
    memcpy(elf.data + 60, &shnum_val, 2);
    uint16_t shstrndx_val = shstrndx;
    memcpy(elf.data + 62, &shstrndx_val, 2);
    buf_bytes(&elf, body.data, body.len);

    /* --- section headers --- */
    /* Section data begins right after the ELF header (file offset hdr_size);
     * all stored offsets below are body-relative, so add hdr_size. */
    /* [0] NULL */
    buf_pad(&elf, ELF64_SHDR_SIZE);
    /* [1] .text */
    write_shdr(&elf, shname_text, SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR,
               0, hdr_size + off_text, m->text.len, 0, 0,
               m->text_align ? m->text_align : 16, 0);
    /* [2] .rodata */
    write_shdr(&elf, shname_rodata, SHT_PROGBITS, SHF_ALLOC,
               0, hdr_size + off_rodata, m->rodata.len, 0, 0,
               m->rodata_align ? m->rodata_align : 8, 0);
    /* [3] .data */
    write_shdr(&elf, shname_data, SHT_PROGBITS, SHF_ALLOC | SHF_WRITE,
               0, hdr_size + off_data, m->data.len, 0, 0,
               m->data_align ? m->data_align : 8, 0);
    /* [4] .bss */
    write_shdr(&elf, shname_bss, SHT_NOBITS, SHF_ALLOC | SHF_WRITE,
               0, hdr_size + off_data + m->data.len, m->bss_size, 0, 0,
               m->bss_align ? m->bss_align : 8, 0);
    /* [5] .tdata */
    write_shdr(&elf, shname_tdata, SHT_PROGBITS, SHF_ALLOC | SHF_WRITE | SHF_TLS,
               0, hdr_size + off_tdata, m->tdata.len, 0, 0,
               m->tdata_align ? m->tdata_align : 8, 0);
    /* [6] .tbss */
    write_shdr(&elf, shname_tbss, SHT_NOBITS, SHF_ALLOC | SHF_WRITE | SHF_TLS,
               0, hdr_size + off_tdata + m->tdata.len, m->tbss_size, 0, 0,
               m->tbss_align ? m->tbss_align : 8, 0);
    /* Section indices (0-based): 0=NULL, 1=.text, 2=.rodata, 3=.data, 4=.bss,
     * 5=.tdata, 6=.tbss, 7=.symtab, 8=.strtab, 9=.shstrtab, 10=.rela.text,
     * 11=.rela.data, 12=.rela.tdata. */
    unsigned symtab_idx = 7;
    unsigned strtab_idx = 8;
    /* Symbol table: sh_info = first_global (computed during symtab emission) */
    write_shdr(&elf, shname_symtab, SHT_SYMTAB, 0,
               0, hdr_size + off_symtab, symtab.len, strtab_idx, first_global, 8,
               ELF64_SYM_SIZE);
    /* .strtab */
    write_shdr(&elf, shname_strtab, SHT_STRTAB, 0,
               0, hdr_size + off_strtab, strtab.len, 0, 0, 1, 0);
    /* .shstrtab */
    write_shdr(&elf, shname_shstrtab, SHT_STRTAB, 0,
               0, hdr_size + off_shstrtab, shstrtab.len, 0, 0, 1, 0);
    /* .rela.text (sh_link = symtab index, sh_info = text index) */
    write_shdr(&elf, shname_rela_text, SHT_RELA, 0,
               0, hdr_size + off_rela_text, rela_text.len, symtab_idx,
               1 /* .text */, 8, ELF64_RELA_SIZE);
    /* .rela.data (sh_link = symtab index, sh_info = data index) */
    write_shdr(&elf, shname_rela_data, SHT_RELA, 0,
               0, hdr_size + off_rela_data, rela_data.len, symtab_idx,
               3 /* .data */, 8, ELF64_RELA_SIZE);
    /* .rela.tdata (sh_link = symtab index, sh_info = tdata index) */
    write_shdr(&elf, shname_rela_tdata, SHT_RELA, 0,
               0, hdr_size + off_rela_tdata, rela_tdata.len, symtab_idx,
               5 /* .tdata */, 8, ELF64_RELA_SIZE);
    /* .note.GNU-stack: empty PROGBITS, no SHF_EXECINSTR → non-exec stack. */
    write_shdr(&elf, shname_gnu_stack, SHT_PROGBITS, 0,
               0, 0, 0, 0, 0, 1, 0);
    if (have_dbg) {
        write_shdr(&elf, shname_fakecc_dbg, SHT_PROGBITS, 0,
                   0, hdr_size + off_fakecc_dbg, fakecc_dbg.len, 0, 0, 1, 0);
    }
    if (have_init) {
        unsigned ia_idx = have_dbg ? 15 : 14;
        write_shdr(&elf, shname_init_array, SHT_INIT_ARRAY, SHF_ALLOC | SHF_WRITE,
                   0, hdr_size + off_init_array, m->init_array.len, 0, 0, 8, 8);
        write_shdr(&elf, shname_rela_init, SHT_RELA, 0,
                   0, hdr_size + off_rela_init, rela_init.len, symtab_idx,
                   ia_idx, 8, ELF64_RELA_SIZE);
    }
    if (have_fini) {
        unsigned fa_idx = (have_dbg ? 15 : 14) + (have_init ? 2 : 0);
        write_shdr(&elf, shname_fini_array, SHT_FINI_ARRAY, SHF_ALLOC | SHF_WRITE,
                   0, hdr_size + off_fini_array, m->fini_array.len, 0, 0, 8, 8);
        write_shdr(&elf, shname_rela_fini, SHT_RELA, 0,
                   0, hdr_size + off_rela_fini, rela_fini.len, symtab_idx,
                   fa_idx, 8, ELF64_RELA_SIZE);
    }

    (void)shstrndx; (void)symtab_idx; (void)strtab_idx; (void)first_global;
    (void)shname_fakecc_dbg;

    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "fakecc: cannot write '%s'\n", path); exit(1); }
    fwrite(elf.data, 1, elf.len, f);
    fclose(f);

    free(sym_remap);
    buffer_free(&shstrtab);
    buffer_free(&strtab);
    buffer_free(&symtab);
    buffer_free(&rela_text);
    buffer_free(&rela_data);
    buffer_free(&rela_tdata);
    buffer_free(&rela_init);
    buffer_free(&rela_fini);
    buffer_free(&fakecc_dbg);
    buffer_free(&body);
    buffer_free(&elf);
}

/* ------------------------------------------------------------------ */
/* emit_elf — single-TU convenience wrapper                            */
/* ------------------------------------------------------------------ */

void emit_elf(const EmitModule *m, const char *path) {
    EmitModule *arr = (EmitModule *)m;
    emit_link(&arr, 1, path, NULL, 0, 0, NULL, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/* emit_obj_read — read a relocatable object file into an EmitModule    */
/* ------------------------------------------------------------------ */

/* Read a uint64 from a byte buffer (little-endian). */
static uint64_t rd_u64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static uint32_t rd_u32(const unsigned char *p) {
    return (uint32_t)rd_u64(p);
}
static uint16_t rd_u16(const unsigned char *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void bump_align(size_t *dst, uint64_t a) {
    if (a > *dst) *dst = (size_t)a;
}

/* gcc constructor(100) → `.init_array.00100`; default is a bare `.init_array`. */
static int prio_from_array_name(const char *sname, const char *base) {
    size_t n = strlen(base);
    if (strcmp(sname, base) == 0) return INIT_PRIO_DEFAULT;
    if (strncmp(sname, base, n) == 0 && sname[n] == '.' && sname[n + 1]) {
        int p = atoi(sname + n + 1);
        if (p >= 0 && p <= INIT_PRIO_DEFAULT) return p;
    }
    return INIT_PRIO_DEFAULT;
}

static void prio_fill_slots(int **prio, size_t byte_off, size_t byte_sz, int p) {
    size_t i0 = byte_off / 8;
    size_t n = byte_sz / 8;
    size_t total = i0 + n;
    if (total == 0) return;
    *prio = realloc(*prio, total * sizeof(int));
    if (!*prio) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
    for (size_t i = 0; i < n; i++)
        (*prio)[i0 + i] = p;
}

int emit_obj_read(const char *path, EmitModule *m) {
    if (target_current()->objfmt == TARGET_OBJFMT_MACHO)
        return macho_read_object(path, m);
    if (target_current()->objfmt != TARGET_OBJFMT_ELF) {
        fprintf(stderr,
                "fakecc: object reading for target '%s' is not implemented yet\n",
                target_current()->triple);
        return -1;
    }
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "fakecc: cannot open '%s'\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)fsize);
    size_t nread = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    if (nread != (size_t)fsize) {
        fprintf(stderr, "fakecc: short read on '%s'\n", path); free(buf); return -1;
    }

    /* Validate ELF header. */
    if (fsize < 64 || buf[0] != 0x7f || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F') {
        fprintf(stderr, "fakecc: '%s' is not an ELF object\n", path); free(buf); return -1;
    }
    if (rd_u16(buf + 16) != ET_REL) {
        fprintf(stderr, "fakecc: '%s' is not a relocatable object\n", path); free(buf); return -1;
    }

    uint64_t shoff = rd_u64(buf + 40);
    uint16_t shentsize = rd_u16(buf + 58);
    uint16_t shnum = rd_u16(buf + 60);
    uint16_t shstrndx = rd_u16(buf + 62);

    emit_module_init(m);

    /* Find section name string table. */
    const unsigned char *shstr_sh = buf + shoff + (size_t)shstrndx * shentsize;
    uint64_t shstr_off = rd_u64(shstr_sh + 24);
    const char *shstr = (const char *)buf + shstr_off;

    /* Map section index → section id (SECT_TEXT etc.) by name, and find
     * symtab.  Concatenate `.text*` (SHF_EXECINSTR), `.rodata*`,
     * `.data`/`.data.*` (including `.data.rel.ro`), `.bss`/`.bss.*`,
     * `.tdata`/`.tdata.*` and `.tbss`/`.tbss.*` so gcc mix objects work. */
    int symtab_idx = -1, strtab_idx = -1;
    int *td_idx = NULL;
    size_t *td_off = NULL;
    int n_td = 0, cap_td = 0;
    size_t total_td = 0;
    int *tb_idx = NULL;
    size_t *tb_off = NULL;
    int n_tb = 0, cap_tb = 0;
    size_t total_tb = 0;
    int *ia_idx = NULL;
    size_t *ia_off = NULL;
    int n_ia = 0, cap_ia = 0;
    size_t total_ia = 0;
    int *fa_idx = NULL;
    size_t *fa_off = NULL;
    int n_fa = 0, cap_fa = 0;
    size_t total_fa = 0;
    int fakecc_dbg_idx = -1;
    int *ro_idx = NULL;
    size_t *ro_off = NULL;
    int n_ro = 0, cap_ro = 0;
    size_t total_ro = 0;
    int *tx_idx = NULL;
    size_t *tx_off = NULL;
    int n_tx = 0, cap_tx = 0;
    size_t total_tx = 0;
    int *da_idx = NULL;
    size_t *da_off = NULL;
    int n_da = 0, cap_da = 0;
    size_t total_da = 0;
    int *bs_idx = NULL;
    size_t *bs_off = NULL;
    int n_bs = 0, cap_bs = 0;
    size_t total_bs = 0;
    for (int s = 0; s < shnum; s++) {
        const unsigned char *sh = buf + shoff + (size_t)s * shentsize;
        uint32_t name_idx = rd_u32(sh);
        const char *sname = shstr + name_idx;
        uint32_t type = rd_u32(sh + 4);
        uint64_t flags = rd_u64(sh + 8);
        uint64_t sh_align = rd_u64(sh + 48);
        if (type == SHT_PROGBITS && (flags & SHF_EXECINSTR)
            && !(flags & SHF_TLS)) {
            uint64_t sz = rd_u64(sh + 32);
            uint64_t align = sh_align < 1 ? 1 : sh_align;
            bump_align(&m->text_align, align);
            size_t pad = (size_t)((align - (total_tx % (size_t)align)) % (size_t)align);
            total_tx += pad;
            if (n_tx >= cap_tx) {
                cap_tx = cap_tx ? cap_tx * 2 : 4;
                tx_idx = realloc(tx_idx, (size_t)cap_tx * sizeof(int));
                tx_off = realloc(tx_off, (size_t)cap_tx * sizeof(size_t));
                if (!tx_idx || !tx_off) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
            }
            tx_idx[n_tx] = s;
            tx_off[n_tx] = total_tx;
            n_tx++;
            total_tx += (size_t)sz;
        } else if (strncmp(sname, ".rodata", 7) == 0 && type == SHT_PROGBITS) {
            uint64_t sz = rd_u64(sh + 32);
            uint64_t align = sh_align;
            if (align < 1) align = 1;
            bump_align(&m->rodata_align, align);
            size_t pad = (size_t)((align - (total_ro % (size_t)align)) % (size_t)align);
            total_ro += pad;
            if (n_ro >= cap_ro) {
                cap_ro = cap_ro ? cap_ro * 2 : 4;
                ro_idx = realloc(ro_idx, (size_t)cap_ro * sizeof(int));
                ro_off = realloc(ro_off, (size_t)cap_ro * sizeof(size_t));
                if (!ro_idx || !ro_off) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
            }
            ro_idx[n_ro] = s;
            ro_off[n_ro] = total_ro;
            n_ro++;
            total_ro += (size_t)sz;
        } else if (type == SHT_PROGBITS && !(flags & SHF_EXECINSTR)
                   && !(flags & SHF_TLS)
                   && (strcmp(sname, ".data") == 0 || strncmp(sname, ".data.", 6) == 0)) {
            uint64_t sz = rd_u64(sh + 32);
            uint64_t align = sh_align < 1 ? 1 : sh_align;
            bump_align(&m->data_align, align);
            size_t pad = (size_t)((align - (total_da % (size_t)align)) % (size_t)align);
            total_da += pad;
            if (n_da >= cap_da) {
                cap_da = cap_da ? cap_da * 2 : 4;
                da_idx = realloc(da_idx, (size_t)cap_da * sizeof(int));
                da_off = realloc(da_off, (size_t)cap_da * sizeof(size_t));
                if (!da_idx || !da_off) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
            }
            da_idx[n_da] = s;
            da_off[n_da] = total_da;
            n_da++;
            total_da += (size_t)sz;
        } else if (type == SHT_NOBITS && !(flags & SHF_TLS)
                   && (strcmp(sname, ".bss") == 0 || strncmp(sname, ".bss.", 5) == 0)) {
            uint64_t sz = rd_u64(sh + 32);
            uint64_t align = sh_align < 1 ? 1 : sh_align;
            bump_align(&m->bss_align, align);
            size_t pad = (size_t)((align - (total_bs % (size_t)align)) % (size_t)align);
            total_bs += pad;
            if (n_bs >= cap_bs) {
                cap_bs = cap_bs ? cap_bs * 2 : 4;
                bs_idx = realloc(bs_idx, (size_t)cap_bs * sizeof(int));
                bs_off = realloc(bs_off, (size_t)cap_bs * sizeof(size_t));
                if (!bs_idx || !bs_off) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
            }
            bs_idx[n_bs] = s;
            bs_off[n_bs] = total_bs;
            n_bs++;
            total_bs += (size_t)sz;
        } else if (type == SHT_PROGBITS
                   && ((flags & SHF_TLS)
                       || strcmp(sname, ".tdata") == 0
                       || strncmp(sname, ".tdata.", 7) == 0)
                   && !(flags & SHF_EXECINSTR)) {
            uint64_t sz = rd_u64(sh + 32);
            uint64_t align = sh_align < 1 ? 1 : sh_align;
            bump_align(&m->tdata_align, align);
            size_t pad = (size_t)((align - (total_td % (size_t)align)) % (size_t)align);
            total_td += pad;
            if (n_td >= cap_td) {
                cap_td = cap_td ? cap_td * 2 : 4;
                td_idx = realloc(td_idx, (size_t)cap_td * sizeof(int));
                td_off = realloc(td_off, (size_t)cap_td * sizeof(size_t));
                if (!td_idx || !td_off) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
            }
            td_idx[n_td] = s;
            td_off[n_td] = total_td;
            n_td++;
            total_td += (size_t)sz;
        } else if (type == SHT_NOBITS
                   && ((flags & SHF_TLS)
                       || strcmp(sname, ".tbss") == 0
                       || strncmp(sname, ".tbss.", 6) == 0)) {
            uint64_t sz = rd_u64(sh + 32);
            uint64_t align = sh_align < 1 ? 1 : sh_align;
            bump_align(&m->tbss_align, align);
            size_t pad = (size_t)((align - (total_tb % (size_t)align)) % (size_t)align);
            total_tb += pad;
            if (n_tb >= cap_tb) {
                cap_tb = cap_tb ? cap_tb * 2 : 4;
                tb_idx = realloc(tb_idx, (size_t)cap_tb * sizeof(int));
                tb_off = realloc(tb_off, (size_t)cap_tb * sizeof(size_t));
                if (!tb_idx || !tb_off) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
            }
            tb_idx[n_tb] = s;
            tb_off[n_tb] = total_tb;
            n_tb++;
            total_tb += (size_t)sz;
        } else if (type == SHT_INIT_ARRAY
                   || strcmp(sname, ".init_array") == 0
                   || strncmp(sname, ".init_array.", 12) == 0) {
            uint64_t sz = rd_u64(sh + 32);
            uint64_t align = sh_align < 1 ? 8 : sh_align;
            bump_align(&m->init_array_align, align);
            size_t pad = (size_t)((align - (total_ia % (size_t)align)) % (size_t)align);
            total_ia += pad;
            if (n_ia >= cap_ia) {
                cap_ia = cap_ia ? cap_ia * 2 : 4;
                ia_idx = realloc(ia_idx, (size_t)cap_ia * sizeof(int));
                ia_off = realloc(ia_off, (size_t)cap_ia * sizeof(size_t));
                if (!ia_idx || !ia_off) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
            }
            ia_idx[n_ia] = s;
            ia_off[n_ia] = total_ia;
            n_ia++;
            total_ia += (size_t)sz;
        } else if (type == SHT_FINI_ARRAY
                   || strcmp(sname, ".fini_array") == 0
                   || strncmp(sname, ".fini_array.", 12) == 0) {
            uint64_t sz = rd_u64(sh + 32);
            uint64_t align = sh_align < 1 ? 8 : sh_align;
            bump_align(&m->fini_array_align, align);
            size_t pad = (size_t)((align - (total_fa % (size_t)align)) % (size_t)align);
            total_fa += pad;
            if (n_fa >= cap_fa) {
                cap_fa = cap_fa ? cap_fa * 2 : 4;
                fa_idx = realloc(fa_idx, (size_t)cap_fa * sizeof(int));
                fa_off = realloc(fa_off, (size_t)cap_fa * sizeof(size_t));
                if (!fa_idx || !fa_off) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
            }
            fa_idx[n_fa] = s;
            fa_off[n_fa] = total_fa;
            n_fa++;
            total_fa += (size_t)sz;
        } else if (strcmp(sname, ".symtab") == 0 && type == SHT_SYMTAB) symtab_idx = s;
        else if (strcmp(sname, ".strtab") == 0 && type == SHT_STRTAB) strtab_idx = s;
        else if (strcmp(sname, ".fakecc_dbg") == 0 && type == SHT_PROGBITS) fakecc_dbg_idx = s;
    }

    /* String table (for symbol names). */
    const unsigned char *strtab_data = NULL;
    size_t strtab_len = 0;
    if (strtab_idx >= 0) {
        const unsigned char *sh = buf + shoff + (size_t)strtab_idx * shentsize;
        uint64_t stoff = rd_u64(sh + 24); uint64_t sz = rd_u64(sh + 32);
        strtab_data = buf + stoff;
        strtab_len = (size_t)sz;
    }

    /* Read section data. */
    if (n_tx > 0) {
        m->text.data = malloc(total_tx ? total_tx : 1);
        if (!m->text.data) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        memset(m->text.data, 0, total_tx);
        m->text.len = total_tx;
        m->text.cap = total_tx;
        for (int i = 0; i < n_tx; i++) {
            const unsigned char *sh = buf + shoff + (size_t)tx_idx[i] * shentsize;
            uint64_t off = rd_u64(sh + 24); uint64_t sz = rd_u64(sh + 32);
            if (sz > 0) memcpy(m->text.data + tx_off[i], buf + off, (size_t)sz);
        }
    }
    if (n_ro > 0) {
        m->rodata.data = malloc(total_ro ? total_ro : 1);
        if (!m->rodata.data) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        memset(m->rodata.data, 0, total_ro);
        m->rodata.len = total_ro;
        m->rodata.cap = total_ro;
        for (int i = 0; i < n_ro; i++) {
            const unsigned char *sh = buf + shoff + (size_t)ro_idx[i] * shentsize;
            uint64_t off = rd_u64(sh + 24); uint64_t sz = rd_u64(sh + 32);
            if (sz > 0) memcpy(m->rodata.data + ro_off[i], buf + off, (size_t)sz);
        }
    }
    if (n_da > 0) {
        m->data.data = malloc(total_da ? total_da : 1);
        if (!m->data.data) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        memset(m->data.data, 0, total_da);
        m->data.len = total_da;
        m->data.cap = total_da;
        for (int i = 0; i < n_da; i++) {
            const unsigned char *sh = buf + shoff + (size_t)da_idx[i] * shentsize;
            uint64_t off = rd_u64(sh + 24); uint64_t sz = rd_u64(sh + 32);
            if (sz > 0) memcpy(m->data.data + da_off[i], buf + off, (size_t)sz);
        }
    }
    if (n_bs > 0)
        m->bss_size = total_bs;
    if (n_td > 0) {
        m->tdata.data = malloc(total_td ? total_td : 1);
        if (!m->tdata.data) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        memset(m->tdata.data, 0, total_td);
        m->tdata.len = total_td;
        m->tdata.cap = total_td ? total_td : 1;
        for (int i = 0; i < n_td; i++) {
            const unsigned char *sh = buf + shoff + (size_t)td_idx[i] * shentsize;
            uint64_t off = rd_u64(sh + 24); uint64_t sz = rd_u64(sh + 32);
            if (sz > 0) memcpy(m->tdata.data + td_off[i], buf + off, (size_t)sz);
        }
    }
    if (n_tb > 0)
        m->tbss_size = total_tb;
    if (n_ia > 0) {
        m->init_array.data = malloc(total_ia ? total_ia : 1);
        if (!m->init_array.data) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        memset(m->init_array.data, 0, total_ia);
        m->init_array.len = total_ia;
        m->init_array.cap = total_ia ? total_ia : 1;
        for (int i = 0; i < n_ia; i++) {
            const unsigned char *sh = buf + shoff + (size_t)ia_idx[i] * shentsize;
            uint32_t name_idx = rd_u32(sh);
            const char *sname = shstr + name_idx;
            uint64_t off = rd_u64(sh + 24); uint64_t sz = rd_u64(sh + 32);
            if (sz > 0) memcpy(m->init_array.data + ia_off[i], buf + off, (size_t)sz);
            prio_fill_slots(&m->init_prio, ia_off[i], (size_t)sz,
                            prio_from_array_name(sname, ".init_array"));
        }
    }
    if (n_fa > 0) {
        m->fini_array.data = malloc(total_fa ? total_fa : 1);
        if (!m->fini_array.data) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        memset(m->fini_array.data, 0, total_fa);
        m->fini_array.len = total_fa;
        m->fini_array.cap = total_fa ? total_fa : 1;
        for (int i = 0; i < n_fa; i++) {
            const unsigned char *sh = buf + shoff + (size_t)fa_idx[i] * shentsize;
            uint32_t name_idx = rd_u32(sh);
            const char *sname = shstr + name_idx;
            uint64_t off = rd_u64(sh + 24); uint64_t sz = rd_u64(sh + 32);
            if (sz > 0) memcpy(m->fini_array.data + fa_off[i], buf + off, (size_t)sz);
            prio_fill_slots(&m->fini_prio, fa_off[i], (size_t)sz,
                            prio_from_array_name(sname, ".fini_array"));
        }
    }

    /* Read symbol table. */
    if (symtab_idx >= 0) {
        const unsigned char *sh = buf + shoff + (size_t)symtab_idx * shentsize;
        uint64_t off = rd_u64(sh + 24); uint64_t sz = rd_u64(sh + 32);
        uint32_t entsize = rd_u32(sh + 56);
        if (entsize == 0) entsize = ELF64_SYM_SIZE;
        size_t count = sz / entsize;
        for (size_t i = 0; i < count; i++) {
            const unsigned char *sym = buf + off + i * entsize;
            uint32_t name_idx = rd_u32(sym);
            uint8_t info = sym[4];
            uint8_t other = sym[5];
            uint16_t shndx = rd_u16(sym + 6);
            uint64_t value = rd_u64(sym + 8);
            uint64_t size = rd_u64(sym + 16);
            (void)other;
            const char *name = NULL;
            if (name_idx != 0 && strtab_data != NULL &&
                (size_t)name_idx < strtab_len) {
                name = (const char *)strtab_data + name_idx;
            }
            uint16_t mapped_shndx = shndx;
            size_t mapped_value = (size_t)value;
            int mapped = 0;
            if (shndx == SHN_COMMON) {
                mapped_shndx = SHN_COMMON;
                mapped_value = (size_t)value; /* alignment */
                mapped = 1;
            }
            for (int ri = 0; !mapped && ri < n_ro; ri++) {
                if (shndx == ro_idx[ri]) {
                    mapped_shndx = SECT_RODATA;
                    mapped_value = (size_t)value + ro_off[ri];
                    mapped = 1;
                }
            }
            for (int ti = 0; !mapped && ti < n_tx; ti++) {
                if (shndx == tx_idx[ti]) {
                    mapped_shndx = SECT_TEXT;
                    mapped_value = (size_t)value + tx_off[ti];
                    mapped = 1;
                }
            }
            for (int di = 0; !mapped && di < n_da; di++) {
                if (shndx == da_idx[di]) {
                    mapped_shndx = SECT_DATA;
                    mapped_value = (size_t)value + da_off[di];
                    mapped = 1;
                }
            }
            for (int bi = 0; !mapped && bi < n_bs; bi++) {
                if (shndx == bs_idx[bi]) {
                    mapped_shndx = SECT_BSS;
                    mapped_value = (size_t)value + bs_off[bi];
                    mapped = 1;
                }
            }
            for (int ki = 0; !mapped && ki < n_td; ki++) {
                if (shndx == td_idx[ki]) {
                    mapped_shndx = SECT_TDATA;
                    mapped_value = (size_t)value + td_off[ki];
                    mapped = 1;
                }
            }
            for (int ki = 0; !mapped && ki < n_tb; ki++) {
                if (shndx == tb_idx[ki]) {
                    mapped_shndx = SECT_TBSS;
                    mapped_value = (size_t)value + tb_off[ki];
                    mapped = 1;
                }
            }
            if (!mapped) {
                if (shndx == 0) mapped_shndx = SECT_UNDEF;
            }
            emit_module_add_symbol(m, name,
                                   (uint8_t)(info >> 4), (uint8_t)(info & 0xf),
                                   mapped_shndx, mapped_value, (size_t)size);
        }
    }

    /* Read relocations by sh_info (covers .rela.text.startup, .rela.data.rel.ro). */
    for (int s = 0; s < shnum; s++) {
        const unsigned char *sh = buf + shoff + (size_t)s * shentsize;
        if (rd_u32(sh + 4) != SHT_RELA) continue;
        uint32_t info = rd_u32(sh + 44);
        uint64_t off = rd_u64(sh + 24); uint64_t sz = rd_u64(sh + 32);
        uint32_t entsize = rd_u32(sh + 56);
        if (entsize == 0) entsize = ELF64_RELA_SIZE;
        size_t count = sz / entsize;
        int tx_i = -1, da_i = -1, ro_i = -1, td_i = -1, ia_i = -1, fa_i = -1;
        for (int ti = 0; ti < n_tx; ti++)
            if ((int)info == tx_idx[ti]) { tx_i = ti; break; }
        for (int di = 0; di < n_da; di++)
            if ((int)info == da_idx[di]) { da_i = di; break; }
        for (int ri = 0; ri < n_ro; ri++)
            if ((int)info == ro_idx[ri]) { ro_i = ri; break; }
        for (int ki = 0; ki < n_td; ki++)
            if ((int)info == td_idx[ki]) { td_i = ki; break; }
        for (int ki = 0; ki < n_ia; ki++)
            if ((int)info == ia_idx[ki]) { ia_i = ki; break; }
        for (int ki = 0; ki < n_fa; ki++)
            if ((int)info == fa_idx[ki]) { fa_i = ki; break; }
        if (tx_i < 0 && da_i < 0 && ro_i < 0 && td_i < 0 && ia_i < 0 && fa_i < 0) continue;
        for (size_t i = 0; i < count; i++) {
            const unsigned char *r = buf + off + i * entsize;
            uint64_t roff = rd_u64(r);
            uint64_t rinfo = rd_u64(r + 8);
            uint64_t raddend = rd_u64(r + 16);
            uint32_t sym = (uint32_t)(rinfo >> 32);
            uint32_t type = (uint32_t)(rinfo & 0xffffffff);
            if (tx_i >= 0) {
                emit_module_add_reloc(m, (size_t)roff + tx_off[tx_i],
                                      type, (int)sym, (int32_t)raddend);
            } else if (da_i >= 0) {
                emit_module_add_data_reloc(m, (size_t)roff + da_off[da_i],
                                           type, (int)sym, (int32_t)raddend);
                m->data_relocs[m->num_data_relocs - 1].shndx = SECT_DATA;
            } else if (td_i >= 0) {
                emit_module_add_data_reloc(m, (size_t)roff + td_off[td_i],
                                           type, (int)sym, (int32_t)raddend);
                m->data_relocs[m->num_data_relocs - 1].shndx = SECT_TDATA;
            } else if (ia_i >= 0) {
                emit_module_add_data_reloc(m, (size_t)roff + ia_off[ia_i],
                                           type, (int)sym, (int32_t)raddend);
                m->data_relocs[m->num_data_relocs - 1].shndx = SECT_INIT_ARRAY;
            } else if (fa_i >= 0) {
                emit_module_add_data_reloc(m, (size_t)roff + fa_off[fa_i],
                                           type, (int)sym, (int32_t)raddend);
                m->data_relocs[m->num_data_relocs - 1].shndx = SECT_FINI_ARRAY;
            } else {
                emit_module_add_data_reloc(m, (size_t)roff + ro_off[ro_i],
                                           type, (int)sym, (int32_t)raddend);
                m->data_relocs[m->num_data_relocs - 1].shndx = SECT_RODATA;
            }
        }
    }

    if (fakecc_dbg_idx >= 0) {
        const unsigned char *sh = buf + shoff + (size_t)fakecc_dbg_idx * shentsize;
        uint64_t off = rd_u64(sh + 24); uint64_t sz = rd_u64(sh + 32);
        if (sz > 0 && debug_deserialize(m, buf + off, (size_t)sz) != 0) {
            fprintf(stderr, "fakecc: warning: ignoring corrupt .fakecc_dbg in '%s'\n",
                    path);
        }
    }

    free(ro_idx);
    free(ro_off);
    free(tx_idx);
    free(tx_off);
    free(da_idx);
    free(da_off);
    free(bs_idx);
    free(bs_off);
    free(td_idx);
    free(td_off);
    free(tb_idx);
    free(tb_off);
    free(ia_idx);
    free(ia_off);
    free(fa_idx);
    free(fa_off);
    free(buf);
    return 0;
}
