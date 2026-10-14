#include "fakecc/emit.h"
#include "fakecc/common.h"
#include "fakecc/debug.h"
#include "fakecc/target.h"
#include "fakecc/macho.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/* ELF constants                                                       */
/* ------------------------------------------------------------------ */

#define ELFCLASS64      2
#define ELFDATA2LSB     1
#define EV_CURRENT      1
#define ELFOSABI_NONE   0
#define ET_EXEC         2
#define ET_DYN          3
#define EM_X86_64       62

#define PT_LOAD         1
#define PT_INTERP       3
#define PT_DYNAMIC      2
#define PT_TLS          7
#define PT_GNU_STACK    0x6474e551
#define PF_X            1
#define PF_W            2
#define PF_R            4

#define ELF_BASE        0x400000
#define PAGE_SIZE       0x1000

#define ELF64_EHDR_SIZE  64
#define ELF64_PHDR_SIZE  56
#define ELF64_SHDR_SIZE  64
#define ELF64_SYM_SIZE   24

#define SHT_NULL         0
#define SHT_PROGBITS     1
#define SHT_SYMTAB       2
#define SHT_STRTAB       3
#define SHT_RELA         4
#define SHT_HASH         5
#define SHT_DYNAMIC      6
#define SHT_NOBITS       8
#define SHT_DYNSYM      11
#define SHT_INIT_ARRAY  14
#define SHT_FINI_ARRAY  15
#define SHT_DYNSTR      18

#define SHF_WRITE        0x1
#define SHF_ALLOC        0x2
#define SHF_EXECINSTR    0x4
#define SHF_TLS          0x400

#define STB_LOCAL        0
#define STB_GLOBAL       1
#define STB_WEAK         2
#define SHN_COMMON       0xfff2
#define STT_SECTION      3

#define DT_NULL         0
#define DT_NEEDED       1
#define DT_SONAME       14
#define DT_STRTAB       5
#define DT_SYMTAB       6
#define DT_SYMENT       11
#define DT_STRSZ        10
#define DT_HASH         4
#define DT_PLTGOT       3
#define DT_PLTRELSZ     2
#define DT_PLTREL       20
#define DT_JMPREL       23
#define DT_RELA         7
#define DT_RELASZ       8
#define DT_RELENT       9
#define DT_RUNPATH      29
#define DT_FLAGS        30
#define DT_INIT_ARRAY   25
#define DT_FINI_ARRAY   26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DF_STATIC_TLS   0x10
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE  8
#define STT_OBJECT      1
#define STT_FUNC        2
#define STT_TLS         6

#define INIT_ARRAY_START 36 /* movabs×2 + test + jz + call/add/dec/jnz */
#define FINI_ARRAY_START 42 /* save eax + reverse walk + restore edi */
#define IREL_START       42 /* apply IRELATIVE GOT slots before ctors */

static int reloc_is_tls_gd(uint32_t t) {
    return t == R_X86_64_TLSGD;
}

static int reloc_is_tls_ld(uint32_t t) {
    return t == R_X86_64_TLSLD;
}

static int reloc_is_copy_kind(uint32_t t) {
    return t == R_X86_64_PC32 || t == R_X86_64_32 || t == R_X86_64_32S
        || t == R_X86_64_64;
}

static int reloc_is_gotpcrel(uint32_t t) {
    return t == R_X86_64_GOTPCREL
        || t == R_X86_64_GOTPCRELX
        || t == R_X86_64_REX_GOTPCRELX;
}

static int reloc_is_pc(uint32_t t) {
    return t == R_X86_64_PC32 || t == R_X86_64_PLT32 || t == R_X86_64_PC64;
}

static int reloc_is_abs32(uint32_t t) {
    return t == R_X86_64_32 || t == R_X86_64_32S;
}

static int reloc_width(uint32_t t) {
    if (t == R_X86_64_64 || t == R_X86_64_PC64) return 8;
    return 4;
}

static void pad_buf_to(Buffer *b, size_t align) {
    if (align < 1) align = 1;
    while (b->len % align) {
        char z = 0;
        buffer_append(b, &z, 1);
    }
}

static size_t pad_size_to(size_t n, size_t align) {
    if (align < 1) align = 1;
    return (n + align - 1) / align * align;
}

/* Look up NAME in a DT_NEEDED shared object.  Returns st_size (0 if missing)
 * and optionally the ELF STT_* type and copy-slot alignment. */
static size_t so_symbol_lookup(const char **lib_paths, size_t npaths,
                               const char **needed, size_t nneeded,
                               const char *name, uint8_t *out_type,
                               size_t *out_align) {
    if (out_type) *out_type = 0;
    if (out_align) *out_align = 8;
    if (!name || !name[0]) return 0;
    for (size_t ni = 0; ni < nneeded; ni++) {
        const char *soname = needed[ni];
        for (size_t d = 0; d <= npaths; d++) {
            char path[512];
            if (d < npaths)
                snprintf(path, sizeof path, "%s/%s", lib_paths[d], soname);
            else
                snprintf(path, sizeof path, "/lib64/%s", soname);
            FILE *f = fopen(path, "rb");
            if (!f) continue;
            unsigned char ehdr[64];
            if (fread(ehdr, 1, 64, f) != 64) { fclose(f); continue; }
            if (ehdr[0] != 0x7f) { fclose(f); continue; }
            uint64_t shoff = 0;
            memcpy(&shoff, ehdr + 40, 8);
            uint16_t shentsize = 0, shnum = 0, shstrndx = 0;
            memcpy(&shentsize, ehdr + 58, 2);
            memcpy(&shnum, ehdr + 60, 2);
            memcpy(&shstrndx, ehdr + 62, 2);
            if (shentsize < 64 || shnum == 0) { fclose(f); continue; }
            unsigned char *shdrs = malloc((size_t)shnum * shentsize);
            if (!shdrs) { fclose(f); continue; }
            if (fseek(f, (long)shoff, SEEK_SET) != 0
                || fread(shdrs, shentsize, shnum, f) != shnum) {
                free(shdrs); fclose(f); continue;
            }
            int dynsym = -1, dynstr = -1;
            for (int s = 0; s < shnum; s++) {
                uint32_t type = 0;
                memcpy(&type, shdrs + (size_t)s * shentsize + 4, 4);
                if (type == 11) dynsym = s;      /* SHT_DYNSYM */
                if (type == 3 && dynstr < 0) dynstr = s; /* first STRTAB; prefer link */
            }
            if (dynsym >= 0) {
                uint32_t link = 0;
                memcpy(&link, shdrs + (size_t)dynsym * shentsize + 40, 4);
                if (link > 0 && link < shnum) dynstr = (int)link;
            }
            size_t found_sz = 0;
            if (dynsym >= 0 && dynstr >= 0) {
                uint64_t symoff = 0, symsz = 0, stroff = 0, strsz = 0;
                memcpy(&symoff, shdrs + (size_t)dynsym * shentsize + 24, 8);
                memcpy(&symsz, shdrs + (size_t)dynsym * shentsize + 32, 8);
                memcpy(&stroff, shdrs + (size_t)dynstr * shentsize + 24, 8);
                memcpy(&strsz, shdrs + (size_t)dynstr * shentsize + 32, 8);
                unsigned char *syms = malloc(symsz ? symsz : 1);
                unsigned char *strs = malloc(strsz ? strsz : 1);
                if (syms && strs
                    && fseek(f, (long)symoff, SEEK_SET) == 0
                    && fread(syms, 1, (size_t)symsz, f) == (size_t)symsz
                    && fseek(f, (long)stroff, SEEK_SET) == 0
                    && fread(strs, 1, (size_t)strsz, f) == (size_t)strsz) {
                    size_t nsym = (size_t)symsz / 24;
                    int found = 0;
                    for (size_t k = 0; k < nsym; k++) {
                        uint32_t noff = 0;
                        memcpy(&noff, syms + k * 24, 4);
                        if (noff >= strsz) continue;
                        if (strcmp((char *)strs + noff, name) == 0) {
                            uint8_t info = 0;
                            memcpy(&info, syms + k * 24 + 4, 1);
                            if (out_type) *out_type = info & 0xf;
                            memcpy(&found_sz, syms + k * 24 + 16, 8);
                            uint16_t shn = 0;
                            memcpy(&shn, syms + k * 24 + 6, 2);
                            uint64_t stv = 0;
                            memcpy(&stv, syms + k * 24 + 8, 8);
                            size_t al = 1;
                            if (shn > 0 && shn < shnum) {
                                memcpy(&al, shdrs + (size_t)shn * shentsize + 48, 8);
                                if (al < 1) al = 1;
                            }
                            if (stv) {
                                size_t low = (size_t)stv & -(size_t)stv;
                                if (low > al) al = low;
                            }
                            if (al > 4096) al = 4096;
                            if (out_align) *out_align = al;
                            found = 1;
                            break;
                        }
                    }
                    if (found) {
                        free(syms); free(strs);
                        free(shdrs); fclose(f);
                        return found_sz;
                    }
                }
                free(syms); free(strs);
            }
            free(shdrs); fclose(f);
        }
    }
    return 0;
}

typedef struct { int defined; int shndx; size_t value; uint8_t binding; uint8_t type; } LinkSymInfo;

/* Prefer STB_GLOBAL, then STB_WEAK, then SHN_COMMON. */
static size_t find_export_gsi(EmitModule **mods, size_t n, const size_t *mod_sym_base,
                              const LinkSymInfo *sinfo, const char *nm) {
    size_t best_g = (size_t)-1, best_w = (size_t)-1, best_c = (size_t)-1;
    if (!nm || !nm[0]) return (size_t)-1;
    for (size_t mi = 0; mi < n; mi++) {
        EmitModule *om = mods[mi];
        for (size_t mj = 0; mj < om->num_syms; mj++) {
            size_t ogsi = mod_sym_base[mi] + mj;
            if (!sinfo[ogsi].defined || !om->syms[mj].name) continue;
            if (strcmp(om->syms[mj].name, nm) != 0) continue;
            int sh = sinfo[ogsi].shndx;
            uint8_t b = sinfo[ogsi].binding;
            if (sh == SHN_COMMON) {
                if (best_c == (size_t)-1) best_c = ogsi;
                continue;
            }
            if (b == STB_GLOBAL) {
                if (best_g == (size_t)-1) best_g = ogsi;
            } else if (b == STB_WEAK) {
                if (best_w == (size_t)-1) best_w = ogsi;
            }
        }
    }
    if (best_g != (size_t)-1) return best_g;
    if (best_w != (size_t)-1) return best_w;
    return best_c;
}

#define STB_GLOBAL      1
#define SHN_UNDEF       0
#define STT_FUNC        2

static const char INTERP_PATH[] = "/lib64/ld-linux-x86-64.so.2";

#define START_SIZE  22 /* gen_start: mov_edi(3)+lea_rsi(5)+call(5)+mov_reg(2)+mov_imm(5)+syscall(2) */
#define CALL_SIZE   5

/* ================================================================== */
/* ELF output primitives                                               */
/* ================================================================== */

static void emit_byte(Buffer *b, uint8_t v) { buffer_append(b, (const char *)&v, 1); }
static void emit_int32(Buffer *b, int32_t v) { buffer_append(b, (const char *)&v, 4); }

static void *xcalloc(size_t nmemb, size_t size) {
    void *p = calloc(nmemb, size);
    if (!p) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
    return p;
}

static void buf_u8(Buffer *b, uint8_t v) { buffer_append(b, (const char *)&v, 1); }
static void buf_pad(Buffer *b, size_t n) { while (n--) buf_u8(b, 0); }
static void buf_bytes(Buffer *b, const char *d, size_t n) { buffer_append(b, d, n); }
static void buf_u16(Buffer *b, uint16_t v) { buffer_append(b, (const char *)&v, 2); }
static void buf_u32(Buffer *b, uint32_t v) { buffer_append(b, (const char *)&v, 4); }
static void buf_u64(Buffer *b, uint64_t v) { buffer_append(b, (const char *)&v, 8); }

static void write_ehdr(Buffer *b, uint16_t e_type, uint64_t entry, uint64_t phoff,
                       uint16_t phnum) {
    buffer_append(b, "\x7f" "ELF", 4);
    char ident[12];
    memset(ident, 0, 12);
    ident[0] = ELFCLASS64;
    ident[1] = ELFDATA2LSB;
    ident[2] = EV_CURRENT;
    ident[3] = ELFOSABI_NONE;
    buffer_append(b, ident, 12);
    buf_u16(b, e_type);
    buf_u16(b, EM_X86_64);
    buf_u32(b, EV_CURRENT);
    buf_u64(b, entry);
    buf_u64(b, phoff);
    buf_u64(b, 0); /* e_shoff */
    buf_u32(b, 0); /* e_flags */
    buf_u16(b, ELF64_EHDR_SIZE);
    buf_u16(b, ELF64_PHDR_SIZE);
    buf_u16(b, phnum);
    buf_u16(b, 64);  /* e_shentsize */
    buf_u16(b, 0);   /* e_shnum */
    buf_u16(b, 0);   /* e_shstrndx */
}

static void write_phdr(Buffer *b, uint32_t type, uint32_t flags,
                       uint64_t offset, uint64_t vaddr,
                       uint64_t filesz, uint64_t memsz,
                       uint64_t align) {
    buf_u32(b, type);
    buf_u32(b, flags);
    buf_u64(b, offset);
    buf_u64(b, vaddr);
    buf_u64(b, vaddr); /* paddr */
    buf_u64(b, filesz);
    buf_u64(b, memsz);
    buf_u64(b, align);
}

static void write_shdr_exec(Buffer *b, uint32_t name, uint32_t type,
                            uint64_t flags, uint64_t addr, uint64_t offset,
                            uint64_t size, uint32_t link, uint32_t info,
                            uint64_t addralign, uint64_t entsize) {
    buf_u32(b, name);
    buf_u32(b, type);
    buf_u64(b, flags);
    buf_u64(b, addr);
    buf_u64(b, offset);
    buf_u64(b, size);
    buf_u32(b, link);
    buf_u32(b, info);
    buf_u64(b, addralign);
    buf_u64(b, entsize);
}

static uint32_t append_string(Buffer *b, const char *s) {
    uint32_t off = (uint32_t)b->len;
    buf_bytes(b, s, strlen(s) + 1);
    return off;
}

static void write_sym(Buffer *b, uint32_t name, uint8_t binding, uint8_t type,
                      uint16_t shndx, uint64_t value, uint64_t size) {
    buf_u32(b, name);
    buf_u8(b, (uint8_t)((binding << 4) | (type & 0xf)));
    buf_u8(b, 0);
    buf_u16(b, shndx);
    buf_u64(b, value);
    buf_u64(b, size);
}

/*
 * Add the non-ALLOC metadata after all PT_LOAD file content.  The first four
 * section headers point back into those segments; every other section lives
 * after them and therefore cannot enlarge a load segment's p_filesz.
 */
/* Where each output section landed, both in the file and in memory.  Passed
 * as one struct so finalize_sections stays within the dialect's argument
 * limit (and so callers can't transpose two same-typed arguments). */
typedef struct {
    uint64_t code_vaddr;
    uint64_t data_vaddr;
    uint64_t bss_vaddr;
    size_t   text_offset;
    size_t   data_file_offset;
    size_t   text_len;
    size_t   rodata_len;
    size_t   data_len;
    size_t   bss_file_offset;
    size_t   bss_size;
    size_t   start_size;
    /* Thread-local storage template.  `tls_vaddr` is the start of the TLS
     * image in memory (after .bss, page-aligned), `tls_file_offset` is where
     * the initialized portion (.tdata) lives on disk (after .bss filesize
     * in the RW segment), `tls_filesize` is the on-disk size (== tdata
     * size; .tbss contributes memsz only), `tls_memsize` is tdata + tbss
     * size (full template length).  have_tls gates emission of the .tdata
     * / .tbss section headers and PT_TLS program header. */
    int      have_tls;
    int      is_shared;
    uint64_t tls_vaddr;
    size_t   tls_file_offset;
    size_t   tls_filesize;
    size_t   tls_memsize;
    /* Dynamic linking sections (valid only when the output is dynamically
     * linked).  File offsets point into the RX segment where the linker
     * appends .dynstr/.dynsym/.hash/.rela.plt/.rela.dyn/.dynamic; vaddrs are
     * their runtime addresses.  finalize_sections uses these to emit the
     * matching section headers so readelf/objdump can locate them. */
    int      have_dynamic;
    size_t   dynstr_off, dynsym_off, hash_off;
    size_t   rela_plt_off, rela_dyn_off, dynamic_off;
    size_t   dynstr_size, dynsym_size, hash_size;
    size_t   rela_plt_size, rela_dyn_size, dynamic_size;
    uint64_t dynstr_vaddr, dynsym_vaddr, hash_vaddr;
    uint64_t rela_plt_vaddr, rela_dyn_vaddr, dynamic_vaddr;
    uint64_t initarr_vaddr;
    uint64_t finiarr_vaddr;
    size_t   initarr_file_offset;
    size_t   initarr_size;
    size_t   finiarr_file_offset;
    size_t   finiarr_size;
    int      have_initarr;
    int      have_finiarr;
} SectionLayout;

static void finalize_sections(
    Buffer *elf, EmitModule **mods, size_t n,
    const size_t *mod_text_off, const size_t *mod_sym_base,
    const size_t *sym_addr, const SectionLayout *lay,
    uint64_t entry, int want_debug) {
    uint64_t code_vaddr = lay->code_vaddr;
    uint64_t data_vaddr = lay->data_vaddr;
    uint64_t bss_vaddr = lay->bss_vaddr;
    size_t text_offset = lay->text_offset;
    size_t data_file_offset = lay->data_file_offset;
    size_t text_len = lay->text_len;
    size_t rodata_len = lay->rodata_len;
    size_t data_len = lay->data_len;
    size_t bss_file_offset = lay->bss_file_offset;
    size_t bss_size = lay->bss_size;
    Buffer symtab, strtab, shstrtab;
    Buffer debug_abbrev, debug_info, debug_str, debug_line, debug_frame;
    Buffer debug_loc;
    buffer_init(&symtab); buffer_init(&strtab); buffer_init(&shstrtab);
    buffer_init(&debug_abbrev); buffer_init(&debug_info);
    buffer_init(&debug_str); buffer_init(&debug_line);
    buffer_init(&debug_frame); buffer_init(&debug_loc);

    buf_u8(&strtab, 0);
    buf_pad(&symtab, ELF64_SYM_SIZE);

    /* ELF requires all locals to precede the first global symbol. */
    for (size_t i = 0; i < n; i++) {
        EmitModule *m = mods[i];
        for (size_t j = 0; j < m->num_syms; j++) {
            const EmitSymbol *s = &m->syms[j];
            if (!s->name || s->type == STT_SECTION ||
                s->shndx == SECT_UNDEF || s->binding != STB_LOCAL)
                continue;
            uint32_t name = append_string(&strtab, s->name);
            write_sym(&symtab, name, s->binding, s->type, s->shndx,
                      sym_addr[mod_sym_base[i] + j], s->size);
        }
    }
    uint32_t first_global = (uint32_t)(symtab.len / ELF64_SYM_SIZE);
    if (!lay->is_shared) {
        uint32_t start_name = append_string(&strtab, "_start");
        write_sym(&symtab, start_name, STB_GLOBAL, 2, SECT_TEXT,
                  entry, lay->start_size);
    }
    for (size_t i = 0; i < n; i++) {
        EmitModule *m = mods[i];
        for (size_t j = 0; j < m->num_syms; j++) {
            const EmitSymbol *s = &m->syms[j];
            if (!s->name || s->type == STT_SECTION ||
                s->shndx == SECT_UNDEF || s->binding == STB_LOCAL)
                continue;
            uint32_t name = append_string(&strtab, s->name);
            write_sym(&symtab, name, s->binding, s->type, s->shndx,
                      sym_addr[mod_sym_base[i] + j], s->size);
        }
    }

    EmitModule dbg;
    int have_dbg = want_debug;
    if (have_dbg) {
        emit_module_init(&dbg);
        dbg.text.len = text_len;
        for (size_t i = 0; i < n; i++) {
            EmitModule *m = mods[i];
            if (!dbg.dbg_tu_name && m->dbg_tu_name)
                dbg.dbg_tu_name = xstrdup(m->dbg_tu_name);
            for (size_t j = 0; j < m->num_dbg_lines; j++) {
                const DebugLineEntry *line = &m->dbg_lines[j];
                emit_module_add_dbg_line(&dbg, line->file, line->line,
                                         line->col,
                                         mod_text_off[i] + line->pc_off);
            }
            for (size_t j = 0; j < m->num_dbg_funcs; j++) {
                const DebugFunc *f = &m->dbg_funcs[j];
                int fi = emit_module_add_dbg_func(
                    &dbg, f->name, f->file, f->line,
                    mod_text_off[i] + f->start_pc);
                emit_module_dbg_func_end(
                    &dbg, fi, mod_text_off[i] + f->end_pc,
                    mod_text_off[i] + f->prologue_end_pc);
                for (size_t k = 0; k < f->num_vars; k++) {
                    emit_module_add_dbg_var(&dbg, fi, &f->vars[k]);
                    /* Location-list ranges are offsets into the module's own
                     * .text; rebase them onto the linked image. */
                    DebugVar *dv = &dbg.dbg_funcs[fi].vars[
                        dbg.dbg_funcs[fi].num_vars - 1];
                    for (size_t r = 0; r < dv->num_ranges; r++) {
                        dv->ranges[r].pc_start += mod_text_off[i];
                        dv->ranges[r].pc_end += mod_text_off[i];
                    }
                }
                for (size_t k = 0; k < f->num_call_sites; k++) {
                    DebugCallSite cs = f->call_sites[k];
                    /* Shallow fields; add_dbg_call_site deep-copies. */
                    cs.call_pc += mod_text_off[i];
                    cs.return_pc += mod_text_off[i];
                    emit_module_add_dbg_call_site(&dbg, fi, &cs);
                }
            }
            for (size_t j = 0; j < m->num_dbg_globals; j++)
                emit_module_add_dbg_global(&dbg, &m->dbg_globals[j]);
            for (size_t j = 0; j < m->num_syms; j++) {
                const EmitSymbol *s = &m->syms[j];
                if (!s->name || s->shndx == SECT_UNDEF) continue;
                emit_module_add_symbol(
                    &dbg, s->name, s->binding, s->type, s->shndx,
                    sym_addr[mod_sym_base[i] + j], s->size);
            }
        }
        debug_emit_dwarf(&dbg, code_vaddr, &debug_abbrev, &debug_info,
                         &debug_str, &debug_line, &debug_frame, &debug_loc);
    }

    buf_u8(&shstrtab, 0);
    uint32_t shname_text = append_string(&shstrtab, ".text");
    uint32_t shname_rodata = append_string(&shstrtab, ".rodata");
    uint32_t shname_data = append_string(&shstrtab, ".data");
    uint32_t shname_bss = append_string(&shstrtab, ".bss");
    uint32_t shname_tdata = 0, shname_tbss = 0;
    if (lay->have_tls) {
        shname_tdata = append_string(&shstrtab, ".tdata");
        shname_tbss = append_string(&shstrtab, ".tbss");
    }
    uint32_t shname_init_array = 0;
    if (lay->have_initarr)
        shname_init_array = append_string(&shstrtab, ".init_array");
    uint32_t shname_fini_array = 0;
    if (lay->have_finiarr)
        shname_fini_array = append_string(&shstrtab, ".fini_array");
    uint32_t shname_symtab = append_string(&shstrtab, ".symtab");
    uint32_t shname_strtab = append_string(&shstrtab, ".strtab");
    uint32_t shname_shstrtab = append_string(&shstrtab, ".shstrtab");
    uint32_t shname_debug_abbrev = 0, shname_debug_info = 0;
    uint32_t shname_debug_str = 0, shname_debug_line = 0;
    uint32_t shname_debug_frame = 0, shname_debug_loc = 0;
    if (have_dbg) {
        shname_debug_abbrev = append_string(&shstrtab, ".debug_abbrev");
        shname_debug_info = append_string(&shstrtab, ".debug_info");
        shname_debug_str = append_string(&shstrtab, ".debug_str");
        shname_debug_line = append_string(&shstrtab, ".debug_line");
        shname_debug_frame = append_string(&shstrtab, ".debug_frame");
        shname_debug_loc = append_string(&shstrtab, ".debug_loc");
    }
    /* Dynamic section names.  Only emitted when the output links dynamic
     * sections; their indices follow the debug blocks (when present) so the
     * DWARF section indices that debug.c bakes into .debug_info stay valid. */
    uint32_t shname_dynstr = 0, shname_dynsym = 0, shname_hash = 0;
    uint32_t shname_rela_plt = 0, shname_rela_dyn = 0, shname_dynamic = 0;
    if (lay->have_dynamic) {
        shname_dynstr = append_string(&shstrtab, ".dynstr");
        shname_dynsym = append_string(&shstrtab, ".dynsym");
        shname_hash = append_string(&shstrtab, ".hash");
        shname_rela_plt = append_string(&shstrtab, ".rela.plt");
        shname_rela_dyn = append_string(&shstrtab, ".rela.dyn");
        shname_dynamic = append_string(&shstrtab, ".dynamic");
    }

    while (elf->len & 7) buf_u8(elf, 0);
    size_t off_symtab = elf->len;
    buf_bytes(elf, symtab.data, symtab.len);
    size_t off_strtab = elf->len;
    buf_bytes(elf, strtab.data, strtab.len);
    size_t off_shstrtab = elf->len;
    buf_bytes(elf, shstrtab.data, shstrtab.len);
    size_t off_debug_abbrev = elf->len;
    if (have_dbg) buf_bytes(elf, debug_abbrev.data, debug_abbrev.len);
    size_t off_debug_info = elf->len;
    if (have_dbg) buf_bytes(elf, debug_info.data, debug_info.len);
    size_t off_debug_str = elf->len;
    if (have_dbg) buf_bytes(elf, debug_str.data, debug_str.len);
    size_t off_debug_line = elf->len;
    if (have_dbg) buf_bytes(elf, debug_line.data, debug_line.len);
    if (have_dbg) while (elf->len & 7) buf_u8(elf, 0);
    size_t off_debug_frame = elf->len;
    if (have_dbg) buf_bytes(elf, debug_frame.data, debug_frame.len);
    if (have_dbg) while (elf->len & 7) buf_u8(elf, 0);
    size_t off_debug_loc = elf->len;
    if (have_dbg) buf_bytes(elf, debug_loc.data, debug_loc.len);

    while (elf->len & 7) buf_u8(elf, 0);
    uint64_t shoff = elf->len;
    buf_pad(elf, ELF64_SHDR_SIZE);
    write_shdr_exec(elf, shname_text, SHT_PROGBITS,
                    SHF_ALLOC | SHF_EXECINSTR, code_vaddr, text_offset,
                    text_len, 0, 0, 16, 0);
    write_shdr_exec(elf, shname_rodata, SHT_PROGBITS, SHF_ALLOC,
                    code_vaddr + text_len, text_offset + text_len,
                    rodata_len, 0, 0, 8, 0);
    write_shdr_exec(elf, shname_data, SHT_PROGBITS, SHF_ALLOC | SHF_WRITE,
                    data_vaddr, data_file_offset, data_len, 0, 0, 8, 0);
    write_shdr_exec(elf, shname_bss, SHT_NOBITS, SHF_ALLOC | SHF_WRITE,
                    bss_vaddr, bss_file_offset,
                    bss_size, 0, 0, 8, 0);
    if (lay->have_tls) {
        /* .tdata: file-resident initialized __thread variables.
         * .tbss: zero-init __thread variables (SHT_NOBITS).  Both are
         * SHF_TLS — the loader uses this flag together with PT_TLS to
         * allocate the per-thread template. */
        if (lay->tls_filesize > 0) {
            write_shdr_exec(elf, shname_tdata, SHT_PROGBITS,
                            SHF_ALLOC | SHF_TLS,
                            lay->tls_vaddr, lay->tls_file_offset,
                            lay->tls_filesize, 0, 0, 8, 0);
        }
        if (lay->tls_memsize > lay->tls_filesize) {
            size_t tbss_bytes = lay->tls_memsize - lay->tls_filesize;
            write_shdr_exec(elf, shname_tbss, SHT_NOBITS,
                            SHF_ALLOC | SHF_TLS,
                            lay->tls_vaddr + lay->tls_filesize,
                            lay->tls_file_offset + lay->tls_filesize,
                            tbss_bytes, 0, 0, 8, 0);
        }
    }
    int array_sections = 0;
    if (lay->have_initarr && lay->initarr_size > 0) {
        write_shdr_exec(elf, shname_init_array, SHT_INIT_ARRAY,
                        SHF_ALLOC | SHF_WRITE,
                        lay->initarr_vaddr, lay->initarr_file_offset,
                        lay->initarr_size, 0, 0, 8, 8);
        array_sections++;
    }
    if (lay->have_finiarr && lay->finiarr_size > 0) {
        write_shdr_exec(elf, shname_fini_array, SHT_FINI_ARRAY,
                        SHF_ALLOC | SHF_WRITE,
                        lay->finiarr_vaddr, lay->finiarr_file_offset,
                        lay->finiarr_size, 0, 0, 8, 8);
        array_sections++;
    }
    /* Count TLS sections actually emitted — only those with non-zero size
     * take a slot, so .tdata-only / .tbss-only / both combinations all
     * produce the right shnum and downstream section indices. */
    int tls_sections = 0;
    if (lay->have_tls) {
        if (lay->tls_filesize > 0) tls_sections++;
        if (lay->tls_memsize > lay->tls_filesize) tls_sections++;
    }
    write_shdr_exec(elf, shname_symtab, SHT_SYMTAB, 0, 0, off_symtab,
                    symtab.len, 6 + tls_sections + array_sections, first_global, 8, ELF64_SYM_SIZE);
    write_shdr_exec(elf, shname_strtab, SHT_STRTAB, 0, 0, off_strtab,
                    strtab.len, 0, 0, 1, 0);
    write_shdr_exec(elf, shname_shstrtab, SHT_STRTAB, 0, 0, off_shstrtab,
                    shstrtab.len, 0, 0, 1, 0);
    if (have_dbg) {
        write_shdr_exec(elf, shname_debug_abbrev, SHT_PROGBITS, 0, 0,
                        off_debug_abbrev, debug_abbrev.len, 0, 0, 1, 0);
        write_shdr_exec(elf, shname_debug_info, SHT_PROGBITS, 0, 0,
                        off_debug_info, debug_info.len, 0, 0, 1, 0);
        write_shdr_exec(elf, shname_debug_str, SHT_PROGBITS, 0, 0,
                        off_debug_str, debug_str.len, 0, 0, 1, 0);
        write_shdr_exec(elf, shname_debug_line, SHT_PROGBITS, 0, 0,
                        off_debug_line, debug_line.len, 0, 0, 1, 0);
        write_shdr_exec(elf, shname_debug_frame, SHT_PROGBITS, 0, 0,
                        off_debug_frame, debug_frame.len, 0, 0, 8, 0);
        write_shdr_exec(elf, shname_debug_loc, SHT_PROGBITS, 0, 0,
                        off_debug_loc, debug_loc.len, 0, 0, 1, 0);
    }
    /* Dynamic sections follow the debug blocks (when present) so the DWARF
     * section indices baked into .debug_info stay valid.  Section index of
     * the first dynamic section: 8 (no dbg) or 14 (with dbg).  When TLS is
     * present, .tdata and .tbss (only those that exist) take indices 8 and
     * 9 (or 14 and 15 with debug), shifting dyn_base accordingly. */
    int dyn_base = (have_dbg ? 14 : 8) + tls_sections + array_sections;
    if (lay->have_dynamic) {
        /* .dynstr is a string table: use SHT_STRTAB (not SHT_DYNSTR).  readelf
         * resolves DT_NEEDED strings by locating the section whose sh_type is
         * SHT_STRTAB and whose addr range covers DT_STRTAB — a DYNSTR-typed
         * section is invisible to that lookup and the SONAME prints raw. */
        write_shdr_exec(elf, shname_dynstr, SHT_STRTAB, SHF_ALLOC,
                        lay->dynstr_vaddr, lay->dynstr_off, lay->dynstr_size,
                        0, 0, 1, 0);
        /* .dynsym: link -> .dynstr (its string table); info -> first global
         * symbol (index 1, right after the mandatory NULL symbol). */
        write_shdr_exec(elf, shname_dynsym, SHT_DYNSYM, SHF_ALLOC,
                        lay->dynsym_vaddr, lay->dynsym_off, lay->dynsym_size,
                        dyn_base + 0, 1, 8, 24);
        write_shdr_exec(elf, shname_hash, SHT_HASH, SHF_ALLOC,
                        lay->hash_vaddr, lay->hash_off, lay->hash_size,
                        dyn_base + 1, 0, 4, 4);
        /* .rela.plt: applies to the PLT (part of .text, index 1). */
        write_shdr_exec(elf, shname_rela_plt, SHT_RELA, SHF_ALLOC,
                        lay->rela_plt_vaddr, lay->rela_plt_off,
                        lay->rela_plt_size, dyn_base + 1, 1, 8, 24);
        /* .rela.dyn: applies to the GOT-in-.data (index 3). */
        write_shdr_exec(elf, shname_rela_dyn, SHT_RELA, SHF_ALLOC,
                        lay->rela_dyn_vaddr, lay->rela_dyn_off,
                        lay->rela_dyn_size, dyn_base + 1, 3, 8, 24);
        /* .dynamic: link -> .dynstr. */
        write_shdr_exec(elf, shname_dynamic, SHT_DYNAMIC,
                        SHF_ALLOC | SHF_WRITE, lay->dynamic_vaddr,
                        lay->dynamic_off, lay->dynamic_size,
                        dyn_base + 0, 0, 8, 16);
    }

    uint16_t shnum = (uint16_t)(8 + (have_dbg ? 6 : 0) + tls_sections + array_sections
                               + (lay->have_dynamic ? 6 : 0));
    uint16_t shstrndx = (uint16_t)(7 + tls_sections + array_sections);
    memcpy(elf->data + 40, &shoff, sizeof(shoff));
    memcpy(elf->data + 60, &shnum, sizeof(shnum));
    memcpy(elf->data + 62, &shstrndx, sizeof(shstrndx));

    if (have_dbg) {
        dbg.text.len = 0;
        emit_module_free(&dbg);
    }
    buffer_free(&symtab); buffer_free(&strtab); buffer_free(&shstrtab);
    buffer_free(&debug_abbrev); buffer_free(&debug_info);
    buffer_free(&debug_str); buffer_free(&debug_line);
    buffer_free(&debug_frame); buffer_free(&debug_loc);
}

/* Emit the process entry stub.  `exit_plt_vaddr` is the address of the
 * `exit` PLT entry, or 0 for a static executable (no libc to call).
 *
 * Returning from main is not the same as calling exit(): libc buffers stdout,
 * and the buffer is drained by an atexit handler that only exit() runs.  A
 * raw exit_group syscall skips it, so a dynamically linked program that
 * printf()s and then returns from main writes nothing at all.  When libc is
 * present, hand control to its exit(); keep the syscall for static binaries.
 *
 * Both forms are exactly START_SIZE bytes so the layout above does not have to
 * know which one it gets. */
typedef struct {
    uint64_t call_vaddr;
    uint64_t main_vaddr;
    uint64_t exit_plt_vaddr;
    uint64_t tls_vaddr;
    uint64_t tcb_vaddr;
    uint64_t tls_memsize;
    uint64_t tdata_len;
    uint64_t init_start;
    uint64_t init_count;
    uint64_t fini_start;
    uint64_t fini_count;
    uint64_t irel_got;
    uint64_t irel_count;
    int have_tls;
} StartInfo;

static void gen_start(Buffer *code, const StartInfo *st) {
    uint64_t call_vaddr = st->call_vaddr;
    uint64_t main_vaddr = st->main_vaddr;
    uint64_t exit_plt_vaddr = st->exit_plt_vaddr;
    int have_tls = st->have_tls;
    uint64_t tls_vaddr = st->tls_vaddr;
    uint64_t tcb_vaddr = st->tcb_vaddr;
    size_t tls_memsize = st->tls_memsize;
    size_t tdata_len = st->tdata_len;
    uint64_t init_start = st->init_start;
    uint64_t init_count = st->init_count;
    uint64_t fini_start = st->fini_start;
    uint64_t fini_count = st->fini_count;
    uint64_t irel_got = st->irel_got;
    uint64_t irel_count = st->irel_count;
    size_t prefix_len = 0;
    if (have_tls) {
        if (tdata_len > 0) {
            prefix_len += 27;
            /* 1. movabs $tls_vaddr, %rsi (48 be <8-byte-imm>) */
            uint8_t m_rsi[2] = {0x48, 0xbe};
            buffer_append(code, (const char *)m_rsi, 2);
            buffer_append(code, (const char *)&tls_vaddr, 8);
            /* 2. movabs $(tcb_vaddr - tls_memsize), %rdi (48 bf <8-byte-imm>) */
            uint64_t init_tls_addr = tcb_vaddr - tls_memsize;
            uint8_t m_rdi[2] = {0x48, 0xbf};
            buffer_append(code, (const char *)m_rdi, 2);
            buffer_append(code, (const char *)&init_tls_addr, 8);
            /* 3. mov $tdata_len, %ecx (b9 <4-byte-imm>) */
            uint8_t m_ecx[1] = {0xb9};
            uint32_t td_len32 = (uint32_t)tdata_len;
            buffer_append(code, (const char *)m_ecx, 1);
            buffer_append(code, (const char *)&td_len32, 4);
            /* 4. rep movsb (f3 a4) */
            uint8_t rep_movsb[2] = {0xf3, 0xa4};
            buffer_append(code, (const char *)rep_movsb, 2);
        }
        prefix_len += 25;
        /* 5. movabs $tcb_vaddr, %rsi (48 be <8-byte-imm>) */
        uint8_t m_rsi[2] = {0x48, 0xbe};
        buffer_append(code, (const char *)m_rsi, 2);
        buffer_append(code, (const char *)&tcb_vaddr, 8);
        /* 6. mov %rsi, (%rsi) (48 89 36) */
        uint8_t st_rsi[3] = {0x48, 0x89, 0x36};
        buffer_append(code, (const char *)st_rsi, 3);
        /* 7. mov $158, %eax (b8 9e 00 00 00) */
        uint8_t m_eax[5] = {0xb8, 0x9e, 0x00, 0x00, 0x00};
        buffer_append(code, (const char *)m_eax, 5);
        /* 8. mov $0x1002, %edi (bf 02 10 00 00) */
        uint8_t m_edi[5] = {0xbf, 0x02, 0x10, 0x00, 0x00};
        buffer_append(code, (const char *)m_edi, 5);
        /* 9. syscall (0f 05) -- arch_prctl(0x1002, tcb_vaddr) */
        uint8_t sysc[2] = {0x0f, 0x05};
        buffer_append(code, (const char *)sysc, 2);
    }
    /* Static binaries have no ld.so to apply IRELATIVE; call each resolver
     * and store the implementation pointer into the GOT slot (pre-filled
     * with the resolver address).  Exactly IREL_START bytes. */
    if (irel_count > 0) {
        prefix_len += IREL_START;
        uint8_t m_rbx[2] = {0x48, 0xbb}; /* movabs $irel_got, %rbx */
        buffer_append(code, (const char *)m_rbx, 2);
        buffer_append(code, (const char *)&irel_got, 8);
        uint8_t m_r12[2] = {0x49, 0xbc}; /* movabs $irel_count, %r12 */
        buffer_append(code, (const char *)m_r12, 2);
        buffer_append(code, (const char *)&irel_count, 8);
        uint8_t test_r12[3] = {0x4d, 0x85, 0xe4};
        buffer_append(code, (const char *)test_r12, 3);
        uint8_t jz[2] = {0x74, 0x11}; /* skip 17-byte body */
        buffer_append(code, (const char *)jz, 2);
        uint8_t ld_rax[3] = {0x48, 0x8b, 0x03}; /* mov (%rbx), %rax */
        buffer_append(code, (const char *)ld_rax, 3);
        uint8_t call_rax[2] = {0xff, 0xd0}; /* call *%rax */
        buffer_append(code, (const char *)call_rax, 2);
        uint8_t st_rax[3] = {0x48, 0x89, 0x03}; /* mov %rax, (%rbx) */
        buffer_append(code, (const char *)st_rax, 3);
        uint8_t add_rbx[4] = {0x48, 0x83, 0xc3, 0x08};
        buffer_append(code, (const char *)add_rbx, 4);
        uint8_t dec_r12[3] = {0x49, 0xff, 0xcc};
        buffer_append(code, (const char *)dec_r12, 3);
        uint8_t jnz[2] = {0x75, 0xef}; /* jnz: 17-byte body back */
        buffer_append(code, (const char *)jnz, 2);
    }
    /* Static executables have no ld.so to walk DT_INIT_ARRAY, so _start
     * calls each constructor pointer before main.  rbx/r12 are callee-saved
     * so constructors preserve the walk state.  Exactly INIT_ARRAY_START
     * bytes so start_size stays a compile-time layout constant. */
    if (init_count > 0) {
        prefix_len += INIT_ARRAY_START;
        uint8_t m_rbx[2] = {0x48, 0xbb}; /* movabs $init_start, %rbx */
        buffer_append(code, (const char *)m_rbx, 2);
        buffer_append(code, (const char *)&init_start, 8);
        uint8_t m_r12[2] = {0x49, 0xbc}; /* movabs $init_count, %r12 */
        buffer_append(code, (const char *)m_r12, 2);
        buffer_append(code, (const char *)&init_count, 8);
        uint8_t test_r12[3] = {0x4d, 0x85, 0xe4}; /* test %r12, %r12 */
        buffer_append(code, (const char *)test_r12, 3);
        uint8_t jz[2] = {0x74, 0x0b}; /* jz 1f  (skip 11-byte loop body) */
        buffer_append(code, (const char *)jz, 2);
        uint8_t call_ind[2] = {0xff, 0x13}; /* call *(%rbx) */
        buffer_append(code, (const char *)call_ind, 2);
        uint8_t add_rbx[4] = {0x48, 0x83, 0xc3, 0x08}; /* add $8, %rbx */
        buffer_append(code, (const char *)add_rbx, 4);
        uint8_t dec_r12[3] = {0x49, 0xff, 0xcc}; /* dec %r12 */
        buffer_append(code, (const char *)dec_r12, 3);
        uint8_t jnz[2] = {0x75, 0xf5}; /* jnz loop */
        buffer_append(code, (const char *)jnz, 2);
    }
    /* SysV ABI: main(argc @ edi, argv @ rsi). At process entry the kernel
     * leaves [rsp]=argc, [rsp+8]=argv. Load them before calling main. */
    uint8_t mov_edi[] = {0x8b, 0x3c, 0x24}; /* mov edi, [rsp] */
    buffer_append(code, (const char *)mov_edi, 3);
    uint8_t lea_rsi[] = {0x48, 0x8d, 0x74, 0x24, 0x08}; /* lea rsi, [rsp+8] */
    buffer_append(code, (const char *)lea_rsi, 5);
    /* call main (rel32 is relative to end of the 5-byte call, i.e. call_vaddr+prefix_len+8+5) */
    uint8_t call_opcode = 0xe8;
    buffer_append(code, (const char *)&call_opcode, 1);
    int32_t rel = (int32_t)(main_vaddr - (call_vaddr + prefix_len + 3 + 5 + CALL_SIZE));
    buffer_append(code, (const char *)&rel, 4);
    if (fini_count > 0) {
        uint8_t save[3] = {0x41, 0x89, 0xc5}; /* mov %eax, %r13d */
        buffer_append(code, (const char *)save, 3);
        uint64_t last = fini_start + (fini_count - 1) * 8;
        uint8_t m_rbx[2] = {0x48, 0xbb};
        buffer_append(code, (const char *)m_rbx, 2);
        buffer_append(code, (const char *)&last, 8);
        uint8_t m_r12[2] = {0x49, 0xbc};
        buffer_append(code, (const char *)m_r12, 2);
        buffer_append(code, (const char *)&fini_count, 8);
        uint8_t test_r12[3] = {0x4d, 0x85, 0xe4};
        buffer_append(code, (const char *)test_r12, 3);
        uint8_t jz[2] = {0x74, 0x0b};
        buffer_append(code, (const char *)jz, 2);
        uint8_t call_ind[2] = {0xff, 0x13};
        buffer_append(code, (const char *)call_ind, 2);
        uint8_t sub_rbx[4] = {0x48, 0x83, 0xeb, 0x08};
        buffer_append(code, (const char *)sub_rbx, 4);
        uint8_t dec_r12[3] = {0x49, 0xff, 0xcc};
        buffer_append(code, (const char *)dec_r12, 3);
        uint8_t jnz[2] = {0x75, 0xf5};
        buffer_append(code, (const char *)jnz, 2);
        uint8_t restore[3] = {0x44, 0x89, 0xef}; /* mov %r13d, %edi */
        buffer_append(code, (const char *)restore, 3);
    } else {
        uint8_t mov_reg[] = {0x89, 0xc7}; /* mov edi, eax */
        buffer_append(code, (const char *)mov_reg, 2);
    }
    if (exit_plt_vaddr != 0) {
        buffer_append(code, (const char *)&call_opcode, 1);
        size_t after_main = prefix_len + 3 + 5 + CALL_SIZE
            + (fini_count > 0 ? (size_t)FINI_ARRAY_START : 2);
        int32_t erel = (int32_t)(exit_plt_vaddr -
                                 (call_vaddr + after_main + CALL_SIZE));
        buffer_append(code, (const char *)&erel, 4);
        uint8_t ud2[] = {0x0f, 0x0b}; /* exit() does not return */
        buffer_append(code, (const char *)ud2, 2);
    } else {
        uint8_t mov_imm[] = {0xb8, 0x3c, 0x00, 0x00, 0x00}; /* mov eax, 60 */
        buffer_append(code, (const char *)mov_imm, 5);
        uint8_t syscall[] = {0x0f, 0x05};
        buffer_append(code, (const char *)syscall, 2);
    }
}

static unsigned long elf_hash(const char *name) {
    unsigned long h = 0, g;
    while (*name) {
        h = (h << 4) + (unsigned char)*name++;
        g = h & 0xf0000000;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

/* ================================================================== */
/* PLT emission                                                        */
/* ================================================================== */

static size_t emit_plt0(Buffer *code, size_t got_fixup[2]) {
    size_t start = code->len;
    emit_byte(code, 0xFF); emit_byte(code, 0x35); /* push qword [rip+disp] */
    got_fixup[0] = code->len;
    emit_int32(code, 0);
    emit_byte(code, 0xFF); emit_byte(code, 0x25); /* jmp qword [rip+disp] */
    got_fixup[1] = code->len;
    emit_int32(code, 0);
    while ((code->len - start) < 16) emit_byte(code, 0x90);
    return start;
}

static size_t emit_plt_entry(Buffer *code, size_t idx, size_t plt0_off,
                             size_t *got_fixup) {
    size_t start = code->len;
    emit_byte(code, 0xFF); emit_byte(code, 0x25); /* jmp qword [rip+disp] */
    *got_fixup = code->len;
    emit_int32(code, 0);
    emit_byte(code, 0x68); /* push imm32 (relocation index) */
    emit_int32(code, (int32_t)idx);
    emit_byte(code, 0xE9); /* jmp rel32 */
    size_t pjmp = code->len;
    emit_int32(code, 0);
    int32_t rel = (int32_t)plt0_off - (int32_t)(pjmp + 4);
    memcpy(code->data + pjmp, &rel, 4);
    return start;
}

/* ================================================================== */
/* emit_link                                                           */
/* ================================================================== */

typedef struct { int prio; size_t idx; } ArrSlotOrd;

/* Stable sort of 8-byte constructor/destructor slots by priority (smaller first).
 * Writes old_slot → new_byte_offset into `remap` (length nslots).
 * Insertion sort avoids a qsort callback (function-pointer codegen is a
 * bootstrap-sensitive dialect corner, and n is tiny). */
static void sort_array_slots(Buffer *arr, int *prio, size_t **remap) {
    size_t nslots = arr->len / 8;
    *remap = NULL;
    if (nslots == 0) return;
    *remap = xcalloc(nslots, sizeof(size_t));
    ArrSlotOrd *ord = xcalloc(nslots, sizeof(ArrSlotOrd));
    for (size_t i = 0; i < nslots; i++) {
        ord[i].idx = i;
        ord[i].prio = prio ? prio[i] : INIT_PRIO_DEFAULT;
    }
    for (size_t i = 1; i < nslots; i++) {
        ArrSlotOrd key = ord[i];
        size_t j = i;
        while (j > 0) {
            int cmp = (ord[j - 1].prio > key.prio) - (ord[j - 1].prio < key.prio);
            if (cmp == 0)
                cmp = (ord[j - 1].idx > key.idx) - (ord[j - 1].idx < key.idx);
            if (cmp <= 0) break;
            ord[j] = ord[j - 1];
            j--;
        }
        ord[j] = key;
    }
    char *tmp = malloc(arr->len ? arr->len : 1);
    int *ptmp = malloc(nslots * sizeof(int));
    if (!tmp || !ptmp) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
    for (size_t ni = 0; ni < nslots; ni++) {
        size_t oi = ord[ni].idx;
        memcpy(tmp + ni * 8, arr->data + oi * 8, 8);
        ptmp[ni] = ord[ni].prio;
        (*remap)[oi] = ni * 8;
    }
    memcpy(arr->data, tmp, arr->len);
    if (prio) memcpy(prio, ptmp, nslots * sizeof(int));
    free(tmp); free(ptmp); free(ord);
}

static int ext_find_or_add(char ***ext, int *num_ext, const char *name) {
    for (int e = 0; e < *num_ext; e++)
        if (strcmp((*ext)[e], name) == 0) return e;
    *ext = realloc(*ext, ((size_t)*num_ext + 1) * sizeof(char *));
    (*ext)[(*num_ext)++] = xstrdup(name);
    return *num_ext - 1;
}

/* Append a DT_NEEDED soname if it is not already present. */
static void needed_add(char ***needed, int *num, const char *soname) {
    for (int i = 0; i < *num; i++)
        if (strcmp((*needed)[i], soname) == 0) return;
    *needed = realloc(*needed, ((size_t)*num + 1) * sizeof(char *));
    if (!*needed) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
    (*needed)[(*num)++] = xstrdup(soname);
}

void emit_link(EmitModule **mods, size_t n, const char *path,
               const char **needed_in, size_t num_needed_in, int nodefaultlibs,
               const char **lib_paths, size_t num_lib_paths,
               int want_debug, int is_shared) {
    /* Linker dispatch: the arm64-macos backend emits an ad-hoc signed
     * Mach-O PIE; everything below is the ELF executable/DSO linker. */
    if (target_current()->objfmt == TARGET_OBJFMT_MACHO) {
        /* -shared on Mach-O is refused in main(), before anything is
         * compiled, so that the diagnostic names the real reason instead of
         * surfacing as "no 'main' function found" from whichever stage
         * happened to look first.  This is the backstop for any caller that
         * reaches the linker directly rather than through the driver. */
        if (is_shared) {
            fprintf(stderr,
                    "fakecc: -shared is not implemented for the "
                    "arm64-macos target\n");
            exit(1);
        }
        /* The direct-write fast path packs exactly one module and has no room
         * to relocate anything, so it is only valid for a lone freestanding
         * TU whose text already starts with the LC_MAIN stub.  Modules that
         * recorded symbols (-c, hence also anything a multi-TU link leaves
         * behind) need the relocating link instead. */
        int from_objects = n > 1;
        for (size_t i = 0; i < n; i++)
            if (mods[i]->num_syms) from_objects = 1;
        int rc;
        /* -nostdlib or -nodefaultlibs: skip the libSystem LC_LOAD_DYLIB. */
        int skip_libsystem = nodefaultlibs;
        if (from_objects)
            rc = macho_link_objects(mods, n, path, skip_libsystem);
        else
            rc = macho_write_exec(mods[0], macho_text_offset(), path, skip_libsystem);
        if (rc != 0) exit(1);
        if (macho_codesign(path) != 0) exit(1);
        return;
    }
    /* ---- Merge sections ---- */
    Buffer text, rodata, data, tdata, initarr, finiarr;
    buffer_init(&text); buffer_init(&rodata); buffer_init(&data);
    buffer_init(&tdata);
    buffer_init(&initarr);
    buffer_init(&finiarr);
    size_t bss_size = 0;
    size_t tbss_size = 0;
    size_t *mod_text_off = xcalloc(n, sizeof(size_t));
    size_t *mod_rodata_off = xcalloc(n, sizeof(size_t));
    size_t *mod_data_off = xcalloc(n, sizeof(size_t));
    size_t *mod_bss_off = xcalloc(n, sizeof(size_t));
    size_t *mod_tdata_off = xcalloc(n, sizeof(size_t));
    size_t *mod_tbss_off = xcalloc(n, sizeof(size_t));
    size_t *mod_init_off = xcalloc(n, sizeof(size_t));
    size_t *mod_fini_off = xcalloc(n, sizeof(size_t));
    int *concat_init_prio = NULL;
    int *concat_fini_prio = NULL;

    for (size_t i = 0; i < n; i++) {
        EmitModule *m = mods[i];
        size_t ta = m->text_align > 16 ? m->text_align : 16;
        pad_buf_to(&text, ta);
        mod_text_off[i] = text.len;
        buffer_append(&text, m->text.data, m->text.len);
        pad_buf_to(&rodata, m->rodata_align ? m->rodata_align : 8);
        mod_rodata_off[i] = rodata.len;
        buffer_append(&rodata, m->rodata.data, m->rodata.len);
        pad_buf_to(&data, m->data_align ? m->data_align : 8);
        mod_data_off[i] = data.len;
        buffer_append(&data, m->data.data, m->data.len);
        bss_size = pad_size_to(bss_size, m->bss_align ? m->bss_align : 8);
        mod_bss_off[i] = bss_size;
        bss_size += m->bss_size;
        /* .tdata / .tbss concatenated per-module.  tdata is file-resident
         * (loaded into the TLS template); tbss is zero-fill at run time and
         * contributes only to memsz of PT_TLS. */
        pad_buf_to(&tdata, m->tdata_align ? m->tdata_align : 8);
        mod_tdata_off[i] = tdata.len;
        buffer_append(&tdata, m->tdata.data, m->tdata.len);
        tbss_size = pad_size_to(tbss_size, m->tbss_align ? m->tbss_align : 8);
        mod_tbss_off[i] = tbss_size;
        tbss_size += m->tbss_size;
        pad_buf_to(&initarr, m->init_array_align ? m->init_array_align : 8);
        mod_init_off[i] = initarr.len;
        {
            size_t before = initarr.len;
            buffer_append(&initarr, m->init_array.data, m->init_array.len);
            size_t nnew = initarr.len / 8;
            size_t nold = before / 8;
            if (nnew > 0) {
                concat_init_prio = realloc(concat_init_prio, nnew * sizeof(int));
                if (!concat_init_prio) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
                for (size_t s = nold; s < nnew; s++)
                    concat_init_prio[s] = INIT_PRIO_DEFAULT;
                if (m->init_prio) {
                    size_t ms = m->init_array.len / 8;
                    memcpy(concat_init_prio + nold, m->init_prio, ms * sizeof(int));
                }
            }
        }
        pad_buf_to(&finiarr, m->fini_array_align ? m->fini_array_align : 8);
        mod_fini_off[i] = finiarr.len;
        {
            size_t before = finiarr.len;
            buffer_append(&finiarr, m->fini_array.data, m->fini_array.len);
            size_t nnew = finiarr.len / 8;
            size_t nold = before / 8;
            if (nnew > 0) {
                concat_fini_prio = realloc(concat_fini_prio, nnew * sizeof(int));
                if (!concat_fini_prio) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
                for (size_t s = nold; s < nnew; s++)
                    concat_fini_prio[s] = INIT_PRIO_DEFAULT;
                if (m->fini_prio) {
                    size_t ms = m->fini_array.len / 8;
                    memcpy(concat_fini_prio + nold, m->fini_prio, ms * sizeof(int));
                }
            }
        }
    }
    size_t *init_remap = NULL, *fini_remap = NULL;
    sort_array_slots(&initarr, concat_init_prio, &init_remap);
    sort_array_slots(&finiarr, concat_fini_prio, &fini_remap);
    size_t max_ro_align = 8, max_bss_align = 8, max_td_align = 8, max_tbss_align = 8;
    for (size_t i = 0; i < n; i++) {
        if (mods[i]->rodata_align > max_ro_align) max_ro_align = mods[i]->rodata_align;
        if (mods[i]->bss_align > max_bss_align) max_bss_align = mods[i]->bss_align;
        if (mods[i]->tdata_align > max_td_align) max_td_align = mods[i]->tdata_align;
        if (mods[i]->tbss_align > max_tbss_align) max_tbss_align = mods[i]->tbss_align;
    }

    /* ---- Per-module symbol base indices ---- */
    size_t *mod_sym_base = xcalloc(n + 1, sizeof(size_t));
    for (size_t i = 0; i < n; i++)
        mod_sym_base[i + 1] = mod_sym_base[i] + mods[i]->num_syms;
    size_t total_syms = mod_sym_base[n];

    LinkSymInfo *sinfo = xcalloc(total_syms, sizeof(LinkSymInfo));
    for (size_t i = 0; i < n; i++) {
        EmitModule *m = mods[i];
        for (size_t j = 0; j < m->num_syms; j++) {
            size_t gsi = mod_sym_base[i] + j;
            sinfo[gsi].shndx = m->syms[j].shndx;
            sinfo[gsi].value = m->syms[j].value;
            sinfo[gsi].binding = m->syms[j].binding;
            sinfo[gsi].type = m->syms[j].type;
            sinfo[gsi].defined = (m->syms[j].shndx != SECT_UNDEF);
        }
    }

    /* Tentative COMMON (SHN_COMMON) symbols share one BSS slot per name
     * unless a real GLOBAL/WEAK definition exists.  st_value is alignment. */
    typedef struct { const char *name; size_t align, size, off; } CommonEnt;
    CommonEnt *commons = NULL;
    int n_commons = 0, cap_commons = 0;
    for (size_t i = 0; i < n; i++) {
        EmitModule *m = mods[i];
        for (size_t j = 0; j < m->num_syms; j++) {
            if (m->syms[j].shndx != SHN_COMMON || !m->syms[j].name) continue;
            const char *nm = m->syms[j].name;
            size_t win = find_export_gsi(mods, n, mod_sym_base, sinfo, nm);
            if (win != (size_t)-1 && sinfo[win].shndx != SHN_COMMON) continue;
            int found = -1;
            for (int c = 0; c < n_commons; c++)
                if (strcmp(commons[c].name, nm) == 0) { found = c; break; }
            size_t al = m->syms[j].value ? m->syms[j].value : 1;
            size_t sz = m->syms[j].size ? m->syms[j].size : al;
            if (found < 0) {
                if (n_commons >= cap_commons) {
                    cap_commons = cap_commons ? cap_commons * 2 : 4;
                    commons = realloc(commons, (size_t)cap_commons * sizeof(CommonEnt));
                    if (!commons) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
                }
                commons[n_commons].name = nm;
                commons[n_commons].align = al;
                commons[n_commons].size = sz;
                commons[n_commons].off = 0;
                n_commons++;
            } else {
                if (al > commons[found].align) commons[found].align = al;
                if (sz > commons[found].size) commons[found].size = sz;
            }
        }
    }
    for (int c = 0; c < n_commons; c++) {
        if (commons[c].align > max_bss_align) max_bss_align = commons[c].align;
        bss_size = pad_size_to(bss_size, commons[c].align);
        commons[c].off = bss_size;
        bss_size += commons[c].size;
    }

    /* ---- PLT slot assignment for undefined referenced symbols (functions)
     * Only symbols that are not defined as GLOBAL in ANY linked module are
     * truly external (e.g. libc).  Cross-module references stay static. */
    char **ext_list = NULL;
    int num_ext = 0;
    int *reloc_ext_idx = xcalloc(total_syms, sizeof(int));
    int *reloc_data_abs_idx = xcalloc(total_syms, sizeof(int));
    for (size_t i = 0; i < total_syms; i++) {
        reloc_ext_idx[i] = -1;
        reloc_data_abs_idx[i] = -1;
    }
    char **abs_ext_list = NULL;
    int num_abs_ext = 0;

    /* Helper: is `nm` defined GLOBAL in any module? */
    /* (open-coded below to avoid a nested function) */

    for (size_t i = 0; i < n; i++) {
        EmitModule *m = mods[i];
        for (size_t r = 0; r < m->num_relocs; r++) {
            if (reloc_is_gotpcrel(m->relocs[r].type)) continue;
            if (m->relocs[r].type == R_X86_64_GOTTPOFF) continue;
            if (reloc_is_tls_gd(m->relocs[r].type)) continue;
            if (reloc_is_tls_ld(m->relocs[r].type)) continue;
            if (m->relocs[r].type == R_X86_64_DTPOFF32) continue;
            size_t gsi = mod_sym_base[i] + m->relocs[r].sym;
            if (sinfo[gsi].defined) continue;
            const char *nm = m->syms[m->relocs[r].sym].name
                             ? m->syms[m->relocs[r].sym].name : "";
            if (m->relocs[r].type == R_X86_64_PLT32
                && strcmp(nm, "__tls_get_addr") == 0) {
                int pair_skip = 0;
                for (size_t r2 = 0; r2 < m->num_relocs; r2++) {
                    if (m->relocs[r2].type == R_X86_64_TLSGD
                        && m->relocs[r2].offset + 8 == m->relocs[r].offset) {
                        pair_skip = 1;
                        break;
                    }
                    /* TLSLD→LE in executables drops the call; DSOs keep it. */
                    if (!is_shared && m->relocs[r2].type == R_X86_64_TLSLD
                        && m->relocs[r2].offset + 5 == m->relocs[r].offset) {
                        pair_skip = 1;
                        break;
                    }
                }
                if (pair_skip) continue;
            }
            uint8_t stt = m->syms[m->relocs[r].sym].type;
            if (reloc_is_copy_kind(m->relocs[r].type)
                && m->relocs[r].type != R_X86_64_PLT32
                && stt != STT_FUNC
                && stt != STT_TLS
                && find_export_gsi(mods, n, mod_sym_base, sinfo, nm) == (size_t)-1) {
                /* Direct access to a DSO object → COPY, not PLT. */
                continue;
            }
            if (find_export_gsi(mods, n, mod_sym_base, sinfo, nm) == (size_t)-1)
                reloc_ext_idx[gsi] = ext_find_or_add(&ext_list, &num_ext, nm);
        }
        /* .data R_X86_64_64 fixups also reference symbols.  A file-scope
         * initializer like `static double (*fp)(double) = asin;` never emits a
         * text reloc, so without this scan the pointer is patched to PLT0 /
         * the ELF entry stub and calling it re-enters `_start`.  Match LOCAL
         * definitions too (`static int gx[]` is STB_LOCAL).  Unresolved names
         * get a PLT slot — GLOB_DAT / abs64 is for GOTPCREL data objects. */
        for (size_t r = 0; r < m->num_data_relocs; r++) {
            size_t gsi = mod_sym_base[i] + m->data_relocs[r].sym;
            if (sinfo[gsi].defined) continue;
            if (reloc_ext_idx[gsi] >= 0) continue;
            const char *nm = m->syms[m->data_relocs[r].sym].name
                             ? m->syms[m->data_relocs[r].sym].name : "";
            int resolved = 0;
            for (size_t mi = 0; mi < n && !resolved; mi++) {
                EmitModule *om = mods[mi];
                for (size_t mj = 0; mj < om->num_syms; mj++) {
                    size_t ogsi = mod_sym_base[mi] + mj;
                    if (sinfo[ogsi].defined && om->syms[mj].name
                        && strcmp(om->syms[mj].name, nm) == 0) {
                        resolved = 1;
                        break;
                    }
                }
            }
            if (!resolved)
                reloc_ext_idx[gsi] = ext_find_or_add(&ext_list, &num_ext, nm);
        }
    }

    /* ---- Data GOT slot assignment for external variables (GOTPCREL) ----
     * An IR_GADDR of a symbol undefined in the referencing module emits a
     * GOTPCREL reloc.  We assign each such referenced symbol a slot in the
     * data GOT (right after the PLT GOT slots).  The linker fills the slot
     * with the symbol's address: statically if the symbol is defined in
     * another module (cross-module global), or via a R_X86_64_GLOB_DAT
     * relocation if it is truly external (libc variable like `stderr`). */
    char **data_ext_list = NULL;
    int num_data_ext = 0;
    int *reloc_data_got_idx = xcalloc(total_syms, sizeof(int));
    for (size_t i = 0; i < total_syms; i++) reloc_data_got_idx[i] = -1;
    for (size_t i = 0; i < n; i++) {
        EmitModule *m = mods[i];
        for (size_t r = 0; r < m->num_relocs; r++) {
            if (!reloc_is_gotpcrel(m->relocs[r].type)) continue;
            size_t gsi = mod_sym_base[i] + m->relocs[r].sym;
            if (reloc_data_got_idx[gsi] >= 0) continue;
            const char *nm = m->syms[m->relocs[r].sym].name
                             ? m->syms[m->relocs[r].sym].name : "";
            reloc_data_got_idx[gsi] = ext_find_or_add(&data_ext_list, &num_data_ext, nm);
        }
    }
    /* Determine, per data-external slot, whether the symbol is defined in any
     * module (cross-module global) or truly external (libc).  This only needs
     * symbol names + definitions, NOT final addresses, so it can run now —
     * before layout — which lets the .rela.dyn buffer be sized for layout. */
    int *data_got_external = num_data_ext ? xcalloc(num_data_ext, sizeof(int)) : NULL;
    int num_true_data_ext = 0;
    for (int j = 0; j < num_data_ext; j++) {
        const char *nm = data_ext_list[j];
        int found = find_export_gsi(mods, n, mod_sym_base, sinfo, nm) != (size_t)-1;
        data_got_external[j] = !found;
        if (!found) num_true_data_ext++;
    }

    /* ---- TLS Initial-Exec GOT slots (GOTTPOFF) ----
     * Each unique TLS symbol referenced via GOTTPOFF gets a GOT qword after
     * the data GOT.  LOCAL symbols stay unique per gsi (two TUs may both
     * have `static __thread int x`).  GLOBAL symbols merge by name. */
    int *reloc_tls_ie_idx = xcalloc(total_syms, sizeof(int));
    for (size_t i = 0; i < total_syms; i++) reloc_tls_ie_idx[i] = -1;
    int *tls_ie_gsi = NULL;
    const char **tls_ie_name = NULL;
    int num_tls_ie = 0;
    for (size_t i = 0; i < n; i++) {
        EmitModule *m = mods[i];
        for (size_t r = 0; r < m->num_relocs; r++) {
            if (reloc_is_tls_ld(m->relocs[r].type)) continue;
            if (m->relocs[r].type != R_X86_64_GOTTPOFF
                && !reloc_is_tls_gd(m->relocs[r].type)) continue;
            size_t gsi = mod_sym_base[i] + m->relocs[r].sym;
            if (reloc_tls_ie_idx[gsi] >= 0) continue;
            const char *nm = m->syms[m->relocs[r].sym].name
                             ? m->syms[m->relocs[r].sym].name : "";
            int is_local = sinfo[gsi].defined && sinfo[gsi].binding == STB_LOCAL;
            int slot = -1;
            if (!is_local && nm[0]) {
                for (int j = 0; j < num_tls_ie; j++) {
                    if (tls_ie_name[j] && strcmp(tls_ie_name[j], nm) == 0) {
                        slot = j;
                        break;
                    }
                }
            }
            if (slot < 0) {
                slot = num_tls_ie++;
                tls_ie_gsi = realloc(tls_ie_gsi, (size_t)num_tls_ie * sizeof(int));
                tls_ie_name = realloc(tls_ie_name, (size_t)num_tls_ie * sizeof(char *));
                if (!tls_ie_gsi || !tls_ie_name) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
                tls_ie_gsi[slot] = (int)gsi;
                tls_ie_name[slot] = is_local ? NULL : nm;
            }
            reloc_tls_ie_idx[gsi] = slot;
        }
    }

    /* ---- COPY relocs: direct (non-GOT) refs to DSO data objects ----
     * Only executables copy; a DSO uses GLOB_DAT / symbolic relocs. */
    char **copy_list = NULL;
    int num_copy = 0;
    int *reloc_copy_idx = xcalloc(total_syms, sizeof(int));
    for (size_t i = 0; i < total_syms; i++) reloc_copy_idx[i] = -1;
    for (size_t i = 0; i < n && !is_shared; i++) {
        EmitModule *m = mods[i];
        for (size_t r = 0; r < m->num_relocs; r++) {
            uint32_t rt = m->relocs[r].type;
            if (!reloc_is_copy_kind(rt) || rt == R_X86_64_PLT32) continue;
            size_t gsi = mod_sym_base[i] + m->relocs[r].sym;
            if (sinfo[gsi].defined || reloc_copy_idx[gsi] >= 0) continue;
            if (reloc_ext_idx[gsi] >= 0) continue;
            uint8_t stt = m->syms[m->relocs[r].sym].type;
            if (stt == STT_FUNC || stt == STT_TLS) continue;
            const char *nm = m->syms[m->relocs[r].sym].name
                             ? m->syms[m->relocs[r].sym].name : "";
            if (find_export_gsi(mods, n, mod_sym_base, sinfo, nm) != (size_t)-1)
                continue;
            reloc_copy_idx[gsi] = ext_find_or_add(&copy_list, &num_copy, nm);
        }
        for (size_t r = 0; r < m->num_data_relocs; r++) {
            if (m->data_relocs[r].type != R_X86_64_64) continue;
            size_t gsi = mod_sym_base[i] + m->data_relocs[r].sym;
            if (sinfo[gsi].defined || reloc_copy_idx[gsi] >= 0) continue;
            if (reloc_ext_idx[gsi] >= 0) continue;
            uint8_t stt = m->syms[m->data_relocs[r].sym].type;
            /* Data-pointer abs64 to STT_NOTYPE is a function pointer (PLT).
             * Only a real STT_OBJECT is a COPY of DSO data. */
            if (stt != STT_OBJECT) continue;
            const char *nm = m->syms[m->data_relocs[r].sym].name
                             ? m->syms[m->data_relocs[r].sym].name : "";
            if (find_export_gsi(mods, n, mod_sym_base, sinfo, nm) != (size_t)-1)
                continue;
            reloc_copy_idx[gsi] = ext_find_or_add(&copy_list, &num_copy, nm);
        }
    }

    int *tls_ie_is_undef = num_tls_ie ? xcalloc((size_t)num_tls_ie, sizeof(int)) : NULL;
    char **tls_und_list = NULL;
    int num_tls_und = 0;
    int *tls_ie_und_idx = num_tls_ie ? xcalloc((size_t)num_tls_ie, sizeof(int)) : NULL;
    for (int j = 0; j < num_tls_ie; j++) {
        if (tls_ie_und_idx) tls_ie_und_idx[j] = -1;
        size_t gsi = (size_t)tls_ie_gsi[j];
        int defined = 0;
        if (sinfo[gsi].defined
            && (sinfo[gsi].shndx == SECT_TDATA || sinfo[gsi].shndx == SECT_TBSS))
            defined = 1;
        else {
            const char *nm = tls_ie_name[j] ? tls_ie_name[j] : "";
            size_t win = find_export_gsi(mods, n, mod_sym_base, sinfo, nm);
            if (win != (size_t)-1
                && (sinfo[win].shndx == SECT_TDATA || sinfo[win].shndx == SECT_TBSS))
                defined = 1;
        }
        if (!defined) {
            tls_ie_is_undef[j] = 1;
            const char *nm = tls_ie_name[j] ? tls_ie_name[j] : "";
            tls_ie_und_idx[j] = ext_find_or_add(&tls_und_list, &num_tls_und, nm);
        }
    }
    int *tls_und_weak = num_tls_und ? xcalloc((size_t)num_tls_und, sizeof(int)) : NULL;
    for (int t = 0; t < num_tls_und; t++) {
        int any = 0, strong = 0;
        const char *nm = tls_und_list[t];
        for (size_t i = 0; i < n; i++) {
            EmitModule *m = mods[i];
            for (size_t j = 0; j < m->num_syms; j++) {
                if (!m->syms[j].name || strcmp(m->syms[j].name, nm) != 0) continue;
                if (sinfo[mod_sym_base[i] + j].defined) continue;
                any = 1;
                if (m->syms[j].binding != STB_WEAK) strong = 1;
            }
        }
        tls_und_weak[t] = any && !strong;
    }

    /* Dynamic link when something is truly undefined, or when producing a
     * shared library (ET_DYN always needs .dynamic / .dynsym for exports). */
    int need_dynamic = is_shared
        || (num_ext > 0 || num_true_data_ext > 0 || num_abs_ext > 0
            || num_copy > 0 || num_tls_und > 0);

    /* ---- Shared-library DT_NEEDED list (from -l only; no automatic libc) ----
     * Builtin runtime/ supplies the hosted stdlib.  System libs are opt-in via -l,
     * like cgo.  `-nodefaultlibs` is retained as a no-op alias for scripts
     * that still pass it (defaults already skip libc). */
    char **needed = NULL;
    int num_needed = 0;
    for (size_t i = 0; i < num_needed_in; i++)
        needed_add(&needed, &num_needed, needed_in[i]);
    (void)nodefaultlibs;

    size_t *copy_size = num_copy ? xcalloc((size_t)num_copy, sizeof(size_t)) : NULL;
    size_t *copy_off = num_copy ? xcalloc((size_t)num_copy, sizeof(size_t)) : NULL;
    {
        int w = 0;
        for (int j = 0; j < num_copy; j++) {
            uint8_t ty = 0;
            size_t al = 8;
            size_t sz = so_symbol_lookup((const char **)lib_paths, num_lib_paths,
                                         (const char **)needed, (size_t)num_needed,
                                         copy_list[j], &ty, &al);
            /* Direct PC32 to a DSO *function* (STT_NOTYPE in the .o) must stay
             * a PLT call, not a COPY of the function body into .bss. */
            if (ty != STT_OBJECT) {
                int eidx = ext_find_or_add(&ext_list, &num_ext, copy_list[j]);
                for (size_t gsi = 0; gsi < total_syms; gsi++) {
                    if (reloc_copy_idx[gsi] == j) {
                        reloc_copy_idx[gsi] = -1;
                        reloc_ext_idx[gsi] = eidx;
                    }
                }
                free(copy_list[j]);
                continue;
            }
            if (sz < 1) sz = 8;
            if (al < 1) al = 8;
            if (w != j) {
                for (size_t gsi = 0; gsi < total_syms; gsi++)
                    if (reloc_copy_idx[gsi] == j) reloc_copy_idx[gsi] = w;
                copy_list[w] = copy_list[j];
            }
            copy_size[w] = sz;
            bss_size = pad_size_to(bss_size, al);
            copy_off[w] = bss_size;
            bss_size += sz;
            if (al > max_bss_align) max_bss_align = al;
            w++;
        }
        num_copy = w;
    }

    int have_tlsld = 0;
    for (size_t i = 0; i < n && !have_tlsld; i++) {
        EmitModule *m = mods[i];
        for (size_t r = 0; r < m->num_relocs; r++) {
            if (reloc_is_tls_ld(m->relocs[r].type)) { have_tlsld = 1; break; }
        }
    }
    int tlsld_got_pair = is_shared && have_tlsld;

    /* STT_GNU_IFUNC: one GOT slot + `jmp [got]` thunk per resolver. */
    int num_ifunc = 0;
    size_t *ifunc_gsi = NULL;
    const char **ifunc_name = NULL;
    size_t *ifunc_thunk_off = NULL;
    size_t *ifunc_got_fixup = NULL;
    for (size_t i = 0; i < n; i++) {
        EmitModule *m = mods[i];
        for (size_t j = 0; j < m->num_syms; j++) {
            size_t gsi = mod_sym_base[i] + j;
            if (!sinfo[gsi].defined || sinfo[gsi].shndx != SECT_TEXT) continue;
            if (m->syms[j].type != STT_GNU_IFUNC) continue;
            const char *nm = m->syms[j].name ? m->syms[j].name : "";
            int dup = 0;
            if (nm[0] && sinfo[gsi].binding != STB_LOCAL) {
                for (int k = 0; k < num_ifunc; k++) {
                    if (ifunc_name[k] && strcmp(ifunc_name[k], nm) == 0) {
                        dup = 1;
                        break;
                    }
                }
            }
            if (dup) continue;
            ifunc_gsi = realloc(ifunc_gsi, ((size_t)num_ifunc + 1) * sizeof(size_t));
            ifunc_name = realloc(ifunc_name, ((size_t)num_ifunc + 1) * sizeof(char *));
            if (!ifunc_gsi || !ifunc_name) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
            ifunc_gsi[num_ifunc] = gsi;
            ifunc_name[num_ifunc] = nm[0] ? nm : NULL;
            num_ifunc++;
        }
    }

    /* DT_SONAME defaults to the output basename so `gcc -lfoo` / `fakecc -lfoo`
     * resolve the same file the driver wrote. */
    char *soname = NULL;
    if (is_shared && path) {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        if (base[0]) soname = xstrdup(base);
    }

    /* Symbols defined in this link unit that a shared object must export. */
    struct {
        const char *name;
        size_t gsi;
        uint8_t type;
        uint16_t shndx;
        size_t size;
    } *exports = NULL;
    int num_exports = 0;
    if (is_shared) {
        for (size_t i = 0; i < n; i++) {
            EmitModule *m = mods[i];
            for (size_t j = 0; j < m->num_syms; j++) {
                size_t gsi = mod_sym_base[i] + j;
                const EmitSymbol *sym = &m->syms[j];
                if (!sinfo[gsi].defined) continue;
                if (sinfo[gsi].binding != STB_GLOBAL) continue;
                if (!sym->name || sym->type == STT_SECTION) continue;
                /* Dedup by name (first definition wins). */
                int dup = 0;
                for (int e = 0; e < num_exports; e++) {
                    if (strcmp(exports[e].name, sym->name) == 0) { dup = 1; break; }
                }
                if (dup) continue;
                exports = realloc(exports, ((size_t)num_exports + 1) * sizeof(*exports));
                if (!exports) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
                exports[num_exports].name = sym->name;
                exports[num_exports].gsi = gsi;
                exports[num_exports].type = sym->type ? sym->type : STT_FUNC;
                exports[num_exports].shndx = sym->shndx;
                exports[num_exports].size = sym->size;
                num_exports++;
            }
        }
    }

    /* Colon-separated DT_RUNPATH from -L directories (and dirs of explicit .so). */
    char *runpath = NULL;
    if (need_dynamic && num_lib_paths > 0) {
        size_t len = 1; /* NUL */
        for (size_t i = 0; i < num_lib_paths; i++)
            len += strlen(lib_paths[i]) + (i > 0 ? 1 : 0);
        runpath = malloc(len);
        if (!runpath) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        size_t pos = 0;
        for (size_t i = 0; i < num_lib_paths; i++) {
            if (i > 0) runpath[pos++] = ':';
            size_t n = strlen(lib_paths[i]);
            memcpy(runpath + pos, lib_paths[i], n);
            pos += n;
        }
        runpath[pos] = '\0';
    }

    /* ---- Reserve a PLT slot for exit() when libc is linked ----
     * The entry stub calls it instead of issuing exit_group directly, so libc
     * gets to run its atexit handlers (stdout is flushed by one of them).
     * Without libc in DT_NEEDED, keep the raw syscall exit path — the builtin
     * runtime's exit() is called directly when defined in a linked module. */
    int exit_ext_idx = -1;
    int have_libc = 0;
    for (int i = 0; i < num_needed; i++) {
        if (strcmp(needed[i], "libc.so.6") == 0) { have_libc = 1; break; }
    }
    if (need_dynamic && have_libc && !is_shared)
        exit_ext_idx = ext_find_or_add(&ext_list, &num_ext, "exit");

    /* ---- Build PLT at end of .text (only when truly external funcs) ---- */
    size_t *plt_entry_off = num_ext ? xcalloc(num_ext, sizeof(size_t)) : NULL;
    size_t *plt_got_fixup = num_ext ? xcalloc(num_ext, sizeof(size_t)) : NULL;
    size_t plt0_got_fixup[2] = {0, 0};
    size_t plt0_off = 0;
    if (num_ext > 0) {
        plt0_off = emit_plt0(&text, plt0_got_fixup);
        for (int e = 0; e < num_ext; e++)
            plt_entry_off[e] = emit_plt_entry(&text, e, plt0_off, &plt_got_fixup[e]);
    }
    ifunc_thunk_off = num_ifunc ? xcalloc((size_t)num_ifunc, sizeof(size_t)) : NULL;
    ifunc_got_fixup = num_ifunc ? xcalloc((size_t)num_ifunc, sizeof(size_t)) : NULL;
    for (int k = 0; k < num_ifunc; k++) {
        ifunc_thunk_off[k] = text.len;
        emit_byte(&text, 0xFF); emit_byte(&text, 0x25); /* jmp qword [rip+disp] */
        ifunc_got_fixup[k] = text.len;
        emit_int32(&text, 0);
    }
    /* ---- Build dynamic-linking section buffers (sizes needed for layout) ---- */
    Buffer dynstr, dynsym, hash, rela_plt, rela_dyn, dynamic;
    buffer_init(&dynstr); buffer_init(&dynsym); buffer_init(&hash);
    buffer_init(&rela_plt); buffer_init(&rela_dyn); buffer_init(&dynamic);
    size_t interp_len = (need_dynamic && !is_shared) ? sizeof(INTERP_PATH) : 0;

    /* External symbols that need dynsym entries: only TRULY external ones.
     * Cross-module data still occupies GOT slots (num_data_ext) but does not
     * appear in .dynsym.  Shared libraries additionally export every defined
     * GLOBAL.  dynstr layout when dynamic:
     * "\0" + DT_NEEDED sonames + [DT_SONAME] + [DT_RUNPATH] + true-ext func names
     * + true-ext data names + unresolved data-object names + [exported defined names]. */
    int num_abs64_relocs = 0;
    int num_tpoff_dyn = 0;
    for (size_t i = 0; i < n; i++) {
        EmitModule *m = mods[i];
        for (size_t r = 0; r < m->num_data_relocs; r++) {
            size_t gsi = mod_sym_base[i] + m->data_relocs[r].sym;
            if (reloc_data_abs_idx[gsi] >= 0) num_abs64_relocs++;
        }
    }
    num_tpoff_dyn = is_shared ? num_tls_ie : num_tls_und;
    int num_dynsym_ext = num_ext + num_true_data_ext + num_abs_ext
        + num_tls_und + num_copy + num_exports;
    int dyn_tls_und0 = 1 + num_ext + num_true_data_ext + num_abs_ext;
    int dyn_copy0 = dyn_tls_und0 + num_tls_und;
    int dyn_export0 = dyn_copy0 + num_copy;
    size_t needed_str_bytes = 0;
    size_t soname_str_bytes = 0;
    size_t soname_dynstr_off = 0;
    size_t runpath_str_bytes = 0;
    size_t runpath_dynstr_off = 0;
    /* Shared objects need R_X86_64_RELATIVE for every link-time absolute
     * address that lives in RW data: pointer initializers, internal GOT
     * slots, GOT[0] (= &_DYNAMIC), and __fakecc_tls_image.  Unresolved
     * data objects use R_X86_64_64 instead of RELATIVE. */
    int num_data_ptr_rel = 0;
    int num_internal_got = 0;
    int tls_image_rel = 0;
    if (is_shared) {
        for (size_t i = 0; i < n; i++) {
            EmitModule *m = mods[i];
            for (size_t r = 0; r < m->num_data_relocs; r++) {
                size_t gsi = mod_sym_base[i] + m->data_relocs[r].sym;
                if (reloc_data_abs_idx[gsi] >= 0) continue;
                /* PC-relative and 32-bit abs are not R_X86_64_RELATIVE. */
                uint32_t rt = m->data_relocs[r].type;
                if (reloc_is_pc(rt) || reloc_is_abs32(rt)) continue;
                num_data_ptr_rel++;
            }
        }
        for (int j = 0; j < num_data_ext; j++)
            if (!data_got_external[j]) num_internal_got++;
        if (tdata.len > 0 || tbss_size > 0) {
            size_t mi, mj;
            for (mi = 0; mi < n && !tls_image_rel; mi++) {
                EmitModule *m = mods[mi];
                for (mj = 0; mj < m->num_syms; mj++) {
                    if (m->syms[mj].name
                        && strcmp(m->syms[mj].name, "__fakecc_tls_image") == 0) {
                        tls_image_rel = 1;
                        break;
                    }
                }
            }
        }
    }
    int num_relative = is_shared ? (1 + num_data_ptr_rel + num_internal_got + tls_image_rel) : 0;
    if (need_dynamic) {
        buf_u8(&dynstr, 0);
        for (int i = 0; i < num_needed; i++) {
            buf_bytes(&dynstr, needed[i], strlen(needed[i]) + 1);
            needed_str_bytes += strlen(needed[i]) + 1;
        }
        if (soname) {
            soname_dynstr_off = dynstr.len;
            buf_bytes(&dynstr, soname, strlen(soname) + 1);
            soname_str_bytes = strlen(soname) + 1;
        }
        if (runpath) {
            runpath_dynstr_off = dynstr.len;
            buf_bytes(&dynstr, runpath, strlen(runpath) + 1);
            runpath_str_bytes = strlen(runpath) + 1;
        }
        for (int i = 0; i < num_ext; i++)
            buf_bytes(&dynstr, ext_list[i], strlen(ext_list[i]) + 1);
        for (int j = 0; j < num_data_ext; j++) {
            if (!data_got_external[j]) continue;
            buf_bytes(&dynstr, data_ext_list[j], strlen(data_ext_list[j]) + 1);
        }
        for (int a = 0; a < num_abs_ext; a++)
            buf_bytes(&dynstr, abs_ext_list[a], strlen(abs_ext_list[a]) + 1);
        for (int t = 0; t < num_tls_und; t++)
            buf_bytes(&dynstr, tls_und_list[t], strlen(tls_und_list[t]) + 1);
        for (int c = 0; c < num_copy; c++)
            buf_bytes(&dynstr, copy_list[c], strlen(copy_list[c]) + 1);
        for (int e = 0; e < num_exports; e++)
            buf_bytes(&dynstr, exports[e].name, strlen(exports[e].name) + 1);
        buf_pad(&dynsym, 24); /* [0] NULL symbol */
        for (int k = 0; k < num_ext + num_true_data_ext + num_abs_ext; k++) {
            buf_u32(&dynsym, 0); /* st_name patched below */
            buf_u8(&dynsym, STB_GLOBAL << 4);
            buf_u8(&dynsym, 0);
            buf_u16(&dynsym, SHN_UNDEF);
            buf_u64(&dynsym, 0);
            buf_u64(&dynsym, 0);
        }
        for (int t = 0; t < num_tls_und; t++) {
            buf_u32(&dynsym, 0);
            uint8_t bind = (tls_und_weak && tls_und_weak[t]) ? STB_WEAK : STB_GLOBAL;
            buf_u8(&dynsym, (uint8_t)((bind << 4) | STT_TLS));
            buf_u8(&dynsym, 0);
            buf_u16(&dynsym, SHN_UNDEF);
            buf_u64(&dynsym, 0);
            buf_u64(&dynsym, 0);
        }
        for (int c = 0; c < num_copy; c++) {
            buf_u32(&dynsym, 0);
            buf_u8(&dynsym, (uint8_t)((STB_GLOBAL << 4) | STT_OBJECT));
            buf_u8(&dynsym, 0);
            buf_u16(&dynsym, 4); /* .bss */
            buf_u64(&dynsym, 0); /* st_value patched after layout */
            buf_u64(&dynsym, copy_size[c]);
        }
        for (int e = 0; e < num_exports; e++) {
            buf_u32(&dynsym, 0); /* st_name patched below */
            buf_u8(&dynsym, (uint8_t)((STB_GLOBAL << 4) | (exports[e].type & 0xf)));
            buf_u8(&dynsym, 0);
            buf_u16(&dynsym, exports[e].shndx);
            buf_u64(&dynsym, 0); /* st_value patched after layout */
            buf_u64(&dynsym, exports[e].size);
        }
        size_t nsyms = 1 + (size_t)num_dynsym_ext;
        size_t nbucket = (nsyms < 2) ? 1 : 3;
        uint32_t *bucket = calloc(nbucket, sizeof(uint32_t));
        uint32_t *chain = calloc(nsyms, sizeof(uint32_t));
        for (size_t i = 0; i < nsyms; i++) {
            const char *nm = "";
            if (i > 0) {
                int idx = (int)(i - 1);
                if (idx < num_ext) nm = ext_list[idx];
                else {
                    int want = idx - num_ext;
                    if (want < num_true_data_ext) {
                        int seen = 0;
                        for (int j = 0; j < num_data_ext; j++) {
                            if (!data_got_external[j]) continue;
                            if (seen == want) { nm = data_ext_list[j]; break; }
                            seen++;
                        }
                    } else {
                        want -= num_true_data_ext;
                        if (want < num_abs_ext) nm = abs_ext_list[want];
                        else {
                            want -= num_abs_ext;
                            if (want < num_tls_und) nm = tls_und_list[want];
                            else {
                                want -= num_tls_und;
                                if (want < num_copy) nm = copy_list[want];
                                else nm = exports[want - num_copy].name;
                            }
                        }
                    }
                }
            }
            uint32_t b = (uint32_t)(elf_hash(nm) % nbucket);
            chain[i] = bucket[b];
            bucket[b] = (uint32_t)i;
        }
        buf_u32(&hash, (uint32_t)nbucket);
        buf_u32(&hash, (uint32_t)nsyms);
        for (size_t i = 0; i < nbucket; i++) buf_u32(&hash, bucket[i]);
        for (size_t i = 0; i < nsyms; i++) buf_u32(&hash, chain[i]);
        free(bucket); free(chain);
        for (int i = 0; i < num_ext; i++) {
            buf_u64(&rela_plt, 0);
            buf_u64(&rela_plt, ((uint64_t)(i + 1) << 32) | R_X86_64_JUMP_SLOT);
            buf_u64(&rela_plt, 0);
        }
        {
            int dyn_sym_i = 1 + num_ext;
            for (int j = 0; j < num_data_ext; j++) {
                if (!data_got_external[j]) continue;
                buf_u64(&rela_dyn, 0);
                buf_u64(&rela_dyn, ((uint64_t)dyn_sym_i << 32) | R_X86_64_GLOB_DAT);
                buf_u64(&rela_dyn, 0);
                dyn_sym_i++;
            }
            dyn_sym_i = 1 + num_ext + num_true_data_ext;
            for (int a = 0; a < num_abs64_relocs; a++) {
                buf_u64(&rela_dyn, 0);
                buf_u64(&rela_dyn, R_X86_64_64); /* r_info sym patched below */
                buf_u64(&rela_dyn, 0);
            }
            (void)dyn_sym_i;
        }
        for (int r = 0; r < num_relative; r++) {
            buf_u64(&rela_dyn, 0); /* r_offset patched below */
            buf_u64(&rela_dyn, R_X86_64_RELATIVE);
            buf_u64(&rela_dyn, 0); /* r_addend patched below */
        }
        for (int t = 0; t < num_tpoff_dyn; t++) {
            buf_u64(&rela_dyn, 0);
            buf_u64(&rela_dyn, R_X86_64_TPOFF64);
            buf_u64(&rela_dyn, 0);
        }
        for (int c = 0; c < num_copy; c++) {
            buf_u64(&rela_dyn, 0);
            buf_u64(&rela_dyn, ((uint64_t)(dyn_copy0 + c) << 32) | R_X86_64_COPY);
            buf_u64(&rela_dyn, 0);
        }
        for (int k = 0; k < num_ifunc; k++) {
            buf_u64(&rela_dyn, 0);
            buf_u64(&rela_dyn, R_X86_64_IRELATIVE);
            buf_u64(&rela_dyn, 0);
        }
        if (tlsld_got_pair) {
            buf_u64(&rela_dyn, 0);
            buf_u64(&rela_dyn, R_X86_64_DTPMOD64);
            buf_u64(&rela_dyn, 0);
        }
    }

    /* ---- Compute layout ---- */
    int have_tls = (tdata.len > 0 || tbss_size > 0);
    int init_tls_in_start = have_tls && !need_dynamic;
    int run_ctors = !is_shared && !need_dynamic && initarr.len >= 8;
    int run_dtors = !is_shared && !need_dynamic && finiarr.len >= 8;
    int run_irel = !is_shared && !need_dynamic && num_ifunc > 0;
    size_t start_size = 0;
    if (!is_shared) {
        start_size = init_tls_in_start ? (START_SIZE + 25 + (tdata.len > 0 ? 27 : 0)) : START_SIZE;
        if (run_ctors) start_size += INIT_ARRAY_START;
        if (run_dtors) start_size += (size_t)FINI_ARRAY_START - 2;
        if (run_irel) start_size += IREL_START;
    }
    /* phnum is finalized after we know whether a data segment is needed;
     * reserve header space for the maximum so segment file offsets are stable.
     * GNU_STACK is always present (non-executable stack).  Shared objects
     * omit PT_INTERP. */
    uint16_t phnum_max;
    if (is_shared)
        phnum_max = have_tls ? 5 : 4; /* RX, RW, DYNAMIC, GNU_STACK, [TLS] */
    else
        phnum_max = have_tls ? 6 : 5; /* RX, RW, INTERP, DYNAMIC, GNU_STACK, [TLS] */
    size_t hdr_size = ELF64_EHDR_SIZE + ELF64_PHDR_SIZE * phnum_max;
    size_t start_offset = hdr_size;
    size_t text_offset = start_offset + start_size;
    uint64_t base = is_shared ? 0 : ELF_BASE;
    uint64_t code_vaddr = base + text_offset;
    /* Combined .rodata follows .text; pad .text so rodata's vaddr matches
     * max_ro_align even when text_offset is not 16-aligned
     * (hdr + _start = 0x16e).  This MUST happen before the layout sizes
     * below: the pad grows the physical rx content, so computing file
     * offsets first and padding afterwards desynchronizes the physical
     * image from the declared layout (the GOT would then be written a
     * few bytes after its resolved slots, which crashes lazy glibc loaders
     * while eager musl/gcompat loaders paper over it). */
    {
        size_t ro_va = (size_t)(code_vaddr + text.len);
        size_t extra = pad_size_to(ro_va, max_ro_align) - ro_va;
        while (extra--) {
            char z = 0;
            buffer_append(&text, &z, 1);
        }
    }
    /* .dynamic size: num_needed×DT_NEEDED + [DT_SONAME] + [DT_RUNPATH] + 9 fixed tags +
     * DT_NULL, plus DT_RELA/RELASZ/RELENT when .rela.dyn is non-empty. */
    size_t dynamic_size = 0;
    if (need_dynamic) {
        int have_rela_dyn = (num_true_data_ext > 0 || num_relative > 0
                             || num_abs64_relocs > 0 || num_tpoff_dyn > 0
                             || num_copy > 0 || num_ifunc > 0 || tlsld_got_pair);
        int have_dt_flags = is_shared && num_tls_ie > 0;
        int have_dt_init = initarr.len > 0;
        int have_dt_fini = finiarr.len > 0;
        dynamic_size = (size_t)(num_needed + 10 + (runpath ? 1 : 0) + (soname ? 1 : 0)
                                + (have_rela_dyn ? 3 : 0)
                                + (have_dt_flags ? 1 : 0)
                                + (have_dt_init ? 2 : 0)
                                + (have_dt_fini ? 2 : 0)) * 16;
    }
    size_t dyn_sections_len = interp_len + dynstr.len + dynsym.len + hash.len
        + rela_plt.len + rela_dyn.len;
    /* Shared objects keep .dynamic in the RW segment so the dynamic linker can
     * write DT_DEBUG; executables keep the historical RX placement. */
    size_t dyn_in_rx = is_shared ? 0 : dynamic_size;
    size_t rx_content_len = start_size + text.len + rodata.len + dyn_sections_len + dyn_in_rx;
    size_t rx_filesz = hdr_size + rx_content_len;
    size_t data_file_offset = rx_filesz;
    if (data_file_offset & (PAGE_SIZE - 1))
        data_file_offset = (data_file_offset + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
    uint64_t data_vaddr = base + data_file_offset;
    size_t got_data_off = data.len;
    while (got_data_off & 7) got_data_off++;
    uint64_t got_vaddr = data_vaddr + got_data_off;
    size_t layout_got_bytes = need_dynamic
        ? (size_t)(3 + num_ext + num_data_ext + num_tls_ie + num_ifunc) * 8
            + (tlsld_got_pair ? 16 : 0)
        : ((num_data_ext + num_tls_ie + num_ifunc) > 0
               ? (size_t)(3 + num_data_ext + num_tls_ie + num_ifunc) * 8 : 0);
    size_t got_end_off = layout_got_bytes
        ? got_data_off + layout_got_bytes : data.len;
    /* Shared: .dynamic follows the GOT in the RW segment. */
    size_t dynamic_data_off = got_end_off;
    if (is_shared && need_dynamic) {
        while (dynamic_data_off & 7) dynamic_data_off++;
    }
    size_t dynamic_rw_end = (is_shared && need_dynamic)
        ? dynamic_data_off + dynamic_size : got_end_off;

    /* If have_tls, .tdata lives right after GOT/.dynamic in the data segment so it is
     * resident in memory (part of RW PT_LOAD) and matches standard ELF layout. */
    size_t tdata_data_off = dynamic_rw_end;
    size_t tls_p_align = 16;
    if (max_td_align > tls_p_align) tls_p_align = max_td_align;
    if (max_tbss_align > tls_p_align) tls_p_align = max_tbss_align;
    if (have_tls) tdata_data_off = pad_size_to(tdata_data_off, tls_p_align);
    uint64_t tls_vaddr = data_vaddr + tdata_data_off;
    size_t tls_file_offset_base = data_file_offset + tdata_data_off;
    size_t rw_filesz = tdata_data_off + (have_tls ? tdata.len : 0);
    size_t initarr_data_off = pad_size_to(rw_filesz, 8);
    uint64_t initarr_vaddr = data_vaddr + initarr_data_off;
    size_t initarr_file_offset = data_file_offset + initarr_data_off;
    if (initarr.len > 0)
        rw_filesz = initarr_data_off + initarr.len;
    size_t finiarr_data_off = pad_size_to(rw_filesz, 8);
    uint64_t finiarr_vaddr = data_vaddr + finiarr_data_off;
    size_t finiarr_file_offset = data_file_offset + finiarr_data_off;
    if (finiarr.len > 0)
        rw_filesz = finiarr_data_off + finiarr.len;

    size_t bss_data_off = rw_filesz;
    bss_data_off = pad_size_to(bss_data_off, max_bss_align);
    uint64_t bss_vaddr = data_vaddr + bss_data_off;
    size_t bss_file_offset = data_file_offset + bss_data_off;

    /* ---- Compute final symbol addresses ---- */
    size_t *sym_addr = xcalloc(total_syms, sizeof(size_t));
    /* .tbss follows .tdata in the TLS template.  Object-file .tbss offsets
     * are section-relative (aligned to the start of .tbss), so the linker
     * must pad after .tdata until the combined TLS offset of .tbss is a
     * multiple of max_tbss_align; otherwise an aligned(32) tbss object
     * sitting after a 1-byte tdata lands at offset 1. */
    size_t tls_filesize = tdata.len;
    size_t tbss_tls_off = tdata.len;
    if (tbss_size > 0)
        tbss_tls_off = pad_size_to(tdata.len, max_tbss_align);
    size_t tls_memsize = tbss_tls_off + tbss_size;
    /* Reserve space in .bss for the initial thread's TLS block and TCB.
     * The image start must be p_align-aligned so object offsets keep their
     * declared alignment at run time (same contract as PT_TLS p_align). */
    size_t tls_bss_alloc_off = 0;
    if (have_tls) {
        size_t start = pad_size_to(bss_size, tls_p_align);
        size_t mis = (size_t)((bss_vaddr + start) % tls_p_align);
        if (mis) start += tls_p_align - mis;
        tls_bss_alloc_off = start;
        bss_size = start + tls_memsize + 16;
    }
    uint64_t tcb_vaddr = bss_vaddr + tls_bss_alloc_off + tls_memsize;
    /* The TLS template (PT_TLS) lives in memory right after GOT (part of RW PT_LOAD).
     * tdata (initialized) is file-resident, tbss (zero-init) is not.
     * tls_end_vaddr is p_vaddr + p_memsz; the TPOFF32 reloc writes
     * `S - tls_end_vaddr + A`, a negative offset from %fs:0 to the symbol. */
    uint64_t tls_end_vaddr = tls_vaddr + tls_memsize;
    /* tdata/tbss virtual offsets inside the TLS template */
    uint64_t tdata_vaddr = tls_vaddr;
    uint64_t tbss_vaddr = tls_vaddr + tbss_tls_off;
    for (size_t i = 0; i < n; i++) {
        EmitModule *m = mods[i];
        for (size_t j = 0; j < m->num_syms; j++) {
            size_t gsi = mod_sym_base[i] + j;
            const EmitSymbol *sym = &m->syms[j];
            if (sym->shndx == SECT_UNDEF) continue;
            switch (sym->shndx) {
            case SECT_TEXT:
                sym_addr[gsi] = code_vaddr + mod_text_off[i] + sym->value; break;
            case SECT_RODATA:
                sym_addr[gsi] = code_vaddr + text.len + mod_rodata_off[i] + sym->value; break;
            case SECT_DATA:
                sym_addr[gsi] = data_vaddr + mod_data_off[i] + sym->value; break;
            case SECT_BSS:
                sym_addr[gsi] = bss_vaddr + mod_bss_off[i] + sym->value; break;
            case SECT_TDATA:
                sym_addr[gsi] = tdata_vaddr + mod_tdata_off[i] + sym->value; break;
            case SECT_TBSS:
                sym_addr[gsi] = tbss_vaddr + mod_tbss_off[i] + sym->value; break;
            default:
                if (sym->shndx == SHN_COMMON) {
                    const char *nm = sym->name ? sym->name : "";
                    size_t win = find_export_gsi(mods, n, mod_sym_base, sinfo, nm);
                    if (win != (size_t)-1 && sinfo[win].shndx != SHN_COMMON)
                        break; /* filled in a second pass */
                    int ci;
                    for (ci = 0; ci < n_commons; ci++)
                        if (strcmp(commons[ci].name, nm) == 0) break;
                    if (ci < n_commons)
                        sym_addr[gsi] = bss_vaddr + commons[ci].off;
                } else {
                    sym_addr[gsi] = sym->value;
                }
                break;
            }
        }
    }
    for (size_t i = 0; i < n; i++) {
        EmitModule *m = mods[i];
        for (size_t j = 0; j < m->num_syms; j++) {
            size_t gsi = mod_sym_base[i] + j;
            if (m->syms[j].shndx != SHN_COMMON || sym_addr[gsi] != 0) continue;
            const char *nm = m->syms[j].name ? m->syms[j].name : "";
            size_t win = find_export_gsi(mods, n, mod_sym_base, sinfo, nm);
            if (win != (size_t)-1)
                sym_addr[gsi] = sym_addr[win];
        }
    }

    /* ---- Resolve static data GOT fill values ----
     * For cross-module data globals (data_got_external[j] == 0) the GOT slot is
     * filled at link time with the symbol's final address.  Truly-external
     * slots (data_got_external[j] == 1) are left for the dynamic linker, so
     * they need no static address here.  This needs sym_addr, hence runs after
     * layout. */
    size_t *data_got_addr = num_data_ext ? xcalloc(num_data_ext, sizeof(size_t)) : NULL;
    for (int j = 0; j < num_data_ext; j++) {
        if (data_got_external[j]) continue; /* external: dynlinker fills it */
        const char *nm = data_ext_list[j];
        size_t ogsi = find_export_gsi(mods, n, mod_sym_base, sinfo, nm);
        if (ogsi != (size_t)-1)
            data_got_addr[j] = sym_addr[ogsi];
    }

    /* Static TPOFF for each TLS IE GOT slot: S - tls_end.  Shared objects
     * leave the slot at 0 and emit TPOFF64 for the dynamic linker. */
    uint64_t *tls_ie_tpoff = num_tls_ie ? xcalloc((size_t)num_tls_ie, sizeof(uint64_t)) : NULL;
    uint64_t *tpoff_roff = NULL, *tpoff_add = NULL, *tpoff_info = NULL;
    int tpoff_fill = 0;
    if (num_tpoff_dyn > 0) {
        tpoff_roff = xcalloc((size_t)num_tpoff_dyn, sizeof(uint64_t));
        tpoff_add = xcalloc((size_t)num_tpoff_dyn, sizeof(uint64_t));
        tpoff_info = xcalloc((size_t)num_tpoff_dyn, sizeof(uint64_t));
    }
    for (int j = 0; j < num_tls_ie; j++) {
        size_t gsi = (size_t)tls_ie_gsi[j];
        uint64_t S = 0;
        if (sinfo[gsi].defined
            && (sinfo[gsi].shndx == SECT_TDATA || sinfo[gsi].shndx == SECT_TBSS)) {
            S = sym_addr[gsi];
        } else {
            const char *nm = tls_ie_name[j] ? tls_ie_name[j] : "";
            size_t win = find_export_gsi(mods, n, mod_sym_base, sinfo, nm);
            if (win == (size_t)-1
                || (sinfo[win].shndx != SECT_TDATA
                    && sinfo[win].shndx != SECT_TBSS)) {
                /* Undef IE: leave the GOT slot for a TPOFF64. */
                tls_ie_tpoff[j] = 0;
                if (tpoff_fill < num_tpoff_dyn) {
                    uint32_t dyn_sym = 0;
                    if (tls_ie_und_idx && tls_ie_und_idx[j] >= 0)
                        dyn_sym = (uint32_t)(dyn_tls_und0 + tls_ie_und_idx[j]);
                    tpoff_roff[tpoff_fill] = got_vaddr
                        + (uint64_t)(3 + num_ext + num_data_ext + j) * 8;
                    tpoff_add[tpoff_fill] = 0;
                    tpoff_info[tpoff_fill] =
                        ((uint64_t)dyn_sym << 32) | (uint64_t)R_X86_64_TPOFF64;
                    tpoff_fill++;
                }
                continue;
            }
            S = sym_addr[win];
        }
        tls_ie_tpoff[j] = (uint64_t)((int64_t)S - (int64_t)tls_end_vaddr);
        if (is_shared && tpoff_fill < num_tpoff_dyn) {
            uint32_t dyn_sym = 0;
            uint64_t addend = 0;
            const char *nm = tls_ie_name[j];
            if (nm) {
                for (int e = 0; e < num_exports; e++) {
                    if (strcmp(exports[e].name, nm) == 0) {
                        dyn_sym = (uint32_t)(dyn_export0 + e);
                        break;
                    }
                }
            }
            if (dyn_sym == 0)
                addend = S - tls_vaddr;
            tpoff_roff[tpoff_fill] = got_vaddr
                + (uint64_t)(3 + num_ext + num_data_ext + j) * 8;
            tpoff_add[tpoff_fill] = addend;
            tpoff_info[tpoff_fill] =
                ((uint64_t)dyn_sym << 32) | (uint64_t)R_X86_64_TPOFF64;
            tpoff_fill++;
        }
    }
    uint64_t *abs64_roff = NULL, *abs64_add = NULL, *abs64_info = NULL;
    int abs64_fill = 0;
    if (num_abs64_relocs > 0) {
        abs64_roff = xcalloc((size_t)num_abs64_relocs, sizeof(uint64_t));
        abs64_add = xcalloc((size_t)num_abs64_relocs, sizeof(uint64_t));
        abs64_info = xcalloc((size_t)num_abs64_relocs, sizeof(uint64_t));
    }
    size_t tls_image_doff = (size_t)-1;
    uint64_t tls_image_val = 0;
    /* ---- Fill builtin TLS metadata symbols in .data if present ---- */
    for (size_t mi = 0; mi < n; mi++) {
        EmitModule *om = mods[mi];
        for (size_t mj = 0; mj < om->num_syms; mj++) {
            const char *nm = om->syms[mj].name;
            if (!nm || om->syms[mj].shndx != SECT_DATA) continue;
            size_t doff = mod_data_off[mi] + om->syms[mj].value;
            if (doff + 8 > data.len) continue;
            if (strcmp(nm, "__fakecc_tls_filesz") == 0) {
                uint64_t val = have_tls ? tdata.len : 0;
                memcpy(data.data + doff, &val, 8);
            } else if (strcmp(nm, "__fakecc_tls_memsz") == 0) {
                uint64_t val = have_tls ? tls_memsize : 0;
                memcpy(data.data + doff, &val, 8);
            } else if (strcmp(nm, "__fakecc_tls_image") == 0) {
                uint64_t val = have_tls ? tls_vaddr : 0;
                memcpy(data.data + doff, &val, 8);
                tls_image_doff = doff;
                tls_image_val = val;
            } else if (strcmp(nm, "__fakecc_tls_align") == 0) {
                uint64_t val = have_tls ? tls_p_align : 16;
                memcpy(data.data + doff, &val, 8);
            }
        }
    }

    /* ---- Apply relocations ---- */
    for (size_t i = 0; i < n; i++) {
        EmitModule *m = mods[i];
        for (size_t r = 0; r < m->num_relocs; r++) {
            const EmitReloc *rel = &m->relocs[r];
            size_t patch_in_text = mod_text_off[i] + rel->offset;
            uint64_t P = code_vaddr + patch_in_text;
            if (reloc_is_gotpcrel(rel->type)) {
                /* Load &global from a data GOT entry: the disp targets the GOT
                 * slot, whose qword holds the symbol's address (filled below,
                 * either statically or via R_X86_64_GLOB_DAT).
                 * GOTPCRELX / REX_GOTPCRELX are the same S as GOTPCREL. */
                size_t gsi = mod_sym_base[i] + rel->sym;
                int dgidx = reloc_data_got_idx[gsi];
                uint64_t got_slot_vaddr = got_vaddr + (3 + num_ext + dgidx) * 8;
                int32_t disp = (int32_t)(got_slot_vaddr - (P + 4));
                memcpy(text.data + patch_in_text, &disp, 4);
                continue;
            }
            if (rel->type == R_X86_64_GOTTPOFF) {
                /* TLS Initial-Exec: RIP-relative disp to the GOT slot that
                 * holds this symbol's TPOFF (static fill or TPOFF64). */
                size_t gsi = mod_sym_base[i] + rel->sym;
                int ieidx = reloc_tls_ie_idx[gsi];
                uint64_t got_slot_vaddr = got_vaddr
                    + (uint64_t)(3 + num_ext + num_data_ext + ieidx) * 8;
                int32_t disp = (int32_t)(got_slot_vaddr - (P + 4));
                memcpy(text.data + patch_in_text, &disp, 4);
                continue;
            }
            if (reloc_is_tls_gd(rel->type) || reloc_is_tls_ld(rel->type)) {
                size_t gsi = mod_sym_base[i] + rel->sym;
                int ieidx = reloc_tls_ie_idx[gsi];
                uint64_t got_slot_vaddr = got_vaddr
                    + (uint64_t)(3 + num_ext + num_data_ext + ieidx) * 8;
                if (rel->type == R_X86_64_TLSGD && patch_in_text >= 4
                    && patch_in_text - 4 + 16 <= text.len) {
                    /* Relax TLSGD 16-byte lea+call __tls_get_addr to IE. */
                    size_t seq = patch_in_text - 4;
                    static const uint8_t ie[12] = {
                        0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00,
                        0x48, 0x03, 0x05
                    };
                    memcpy(text.data + seq, ie, 12);
                    uint64_t Pdisp = code_vaddr + seq + 16;
                    int32_t disp = (int32_t)(got_slot_vaddr - Pdisp);
                    memcpy(text.data + seq + 12, &disp, 4);
                } else if (rel->type == R_X86_64_TLSLD && patch_in_text >= 3
                           && patch_in_text - 3 + 12 <= text.len) {
                    if (!is_shared) {
                        /* 12-byte LE: movq %fs:0, %rax with prefixes. */
                        static const uint8_t le[12] = {
                            0x66, 0x66, 0x66, 0x64, 0x48, 0x8b, 0x04, 0x25,
                            0x00, 0x00, 0x00, 0x00
                        };
                        memcpy(text.data + patch_in_text - 3, le, 12);
                    } else {
                        /* Point the lea at a DTPMOD64 GOT pair; keep the call. */
                        uint64_t pair = got_vaddr
                            + (uint64_t)(3 + num_ext + num_data_ext
                                         + num_tls_ie + num_ifunc) * 8;
                        int32_t disp = (int32_t)(pair - (P + 4));
                        memcpy(text.data + patch_in_text, &disp, 4);
                    }
                }
                continue;
            }
            if (rel->type == R_X86_64_DTPOFF32) {
                size_t gsi = mod_sym_base[i] + rel->sym;
                uint64_t S = 0;
                if (sinfo[gsi].defined
                    && (sinfo[gsi].shndx == SECT_TDATA
                        || sinfo[gsi].shndx == SECT_TBSS)) {
                    S = sym_addr[gsi];
                } else {
                    const char *nm = m->syms[rel->sym].name
                                     ? m->syms[rel->sym].name : "";
                    size_t win = find_export_gsi(mods, n, mod_sym_base, sinfo, nm);
                    if (win != (size_t)-1
                        && (sinfo[win].shndx == SECT_TDATA
                            || sinfo[win].shndx == SECT_TBSS))
                        S = sym_addr[win];
                    else if (sinfo[gsi].defined)
                        S = sym_addr[gsi];
                    else
                        S = tls_vaddr;
                }
                int32_t disp = is_shared
                    ? (int32_t)((int64_t)(S + rel->addend) - (int64_t)tls_vaddr)
                    : (int32_t)((int64_t)(S + rel->addend) - (int64_t)tls_end_vaddr);
                memcpy(text.data + patch_in_text, &disp, 4);
                continue;
            }
            if (rel->type == R_X86_64_TPOFF32) {
                /* TLS Local-Exec in an executable (legacy objects).  Shared
                 * objects cannot use TPOFF32 — ld.so rejects reloc type 0x17. */
                size_t gsi = mod_sym_base[i] + rel->sym;
                if (is_shared) {
                    fprintf(stderr,
                            "fakecc: R_X86_64_TPOFF32 cannot be used in a "
                            "shared object (need Initial-Exec GOTTPOFF)\n");
                    exit(1);
                }
                uint64_t S;
                if (sinfo[gsi].defined
                    && (sinfo[gsi].shndx == SECT_TDATA
                        || sinfo[gsi].shndx == SECT_TBSS)) {
                    S = sym_addr[gsi];
                } else {
                    /* Undefined locally — look for a GLOBAL TLS definition in
                     * another module.  No fallback: Local-Exec cannot
                     * resolve truly external symbols. */
                    const char *nm = m->syms[rel->sym].name
                                     ? m->syms[rel->sym].name : "";
                    size_t win = find_export_gsi(mods, n, mod_sym_base, sinfo, nm);
                    if (win == (size_t)-1
                        || (sinfo[win].shndx != SECT_TDATA
                            && sinfo[win].shndx != SECT_TBSS)) {
                        fprintf(stderr,
                                "fakecc: undefined TLS symbol '%s' "
                                "(Local-Exec needs the variable to be defined "
                                "in the same link unit)\n", nm);
                        exit(1);
                    }
                    S = sym_addr[win];
                }
                int32_t disp = (int32_t)((int64_t)(S + rel->addend) - (int64_t)tls_end_vaddr);
                memcpy(text.data + patch_in_text, &disp, 4);
                continue;
            }
            if (rel->type == R_X86_64_PLT32) {
                int skip_gd_call = 0;
                for (size_t r2 = 0; r2 < m->num_relocs; r2++) {
                    if (m->relocs[r2].type == R_X86_64_TLSGD
                        && m->relocs[r2].offset + 8 == rel->offset) {
                        skip_gd_call = 1;
                        break;
                    }
                    if (!is_shared && m->relocs[r2].type == R_X86_64_TLSLD
                        && m->relocs[r2].offset + 5 == rel->offset) {
                        skip_gd_call = 1;
                        break;
                    }
                }
                if (skip_gd_call) continue;
            }
            size_t gsi = mod_sym_base[i] + rel->sym;
            uint64_t S;
            const char *rnm = m->syms[rel->sym].name
                              ? m->syms[rel->sym].name : "";
            size_t found = find_export_gsi(mods, n, mod_sym_base, sinfo, rnm);
            int local_strong = sinfo[gsi].defined
                && sinfo[gsi].binding == STB_GLOBAL
                && sinfo[gsi].shndx != SHN_COMMON;
            if (local_strong) {
                S = sym_addr[gsi];
            } else if (found != (size_t)-1) {
                S = sym_addr[found];
            } else if (sinfo[gsi].defined) {
                S = sym_addr[gsi];
            } else if (reloc_copy_idx[gsi] >= 0) {
                S = bss_vaddr + copy_off[reloc_copy_idx[gsi]];
            } else {
                int eidx = reloc_ext_idx[gsi];
                S = code_vaddr + (eidx >= 0 ? plt_entry_off[eidx] : plt0_off);
            }
            {
                int ik = -1;
                const char *inm = rnm;
                for (int k = 0; k < num_ifunc; k++) {
                    if (ifunc_gsi[k] == gsi) { ik = k; break; }
                    if (inm && ifunc_name[k] && strcmp(ifunc_name[k], inm) == 0) {
                        ik = k; break;
                    }
                }
                if (ik >= 0)
                    S = code_vaddr + ifunc_thunk_off[ik];
            }
            /* 32 / 32S are 4-byte S+A; 64 is 8-byte S+A; PC64 is 8-byte
             * S+A−P.  Remaining .text relocs are 4-byte PC32 / PLT32. */
            if (rel->type == R_X86_64_64) {
                uint64_t abs64 = S + (uint64_t)(int64_t)rel->addend;
                memcpy(text.data + patch_in_text, &abs64, 8);
            } else if (rel->type == R_X86_64_PC64) {
                uint64_t disp64 = S + (uint64_t)(int64_t)rel->addend - P;
                memcpy(text.data + patch_in_text, &disp64, 8);
            } else if (rel->type == R_X86_64_32 || rel->type == R_X86_64_32S) {
                int32_t abs32 = (int32_t)(S + rel->addend);
                memcpy(text.data + patch_in_text, &abs32, 4);
            } else {
                int32_t disp = (int32_t)(S + rel->addend - P);
                memcpy(text.data + patch_in_text, &disp, 4);
            }
        }
    }

    /* ---- Apply data relocations (pointer fixups in .data) ---- */
    uint64_t *rel_roff = NULL;
    uint64_t *rel_add = NULL;
    int rel_fill = 0;
    if (is_shared && num_relative > 0) {
        rel_roff = xcalloc((size_t)num_relative, sizeof(uint64_t));
        rel_add = xcalloc((size_t)num_relative, sizeof(uint64_t));
    }
    for (size_t i = 0; i < n; i++) {
        EmitModule *m = mods[i];
        for (size_t r = 0; r < m->num_data_relocs; r++) {
            const EmitReloc *rel = &m->data_relocs[r];
            int in_tdata = (rel->shndx == SECT_TDATA);
            int in_rodata = (rel->shndx == SECT_RODATA);
            int in_init = (rel->shndx == SECT_INIT_ARRAY);
            int in_fini = (rel->shndx == SECT_FINI_ARRAY);
            size_t patch_in_sec = (in_tdata ? mod_tdata_off[i]
                                  : in_rodata ? mod_rodata_off[i]
                                  : in_init ? mod_init_off[i]
                                  : in_fini ? mod_fini_off[i]
                                  : mod_data_off[i])
                                  + rel->offset;
            if (in_init && init_remap && (patch_in_sec / 8) < (initarr.len / 8))
                patch_in_sec = init_remap[patch_in_sec / 8];
            if (in_fini && fini_remap && (patch_in_sec / 8) < (finiarr.len / 8))
                patch_in_sec = fini_remap[patch_in_sec / 8];
            size_t gsi = mod_sym_base[i] + rel->sym;
            uint64_t S;
            int abs64 = 0;
            int aidx = reloc_data_abs_idx[gsi];
            const char *dnm = m->syms[rel->sym].name
                              ? m->syms[rel->sym].name : "";
            size_t dfound = find_export_gsi(mods, n, mod_sym_base, sinfo, dnm);
            int local_keep = sinfo[gsi].defined
                && sinfo[gsi].shndx != SHN_COMMON
                && sinfo[gsi].binding != STB_WEAK;
            if (local_keep) {
                S = sym_addr[gsi];
            } else if (dfound != (size_t)-1) {
                S = sym_addr[dfound];
            } else if (sinfo[gsi].defined) {
                S = sym_addr[gsi];
            } else if (reloc_copy_idx[gsi] >= 0) {
                S = bss_vaddr + copy_off[reloc_copy_idx[gsi]];
            } else if (reloc_ext_idx[gsi] >= 0) {
                S = code_vaddr + plt_entry_off[reloc_ext_idx[gsi]];
            } else if (aidx >= 0) {
                S = 0;
                abs64 = 1;
            } else {
                fprintf(stderr,
                        "fakecc: data reloc against undefined '%s' "
                        "has no PLT or GLOB_DAT slot\n", dnm);
                exit(1);
            }
            {
                int ik = -1;
                for (int k = 0; k < num_ifunc; k++) {
                    if (ifunc_gsi[k] == gsi) { ik = k; break; }
                    if (dnm && ifunc_name[k] && strcmp(ifunc_name[k], dnm) == 0) {
                        ik = k; break;
                    }
                }
                if (ik >= 0)
                    S = code_vaddr + ifunc_thunk_off[ik];
            }
            /* Apply by reloc type: 64/PC64 are 8 bytes; 32/32S/PC32 are 4. */
            uint64_t A = (uint64_t)(int64_t)rel->addend;
            uint8_t *dst;
            size_t dst_len;
            uint64_t P;
            if (in_tdata) {
                dst = (uint8_t *)tdata.data;
                dst_len = tdata.len;
                P = tls_vaddr + patch_in_sec;
            } else if (in_rodata) {
                dst = (uint8_t *)rodata.data;
                dst_len = rodata.len;
                P = code_vaddr + text.len + patch_in_sec;
            } else if (in_init) {
                dst = (uint8_t *)initarr.data;
                dst_len = initarr.len;
                P = initarr_vaddr + patch_in_sec;
            } else if (in_fini) {
                dst = (uint8_t *)finiarr.data;
                dst_len = finiarr.len;
                P = finiarr_vaddr + patch_in_sec;
            } else {
                dst = (uint8_t *)data.data;
                dst_len = data.len;
                P = data_vaddr + patch_in_sec;
            }
            uint64_t value;
            int width = reloc_width(rel->type);
            if (reloc_is_pc(rel->type))
                value = S + A - P;
            else
                value = S + A;
            if (patch_in_sec + (size_t)width <= dst_len)
                memcpy(dst + patch_in_sec, &value, (size_t)width);
            uint64_t site_va = P;
            if (abs64 && abs64_fill < num_abs64_relocs) {
                abs64_roff[abs64_fill] = site_va;
                abs64_add[abs64_fill] = A;
                abs64_info[abs64_fill] =
                    ((uint64_t)(1 + num_ext + num_true_data_ext + aidx) << 32)
                    | R_X86_64_64;
                abs64_fill++;
            } else if (is_shared && rel_fill < num_relative
                       && !reloc_is_pc(rel->type) && !reloc_is_abs32(rel->type)) {
                rel_roff[rel_fill] = site_va;
                rel_add[rel_fill] = value;
                rel_fill++;
            }
        }
    }
    if (is_shared && tls_image_rel && tls_image_doff != (size_t)-1
        && rel_fill < num_relative) {
        rel_roff[rel_fill] = data_vaddr + tls_image_doff;
        rel_add[rel_fill] = tls_image_val;
        rel_fill++;
    }

    /* ---- Patch PLT GOT fixups ---- */
    if (num_ext > 0) {
        for (int f = 0; f < 2; f++) {
            uint64_t target = got_vaddr + (1 + f) * 8;
            uint64_t rip_next = code_vaddr + plt0_got_fixup[f] + 4;
            int32_t disp = (int32_t)((int64_t)target - (int64_t)rip_next);
            memcpy(text.data + plt0_got_fixup[f], &disp, 4);
        }
        for (int e = 0; e < num_ext; e++) {
            uint64_t target = got_vaddr + (3 + e) * 8;
            uint64_t rip_next = code_vaddr + plt_got_fixup[e] + 4;
            int32_t disp = (int32_t)((int64_t)target - (int64_t)rip_next);
            memcpy(text.data + plt_got_fixup[e], &disp, 4);
        }
    }
    for (int k = 0; k < num_ifunc; k++) {
        uint64_t target = got_vaddr
            + (uint64_t)(3 + num_ext + num_data_ext + num_tls_ie + k) * 8;
        uint64_t rip_next = code_vaddr + ifunc_got_fixup[k] + 4;
        int32_t disp = (int32_t)((int64_t)target - (int64_t)rip_next);
        memcpy(text.data + ifunc_got_fixup[k], &disp, 4);
    }

    /* ---- Find main (and optional static exit for the entry stub) ---- */
    uint64_t main_addr = 0;
    uint64_t exit_static_addr = 0;
    int found_main = 0;
    if (!is_shared) {
        for (size_t i = 0; i < n; i++) {
            EmitModule *m = mods[i];
            for (size_t j = 0; j < m->num_syms; j++) {
                const EmitSymbol *sym = &m->syms[j];
                if (!sym->name || sym->shndx != SECT_TEXT || sym->binding != 1)
                    continue;
                if (strcmp(sym->name, "main") == 0) {
                    main_addr = code_vaddr + mod_text_off[i] + sym->value;
                    found_main = 1;
                }
                if (strcmp(sym->name, "exit") == 0)
                    exit_static_addr = code_vaddr + mod_text_off[i] + sym->value;
            }
        }
        if (!found_main) {
            fprintf(stderr, "fakecc: no 'main' function found\n");
            exit(1);
        }
    }

    /* ================================================================ */
    /* Finalize output                                                  */
    /* ================================================================ */
    if (need_dynamic) {
        /* Patch dynsym st_name fields. */
        {
            size_t acc = 1 + needed_str_bytes + soname_str_bytes + runpath_str_bytes;
            int k = 0;
            for (int i = 0; i < num_ext; i++, k++) {
                uint32_t noff = (uint32_t)acc;
                memcpy(dynsym.data + 24 + (size_t)k * 24, &noff, 4);
                acc += strlen(ext_list[i]) + 1;
            }
            for (int j = 0; j < num_data_ext; j++) {
                if (!data_got_external[j]) continue;
                uint32_t noff = (uint32_t)acc;
                memcpy(dynsym.data + 24 + (size_t)k * 24, &noff, 4);
                acc += strlen(data_ext_list[j]) + 1;
                k++;
            }
            for (int a = 0; a < num_abs_ext; a++, k++) {
                uint32_t noff = (uint32_t)acc;
                memcpy(dynsym.data + 24 + (size_t)k * 24, &noff, 4);
                acc += strlen(abs_ext_list[a]) + 1;
            }
            for (int t = 0; t < num_tls_und; t++, k++) {
                uint32_t noff = (uint32_t)acc;
                memcpy(dynsym.data + 24 + (size_t)k * 24, &noff, 4);
                acc += strlen(tls_und_list[t]) + 1;
            }
            for (int c = 0; c < num_copy; c++, k++) {
                uint32_t noff = (uint32_t)acc;
                size_t ent = 24 + (size_t)k * 24;
                memcpy(dynsym.data + ent, &noff, 4);
                uint64_t val = bss_vaddr + copy_off[c];
                memcpy(dynsym.data + ent + 8, &val, 8);
                acc += strlen(copy_list[c]) + 1;
            }
            for (int e = 0; e < num_exports; e++, k++) {
                uint32_t noff = (uint32_t)acc;
                size_t ent = 24 + (size_t)k * 24;
                memcpy(dynsym.data + ent, &noff, 4);
                uint64_t val = sym_addr[exports[e].gsi];
                /* STT_TLS st_value is the offset from the start of this
                 * object's TLS template, not a load virtual address. */
                if (exports[e].type == 6 /* STT_TLS */
                    || exports[e].shndx == SECT_TDATA
                    || exports[e].shndx == SECT_TBSS)
                    val = val - tls_vaddr;
                memcpy(dynsym.data + ent + 8, &val, 8);
                acc += strlen(exports[e].name) + 1;
            }
        }
        /* Patch rela.plt r_offsets (function externals). */
        for (int i = 0; i < num_ext; i++) {
            uint64_t roff = got_vaddr + (3 + i) * 8;
            memcpy(rela_plt.data + (size_t)i * 24, &roff, 8);
        }
        /* Patch rela.dyn r_offsets (truly-external data variables). */
        {
            int rdj = 0;
            for (int j = 0; j < num_data_ext; j++) {
                if (!data_got_external[j]) continue;
                uint64_t roff = got_vaddr + (3 + num_ext + j) * 8;
                memcpy(rela_dyn.data + (size_t)rdj * 24, &roff, 8);
                rdj++;
            }
            for (int a = 0; a < abs64_fill; a++, rdj++) {
                size_t ent = (size_t)rdj * 24;
                memcpy(rela_dyn.data + ent, &abs64_roff[a], 8);
                memcpy(rela_dyn.data + ent + 8, &abs64_info[a], 8);
                memcpy(rela_dyn.data + ent + 16, &abs64_add[a], 8);
            }
            /* R_X86_64_RELATIVE for pointer initializers, internal GOT slots,
             * then GOT[0] (= link-time &_DYNAMIC). */
            if (num_relative > 0 && rel_roff) {
                for (int j = 0; j < num_data_ext; j++) {
                    if (data_got_external[j]) continue;
                    if (rel_fill < num_relative) {
                        rel_roff[rel_fill] = got_vaddr + (3 + num_ext + j) * 8;
                        rel_add[rel_fill] = data_got_addr[j];
                        rel_fill++;
                    }
                }
                uint64_t dyn_vaddr = is_shared
                    ? (data_vaddr + dynamic_data_off)
                    : (base + hdr_size + start_size + text.len + rodata.len + interp_len
                       + dynstr.len + dynsym.len + hash.len + rela_plt.len + rela_dyn.len);
                if (rel_fill < num_relative) {
                    rel_roff[rel_fill] = got_vaddr;
                    rel_add[rel_fill] = dyn_vaddr;
                    rel_fill++;
                }
                for (int r = 0; r < rel_fill; r++) {
                    size_t ent = (size_t)(rdj + r) * 24;
                    memcpy(rela_dyn.data + ent, &rel_roff[r], 8);
                    memcpy(rela_dyn.data + ent + 16, &rel_add[r], 8);
                }
                for (int r = rel_fill; r < num_relative; r++) {
                    size_t ent = (size_t)(rdj + r) * 24;
                    uint64_t none = 0;
                    memcpy(rela_dyn.data + ent + 8, &none, 8); /* R_X86_64_NONE */
                }
                rdj += num_relative;
            }
            for (int t = 0; t < tpoff_fill; t++, rdj++) {
                size_t ent = (size_t)rdj * 24;
                memcpy(rela_dyn.data + ent, &tpoff_roff[t], 8);
                memcpy(rela_dyn.data + ent + 8, &tpoff_info[t], 8);
                memcpy(rela_dyn.data + ent + 16, &tpoff_add[t], 8);
            }
            for (int c = 0; c < num_copy; c++, rdj++) {
                uint64_t roff = bss_vaddr + copy_off[c];
                memcpy(rela_dyn.data + (size_t)rdj * 24, &roff, 8);
            }
            for (int k = 0; k < num_ifunc; k++, rdj++) {
                size_t ent = (size_t)rdj * 24;
                uint64_t roff = got_vaddr
                    + (uint64_t)(3 + num_ext + num_data_ext + num_tls_ie + k) * 8;
                uint64_t add = sym_addr[ifunc_gsi[k]];
                memcpy(rela_dyn.data + ent, &roff, 8);
                memcpy(rela_dyn.data + ent + 16, &add, 8);
            }
            if (tlsld_got_pair) {
                uint64_t roff = got_vaddr
                    + (uint64_t)(3 + num_ext + num_data_ext + num_tls_ie + num_ifunc) * 8;
                memcpy(rela_dyn.data + (size_t)rdj * 24, &roff, 8);
                rdj++;
            }
        }
        /* .dynamic */
        size_t rx_base_vaddr = base + hdr_size;
        size_t dynstr_off = start_size + text.len + rodata.len + interp_len;
        size_t dynsym_off = dynstr_off + dynstr.len;
        size_t hash_off = dynsym_off + dynsym.len;
        size_t rela_plt_off = hash_off + hash.len;
        size_t rela_dyn_off = rela_plt_off + rela_plt.len;
        /* File offset / vaddr of .dynamic: RW for shared, RX for executables. */
        size_t dynamic_file_off = is_shared
            ? (data_file_offset + dynamic_data_off)
            : (hdr_size + rela_dyn_off + rela_dyn.len);
        uint64_t dynamic_vaddr = is_shared
            ? (data_vaddr + dynamic_data_off)
            : (rx_base_vaddr + rela_dyn_off + rela_dyn.len);
        uint64_t dynstr_vaddr = rx_base_vaddr + dynstr_off;
        /* DT_NEEDED values are byte offsets into .dynstr. */
        {
            size_t off = 1; /* skip the leading NUL */
            for (int i = 0; i < num_needed; i++) {
                buf_u64(&dynamic, DT_NEEDED);
                buf_u64(&dynamic, off);
                off += strlen(needed[i]) + 1;
            }
        }
        if (soname) {
            buf_u64(&dynamic, DT_SONAME);
            buf_u64(&dynamic, soname_dynstr_off);
        }
        if (runpath) {
            buf_u64(&dynamic, DT_RUNPATH);
            buf_u64(&dynamic, runpath_dynstr_off);
        }
        buf_u64(&dynamic, DT_STRTAB);   buf_u64(&dynamic, dynstr_vaddr);
        buf_u64(&dynamic, DT_SYMTAB);   buf_u64(&dynamic, rx_base_vaddr + dynsym_off);
        buf_u64(&dynamic, DT_SYMENT);   buf_u64(&dynamic, 24);
        buf_u64(&dynamic, DT_STRSZ);    buf_u64(&dynamic, dynstr.len);
        buf_u64(&dynamic, DT_HASH);     buf_u64(&dynamic, rx_base_vaddr + hash_off);
        buf_u64(&dynamic, DT_PLTGOT);   buf_u64(&dynamic, got_vaddr);
        buf_u64(&dynamic, DT_PLTRELSZ); buf_u64(&dynamic, rela_plt.len);
        buf_u64(&dynamic, DT_PLTREL);   buf_u64(&dynamic, DT_RELA);
        buf_u64(&dynamic, DT_JMPREL);   buf_u64(&dynamic, rx_base_vaddr + rela_plt_off);
        if (rela_dyn.len > 0) {
            buf_u64(&dynamic, DT_RELA);     buf_u64(&dynamic, rx_base_vaddr + rela_dyn_off);
            buf_u64(&dynamic, DT_RELASZ);   buf_u64(&dynamic, rela_dyn.len);
            buf_u64(&dynamic, DT_RELENT);   buf_u64(&dynamic, 24);
        }
        if (is_shared && num_tls_ie > 0) {
            buf_u64(&dynamic, DT_FLAGS);
            buf_u64(&dynamic, DF_STATIC_TLS);
        }
        if (initarr.len > 0) {
            buf_u64(&dynamic, DT_INIT_ARRAY);
            buf_u64(&dynamic, initarr_vaddr);
            buf_u64(&dynamic, DT_INIT_ARRAYSZ);
            buf_u64(&dynamic, initarr.len);
        }
        if (finiarr.len > 0) {
            buf_u64(&dynamic, DT_FINI_ARRAY);
            buf_u64(&dynamic, finiarr_vaddr);
            buf_u64(&dynamic, DT_FINI_ARRAYSZ);
            buf_u64(&dynamic, finiarr.len);
        }
        buf_u64(&dynamic, DT_NULL);     buf_u64(&dynamic, 0);

        /* ---- Assemble RX segment content ---- */
        Buffer rx;
        buffer_init(&rx);
        if (!is_shared) {
            uint64_t exit_call = 0;
            if (exit_ext_idx >= 0)
                exit_call = code_vaddr + plt_entry_off[exit_ext_idx];
            else if (exit_static_addr)
                exit_call = exit_static_addr;
            StartInfo sti;
            memset(&sti, 0, sizeof(sti));
            sti.call_vaddr = base + start_offset;
            sti.main_vaddr = main_addr;
            sti.exit_plt_vaddr = exit_call;
            sti.have_tls = init_tls_in_start;
            sti.tls_vaddr = tls_vaddr;
            sti.tcb_vaddr = tcb_vaddr;
            sti.tls_memsize = tls_memsize;
            sti.tdata_len = tdata.len;
            gen_start(&rx, &sti);
        }
        buf_bytes(&rx, text.data, text.len);
        buf_bytes(&rx, rodata.data, rodata.len);
        size_t interp_off = rx.len;
        if (interp_len)
            buf_bytes(&rx, INTERP_PATH, interp_len);
        buf_bytes(&rx, dynstr.data, dynstr.len);
        buf_bytes(&rx, dynsym.data, dynsym.len);
        buf_bytes(&rx, hash.data, hash.len);
        buf_bytes(&rx, rela_plt.data, rela_plt.len);
        buf_bytes(&rx, rela_dyn.data, rela_dyn.len);
        if (!is_shared)
            buf_bytes(&rx, dynamic.data, dynamic.len);

        uint64_t entry = is_shared ? 0 : (base + start_offset);
        Buffer got;
        buffer_init(&got);
        buf_u64(&got, dynamic_vaddr); /* GOT[0] */
        buf_u64(&got, 0); /* GOT[1] */
        buf_u64(&got, 0); /* GOT[2] */
        for (int i = 0; i < num_ext; i++)
            buf_u64(&got, code_vaddr + plt_entry_off[i] + 6); /* push $i addr */
        for (int j = 0; j < num_data_ext; j++)
            buf_u64(&got, data_got_external[j] ? 0 : data_got_addr[j]);
        for (int j = 0; j < num_tls_ie; j++)
            buf_u64(&got, is_shared ? 0 : tls_ie_tpoff[j]);
        for (int k = 0; k < num_ifunc; k++)
            buf_u64(&got, 0);
        if (tlsld_got_pair) {
            buf_u64(&got, 0);
            buf_u64(&got, 0);
        }

        /* ---- Build ELF ---- */
        /* NOTE: phnum computed AFTER buffer_init() to avoid a codegen
         * limitation where a value in a caller-saved register is not spilled
         * across a function call. */
        Buffer elf;
        buffer_init(&elf);
        uint16_t phnum;
        if (is_shared)
            phnum = have_tls ? 5 : 4; /* RX, RW, DYNAMIC, GNU_STACK, [TLS] */
        else
            phnum = have_tls ? 6 : 5; /* RX, RW, INTERP, DYNAMIC, GNU_STACK, [TLS] */
        write_ehdr(&elf, is_shared ? ET_DYN : ET_EXEC, entry, ELF64_EHDR_SIZE, phnum);
        write_phdr(&elf, PT_LOAD, PF_R | PF_X, 0, base, rx_filesz, rx_filesz, PAGE_SIZE);
        write_phdr(&elf, PT_LOAD, PF_R | PF_W, data_file_offset, data_vaddr,
                   rw_filesz,
                   rw_filesz + bss_size, PAGE_SIZE);
        if (!is_shared) {
            write_phdr(&elf, PT_INTERP, PF_R, hdr_size + interp_off, rx_base_vaddr + interp_off,
                       interp_len, interp_len, 1);
        }
        write_phdr(&elf, PT_DYNAMIC, PF_R | PF_W, dynamic_file_off, dynamic_vaddr,
                   dynamic.len, dynamic.len, 8);
        if (have_tls) {
            write_phdr(&elf, PT_TLS, PF_R, tls_file_offset_base, tls_vaddr,
                       tls_filesize, tls_memsize, tls_p_align);
        }
        write_phdr(&elf, PT_GNU_STACK, PF_R | PF_W, 0, 0, 0, 0, 16);
        buf_bytes(&elf, rx.data, rx.len);
        while (elf.len < data_file_offset) buf_u8(&elf, 0);
        buf_bytes(&elf, data.data, data.len);
        while (elf.len < data_file_offset + got_data_off) buf_u8(&elf, 0);
        buf_bytes(&elf, got.data, got.len);
        if (is_shared) {
            while (elf.len < dynamic_file_off) buf_u8(&elf, 0);
            buf_bytes(&elf, dynamic.data, dynamic.len);
        }
        if (have_tls) {
            while (elf.len < tls_file_offset_base) buf_u8(&elf, 0);
            buf_bytes(&elf, tdata.data, tdata.len);
        }
        if (initarr.len > 0) {
            while (elf.len < initarr_file_offset) buf_u8(&elf, 0);
            buf_bytes(&elf, initarr.data, initarr.len);
        }
        if (finiarr.len > 0) {
            while (elf.len < finiarr_file_offset) buf_u8(&elf, 0);
            buf_bytes(&elf, finiarr.data, finiarr.len);
        }
        SectionLayout lay;
        memset(&lay, 0, sizeof(lay));
        lay.code_vaddr = code_vaddr;
        lay.data_vaddr = data_vaddr;
        lay.bss_vaddr = bss_vaddr;
        lay.text_offset = text_offset;
        lay.data_file_offset = data_file_offset;
        lay.text_len = text.len;
        lay.rodata_len = rodata.len;
        lay.data_len = data.len;
        lay.bss_file_offset = bss_file_offset;
        lay.bss_size = bss_size;
        lay.have_tls = have_tls;
        lay.is_shared = is_shared;
        lay.start_size = start_size;
        lay.tls_vaddr = tls_vaddr;
        lay.tls_file_offset = tls_file_offset_base;
        lay.tls_filesize = tls_filesize;
        lay.tls_memsize = tls_memsize;
        lay.have_initarr = initarr.len > 0;
        lay.initarr_vaddr = initarr_vaddr;
        lay.initarr_file_offset = initarr_file_offset;
        lay.initarr_size = initarr.len;
        lay.have_finiarr = finiarr.len > 0;
        lay.finiarr_vaddr = finiarr_vaddr;
        lay.finiarr_file_offset = finiarr_file_offset;
        lay.finiarr_size = finiarr.len;
        /* RX content is written right after the program headers (file offset
         * hdr_size), so each dynamic section's file offset is hdr_size + its
         * offset inside the rx buffer.  vaddrs already include base+rx_base. */
        lay.have_dynamic = 1;
        lay.dynstr_off = hdr_size + dynstr_off;
        lay.dynsym_off = hdr_size + dynsym_off;
        lay.hash_off = hdr_size + hash_off;
        lay.rela_plt_off = hdr_size + rela_plt_off;
        lay.rela_dyn_off = hdr_size + rela_dyn_off;
        lay.dynamic_off = dynamic_file_off;
        lay.dynstr_size = dynstr.len;
        lay.dynsym_size = dynsym.len;
        lay.hash_size = hash.len;
        lay.rela_plt_size = rela_plt.len;
        lay.rela_dyn_size = rela_dyn.len;
        lay.dynamic_size = dynamic.len;
        lay.dynstr_vaddr = rx_base_vaddr + dynstr_off;
        lay.dynsym_vaddr = rx_base_vaddr + dynsym_off;
        lay.hash_vaddr = rx_base_vaddr + hash_off;
        lay.rela_plt_vaddr = rx_base_vaddr + rela_plt_off;
        lay.rela_dyn_vaddr = rx_base_vaddr + rela_dyn_off;
        lay.dynamic_vaddr = dynamic_vaddr;
        finalize_sections(&elf, mods, n, mod_text_off, mod_sym_base, sym_addr,
                          &lay, entry, want_debug);

        FILE *f = fopen(path, "wb");
        if (!f) { fprintf(stderr, "fakecc: cannot write '%s'\n", path); exit(1); }
        fwrite(elf.data, 1, elf.len, f);
        fclose(f);
        chmod(path, is_shared ? 0644 : 0755);

        buffer_free(&rx); buffer_free(&got); buffer_free(&elf);
    } else {
        /* ---- Static executable (no truly-external symbols) ---- */
        uint64_t entry = base + start_offset;
        Buffer rx;
        buffer_init(&rx);
        StartInfo sti;
        memset(&sti, 0, sizeof(sti));
        sti.call_vaddr = base + start_offset;
        sti.main_vaddr = main_addr;
        sti.exit_plt_vaddr = exit_static_addr;
        sti.have_tls = init_tls_in_start;
        sti.tls_vaddr = tls_vaddr;
        sti.tcb_vaddr = tcb_vaddr;
        sti.tls_memsize = tls_memsize;
        sti.tdata_len = tdata.len;
        sti.init_start = run_ctors ? initarr_vaddr : 0;
        sti.init_count = run_ctors ? (uint64_t)(initarr.len / 8) : 0;
        sti.fini_start = run_dtors ? finiarr_vaddr : 0;
        sti.fini_count = run_dtors ? (uint64_t)(finiarr.len / 8) : 0;
        sti.irel_got = run_irel
            ? (got_vaddr + (uint64_t)(3 + num_data_ext + num_tls_ie) * 8) : 0;
        sti.irel_count = run_irel ? (uint64_t)num_ifunc : 0;
        gen_start(&rx, &sti);
        buf_bytes(&rx, text.data, text.len);
        buf_bytes(&rx, rodata.data, rodata.len);

        /* Optional GOT for cross-module data (GOTPCREL filled at link time). */
        size_t got_bytes = 0;
        Buffer got;
        buffer_init(&got);
        if (num_data_ext > 0 || num_tls_ie > 0 || num_ifunc > 0) {
            got_bytes = (size_t)(3 + num_data_ext + num_tls_ie + num_ifunc) * 8;
            buf_u64(&got, 0);
            buf_u64(&got, 0);
            buf_u64(&got, 0);
            for (int j = 0; j < num_data_ext; j++)
                buf_u64(&got, data_got_addr[j]);
            for (int j = 0; j < num_tls_ie; j++)
                buf_u64(&got, tls_ie_tpoff[j]);
            for (int k = 0; k < num_ifunc; k++)
                buf_u64(&got, sym_addr[ifunc_gsi[k]]);
        }

        Buffer elf;
        buffer_init(&elf);
        int has_rw = (data.len > 0 || bss_size > 0 || got_bytes > 0
                      || (have_tls && tdata.len > 0) || initarr.len > 0
                      || finiarr.len > 0);
        uint16_t phnum = has_rw ? 2 : 1;
        if (have_tls) phnum++;
        phnum++; /* PT_GNU_STACK */
        write_ehdr(&elf, ET_EXEC, entry, ELF64_EHDR_SIZE, phnum);
        write_phdr(&elf, PT_LOAD, PF_R | PF_X, 0, base,
                   rx_filesz, rx_filesz, PAGE_SIZE);
        if (has_rw) {
            write_phdr(&elf, PT_LOAD, PF_R | PF_W, data_file_offset, data_vaddr,
                       rw_filesz, rw_filesz + bss_size, PAGE_SIZE);
        }
        if (have_tls) {
            write_phdr(&elf, PT_TLS, PF_R, tls_file_offset_base, tls_vaddr,
                       tls_filesize, tls_memsize, tls_p_align);
        }
        write_phdr(&elf, PT_GNU_STACK, PF_R | PF_W, 0, 0, 0, 0, 16);
        while (elf.len < hdr_size)
            buf_u8(&elf, 0);
        buf_bytes(&elf, rx.data, rx.len);
        if (has_rw) {
            while (elf.len < data_file_offset) buf_u8(&elf, 0);
            buf_bytes(&elf, data.data, data.len);
            if (got_bytes > 0)
                while (elf.len < data_file_offset + got_data_off) buf_u8(&elf, 0);
            buf_bytes(&elf, got.data, got.len);
        }
        if (have_tls) {
            while (elf.len < tls_file_offset_base) buf_u8(&elf, 0);
            buf_bytes(&elf, tdata.data, tdata.len);
        }
        if (initarr.len > 0) {
            while (elf.len < initarr_file_offset) buf_u8(&elf, 0);
            buf_bytes(&elf, initarr.data, initarr.len);
        }
        if (finiarr.len > 0) {
            while (elf.len < finiarr_file_offset) buf_u8(&elf, 0);
            buf_bytes(&elf, finiarr.data, finiarr.len);
        }
        SectionLayout lay;
        memset(&lay, 0, sizeof(lay));
        lay.code_vaddr = code_vaddr;
        lay.data_vaddr = data_vaddr;
        lay.bss_vaddr = bss_vaddr;
        lay.text_offset = text_offset;
        lay.data_file_offset = data_file_offset;
        lay.text_len = text.len;
        lay.rodata_len = rodata.len;
        lay.data_len = data.len;
        lay.bss_file_offset = bss_file_offset;
        lay.bss_size = bss_size;
        lay.have_tls = have_tls;
        lay.is_shared = 0;
        lay.start_size = start_size;
        lay.tls_vaddr = tls_vaddr;
        lay.tls_file_offset = tls_file_offset_base;
        lay.tls_filesize = tls_filesize;
        lay.tls_memsize = tls_memsize;
        lay.have_initarr = initarr.len > 0;
        lay.initarr_vaddr = initarr_vaddr;
        lay.initarr_file_offset = initarr_file_offset;
        lay.initarr_size = initarr.len;
        lay.have_finiarr = finiarr.len > 0;
        lay.finiarr_vaddr = finiarr_vaddr;
        lay.finiarr_file_offset = finiarr_file_offset;
        lay.finiarr_size = finiarr.len;
        lay.have_dynamic = 0;
        finalize_sections(&elf, mods, n, mod_text_off, mod_sym_base, sym_addr,
                          &lay, entry, want_debug);

        FILE *f = fopen(path, "wb");
        if (!f) { fprintf(stderr, "fakecc: cannot write '%s'\n", path); exit(1); }
        fwrite(elf.data, 1, elf.len, f);
        fclose(f);
        chmod(path, 0755);

        buffer_free(&rx); buffer_free(&got); buffer_free(&elf);
    }

    buffer_free(&dynstr); buffer_free(&dynsym); buffer_free(&hash);
    buffer_free(&rela_plt); buffer_free(&rela_dyn); buffer_free(&dynamic);
    for (int i = 0; i < num_ext; i++) free(ext_list[i]);
    free(ext_list);
    for (int j = 0; j < num_data_ext; j++) free(data_ext_list[j]);
    free(data_ext_list);
    for (int i = 0; i < num_needed; i++) free(needed[i]);
    free(needed);
    free(runpath);
    free(soname);
    free(exports);
    buffer_free(&text); buffer_free(&rodata); buffer_free(&data);
    buffer_free(&tdata);
    buffer_free(&initarr);
    buffer_free(&finiarr);
    free(concat_init_prio); free(concat_fini_prio);
    free(init_remap); free(fini_remap);
    free(mod_text_off); free(mod_rodata_off); free(mod_data_off); free(mod_bss_off);
    free(mod_tdata_off); free(mod_tbss_off); free(mod_init_off); free(mod_fini_off);
    free(mod_sym_base); free(sym_addr); free(sinfo); free(reloc_ext_idx);
    free(commons);
    free(reloc_data_abs_idx); free(reloc_data_got_idx);
    free(reloc_tls_ie_idx); free(tls_ie_gsi); free(tls_ie_name); free(tls_ie_tpoff);
    free(tls_ie_is_undef); free(tls_ie_und_idx);
    for (int t = 0; t < num_tls_und; t++) free(tls_und_list[t]);
    free(tls_und_list);
    free(tls_und_weak);
    free(ifunc_gsi); free(ifunc_name); free(ifunc_thunk_off); free(ifunc_got_fixup);
    free(reloc_copy_idx); free(copy_size); free(copy_off);
    for (int c = 0; c < num_copy; c++) free(copy_list[c]);
    free(copy_list);
    for (int a = 0; a < num_abs_ext; a++) free(abs_ext_list[a]);
    free(abs_ext_list);
    free(plt_entry_off); free(plt_got_fixup);
    free(data_got_addr); free(data_got_external);
    free(rel_roff); free(rel_add);
    free(tpoff_roff); free(tpoff_add); free(tpoff_info);
    free(abs64_roff); free(abs64_add); free(abs64_info);
}
