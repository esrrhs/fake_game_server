#include "fakecc/macho.h"
#include "fakecc/a64.h"
#include "fakecc/emit.h"
#include "fakecc/reg_arm64.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
/* The bootstrap (FAKECC_SELFHOST) preprocesses with the minimal v0/fakeinc
 * stubs, which lack these headers; the selfhost image never invokes
 * codesign (see macho_codesign). */
#ifndef FAKECC_SELFHOST
#include <sys/wait.h>
#include <unistd.h>
#endif

/* Mach-O 64-bit structures and constants (Apple loader/mach-o headers).
 * Layout below was validated against lld/ld64 output and by executing
 * hand-built images (see .trae/specs/arm64-macos-backend spike notes). */

#define MH_MAGIC_64   0xFEEDFACFu
#define MH_OBJECT     1u
#define MH_EXECUTE    2u
#define MH_SUBSECTIONS_VIA_SYMBOLS 0x00002000u
#define MH_NOUNDEFS   0x00000001u
#define MH_DYLDLINK   0x00000004u
#define MH_TWOLEVEL   0x00000080u
#define MH_PIE        0x00200000u
/* "Contains a section of type S_THREAD_LOCAL_VARIABLES."  Without it dyld
 * never looks for __thread_vars at all, so every descriptor's thunk stays
 * the __tlv_bootstrap error stub and the first access aborts with
 * "thread locals not initialized". */
#define MH_HAS_TLV_DESCRIPTORS 0x00800000u

#define CPU_TYPE_ARM64        0x0100000C
#define CPU_SUBTYPE_ARM64_ALL 0

#define LC_SEGMENT_64      0x19u
#define LC_BUILD_VERSION   0x32u
#define LC_UUID           0x1Bu
#define LC_LOAD_DYLINKER   0x0Eu
#define LC_LOAD_DYLIB      0x0Cu
#define LC_MAIN            0x80000028u
#define LC_DYLD_EXPORTS_TRIE 0x80000033u
#define LC_DYLD_CHAINED_FIXUPS 0x80000034u
#define LC_SYMTAB         0x02u
#define LC_DYSYMTAB       0x0Bu

#define PLATFORM_MACOS 1u

#define VM_PROT_READ  1
#define VM_PROT_WRITE 2
#define VM_PROT_EXEC  4

#define S_REGULAR                  0x00u
#define S_ZEROFILL                 0x01u
#define S_ATTR_PURE_INSTRUCTIONS   0x80000000u
#define S_ATTR_SOME_INSTRUCTIONS   0x00000400u
/* Thread-local storage section types.  dyld finds the descriptor table and
 * the per-thread template by type, not by name: renaming __thread_vars to
 * anything else still works, but changing the type to S_REGULAR makes dyld
 * skip the image and every access abort. */
#define S_THREAD_LOCAL_REGULAR     0x11u
#define S_THREAD_LOCAL_ZEROFILL    0x12u
#define S_THREAD_LOCAL_VARIABLES   0x13u
#define N_EXT  0x01u
#define N_PEXT 0x10u /* private external: hidden / internal visibility */
#define N_SECT 0x0eu

/* Fixed load-image geometry:
 *   mach_header_64 (32) + load commands, then __text at MACHO_TEXT_OFF.
 *   __PAGEZERO guards the low 4 GiB; __TEXT is one 16 KiB page and
 *   __LINKEDIT starts on the next page empty, populated by ad-hoc
 *   codesigning. */
/* arm64 macOS uses 16 KiB pages on current hardware/kernels; segment
 * offsets and file padding follow that. */
#define MACHO_BASE_VA   0x100000000ull
#define MACHO_PAGE_SIZE 0x4000u

#define DYLINKER_PATH "/usr/lib/dyld"
#define LIBSYSTEM_PATH "/usr/lib/libSystem.B.dylib"

/* Packed via per-struct attribute instead of #pragma pack: the bootstrap
 * dialect has no preprocessor but its parser honors
 * __attribute__((packed)) (translate.py preserves it). */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t cputype;
    uint32_t cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
    uint32_t reserved;
} mach_header_64;

typedef struct __attribute__((packed)) {
    uint32_t cmd;
    uint32_t cmdsize;
    char     segname[16];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    int32_t  maxprot;
    int32_t  initprot;
    uint32_t nsects;
    uint32_t flags;
} segment_command_64;

typedef struct __attribute__((packed)) {
    char     sectname[16];
    char     segname[16];
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
} section_64;

typedef struct __attribute__((packed)) {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t symoff;
    uint32_t nsyms;
    uint32_t stroff;
    uint32_t strsize;
} symtab_command;

typedef struct __attribute__((packed)) {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t ilocalsym;
    uint32_t nlocalsym;
    uint32_t iextdefsym;
    uint32_t nextdefsym;
    uint32_t iundefsym;
    uint32_t nundefsym;
    uint32_t tocoff;
    uint32_t ntoc;
    uint32_t modtaboff;
    uint32_t nmodtab;
    uint32_t extrefsymoff;
    uint32_t nextrefsyms;
    uint32_t indirectsymoff;
    uint32_t nindirectsyms;
    uint32_t extreloff;
    uint32_t nextrel;
    uint32_t locreloff;
    uint32_t nlocrel;
} dysymtab_command;

typedef struct __attribute__((packed)) {
    uint32_t n_strx;
    uint8_t  n_type;
    uint8_t  n_sect;
    uint16_t n_desc;
    uint64_t n_value;
} nlist_64;

typedef struct __attribute__((packed)) {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t platform;
    uint32_t minos;       /* packed x.y.z: (x<<16)|(y<<8)|z */
    uint32_t sdk;
    uint32_t ntools;
} build_version_command;

typedef struct __attribute__((packed)) {
    uint32_t cmd;
    uint32_t cmdsize;
    uint64_t entryoff;
    uint64_t stacksize;
} entry_point_command;

typedef struct __attribute__((packed)) {
    uint32_t cmd;
    uint32_t cmdsize;
    unsigned char uuid[16];
} uuid_command;

typedef struct __attribute__((packed)) {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t nameoff;
} dylinker_command;

typedef struct __attribute__((packed)) {
    uint32_t name_offset;
    uint32_t timestamp;
    uint32_t current_version;
    uint32_t compatibility_version;
} dylib;

typedef struct __attribute__((packed)) {
    uint32_t cmd;
    uint32_t cmdsize;
    dylib    dylib;
    char     name[];      /* offset 24, NUL terminated */
} dylib_command;

typedef struct __attribute__((packed)) {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t dataoff;
    uint32_t datasize;
} linkedit_data_command;


/* pad to a multiple of 8 for load-command sizes */
static uint32_t align8(uint32_t n) { return (n + 7u) & ~7u; }

static void put_u16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put_u32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put_u64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

/* DYLD_CHAINED_PTR_64_OFFSET (pointer format 6).  `target` is the vm
 * offset from the mach header; dyld adds the actual load address.
 * Bit layout is dyld_chained_ptr_64_rebase (mach-o/fixup-chains.h):
 *   target 36 | high8 8 | reserved 7 | next 12 | bind 1
 * `next` counts 4-byte steps to the following pointer on the same page.
 * Checked against an ld64 binary on this OS: format 6, page_start is
 * the byte offset of the first pointer in the 16 KiB page. */
static uint64_t encode_chain_ptr(uint64_t target, uint32_t next) {
    return (target & 0xFFFFFFFFFull) | ((uint64_t)(next & 0xFFFu) << 51);
}

/* One link in a __DATA fixup chain.  A rebase names an address inside the
 * image; a bind names a symbol dyld has to find in a dylib, and carries the
 * index of that symbol in the imports table instead of an address. */
typedef struct {
    uint64_t slot;
    uint64_t target;   /* rebase */
    uint32_t ordinal;  /* bind */
    int      is_bind;
} ChainEnt;

static int chain_cmp(const void *a, const void *b) {
    const ChainEnt *x = a;
    const ChainEnt *y = b;
    if (x->slot < y->slot) return -1;
    if (x->slot > y->slot) return 1;
    return 0;
}

/* DYLD_CHAINED_PTR_64_OFFSET with the bind bit set, i.e.
 * dyld_chained_ptr_64_bind (mach-o/fixup-chains.h):
 *   ordinal 24 | addend 8 | reserved 19 | next 12 | bind 1
 * `addend` is an unsigned byte and is 0 throughout: nothing here binds a
 * symbol-plus-offset. */
static uint64_t encode_bind_ptr(uint32_t ordinal, uint32_t next) {
    return ((uint64_t)(ordinal & 0xFFFFFFu))
         | ((uint64_t)(next & 0xFFFu) << 51)
         | (1ull << 63);
}

/* Encode em->rebases and em->binds into __data and build the
 * LC_DYLD_CHAINED_FIXUPS payload plus an empty exports trie.  Only __DATA
 * is chained; const data that contains pointers is placed there by
 * codegen64.  Both kinds share one chain per page, so an entry's `next`
 * stride is measured to whichever kind follows it. */
static int macho_build_fixups(const EmitModule *em, uint64_t data_page,
                              uint64_t data_file, char **data_img_out,
                              Buffer *fixblob, Buffer *trie) {
    /* A fixup may sit in any file-backed section of __DATA, not just
     * __data: the thread-local descriptor table is one too. */
    uint64_t file_bytes = (uint64_t)em->data.len
                        + (uint64_t)em->thread_vars.len
                        + (uint64_t)em->tdata.len;
    if (!file_bytes || data_file == 0) {
        fprintf(stderr, "fakecc: fixup with empty __data\n");
        return -1;
    }
    uint32_t page_count = (uint32_t)(data_file / MACHO_PAGE_SIZE);
    if (page_count == 0 || page_count > 0xFFFFu) {
        fprintf(stderr, "fakecc: __data page count out of range\n");
        return -1;
    }

    size_t n = em->num_rebases + em->num_binds;
    ChainEnt *ce = xmalloc((n ? n : 1) * sizeof *ce);
    for (size_t i = 0; i < em->num_rebases; i++) {
        ce[i].slot = em->rebases[i].slot;
        ce[i].target = em->rebases[i].target;
        ce[i].ordinal = 0;
        ce[i].is_bind = 0;
    }
    /* Imports are deduplicated by name: one entry per symbol, and every
     * cell that needs it cites the same ordinal. */
    const char **imp_name = NULL;
    size_t nimp = 0, capimp = 0;
    for (size_t i = 0; i < em->num_binds; i++) {
        const char *nm = em->binds[i].name;
        uint32_t ord = 0;
        int found = 0;
        for (size_t k = 0; k < nimp; k++)
            if (strcmp(imp_name[k], nm) == 0) { ord = (uint32_t)k; found = 1; break; }
        if (!found) {
            if (nimp == capimp) {
                capimp = capimp ? capimp * 2 : 4;
                imp_name = xrealloc(imp_name, capimp * sizeof *imp_name);
            }
            imp_name[nimp] = nm;
            ord = (uint32_t)nimp;
            nimp++;
        }
        ce[em->num_rebases + i].slot = em->binds[i].slot;
        ce[em->num_rebases + i].target = 0;
        ce[em->num_rebases + i].ordinal = ord;
        ce[em->num_rebases + i].is_bind = 1;
    }
    qsort(ce, n, sizeof *ce, chain_cmp);

    /* The patched image has to cover every file-backed section of __DATA,
     * not just __data: the thread-local descriptor table lives in
     * __thread_vars right behind it, and its thunk cells are binds.  A
     * shorter buffer would push those writes past its end and leave the
     * on-disk descriptor zero, which dyld reads as "rebase to 0". */
    char *img = xmalloc(file_bytes);
    memcpy(img, em->data.data, em->data.len);
    if (em->thread_vars.len)
        memcpy(img + em->data.len, em->thread_vars.data,
               em->thread_vars.len);
    if (em->tdata.len)
        memcpy(img + em->data.len + em->thread_vars.len,
               em->tdata.data, em->tdata.len);

    for (size_t i = 0; i < n; i++) {
        if (ce[i].slot < data_page ||
            ce[i].slot + 8 > data_page + file_bytes) {
            fprintf(stderr, "fakecc: fixup slot %#llx outside __data\n",
                    (unsigned long long)ce[i].slot);
            free(ce); free(imp_name); free(img);
            return -1;
        }
        uint64_t off = ce[i].slot - data_page;
        if ((off & 7) || (off % MACHO_PAGE_SIZE) + 8 > MACHO_PAGE_SIZE) {
            fprintf(stderr, "fakecc: fixup slot %#llx is not an in-page "
                    "8-byte pointer\n", (unsigned long long)ce[i].slot);
            free(ce); free(imp_name); free(img);
            return -1;
        }
        if (!ce[i].is_bind && ce[i].target > 0xFFFFFFFFFull) {
            fprintf(stderr, "fakecc: rebase target exceeds 36 bits\n");
            free(ce); free(imp_name); free(img);
            return -1;
        }
        uint32_t next = 0;
        if (i + 1 < n) {
            uint64_t off2 = ce[i + 1].slot - data_page;
            if ((off / MACHO_PAGE_SIZE) == (off2 / MACHO_PAGE_SIZE)) {
                uint64_t dist = ce[i + 1].slot - ce[i].slot;
                if (dist == 0 || (dist & 3) || dist / 4 > 0xFFF) {
                    fprintf(stderr, "fakecc: fixup chain stride out of range\n");
                    free(ce); free(imp_name); free(img);
                    return -1;
                }
                next = (uint32_t)(dist / 4);
            }
        }
        uint64_t enc = ce[i].is_bind ? encode_bind_ptr(ce[i].ordinal, next)
                                     : encode_chain_ptr(ce[i].target, next);
        memcpy(img + (size_t)off, &enc, 8);
    }

    /* header (28) padded to 32, then starts-in-image for the four
     * segments.  Segment info is 8-aligned, so it begins at starts+24
     * (ld64 does the same).  With no imports the trailing 8 zero bytes
     * match the ld64 blob. */
    uint32_t starts_off = 32;
    uint32_t seg_at = 56;
    uint32_t raw = 22u + 2u * page_count;
    uint32_t seg_size = (raw + 7u) & ~7u;
    uint32_t imports_off = seg_at + seg_size;
    uint32_t symbols_off = imports_off + (uint32_t)nimp * 4u;
    size_t pool = 0;
    for (size_t k = 0; k < nimp; k++) pool += strlen(imp_name[k]) + 1;
    uint32_t blob_size = symbols_off + (uint32_t)pool;
    if (!nimp) blob_size += 8;
    uint8_t *blob = xmalloc(blob_size);
    memset(blob, 0, blob_size);
    put_u32(blob + 4, starts_off);
    put_u32(blob + 8, imports_off);
    put_u32(blob + 12, symbols_off);
    put_u32(blob + 16, (uint32_t)nimp);
    put_u32(blob + 20, 1);                         /* DYLD_CHAINED_IMPORT */
    put_u32(blob + starts_off, 4);                 /* seg_count */
    put_u32(blob + starts_off + 12, 24);           /* seg_info_offset[2] */
    put_u32(blob + seg_at, seg_size);
    put_u16(blob + seg_at + 4, (uint16_t)MACHO_PAGE_SIZE);
    put_u16(blob + seg_at + 6, 6);                 /* PTR_64_OFFSET */
    put_u64(blob + seg_at + 8, data_page);
    put_u16(blob + seg_at + 20, (uint16_t)page_count);
    for (uint32_t p = 0; p < page_count; p++)
        put_u16(blob + seg_at + 22 + 2u * p, 0xFFFF);
    for (size_t i = 0; i < n; i++) {
        uint64_t off = ce[i].slot - data_page;
        uint32_t pg = (uint32_t)(off / MACHO_PAGE_SIZE);
        uint16_t cur = 0;
        memcpy(&cur, blob + seg_at + 22 + 2u * pg, 2);
        if (cur == 0xFFFF)
            put_u16(blob + seg_at + 22 + 2u * pg,
                    (uint16_t)(off % MACHO_PAGE_SIZE));
    }
    /* dyld_chained_import: lib_ordinal 8 | weak_import 1 | name_offset 23.
     * libSystem.B.dylib is the one LC_LOAD_DYLIB this image carries, so
     * every import resolves against ordinal 1. */
    size_t at = 0;
    for (size_t k = 0; k < nimp; k++) {
        put_u32(blob + imports_off + 4u * k,
                (uint32_t)(1u | ((at & 0x7FFFFFu) << 9)));
        memcpy(blob + symbols_off + at, imp_name[k], strlen(imp_name[k]) + 1);
        at += strlen(imp_name[k]) + 1;
    }
    buffer_append(fixblob, (const char *)blob, blob_size);
    free(blob);
    free(ce);
    free(imp_name);

    char empty_trie[2] = {0, 0};
    buffer_append(trie, empty_trie, 2);
    *data_img_out = img;
    return 0;
}

/* mach_header_64 (32) + load commands, then __text at MACHO_TEXT_OFF.
 * The worst case is every optional section at once: __text + __const,
 * __data + __bss + __thread_vars + __thread_data + __thread_bss, plus
 * the fixup pair, which runs the command block past 1 KiB.  2048 leaves
 * ample room for codesign's injected LC_CODE_SIGNATURE on top of that. */
#define MACHO_TEXT_OFF 2048u

uint32_t macho_text_offset(void) { return MACHO_TEXT_OFF; }

/* A thread's block is every initialized __thread object followed by every
 * zero-filled one, so the second half has to start at tdata_len rounded up to
 * the strongest alignment any __thread object asked for.  That is what keeps
 * `__thread int x __attribute__((aligned(32)))` 32-aligned *inside* the
 * block, which is the most a static linker can arrange: dyld allocates the
 * block itself with plain malloc alignment, so the address a program finally
 * sees is only ever as strong as that base plus this offset. */
size_t macho_tlv_block_tbss_base(size_t tdata_len, size_t max_align) {
    size_t a = max_align < 8 ? 8 : max_align;
    return (tdata_len + a - 1) & ~(a - 1);
}

/* Must stay in lock-step with the layout block in macho_write_exec. */
static uint32_t macho_align_log(size_t al, uint32_t min_log);

void macho_section_offsets(const EmitModule *em, size_t text_len,
                           MachoSectOffsets *o) {
    uint64_t ro_off = 0;
    if (em->rodata.len) {
        ro_off = MACHO_TEXT_OFF + (uint64_t)text_len;
        size_t al = em->rodata_align > 16 ? em->rodata_align : 16;
        while (ro_off % al) ro_off++;
    }
    uint64_t text_used = ro_off ? ro_off + em->rodata.len
                                : (uint64_t)MACHO_TEXT_OFF + text_len;
    uint64_t text_pages = (text_used + MACHO_PAGE_SIZE - 1)
                          & ~(uint64_t)(MACHO_PAGE_SIZE - 1);
    /* __DATA holds every file-backed section first, then the zerofill
     * ones: __data, __thread_vars, __thread_data, then __bss and
     * __thread_bss.  A zerofill section in the middle would leave a hole
     * in the file image, and the loader takes the segment's file size
     * from the last file-backed section. */
    uint64_t tv_off = 0, td_off = 0, tbss_off = 0;
    uint64_t file_bytes = (uint64_t)em->data.len;
    if (em->thread_vars.len) {
        tv_off = text_pages + file_bytes;
        file_bytes += (uint64_t)em->thread_vars.len;
        td_off = text_pages + file_bytes;
        file_bytes += (uint64_t)em->tdata.len;
    } else if (em->tdata.len) {
        td_off = text_pages + file_bytes;
        file_bytes += (uint64_t)em->tdata.len;
    }
    uint64_t data_file_total = file_bytes
        ? (file_bytes + MACHO_PAGE_SIZE - 1) & ~(uint64_t)(MACHO_PAGE_SIZE - 1)
        : 0;
    uint64_t bss_vm = ((uint64_t)em->bss_size + 15) & ~(uint64_t)15;
    if (em->bss_size) {
        tbss_off = text_pages + data_file_total + bss_vm;
    }
    if (em->tbss_size && !em->bss_size) {
        tbss_off = text_pages + data_file_total;
    }
    o->ro = ro_off;
    o->data = em->data.len ? text_pages : 0;
    o->bss = em->bss_size ? text_pages + data_file_total : 0;
    o->thread_vars = tv_off;
    o->tdata = td_off;
    o->tbss = tbss_off;
}

int macho_write_exec_text(const Buffer *text, uint64_t entry_off,
                          const char *path) {
    EmitModule em;
    memset(&em, 0, sizeof em);
    buffer_init(&em.text);
    buffer_append(&em.text, text->data, text->len);
    int rc = macho_write_exec(&em, entry_off, path);
    buffer_free(&em.text);
    return rc;
}

int macho_write_exec(const EmitModule *em, uint64_t entry_off,
                     const char *path) {
    const Buffer *text = &em->text;
    const Buffer *rodata = &em->rodata;
    const Buffer *data = &em->data;
    const Buffer *thread_vars = &em->thread_vars;
    const Buffer *tdata = &em->tdata;
    uint64_t bss_bytes = em->bss_size;
    uint64_t tbss_bytes = em->tbss_size;

    /* ── Section/segment layout ── */
    const uint32_t text_off = MACHO_TEXT_OFF;
    uint32_t ro_off = 0;
    if (rodata->len) {
        ro_off = text_off + (uint32_t)text->len;
        size_t al = em->rodata_align > 16 ? em->rodata_align : 16;
        while (ro_off % al) ro_off++;
    }
    uint64_t text_used = ro_off ? (uint64_t)ro_off + rodata->len
                                : (uint64_t)text_off + text->len;
    uint64_t text_pages = (text_used + MACHO_PAGE_SIZE - 1)
                          & ~(uint64_t)(MACHO_PAGE_SIZE - 1);

    int data_present = (data->len > 0 || bss_bytes > 0
                        || thread_vars->len > 0 || tdata->len > 0
                        || tbss_bytes > 0);
    uint64_t data_page = text_pages;
    /* File-backed __data bytes occupy whole pages.  __bss and __thread_bss
     * are zerofill VM tails with no file representation, and both come
     * after the file-backed sections so the segment's file size still ends
     * at its last real byte.  Must agree with macho_section_offsets. */
    uint64_t data_file_bytes = (uint64_t)data->len
                             + (uint64_t)thread_vars->len
                             + (uint64_t)tdata->len;
    uint64_t data_file = data_file_bytes
        ? (data_file_bytes + MACHO_PAGE_SIZE - 1)
          & ~(uint64_t)(MACHO_PAGE_SIZE - 1)
        : 0;
    uint64_t bss_vm = (bss_bytes + 15) & ~(uint64_t)15;
    uint64_t tbss_vm = (tbss_bytes + 15) & ~(uint64_t)15;
    uint64_t data_vm = data_file + bss_vm + tbss_vm;
    uint64_t tv_off = data_page + (uint64_t)data->len;
    uint64_t td_off = tv_off + (uint64_t)thread_vars->len;
    /* __LINKEDIT begins on the next page after the whole __DATA VM image
     * (its file offset doubles as the codesignature injection site). */
    uint64_t link_off = data_present
        ? data_page + ((data_vm + MACHO_PAGE_SIZE - 1)
                       & ~(uint64_t)(MACHO_PAGE_SIZE - 1))
        : text_pages;

    char *data_img = NULL;
    Buffer fixblob, trieblob;
    buffer_init(&fixblob);
    buffer_init(&trieblob);
    int has_fix = em->num_rebases > 0 || em->num_binds > 0;
    if (has_fix &&
        macho_build_fixups(em, data_page, data_file, &data_img,
                           &fixblob, &trieblob) != 0) {
        buffer_free(&fixblob);
        buffer_free(&trieblob);
        return -1;
    }
    uint64_t linkedit_payload = has_fix
        ? (uint64_t)fixblob.len + (uint64_t)trieblob.len : 0;

    /* ── Load command sizes ── */
    /* dyld rejects zero-size sections (their null addr sorts before the
     * segment), so each section — and the whole __DATA segment — is
     * emitted only when it has content. */
    const int has_const_sec = rodata->len > 0;
    const int has_data_sec  = data->len > 0;
    const int has_bss_sec   = bss_bytes > 0;
    /* Thread-local variables split three ways, exactly as ld64 does:
     * the descriptor table, the initialized template, and the zero-fill
     * tail.  The names are not decoration -- dyld looks for them to find
     * out how big a thread's copy has to be. */
    const int has_tvar_sec  = thread_vars->len > 0;
    const int has_tdata_sec = tdata->len > 0;
    const int has_tbss_sec  = tbss_bytes > 0;
    const int n_text_secs   = 1 + (has_const_sec ? 1 : 0);
    const int n_data_secs   = (has_data_sec ? 1 : 0) + (has_bss_sec ? 1 : 0)
                            + (has_tvar_sec ? 1 : 0) + (has_tdata_sec ? 1 : 0)
                            + (has_tbss_sec ? 1 : 0);

    const uint32_t seg_plain_cmd = sizeof(segment_command_64);
    const uint32_t seg_text_cmd  =
        sizeof(segment_command_64) + (uint32_t)n_text_secs * sizeof(section_64);
    const uint32_t seg_data_cmd  = data_present
        ? sizeof(segment_command_64) + (uint32_t)n_data_secs * sizeof(section_64)
        : 0;
    const uint32_t build_cmd = sizeof(build_version_command);
    const uint32_t main_cmd = sizeof(entry_point_command);

    const uint32_t dylinker_name_off = 12u;
    const uint32_t dylinker_cmdsz =
        align8(dylinker_name_off + (uint32_t)strlen(DYLINKER_PATH) + 1);

    const uint32_t dylib_name_off = 24u;
    const uint32_t dylib_cmdsz =
        align8(dylib_name_off + (uint32_t)strlen(LIBSYSTEM_PATH) + 1);

    /* PAGEZERO, __TEXT, [__DATA], __LINKEDIT + the five non-segment
     * commands (build version, uuid, main, dylinker, dylib).
     * Pointer initializers add LC_DYLD_CHAINED_FIXUPS and an empty
     * LC_DYLD_EXPORTS_TRIE (dyld wants the pair). */
    const uint32_t ncmds = (data_present ? 4u : 3u) + 5u + (has_fix ? 2u : 0u);
    const uint32_t uuid_cmd = 24;
    const uint32_t sizeofcmds =
        seg_plain_cmd + seg_text_cmd + seg_data_cmd + seg_plain_cmd
        + build_cmd + uuid_cmd + main_cmd
        + dylinker_cmdsz + dylib_cmdsz
        + (has_fix ? 32u : 0u);

    const uint32_t cmds_end = (uint32_t)sizeof(mach_header_64) + sizeofcmds;
    if (cmds_end > MACHO_TEXT_OFF) {
        fprintf(stderr, "fakecc: Mach-O commands (%u) overflow header pad\n",
                cmds_end);
        free(data_img);
        buffer_free(&fixblob);
        buffer_free(&trieblob);
        return -1;
    }

    Buffer out;
    buffer_init(&out);
    char zero[MACHO_PAGE_SIZE];
    memset(zero, 0, sizeof zero);

#define APPEND_BYTES(p, n) buffer_append(&out, (const char *)(p), (n))
/* Loop form: padding requests may exceed one page. */
#define APPEND_ZERO(n) do {                                                    \
        size_t _left = (n);                                                   \
        while (_left) {                                                       \
            size_t _chunk = _left < sizeof zero ? _left : sizeof zero;        \
            buffer_append(&out, zero, _chunk);                                \
            _left -= _chunk;                                                  \
        }                                                                     \
    } while (0)

    /* ── mach_header_64 ── */
    mach_header_64 mh = {
        MH_MAGIC_64,
        CPU_TYPE_ARM64,
        CPU_SUBTYPE_ARM64_ALL,
        MH_EXECUTE,
        ncmds,
        sizeofcmds,
        MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL | MH_PIE
        | (has_tvar_sec ? MH_HAS_TLV_DESCRIPTORS : 0),
        0,
    };
    APPEND_BYTES(&mh, sizeof mh);

    /* ── LC_SEGMENT_64 __PAGEZERO (guard the low 4 GiB; no file bytes) ── */
    segment_command_64 pz = {0};
    pz.cmd = LC_SEGMENT_64;
    pz.cmdsize = seg_plain_cmd;
    memcpy(pz.segname, "__PAGEZERO", 10);
    pz.vmaddr = 0;
    pz.vmsize = MACHO_BASE_VA;
    pz.maxprot = 0;
    pz.initprot = 0;
    APPEND_BYTES(&pz, sizeof pz);

    /* ── LC_SEGMENT_64 __TEXT (r-x): __text + __const ── */
    segment_command_64 seg = {0};
    seg.cmd = LC_SEGMENT_64;
    seg.cmdsize = seg_text_cmd;
    memcpy(seg.segname, "__TEXT", 6);
    seg.vmaddr = MACHO_BASE_VA;
    seg.vmsize = text_pages;
    seg.fileoff = 0;
    seg.filesize = text_pages;
    seg.maxprot = VM_PROT_READ | VM_PROT_EXEC;
    seg.initprot = VM_PROT_READ | VM_PROT_EXEC;
    seg.nsects = (uint32_t)n_text_secs;
    APPEND_BYTES(&seg, sizeof seg);

    section_64 sec = {0};
    memcpy(sec.sectname, "__text", 6);
    memcpy(sec.segname, "__TEXT", 6);
    sec.addr = MACHO_BASE_VA + text_off;
    sec.size = text->len;
    sec.offset = text_off;
    sec.align = macho_align_log(em->text_align ? em->text_align : 8, 3);
    sec.flags = S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS;
    APPEND_BYTES(&sec, sizeof sec);

    if (has_const_sec) {
        section_64 csec = {0};
        memcpy(csec.sectname, "__const", 7);
        memcpy(csec.segname, "__TEXT", 6);
        csec.addr = MACHO_BASE_VA + ro_off;
        csec.size = rodata->len;
        csec.offset = ro_off;
        csec.align = macho_align_log(em->rodata_align > 16 ? em->rodata_align : 16, 4);
        csec.flags = 0;                  /* S_REGULAR read-only data */
        APPEND_BYTES(&csec, sizeof csec);
    }

    /* ── LC_SEGMENT_64 __DATA (rw-): __data + __bss (only if nonempty) ── */
    if (data_present) {
        segment_command_64 ds = {0};
        ds.cmd = LC_SEGMENT_64;
        ds.cmdsize = seg_data_cmd;
        memcpy(ds.segname, "__DATA", 6);
        ds.vmaddr = MACHO_BASE_VA + data_page;
        ds.vmsize = data_vm;
        ds.fileoff = data_page;
        ds.filesize = data_file;
        ds.maxprot = VM_PROT_READ | VM_PROT_WRITE;
        ds.initprot = VM_PROT_READ | VM_PROT_WRITE;
        ds.nsects = (uint32_t)n_data_secs;
        APPEND_BYTES(&ds, sizeof ds);

        if (has_data_sec) {
            section_64 dsec = {0};
            memcpy(dsec.sectname, "__data", 6);
            memcpy(dsec.segname, "__DATA", 6);
            dsec.addr = MACHO_BASE_VA + data_page;
            dsec.size = data->len;
            dsec.offset = (uint32_t)data_page;
            dsec.align = macho_align_log(em->data_align ? em->data_align : 8, 3);
            APPEND_BYTES(&dsec, sizeof dsec);
        }

        if (has_bss_sec) {
            section_64 bsec = {0};
            memcpy(bsec.sectname, "__bss", 5);
            memcpy(bsec.segname, "__DATA", 6);
            /* bss starts after the page-rounded file-backed data image. */
            bsec.addr = MACHO_BASE_VA + data_page + data_file;
            bsec.size = bss_bytes;
            bsec.offset = 0;            /* zerofill: no file bytes */
            bsec.align = macho_align_log(em->bss_align ? em->bss_align : 8, 3);
            bsec.flags = 0x1;           /* S_ZEROFILL */
            APPEND_BYTES(&bsec, sizeof bsec);
        }

        if (has_tvar_sec) {
            section_64 tsec = {0};
            memcpy(tsec.sectname, "__thread_vars", 13);
            memcpy(tsec.segname, "__DATA", 6);
            tsec.addr = MACHO_BASE_VA + tv_off;
            tsec.size = thread_vars->len;
            tsec.offset = (uint32_t)tv_off;
            tsec.align = macho_align_log(
                em->thread_vars_align ? em->thread_vars_align : 8, 3);
            tsec.flags = S_THREAD_LOCAL_VARIABLES;
            APPEND_BYTES(&tsec, sizeof tsec);
        }

        if (has_tdata_sec) {
            section_64 tsec = {0};
            memcpy(tsec.sectname, "__thread_data", 13);
            memcpy(tsec.segname, "__DATA", 6);
            tsec.addr = MACHO_BASE_VA + td_off;
            tsec.size = tdata->len;
            tsec.offset = (uint32_t)td_off;
            tsec.align = macho_align_log(em->tdata_align ? em->tdata_align : 8, 3);
            tsec.flags = S_THREAD_LOCAL_REGULAR;
            APPEND_BYTES(&tsec, sizeof tsec);
        }

        if (has_tbss_sec) {
            section_64 tsec = {0};
            memcpy(tsec.sectname, "__thread_bss", 12);
            memcpy(tsec.segname, "__DATA", 6);
            /* After __data and the descriptor/template sections, then
             * __bss: all of the file-backed content comes first. */
            tsec.addr = MACHO_BASE_VA + data_page + data_file + bss_vm;
            tsec.size = tbss_bytes;
            tsec.offset = 0;            /* zerofill: no file bytes */
            tsec.align = macho_align_log(em->tbss_align ? em->tbss_align : 8, 3);
            tsec.flags = S_THREAD_LOCAL_ZEROFILL;
            APPEND_BYTES(&tsec, sizeof tsec);
        }
    }

    /* ── LC_SEGMENT_64 __LINKEDIT (codesign fills it) ── */
    segment_command_64 le = {0};
    le.cmd = LC_SEGMENT_64;
    le.cmdsize = seg_plain_cmd;
    memcpy(le.segname, "__LINKEDIT", 10);
    le.vmaddr = MACHO_BASE_VA + link_off;
    le.vmsize = MACHO_PAGE_SIZE;
    /* Leave room for the ad-hoc signature codesign appends after the
     * chained-fixup payload. */
    if (linkedit_payload + 8192 > le.vmsize)
        le.vmsize = (linkedit_payload + 8192 + MACHO_PAGE_SIZE - 1)
                    & ~(uint64_t)(MACHO_PAGE_SIZE - 1);
    le.fileoff = link_off;
    le.filesize = linkedit_payload;
    le.maxprot = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC;
    le.initprot = VM_PROT_READ;
    APPEND_BYTES(&le, sizeof le);

    if (has_fix) {
        linkedit_data_command fx = {
            LC_DYLD_CHAINED_FIXUPS, 16,
            (uint32_t)link_off, (uint32_t)fixblob.len,
        };
        linkedit_data_command tr = {
            LC_DYLD_EXPORTS_TRIE, 16,
            (uint32_t)(link_off + fixblob.len), (uint32_t)trieblob.len,
        };
        APPEND_BYTES(&fx, sizeof fx);
        APPEND_BYTES(&tr, sizeof tr);
    }

    /* ── LC_BUILD_VERSION (macOS 11.0, no tool entries) ── */
    build_version_command bv = {
        LC_BUILD_VERSION, build_cmd, PLATFORM_MACOS,
        (11u << 16), 0, 0,
    };
    APPEND_BYTES(&bv, sizeof bv);

    /* ── LC_UUID (dyld requires one; deterministic stage-1 value) ── */
    uuid_command uc;
    uc.cmd = LC_UUID;
    uc.cmdsize = uuid_cmd;
    for (int i = 0; i < 16; i++)
        uc.uuid[i] = (unsigned char)(0xFA + i);
    APPEND_BYTES(&uc, sizeof uc);

    /* ── LC_MAIN ── */
    entry_point_command ep = {
        LC_MAIN, main_cmd, entry_off, 0,
    };
    APPEND_BYTES(&ep, sizeof ep);

    /* ── LC_LOAD_DYLINKER ── */
    dylinker_command dl = { LC_LOAD_DYLINKER, dylinker_cmdsz, dylinker_name_off };
    APPEND_BYTES(&dl, sizeof dl);
    APPEND_BYTES(DYLINKER_PATH, strlen(DYLINKER_PATH) + 1);
    APPEND_ZERO(dylinker_cmdsz - dylinker_name_off - strlen(DYLINKER_PATH) - 1);

    /* ── LC_LOAD_DYLIB (libSystem; zero imports are legal) ── */
    dylib_command dc;
    memset(&dc, 0, sizeof dc);
    dc.cmd = LC_LOAD_DYLIB;
    dc.cmdsize = dylib_cmdsz;
    dc.dylib.name_offset = dylib_name_off;
    APPEND_BYTES(&dc, 24); /* offsetof(dylib_command, name) — fakecc dialect has no offsetof */
    APPEND_BYTES(LIBSYSTEM_PATH, strlen(LIBSYSTEM_PATH) + 1);
    APPEND_ZERO(dylib_cmdsz - dylib_name_off - strlen(LIBSYSTEM_PATH) - 1);

    /* ── __TEXT segment file bytes: pad → text → __const → page ── */
    while (out.len < text_off) APPEND_ZERO((size_t)(text_off - out.len));
    buffer_append(&out, text->data, text->len);
    if (ro_off) {
        while (out.len < ro_off) APPEND_ZERO((size_t)(ro_off - out.len));
        buffer_append(&out, rodata->data, rodata->len);
    }
    while ((uint64_t)out.len < text_pages)
        APPEND_ZERO((size_t)(text_pages - out.len));

    /* ── __DATA segment file bytes: __data, __thread_vars, __thread_data,
     * padded through the link edit page so codesign can append its
     * signature at link_off.  __bss and __thread_bss have no file bytes;
     * the kernel zero-fills the vm tail. ── */
    if (data_present) {
        if (data_img)
            buffer_append(&out, data_img, data_file_bytes);
        else {
            buffer_append(&out, data->data, data->len);
            if (thread_vars->len)
                buffer_append(&out, thread_vars->data, thread_vars->len);
            if (tdata->len)
                buffer_append(&out, tdata->data, tdata->len);
        }
        while ((uint64_t)out.len < link_off)
            APPEND_ZERO((size_t)(link_off - out.len));
    }
    if (has_fix) {
        buffer_append(&out, fixblob.data, fixblob.len);
        buffer_append(&out, trieblob.data, trieblob.len);
    }
    free(data_img);
    buffer_free(&fixblob);
    buffer_free(&trieblob);
    data_img = NULL;

#undef APPEND_BYTES
#undef APPEND_ZERO

    size_t total_size = out.len;

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "fakecc: cannot create '%s'\n", path);
        buffer_free(&out);
        return -1;
    }
    size_t written = fwrite(out.data, 1, out.len, f);
    fclose(f);
    chmod(path, 0755);
    buffer_free(&out);
    if (written != total_size) {
        fprintf(stderr, "fakecc: short write on '%s' (%zu != %zu)\n",
                path, written, total_size);
        return -1;
    }
    return 0;
}

static int macho_sym_ok(const EmitSymbol *s, const uint8_t *sect_of) {
    if (!s->name) return 0;
    if (s->shndx == SECT_UNDEF || s->shndx == SHN_COMMON) return 1;
    return s->shndx < 9 && sect_of[s->shndx];
}

/* 0 local, 1 defined external, 2 undefined external (commons included). */
static int macho_sym_pass(const EmitSymbol *s) {
    if (s->shndx == SECT_UNDEF || s->shndx == SHN_COMMON) return 2;
    return s->binding == 0 ? 0 : 1;
}

static void macho_write_relocs(Buffer *out, const EmitReloc *rels, size_t n,
                               const int *nmap, size_t nsyms);

/* An extern reloc carrying a non-zero addend occupies TWO Mach-O records:
 * an ARM64_RELOC_ADDEND (type 10) with the signed 24-bit value, immediately
 * before the reloc it qualifies.  UNSIGNED (0) keeps its addend in the data
 * slot, SUBTRACTOR (1) pairs with a following UNSIGNED, and ADDEND (10) is the
 * prefix record itself -- none of the three gets a prefix. */
static int reloc_needs_addend_pair(const EmitReloc *r) {
    return r->addend != 0 && r->type != 0 && r->type != 1 && r->type != 10;
}

/* Number of on-disk records (ADDEND prefixes included) for a reloc array. */
static size_t reloc_record_count(const EmitReloc *rels, size_t n) {
    size_t c = n;
    for (size_t i = 0; i < n; i++)
        if (reloc_needs_addend_pair(&rels[i])) c++;
    return c;
}

static size_t reloc_record_count_sh(const EmitReloc *rels, size_t n,
                                    uint16_t sh) {
    size_t c = 0;
    for (size_t i = 0; i < n; i++)
        if (rels[i].shndx == sh) {
            c++;
            if (reloc_needs_addend_pair(&rels[i])) c++;
        }
    return c;
}

static size_t reloc_count_sh(const EmitReloc *rels, size_t n, uint16_t sh) {
    size_t c = 0;
    for (size_t i = 0; i < n; i++)
        if (rels[i].shndx == sh) c++;
    return c;
}

/* pack_ctor stores init/fini slots as 16-byte records (pointer, priority). */
static void macho_write_relocs_sh(Buffer *out, const EmitReloc *rels, size_t n,
                                  uint16_t sh, const int *nmap, size_t nsyms,
                                  int pack_ctor) {
    size_t nt = reloc_count_sh(rels, n, sh);
    if (!nt) return;
    EmitReloc *tmp = xmalloc(nt * sizeof *tmp);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (rels[i].shndx != sh) continue;
        tmp[k] = rels[i];
        if (pack_ctor) tmp[k].offset = (rels[i].offset / 8) * 16;
        k++;
    }
    macho_write_relocs(out, tmp, nt, nmap, nsyms);
    free(tmp);
}

static void append_hook_section(Buffer *out, const Buffer *arr, const int *prio) {
    size_t n = arr->len / 8;
    for (size_t i = 0; i < n; i++) {
        buffer_append(out, arr->data + i * 8, 8);
        int64_t p = prio ? (int64_t)prio[i] : (int64_t)INIT_PRIO_DEFAULT;
        buffer_append(out, (const char *)&p, 8);
    }
}

static void macho_write_relocs(Buffer *out, const EmitReloc *rels, size_t n,
                               const int *nmap, size_t nsyms) {
    size_t *sorted = n ? xmalloc(n * sizeof(size_t)) : NULL;
    for (size_t i = 0; i < n; i++) sorted[i] = i;
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (rels[sorted[j]].offset > rels[sorted[i]].offset) {
                size_t t = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = t;
            }
        }
    }
    for (size_t i = 0; i < n; i++) {
        const EmitReloc *r = &rels[sorted[i]];
        int sym = (nmap && r->sym < nsyms) ? nmap[r->sym] : 0;
        if (sym < 0) sym = 0;
        if (reloc_needs_addend_pair(r)) {
            /* ARM64_RELOC_ADDEND: same address as the record that follows,
             * r_symbolnum holds the signed 24-bit addend, r_extern = 0.
             * Emitting it immediately before its reloc is what the reader's
             * one-record pending addend expects. */
            int32_t aaddr = (int32_t)r->offset;
            uint32_t aword = ((uint32_t)r->addend & 0xFFFFFFu)
                           | (2u << 25)          /* length = 2 (instruction) */
                           | (10u << 28);        /* ARM64_RELOC_ADDEND */
            buffer_append(out, (const char *)&aaddr, 4);
            buffer_append(out, (const char *)&aword, 4);
        }
        uint32_t ext = (r->type == 10) ? 0u : 1u;
        uint32_t symfield = (r->type == 10)
                          ? ((uint32_t)r->addend & 0xFFFFFFu)
                          : ((uint32_t)sym & 0xFFFFFFu);
        uint32_t pcrel = (r->type == 2 || r->type == 3 || r->type == 5
                          || r->type == 8) ? 1u : 0u;
        uint32_t length = (r->type == 0 || r->type == 1) ? 3u : 2u; /* 8-byte slots */
        uint32_t word = symfield
                      | (pcrel << 24)
                      | (length << 25)
                      | (ext << 27)
                      | ((r->type & 0xFu) << 28);
        int32_t addr = (int32_t)r->offset;
        buffer_append(out, (const char *)&addr, 4);
        buffer_append(out, (const char *)&word, 4);
    }
    free(sorted);
}

/* Mach-O section align is a power-of-two exponent.  Keep the historical
 * minimum so ordinary objects do not get a weaker alignment. */
static uint32_t macho_align_log(size_t al, uint32_t min_log) {
    uint32_t log = min_log;
    if (al < 1) al = 1;
    while (((size_t)1 << log) < al && log < 15) log++;
    return log;
}

static uint64_t align_up_u64(uint64_t v, uint64_t a) {
    if (a <= 1) return v;
    return (v + a - 1) & ~(a - 1);
}

/* MH_OBJECT with __text/__const/__data/__bss and a symbol table.
 * Relocations are left empty: callers that need a global address still
 * die in codegen. */
int macho_write_object(const EmitModule *em, const char *path) {
    int has_ro = em->rodata.len > 0;
    int has_data = em->data.len > 0;
    int has_bss = em->bss_size > 0;
    int has_tdata = em->tdata.len > 0;
    int has_tbss = em->tbss_size > 0;
    int has_init = em->init_array.len > 0;
    int has_fini = em->fini_array.len > 0;
    size_t init_bytes = (em->init_array.len / 8) * 16;
    size_t fini_bytes = (em->fini_array.len / 8) * 16;
    /* An object has to carry the thread-local template and its zero-fill
     * tail, not just the descriptor-less ordinary sections: a symbol whose
     * bytes live in __thread_data can only be named by a section that
     * exists, and the linker needs both to build the merged thread block. */
    uint32_t nsects = 1u + (uint32_t)has_ro + (uint32_t)has_data
                    + (uint32_t)has_bss + (uint32_t)has_tdata
                    + (uint32_t)has_tbss
                    + (uint32_t)has_init + (uint32_t)has_fini;
    uint32_t seg_cmdsize = (uint32_t)sizeof(segment_command_64)
                         + nsects * (uint32_t)sizeof(section_64);
    uint32_t sizeofcmds = seg_cmdsize
                        + (uint32_t)sizeof(build_version_command)
                        + (uint32_t)sizeof(symtab_command)
                        + (uint32_t)sizeof(dysymtab_command);
    uint64_t content_off = align_up_u64(32u + sizeofcmds, 8);

    uint8_t sect_of[9];
    uint64_t sect_addr[9];
    memset(sect_of, 0, sizeof sect_of);
    memset(sect_addr, 0, sizeof sect_addr);
    uint32_t nsec = 1;
    sect_of[SECT_TEXT] = (uint8_t)nsec++;
    if (has_ro) sect_of[SECT_RODATA] = (uint8_t)nsec++;
    if (has_data) sect_of[SECT_DATA] = (uint8_t)nsec++;
    if (has_bss) sect_of[SECT_BSS] = (uint8_t)nsec++;
    if (has_tdata) sect_of[SECT_TDATA] = (uint8_t)nsec++;
    if (has_tbss) sect_of[SECT_TBSS] = (uint8_t)nsec++;

    uint64_t vm = 0;
    uint64_t file = content_off;
    sect_addr[SECT_TEXT] = 0;
    uint64_t text_off = file;
    vm += em->text.len;
    file += em->text.len;

    uint64_t ro_off = 0, data_off = 0;
    if (has_ro) {
        uint64_t al = em->rodata_align ? em->rodata_align : 8;
        vm = align_up_u64(vm, al);
        file = align_up_u64(file, al);
        sect_addr[SECT_RODATA] = vm;
        ro_off = file;
        vm += em->rodata.len;
        file += em->rodata.len;
    }
    if (has_data) {
        uint64_t al = em->data_align ? em->data_align : 8;
        vm = align_up_u64(vm, al);
        file = align_up_u64(file, al);
        sect_addr[SECT_DATA] = vm;
        data_off = file;
        vm += em->data.len;
        file += em->data.len;
    }
    if (has_bss) {
        uint64_t al = em->bss_align ? em->bss_align : 8;
        vm = align_up_u64(vm, al);
        sect_addr[SECT_BSS] = vm;
        vm += em->bss_size;
    }
    uint64_t tdata_off = 0;
    if (has_tdata) {
        uint64_t al = em->tdata_align ? em->tdata_align : 8;
        vm = align_up_u64(vm, al);
        file = align_up_u64(file, al);
        sect_addr[SECT_TDATA] = vm;
        tdata_off = file;
        vm += em->tdata.len;
        file += em->tdata.len;
    }
    if (has_tbss) {
        uint64_t al = em->tbss_align ? em->tbss_align : 8;
        vm = align_up_u64(vm, al);
        sect_addr[SECT_TBSS] = vm;
        vm += em->tbss_size;
    }
    uint64_t init_off = 0, fini_off = 0, init_vm = 0, fini_vm = 0;
    if (has_init) {
        vm = align_up_u64(vm, 8);
        file = align_up_u64(file, 8);
        init_vm = vm;
        init_off = file;
        vm += init_bytes;
        file += init_bytes;
    }
    if (has_fini) {
        vm = align_up_u64(vm, 8);
        file = align_up_u64(file, 8);
        fini_vm = vm;
        fini_off = file;
        vm += fini_bytes;
        file += fini_bytes;
    }
    uint64_t content_end = file;
    uint64_t vm_end = vm;

    size_t nlocal = 0, nglobal = 0, nundef = 0;
    for (size_t i = 0; i < em->num_syms; i++) {
        const EmitSymbol *s = &em->syms[i];
        if (!macho_sym_ok(s, sect_of)) continue;
        int pass = macho_sym_pass(s);
        if (pass == 0) nlocal++;
        else if (pass == 1) nglobal++;
        else nundef++;
    }
    size_t nsyms = nlocal + nglobal + nundef;
    size_t strsize = 1;
    for (size_t i = 0; i < em->num_syms; i++) {
        const EmitSymbol *s = &em->syms[i];
        if (!macho_sym_ok(s, sect_of)) continue;
        strsize += 1 + strlen(s->name) + 1; /* leading '_' */
    }
    size_t nreloc = reloc_record_count(em->relocs, em->num_relocs);
    size_t nreloc_data = reloc_record_count_sh(em->data_relocs,
                                               em->num_data_relocs, SECT_DATA);
    size_t nreloc_tdata = reloc_record_count_sh(em->data_relocs,
                                                em->num_data_relocs,
                                                SECT_TDATA);
    size_t nreloc_init = reloc_record_count_sh(em->data_relocs,
                                               em->num_data_relocs,
                                               SECT_INIT_ARRAY);
    size_t nreloc_fini = reloc_record_count_sh(em->data_relocs,
                                               em->num_data_relocs,
                                               SECT_FINI_ARRAY);
    uint64_t rel_at = align_up_u64(content_end, 8);
    uint64_t reloff = nreloc ? rel_at : 0;
    rel_at += nreloc * 8;
    uint64_t data_reloff = nreloc_data ? rel_at : 0;
    rel_at += nreloc_data * 8;
    uint64_t tdata_reloff = nreloc_tdata ? rel_at : 0;
    rel_at += nreloc_tdata * 8;
    uint64_t init_reloff = nreloc_init ? rel_at : 0;
    rel_at += nreloc_init * 8;
    uint64_t fini_reloff = nreloc_fini ? rel_at : 0;
    rel_at += nreloc_fini * 8;
    uint64_t symoff = align_up_u64(rel_at, 8);
    uint64_t stroff = symoff + nsyms * sizeof(nlist_64);

    Buffer out;
    buffer_init(&out);
    mach_header_64 hdr;
    memset(&hdr, 0, sizeof hdr);
    hdr.magic = MH_MAGIC_64;
    hdr.cputype = CPU_TYPE_ARM64;
    hdr.cpusubtype = CPU_SUBTYPE_ARM64_ALL;
    hdr.filetype = MH_OBJECT;
    hdr.ncmds = 4;
    hdr.sizeofcmds = sizeofcmds;
    hdr.flags = MH_SUBSECTIONS_VIA_SYMBOLS;
    buffer_append(&out, (const char *)&hdr, sizeof hdr);

    segment_command_64 seg;
    memset(&seg, 0, sizeof seg);
    seg.cmd = LC_SEGMENT_64;
    seg.cmdsize = seg_cmdsize;
    seg.vmsize = vm_end;
    seg.fileoff = content_off;
    seg.filesize = content_end - content_off;
    seg.maxprot = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC;
    seg.initprot = seg.maxprot;
    seg.nsects = nsects;
    buffer_append(&out, (const char *)&seg, sizeof seg);

    section_64 sec;
    memset(&sec, 0, sizeof sec);
    memcpy(sec.sectname, "__text", 6);
    memcpy(sec.segname, "__TEXT", 6);
    sec.addr = 0;
    sec.size = em->text.len;
    sec.offset = (uint32_t)text_off;
    sec.align = macho_align_log(em->text_align, 2);
    sec.reloff = nreloc ? (uint32_t)reloff : 0;
    sec.nreloc = (uint32_t)nreloc;
    sec.flags = S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS;
    buffer_append(&out, (const char *)&sec, sizeof sec);
    if (has_ro) {
        memset(&sec, 0, sizeof sec);
        memcpy(sec.sectname, "__const", 7);
        memcpy(sec.segname, "__TEXT", 6);
        sec.addr = sect_addr[SECT_RODATA];
        sec.size = em->rodata.len;
        sec.offset = (uint32_t)ro_off;
        sec.align = macho_align_log(em->rodata_align, 3);
        buffer_append(&out, (const char *)&sec, sizeof sec);
    }
    if (has_data) {
        memset(&sec, 0, sizeof sec);
        memcpy(sec.sectname, "__data", 6);
        memcpy(sec.segname, "__DATA", 6);
        sec.addr = sect_addr[SECT_DATA];
        sec.size = em->data.len;
        sec.offset = (uint32_t)data_off;
        sec.align = macho_align_log(em->data_align, 3);
        sec.reloff = nreloc_data ? (uint32_t)data_reloff : 0;
        sec.nreloc = (uint32_t)nreloc_data;
        buffer_append(&out, (const char *)&sec, sizeof sec);
    }
    if (has_bss) {
        memset(&sec, 0, sizeof sec);
        memcpy(sec.sectname, "__bss", 5);
        memcpy(sec.segname, "__DATA", 6);
        sec.addr = sect_addr[SECT_BSS];
        sec.size = em->bss_size;
        sec.align = macho_align_log(em->bss_align, 3);
        sec.flags = S_ZEROFILL;
        buffer_append(&out, (const char *)&sec, sizeof sec);
    }
    if (has_tdata) {
        memset(&sec, 0, sizeof sec);
        memcpy(sec.sectname, "__thread_data", 13);
        memcpy(sec.segname, "__DATA", 6);
        sec.addr = sect_addr[SECT_TDATA];
        sec.size = em->tdata.len;
        sec.offset = (uint32_t)tdata_off;
        sec.align = macho_align_log(em->tdata_align, 3);
        sec.reloff = nreloc_tdata ? (uint32_t)tdata_reloff : 0;
        sec.nreloc = (uint32_t)nreloc_tdata;
        /* The type is what marks this as a thread-local template, so a
         * static linker knows to concatenate it into the merged block. */
        sec.flags = S_THREAD_LOCAL_REGULAR;
        buffer_append(&out, (const char *)&sec, sizeof sec);
    }
    if (has_tbss) {
        memset(&sec, 0, sizeof sec);
        memcpy(sec.sectname, "__thread_bss", 12);
        memcpy(sec.segname, "__DATA", 6);
        sec.addr = sect_addr[SECT_TBSS];
        sec.size = em->tbss_size;
        sec.align = macho_align_log(em->tbss_align, 3);
        sec.flags = S_THREAD_LOCAL_ZEROFILL;
        buffer_append(&out, (const char *)&sec, sizeof sec);
    }
    if (has_init) {
        memset(&sec, 0, sizeof sec);
        memcpy(sec.sectname, "__init_array", 12);
        memcpy(sec.segname, "__DATA", 6);
        sec.addr = init_vm;
        sec.size = init_bytes;
        sec.offset = (uint32_t)init_off;
        sec.align = 3;
        sec.reloff = nreloc_init ? (uint32_t)init_reloff : 0;
        sec.nreloc = (uint32_t)nreloc_init;
        buffer_append(&out, (const char *)&sec, sizeof sec);
    }
    if (has_fini) {
        memset(&sec, 0, sizeof sec);
        memcpy(sec.sectname, "__fini_array", 12);
        memcpy(sec.segname, "__DATA", 6);
        sec.addr = fini_vm;
        sec.size = fini_bytes;
        sec.offset = (uint32_t)fini_off;
        sec.align = 3;
        sec.reloff = nreloc_fini ? (uint32_t)fini_reloff : 0;
        sec.nreloc = (uint32_t)nreloc_fini;
        buffer_append(&out, (const char *)&sec, sizeof sec);
    }

    build_version_command bv;
    memset(&bv, 0, sizeof bv);
    bv.cmd = LC_BUILD_VERSION;
    bv.cmdsize = (uint32_t)sizeof bv;
    bv.platform = PLATFORM_MACOS;
    bv.minos = 0x000E0000u; /* 14.0.0 */
    bv.sdk = 0x000E0000u;
    buffer_append(&out, (const char *)&bv, sizeof bv);

    symtab_command sy;
    memset(&sy, 0, sizeof sy);
    sy.cmd = LC_SYMTAB;
    sy.cmdsize = (uint32_t)sizeof sy;
    sy.symoff = (uint32_t)symoff;
    sy.nsyms = (uint32_t)nsyms;
    sy.stroff = (uint32_t)stroff;
    sy.strsize = (uint32_t)strsize;
    buffer_append(&out, (const char *)&sy, sizeof sy);

    dysymtab_command dy;
    memset(&dy, 0, sizeof dy);
    dy.cmd = LC_DYSYMTAB;
    dy.cmdsize = (uint32_t)sizeof dy;
    dy.ilocalsym = 0;
    dy.nlocalsym = (uint32_t)nlocal;
    dy.iextdefsym = (uint32_t)nlocal;
    dy.nextdefsym = (uint32_t)nglobal;
    dy.iundefsym = (uint32_t)(nlocal + nglobal);
    dy.nundefsym = (uint32_t)nundef;
    buffer_append(&out, (const char *)&dy, sizeof dy);

    while (out.len < content_off) {
        char z = 0;
        buffer_append(&out, &z, 1);
    }
    if (em->text.len)
        buffer_append(&out, em->text.data, em->text.len);
    if (has_ro) {
        while (out.len < ro_off) { char z = 0; buffer_append(&out, &z, 1); }
        buffer_append(&out, em->rodata.data, em->rodata.len);
    }
    if (has_data) {
        while (out.len < data_off) { char z = 0; buffer_append(&out, &z, 1); }
        buffer_append(&out, em->data.data, em->data.len);
    }
    if (has_tdata) {
        while (out.len < tdata_off) { char z = 0; buffer_append(&out, &z, 1); }
        buffer_append(&out, em->tdata.data, em->tdata.len);
    }
    if (has_init) {
        while (out.len < init_off) { char z = 0; buffer_append(&out, &z, 1); }
        append_hook_section(&out, &em->init_array, em->init_prio);
    }
    if (has_fini) {
        while (out.len < fini_off) { char z = 0; buffer_append(&out, &z, 1); }
        append_hook_section(&out, &em->fini_array, em->fini_prio);
    }
    {
        uint64_t rel_start = symoff;
        if (nreloc) rel_start = reloff;
        else if (nreloc_data) rel_start = data_reloff;
        else if (nreloc_tdata) rel_start = tdata_reloff;
        else if (nreloc_init) rel_start = init_reloff;
        else if (nreloc_fini) rel_start = fini_reloff;
        while (out.len < rel_start) { char z = 0; buffer_append(&out, &z, 1); }
    }

    /* nlist order is locals then globals.  Relocations name that index. */
    int *order = nsyms ? xmalloc(nsyms * sizeof(int)) : NULL;
    int *nmap = em->num_syms ? xmalloc(em->num_syms * sizeof(int)) : NULL;
    for (size_t i = 0; i < em->num_syms; i++) nmap[i] = -1;
    size_t ni = 0;
    for (int pass = 0; pass < 3; pass++) {
        for (size_t i = 0; i < em->num_syms; i++) {
            const EmitSymbol *s = &em->syms[i];
            if (!macho_sym_ok(s, sect_of)) continue;
            if (macho_sym_pass(s) != pass) continue;
            if (order) order[ni] = (int)i;
            if (nmap) nmap[i] = (int)ni;
            ni++;
        }
    }
    /* nreloc counts on-disk records (ADDEND pairs expand 1:2); the array
     * itself still has em->num_relocs entries, so pass that for the bound. */
    macho_write_relocs(&out, em->relocs, em->num_relocs, nmap, em->num_syms);
    macho_write_relocs_sh(&out, em->data_relocs, em->num_data_relocs, SECT_DATA,
                          nmap, em->num_syms, 0);
    macho_write_relocs_sh(&out, em->data_relocs, em->num_data_relocs,
                          SECT_TDATA, nmap, em->num_syms, 0);
    macho_write_relocs_sh(&out, em->data_relocs, em->num_data_relocs,
                          SECT_INIT_ARRAY, nmap, em->num_syms, 1);
    macho_write_relocs_sh(&out, em->data_relocs, em->num_data_relocs,
                          SECT_FINI_ARRAY, nmap, em->num_syms, 1);
    while (out.len < symoff) { char z = 0; buffer_append(&out, &z, 1); }

    /* Locals first, then globals, matching LC_DYSYMTAB.  String offsets
     * are assigned in the same order. */
    size_t str_at = 1;
    for (int pass = 0; pass < 3; pass++) {
        for (size_t i = 0; i < em->num_syms; i++) {
            const EmitSymbol *s = &em->syms[i];
            if (!macho_sym_ok(s, sect_of)) continue;
            if (macho_sym_pass(s) != pass) continue;
            int common = s->shndx == SHN_COMMON;
            int undef = s->shndx == SECT_UNDEF || common;
            nlist_64 nl;
            memset(&nl, 0, sizeof nl);
            nl.n_strx = (uint32_t)str_at;
            nl.n_type = (uint8_t)(undef ? N_EXT
                                : s->binding == 0 ? N_SECT : (N_SECT | N_EXT));
            if (s->binding == 3) nl.n_type |= (uint8_t)N_PEXT;
            nl.n_sect = undef ? 0 : sect_of[s->shndx];
            if (common) {
                unsigned align = s->value ? (unsigned)s->value : 1;
                unsigned log = 0;
                while ((1u << log) < align && log < 15) log++;
                nl.n_desc = (uint16_t)(log << 8);
                nl.n_value = (uint64_t)s->size;
            } else {
                nl.n_value = undef ? 0
                           : sect_addr[s->shndx] + (uint64_t)s->value;
                if (s->binding == 2)
                    nl.n_desc = undef ? 0x0040u : 0x0080u; /* N_WEAK_REF / N_WEAK_DEF */
            }
            nl.n_desc |= (uint16_t)(s->macho_desc & 0x0020u); /* N_NO_DEAD_STRIP */
            buffer_append(&out, (const char *)&nl, sizeof nl);
            str_at += 1 + strlen(s->name) + 1;
        }
    }
    char nul = 0;
    buffer_append(&out, &nul, 1);
    for (int pass = 0; pass < 3; pass++) {
        for (size_t i = 0; i < em->num_syms; i++) {
            const EmitSymbol *s = &em->syms[i];
            if (!macho_sym_ok(s, sect_of)) continue;
            if (macho_sym_pass(s) != pass) continue;
            buffer_append(&out, "_", 1);
            buffer_append(&out, s->name, strlen(s->name) + 1);
        }
    }
    free(order);
    free(nmap);

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "fakecc: cannot create '%s'\n", path);
        buffer_free(&out);
        return -1;
    }
    size_t written = fwrite(out.data, 1, out.len, f);
    size_t total = out.len;
    fclose(f);
    chmod(path, 0644);
    buffer_free(&out);
    if (written != total) {
        fprintf(stderr, "fakecc: short write on '%s'\n", path);
        return -1;
    }
    return 0;
}

/* Mach-O's section_64.sectname is exactly 16 bytes and ld64 does use all
 * sixteen of them -- __compact_unwind is a real, everyday section whose name
 * has no room for a terminator.  strcmp therefore never matches it, and the
 * section drops out of the object for no visible reason.  Compare within the
 * field's width instead: a shorter name has to be followed by a NUL, and a
 * name that fills all 16 bytes matches when it too fills all 16. */
static int sect_is(const char *sect, const char *name) {
    for (int i = 0; i < 16; i++) {
        if (name[i] == '\0') return sect[i] == '\0';
        if (sect[i] != name[i]) return 0;
    }
    return name[16] == '\0';
}

int macho_read_object(const char *path, EmitModule *em) {
    /* An object file carries no user/package distinction -- only a module
     * the driver built from a command-line source is a user TU.  Clear it so
     * stale stack storage cannot masquerade as one. */
    em->is_user_tu = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "fakecc: cannot open '%s'\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < (long)sizeof(mach_header_64)) {
        fclose(f);
        fprintf(stderr, "fakecc: '%s' is too small for a Mach-O object\n", path);
        return -1;
    }
    unsigned char *buf = xmalloc((size_t)fsize);
    if (fread(buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        fclose(f);
        free(buf);
        fprintf(stderr, "fakecc: short read on '%s'\n", path);
        return -1;
    }
    fclose(f);

    mach_header_64 hdr;
    memcpy(&hdr, buf, sizeof hdr);
    if (hdr.magic != MH_MAGIC_64 || hdr.filetype != MH_OBJECT) {
        free(buf);
        fprintf(stderr, "fakecc: '%s' is not an arm64 Mach-O object\n", path);
        return -1;
    }

    typedef struct {
        char     sect[16];
        char     seg[16];
        uint64_t addr, size;
        uint32_t offset, reloff, nreloc, flags, align;
        uint16_t shndx;
        int      meta;   /* link-time metadata: bytes kept, relocations not */
        int      known_unsupported;
        int      no_prio; /* constructor list without a priority field */
    } RSec;
    RSec secs[16];
    int nsec = 0;
    uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;

    const unsigned char *p = buf + sizeof hdr;
    const unsigned char *pend = p + hdr.sizeofcmds;
    if (pend > buf + fsize) pend = buf + fsize;
    for (uint32_t ci = 0; ci < hdr.ncmds && p + 8 <= pend; ci++) {
        uint32_t cmd = 0, cmdsize = 0;
        memcpy(&cmd, p, 4);
        memcpy(&cmdsize, p + 4, 4);
        if (cmdsize < 8 || p + cmdsize > buf + fsize) break;
        if (cmd == LC_SEGMENT_64 && cmdsize >= sizeof(segment_command_64)) {
            segment_command_64 seg;
            memcpy(&seg, p, sizeof seg);
            const unsigned char *sp = p + sizeof seg;
            for (uint32_t si = 0; si < seg.nsects && nsec < 16; si++) {
                if (sp + sizeof(section_64) > p + cmdsize) break;
                section_64 sec;
                memcpy(&sec, sp, sizeof sec);
                sp += sizeof sec;
                RSec *rs = &secs[nsec++];
                memset(rs, 0, sizeof *rs);
                memcpy(rs->sect, sec.sectname, 16);
                memcpy(rs->seg, sec.segname, 16);
                rs->addr = sec.addr;
                rs->size = sec.size;
                rs->offset = sec.offset;
                rs->reloff = sec.reloff;
                rs->nreloc = sec.nreloc;
                rs->flags = sec.flags;
                rs->align = sec.align;
                rs->shndx = 0;
                if (sect_is(rs->sect, "__text")) rs->shndx = SECT_TEXT;
                /* Read-only data arrives under a family of names, and ld64
                 * emits as many as a translation unit needs: __const next to
                 * __cstring next to the literal pools.  They are all just
                 * bytes we must keep addressable, because a relocation can
                 * name anything in them. */
                else if (sect_is(rs->sect, "__const")
                         || sect_is(rs->sect, "__cstring")
                         || sect_is(rs->sect, "__ustring")
                         || sect_is(rs->sect, "__literal4")
                         || sect_is(rs->sect, "__literal8")
                         || sect_is(rs->sect, "__literal16"))
                    rs->shndx = SECT_RODATA;
                else if (sect_is(rs->sect, "__data")) rs->shndx = SECT_DATA;
                else if (sect_is(rs->sect, "__bss")) rs->shndx = SECT_BSS;
                /* __compact_unwind carries a local symbol, so it cannot just
                 * be ignored: skipping it outright would shift every symbol
                 * index and break the relocations that follow.  Its own
                 * relocations are section-relative entries into the unwind
                 * table, which only means something to ld64, so the bytes
                 * travel along as ordinary read-only data and those records
                 * are dropped.  The image ends up without unwind info --
                 * fakecc emits none of its own either way. */
                else if (sect_is(rs->sect, "__compact_unwind")
                         || sect_is(rs->sect, "__eh_frame")) {
                    rs->shndx = SECT_RODATA;
                    rs->meta = 1;
                }
                /* The thread-local names are fixed by ld64; matching on the
                 * section type as well means an object written by another
                 * tool still tells us which half of the block it is.  The
                 * type is the low byte of flags, and the three thread-local
                 * types differ only in bits 0 and 1 -- so it has to be
                 * compared whole, not masked, or the zero-fill type matches
                 * the regular one. */
                else if (sect_is(rs->sect, "__thread_data")
                         || (sec.flags & 0xFFu) == S_THREAD_LOCAL_REGULAR)
                    rs->shndx = SECT_TDATA;
                else if (sect_is(rs->sect, "__thread_bss")
                         || (sec.flags & 0xFFu) == S_THREAD_LOCAL_ZEROFILL)
                    rs->shndx = SECT_TBSS;
                /* The descriptor table.  Nothing here is copied: its binds
                 * and its per-descriptor offsets describe a block layout this
                 * linker is about to merge and renumber, so the table is
                 * rebuilt from scratch.  The section still has to be
                 * *recognised*, because that is where a thread-local symbol
                 * points -- see the symbol loop. */
                else if (sect_is(rs->sect, "__thread_vars")
                         || (sec.flags & 0xFFu) == S_THREAD_LOCAL_VARIABLES) {
                    rs->shndx = SECT_TVARS;
                    rs->meta = 1;
                }
                /* ld64's static-initialisation stub.  Recognised so the
                 * diagnostic can say what is missing, but deliberately not
                 * merged: the section's declared size stops short of the
                 * `__GLOBAL_init_<prio>` entry stub that sits just past its
                 * end, and that stub -- which holds the constructor body and
                 * the once-only guard -- is generated by the linker.  Wiring
                 * up the part that exists and calling into the part that does
                 * not produces an image that branches to itself forever, so a
                 * clear refusal is the better failure. */
                else if (sect_is(rs->sect, "__StaticInit")) {
                    rs->known_unsupported = 1;
                }
                /* Clang's constructor / destructor lists.  Same job as
                 * __init_array, under the name the ELF-shaped toolchain uses,
                 * and what a stock clang .o carries for __attribute__
                 * ((constructor)) and friends. */
                else if (sect_is(rs->sect, "__mod_init_func")) {
                    rs->shndx = SECT_INIT_ARRAY;
                    rs->no_prio = 1;
                } else if (sect_is(rs->sect, "__mod_term_func")) {
                    rs->shndx = SECT_FINI_ARRAY;
                    rs->no_prio = 1;
                }
                else if (sect_is(rs->sect, "__init_array"))
                    rs->shndx = SECT_INIT_ARRAY;
                else if (sect_is(rs->sect, "__fini_array"))
                    rs->shndx = SECT_FINI_ARRAY;
            }
        } else if (cmd == LC_SYMTAB && cmdsize >= sizeof(symtab_command)) {
            symtab_command sy;
            memcpy(&sy, p, sizeof sy);
            symoff = sy.symoff;
            nsyms = sy.nsyms;
            stroff = sy.stroff;
            strsize = sy.strsize;
        }
        p += cmdsize;
    }

    emit_module_init(em);
    /* A pointer stored in read-only data needs a chained fixup, and dyld only
     * applies those to writable pages -- __TEXT is mapped r-x.  So a
     * read-only section that carries data relocations has to travel to
     * __DATA instead, which is the same call fakecc's own codegen makes for a
     * const object holding a pointer.  Decided before any bytes are copied,
     * because the section kind decides the destination buffer. */
    for (int i = 0; i < nsec; i++) {
        RSec *rs = &secs[i];
        if (rs->meta) continue;
        if (rs->shndx != SECT_RODATA || !rs->nreloc) continue;
        if ((uint64_t)rs->reloff + (uint64_t)rs->nreloc * 8 > (uint64_t)fsize)
            continue;   /* out of range: reported with a better message later */
        for (uint32_t k = 0; k < rs->nreloc; k++) {
            uint32_t w = 0;
            memcpy(&w, buf + rs->reloff + k * 8 + 4, 4);
            uint32_t typ = (w >> 28) & 0xFu;
            if (typ == 0 || typ == 1) { rs->shndx = SECT_DATA; break; }
        }
    }
    /* The alignment the object itself used for its thread block.  A
     * descriptor's block offset is measured from the start of that block, so
     * telling an initialized variable from a zero-filled one means knowing
     * where the second half begins -- which is the template's size rounded up
     * to exactly this. */
    size_t tls_max_align = 8;
    for (int i = 0; i < nsec; i++) {
        if (secs[i].shndx != SECT_TDATA && secs[i].shndx != SECT_TBSS) continue;
        if (secs[i].align >= 16) continue;
        size_t al = (size_t)1 << secs[i].align;
        if (al > tls_max_align) tls_max_align = al;
    }
    /* Where each section's bytes landed inside its destination buffer.  One
     * fakecc buffer holds several input sections -- a clang object alone
     * splits read-only data across __const, __cstring and the literal pools
     * -- and the linker adds one base per buffer per module, so a symbol's
     * value has to carry the offset of its own section within that buffer or
     * the last section written would land on top of the first. */
    size_t secbuf_off[16];
    memset(secbuf_off, 0, sizeof secbuf_off);
    for (int i = 0; i < nsec; i++) {
        RSec *rs = &secs[i];
        if (rs->shndx == 0) continue;
        /* A zerofill section has no bytes in the file: its offset is 0, so
         * reading `size` bytes from there would splice the Mach-O header into
         * whatever buffer got picked.  __thread_bss is zerofill too, and its
         * size is recorded from the section header further down.  The type is
         * the low byte of flags and has to be compared whole: masking with
         * S_ZEROFILL also matches S_THREAD_LOCAL_REGULAR (0x11), whose bit 0
         * is set for no reason the skip cares about. */
        if ((rs->flags & 0xFFu) == S_ZEROFILL) continue;
        if (rs->shndx == SECT_TBSS) continue;
        if (rs->shndx == SECT_INIT_ARRAY || rs->shndx == SECT_FINI_ARRAY) {
            if ((uint64_t)rs->offset + rs->size > (uint64_t)fsize) {
                free(buf);
                emit_module_free(em);
                fprintf(stderr, "fakecc: '%s' section extends past the file\n", path);
                return -1;
            }
            int fini = rs->shndx == SECT_FINI_ARRAY;
            Buffer *arr = fini ? &em->fini_array : &em->init_array;
            int **prio = fini ? &em->fini_prio : &em->init_prio;
            const unsigned char *bytes = buf + rs->offset;
            size_t base = arr->len / 8;
            /* Two shapes for the same list.  .init_array holds {pointer,
             * priority} pairs, which is what fakecc writes and what makes an
             * out-of-order list possible at all.  The ELF-shaped name clang
             * uses, __mod_init_func, holds bare pointers already sorted, and
             * reading it with a 16-byte stride swallows two entries per slot
             * and takes the second entry's pointer as a priority -- so half
             * the constructors never run, and the ones that do sort by
             * whatever that pointer happens to be. */
            size_t stride = rs->no_prio ? 8 : 16;
            size_t nslot = (size_t)rs->size / stride;
            for (size_t s = 0; s < nslot; s++) {
                buffer_append(arr, (const char *)bytes + s * stride, 8);
                int64_t pv = INIT_PRIO_DEFAULT;
                if (!rs->no_prio) memcpy(&pv, bytes + s * 16 + 8, 8);
                *prio = realloc(*prio, (base + s + 1) * sizeof(int));
                if (!*prio) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
                (*prio)[base + s] = (int)pv;
            }
            continue;
        }
        if ((uint64_t)rs->offset + rs->size > (uint64_t)fsize) {
            free(buf);
            emit_module_free(em);
            fprintf(stderr, "fakecc: '%s' section extends past the file\n", path);
            return -1;
        }
        Buffer *dst = rs->shndx == SECT_TEXT ? &em->text
                    : rs->shndx == SECT_RODATA ? &em->rodata
                    : rs->shndx == SECT_TDATA ? &em->tdata
                    : &em->data;
        secbuf_off[i] = dst->len;
        buffer_append(dst, (const char *)buf + rs->offset, (size_t)rs->size);
    }
    for (int i = 0; i < nsec; i++) {
        if (secs[i].shndx == SECT_BSS) em->bss_size = (size_t)secs[i].size;
        if (secs[i].shndx == SECT_TBSS) em->tbss_size = (size_t)secs[i].size;
        if (secs[i].align >= 16) continue;
        size_t al = (size_t)1 << secs[i].align;
        if (secs[i].shndx == SECT_TEXT && al > em->text_align)
            em->text_align = al;
        else if (secs[i].shndx == SECT_RODATA && al > em->rodata_align)
            em->rodata_align = al;
        else if (secs[i].shndx == SECT_DATA && al > em->data_align)
            em->data_align = al;
        else if (secs[i].shndx == SECT_BSS && al > em->bss_align)
            em->bss_align = al;
        else if (secs[i].shndx == SECT_TDATA && al > em->tdata_align)
            em->tdata_align = al;
        else if (secs[i].shndx == SECT_TBSS && al > em->tbss_align)
            em->tbss_align = al;
    }

    if ((uint64_t)symoff + (uint64_t)nsyms * sizeof(nlist_64) > (uint64_t)fsize ||
        (uint64_t)stroff + strsize > (uint64_t)fsize) {
        free(buf);
        emit_module_free(em);
        fprintf(stderr, "fakecc: '%s' symbol table is out of range\n", path);
        return -1;
    }
    const char *str = (const char *)buf + stroff;
    /* Defined symbols are collected first and emitted in a second pass,
     * because resolving a thread-local one means following its descriptor to
     * another symbol in the same table -- and that symbol can sit anywhere in
     * it, or even not exist. */
    typedef struct {
        const char *name;
        uint8_t     binding, ty, desc;
        uint16_t    shndx_raw;
        uint32_t    sect;        /* 1-based section index, as in n_sect */
        uint64_t    value;       /* raw n_value */
        uint64_t    size;        /* tentative definition: n_value is the size */
    } DefSym;
    DefSym *defs = nsyms ? xmalloc(nsyms * sizeof(DefSym)) : NULL;
    size_t ndef = 0;
    /* Descriptor slot within __thread_vars -> target symbol index.  Only the
     * third word of a descriptor is filled in from the object; the first is
     * the thunk bind every descriptor shares. */
    int32_t *tvar_target = NULL;
    size_t n_tvar_desc = 0;
    for (int i = 0; i < nsec; i++) {
        if (secs[i].shndx != SECT_TVARS) continue;
        size_t ndesc = secs[i].size / 24;
        if (ndesc == 0) continue;
        int32_t *grown = xrealloc(tvar_target,
                                  (n_tvar_desc + ndesc) * sizeof(int32_t));
        if (!grown) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
        tvar_target = grown;
        for (size_t k = 0; k < ndesc; k++) tvar_target[n_tvar_desc + k] = -1;
        n_tvar_desc += ndesc;
        if (!secs[i].nreloc) continue;
        if ((uint64_t)secs[i].reloff + (uint64_t)secs[i].nreloc * 8 > (uint64_t)fsize)
            continue;   /* out of range: reported with a better message later */
        for (uint32_t k = 0; k < secs[i].nreloc; k++) {
            int32_t at = 0;
            uint32_t word = 0;
            memcpy(&at, buf + secs[i].reloff + k * 8, 4);
            memcpy(&word, buf + secs[i].reloff + k * 8 + 4, 4);
            if (at < 0 || at % 24 != 16) continue;
            size_t idx = (size_t)at / 24;
            if (idx >= ndesc) continue;
            tvar_target[n_tvar_desc - ndesc + idx] = (int32_t)(word & 0xFFFFFFu);
        }
    }
    for (uint32_t i = 0; i < nsyms; i++) {
        nlist_64 nl;
        memcpy(&nl, buf + symoff + i * sizeof nl, sizeof nl);
        const char *raw = (nl.n_strx < strsize) ? str + nl.n_strx : "";
        if (raw[0] == '_') raw++;
        int undef = nl.n_sect == 0;
        if (undef) {
            DefSym *d = &defs[ndef++];
            memset(d, 0, sizeof *d);
            d->name = raw;
            d->desc = (uint8_t)(nl.n_desc & 0x0020u);
            d->shndx_raw = SECT_UNDEF;
            d->value = nl.n_value;
            /* A tentative definition has a name, a type and no section:
             * n_value is its size, and the alignment rides in the low
             * descriptor bits.  A plain undefined reference has a zero value
             * and is weak when the symbol says so.  Which of the two it is
             * decides whether the symbol is allocated at link time at all. */
            if (nl.n_value > 0 && (nl.n_type & N_EXT)) {
                unsigned log = ((unsigned)nl.n_desc >> 8) & 0x0fu;
                d->binding = 1;
                d->value = log ? (size_t)1 << log : 1;   /* alignment */
                d->size = nl.n_value;                     /* bytes wanted */
            } else {
                d->binding = (nl.n_desc & 0x0040) ? 2 : 1;
            }
            continue;
        }
        if (nl.n_sect > nsec || secs[nl.n_sect - 1].shndx == 0) {
            /* Name the symbol and its section: "unsupported section" alone
             * has cost real time on its own, because the usual cause is a
             * section this reader simply does not know yet. */
            const char *where = (nl.n_sect <= nsec)
                ? secs[nl.n_sect - 1].sect : "?";
            int known = (nl.n_sect <= nsec)
                ? secs[nl.n_sect - 1].known_unsupported : 0;
            free(buf); free(defs); free(tvar_target);
            emit_module_free(em);
            if (known) {
                fprintf(stderr,
                        "fakecc: '%s': a %s is out of reach -- this linker "
                        "does not generate ld64's __GLOBAL_init_<prio> entry "
                        "stub yet, and the constructor body lives in it\n",
                        path, where);
                return -1;
            }
            fprintf(stderr, "fakecc: '%s' symbol '%s' in unsupported section "
                    "%s,%s\n", path, raw[0] ? raw : "?", where,
                    (nl.n_sect <= nsec) ? secs[nl.n_sect - 1].seg : "?");
            return -1;
        }
        RSec *rs = &secs[nl.n_sect - 1];
        DefSym *d = &defs[ndef++];
        memset(d, 0, sizeof *d);
        d->name = raw;
        d->binding = (nl.n_desc & 0x0080) ? 2
                   : (nl.n_type & N_PEXT) ? 3
                   : (nl.n_type & N_EXT) ? 1 : 0;
        d->desc = (uint8_t)(nl.n_desc & 0x0020u);
        d->shndx_raw = rs->shndx;
        d->sect = nl.n_sect;
        d->value = nl.n_value;
    }
    /* Emitted in nlist order, undefined entries included: relocation records
     * name nlist indices, so anything else silently renumbers the symbol
     * table out from under them -- every reloc then lands on the wrong
     * symbol, which for a thread-local pair means the two variables swap
     * storage and read each other's bytes. */
    for (size_t k = 0; k < ndef; k++) {
        DefSym *d = &defs[k];
        uint16_t sh = d->shndx_raw;
        size_t value;
        if (sh == SECT_UNDEF) {
            int common = d->size > 0;
            emit_module_add_symbol(em, d->name[0] ? d->name : NULL,
                                   d->binding, common ? 1 : 0,
                                   common ? (uint16_t)SHN_COMMON
                                          : (uint16_t)SECT_UNDEF,
                                   common ? d->value : 0,
                                   common ? (size_t)d->size : 0);
            if (em->num_syms)
                em->syms[em->num_syms - 1].macho_desc = d->desc;
            continue;
        }
        if (sh == SECT_TVARS) {
            /* A thread-local symbol has no address of its own -- every
             * thread gets its own copy -- so it points at its 24-byte
             * tlv_descriptor.  In a *relocatable* object the descriptor's
             * third word is not the block offset: it is a relocation naming
             * `x$tlv$init`, the initializer for a dynamically-resolved block,
             * which in the static case is the variable's own storage.  That
             * target is the only thing in the object saying which half of the
             * template the variable occupies, so follow it -- reading the
             * word as a number instead puts every variable at offset 0, and
             * they all share one slot at run time. */
            uint64_t desc_off = (size_t)(d->value - secs[d->sect - 1].addr);
            int32_t tgt = -1;
            if (desc_off % 24 == 0 && desc_off / 24 < n_tvar_desc)
                tgt = tvar_target[desc_off / 24];
            if (tgt >= 0 && (size_t)tgt < ndef) {
                DefSym *td = &defs[tgt];
                if (td->shndx_raw == SECT_TDATA || td->shndx_raw == SECT_TBSS) {
                    sh = td->shndx_raw;
                    value = secbuf_off[td->sect - 1]
                          + (size_t)(td->value - secs[td->sect - 1].addr);
                } else {
                    tgt = -1;
                }
            } else {
                tgt = -1;
            }
            if (tgt < 0) {
                /* Not every symbol in the table is a variable.  ld64 drops
                 * local `ltmpN` labels inside __thread_vars, and no
                 * relocation ever names one -- but they still occupy an nlist
                 * slot, and this reader's symbol indices have to line up with
                 * the relocation records, so they cannot simply be dropped.
                 * An *external* one is a real thread-local whose descriptor we
                 * failed to make sense of, and that is worth an error. */
                if (d->binding != 0) {
                    free(buf); free(defs); free(tvar_target);
                    emit_module_free(em);
                    fprintf(stderr, "fakecc: '%s' thread-local symbol '%s' has "
                            "no usable initializer in its descriptor\n", path,
                            d->name[0] ? d->name : "?");
                    return -1;
                }
                value = desc_off;
            }
        } else {
            /* Section-relative, plus wherever this section landed in the
             * buffer it was merged into -- see secbuf_off above. */
            value = secbuf_off[d->sect - 1]
                  + (size_t)(d->value - secs[d->sect - 1].addr);
        }
        uint8_t ty = sh == SECT_TEXT ? 2 : 1;
        emit_module_add_symbol(em, d->name[0] ? d->name : NULL, d->binding, ty,
                               sh, value, 0);
        if (em->num_syms)
            em->syms[em->num_syms - 1].macho_desc = d->desc;
    }
    free(defs);
    free(tvar_target);

    for (int i = 0; i < nsec; i++) {
        RSec *rs = &secs[i];
        if (!rs->nreloc || rs->meta) continue;
        if ((uint64_t)rs->reloff + (uint64_t)rs->nreloc * 8 > (uint64_t)fsize) {
            free(buf);
            emit_module_free(em);
            fprintf(stderr, "fakecc: '%s' relocations are out of range\n", path);
            return -1;
        }
        int32_t pending = 0;
        int have_pending = 0;
        /* Section-relative until now: the linker merges buffers, so the slot
         * has to be moved by the same amount its section was. */
        size_t slot_base = secbuf_off[i];
        for (uint32_t ri = 0; ri < rs->nreloc; ri++) {
            int32_t addr = 0;
            uint32_t word = 0;
            memcpy(&addr, buf + rs->reloff + ri * 8, 4);
            memcpy(&word, buf + rs->reloff + ri * 8 + 4, 4);
            uint32_t sym = word & 0xFFFFFFu;
            uint32_t type = (word >> 28) & 0xFu;
            if (type == 10) {
                /* ARM64_RELOC_ADDEND: signed 24-bit addend for the next reloc. */
                pending = (int32_t)(sym & 0xFFFFFFu);
                if (pending & 0x800000) pending |= ~0xFFFFFF;
                have_pending = 1;
                continue;
            }
            int32_t addend = 0;
            if (type == 0 && rs->shndx == SECT_DATA &&
                (size_t)addr + slot_base + 8 <= em->data.len) {
                int64_t full = 0;
                memcpy(&full, em->data.data + addr + slot_base, 8);
                addend = (int32_t)full;
            } else if (type == 0 && rs->shndx == SECT_TDATA &&
                       (size_t)addr + slot_base + 8 <= em->tdata.len) {
                int64_t full = 0;
                memcpy(&full, em->tdata.data + addr + slot_base, 8);
                addend = (int32_t)full;
            }
            if (have_pending) {
                addend = pending;
                have_pending = 0;
            }
            if (rs->shndx == SECT_INIT_ARRAY || rs->shndx == SECT_FINI_ARRAY) {
                /* Same two shapes as above: with no priority field the
                 * entries are 8 bytes apart, and dividing by 16 folds every
                 * second one onto the first, so only one constructor is ever
                 * registered. */
                size_t slot = rs->no_prio ? (size_t)addr
                                         : ((size_t)addr / 16) * 8;
                emit_module_add_data_reloc(em, slot, type, (int)sym, 0);
                em->data_relocs[em->num_data_relocs - 1].shndx = rs->shndx;
            } else if (rs->shndx == SECT_DATA || rs->shndx == SECT_TDATA) {
                /* The site section travels with the record: a reloc aimed at
                 * a slot inside __thread_data has to stay there when the
                 * linker merges the sections, or the initializer ends up
                 * rebased into __data at an offset that means nothing. */
                emit_module_add_data_reloc(em, (size_t)addr + slot_base,
                                           type, (int)sym, addend);
                em->data_relocs[em->num_data_relocs - 1].shndx = rs->shndx;
            } else
                emit_module_add_reloc(em, (size_t)addr, type, (int)sym, addend);
        }
    }
    free(buf);
    return 0;
}

static void patch_adrp(uint32_t *w, uint64_t pc, uint64_t tgt) {
    int64_t pages = (int64_t)((tgt & ~(uint64_t)0xFFF)
                              - (pc & ~(uint64_t)0xFFF)) >> 12;
    *w |= (uint32_t)((pages & 3) << 29)
        | (uint32_t)(((pages >> 2) & 0x7FFFF) << 5);
}

/* ARM64_RELOC_PAGEOFF12 names a *byte* offset, but where that 12-bit
 * immediate lands depends on the instruction it belongs to: add- and
 * sub-immediate hold it whole, while a load or store with an unsigned
 * 12-bit immediate holds it divided by the access size.  fakecc's own codegen
 * only ever emits `add`, so it can always OR the byte offset straight in --
 * but an object from any real toolchain writes `ldr x8, [x8]` whenever a
 * global's address is dereferenced in place, and patching that as if it were
 * an `add` lands up to eight times too far.  Nothing complains: the result is
 * still a plausible address inside __data, so the program reads a zero and
 * dereferences it. */
static void patch_add_pageoff(uint32_t *w, uint64_t tgt) {
    uint64_t off = tgt & 0xFFFu;
    uint32_t op = *w & 0xFF000000u;
    if (op != 0x91000000u && op != 0xD1000000u) {
        uint32_t scale = 1u << ((*w >> 30) & 3u);
        if (scale > 1) off /= scale;
    }
    *w |= (uint32_t)(off & 0xFFFu) << 10;
}

static int patch_bl(uint32_t *w, uint64_t pc, uint64_t tgt) {
    int64_t disp = (int64_t)tgt - (int64_t)pc;
    if ((disp & 3) || disp < -(1 << 27) || disp >= (1 << 27)) {
        fprintf(stderr, "fakecc: branch target out of range\n");
        return -1;
    }
    *w = 0x94000000u | (uint32_t)((disp >> 2) & 0x3FFFFFF);
    return 0;
}

/* Unconditional branch (b): same imm26 geometry as bl, opcode 0x14000000.
 * A cross-TU stub has to be a tail call -- it stands in for the call the
 * caller actually wrote, so the return address the caller pushed must stay
 * the one that returns to it.  Branching with link instead would push a
 * second address and resume past the end of the stub. */
static int patch_b(uint32_t *w, uint64_t pc, uint64_t tgt) {
    int64_t disp = (int64_t)tgt - (int64_t)pc;
    if ((disp & 3) || disp < -(1 << 27) || disp >= (1 << 27)) {
        fprintf(stderr, "fakecc: branch target out of range\n");
        return -1;
    }
    *w = 0x14000000u | (uint32_t)((disp >> 2) & 0x3FFFFFF);
    return 0;
}

/* Mach-O nlist has no size.  The bytes up to the next symbol in the
 * same section are the definition the common is competing with. */
static size_t macho_symbol_span(const EmitModule *m, uint16_t sh, size_t off) {
    size_t end = 0;
    if (sh == SECT_DATA) end = m->data.len;
    else if (sh == SECT_RODATA) end = m->rodata.len;
    else if (sh == SECT_BSS) end = m->bss_size;
    else if (sh == SECT_TDATA) end = m->tdata.len;
    else if (sh == SECT_TBSS) end = m->tbss_size;
    else return 0;
    for (size_t s = 0; s < m->num_syms; s++) {
        const EmitSymbol *o = &m->syms[s];
        if (o->shndx != sh || o->value <= off) continue;
        if (o->value < end) end = o->value;
    }
    return end > off ? end - off : 0;
}

/* One relocating link of -c modules into a PIE image. */
int macho_link_objects(EmitModule **mods, size_t n, const char *path) {
    if (!n) {
        fprintf(stderr, "fakecc: no modules to link\n");
        return -1;
    }
    EmitModule out;
    emit_module_init(&out);

    typedef struct { int prio; size_t mod; size_t slot; uint32_t sym; } HookRef;
    HookRef *ctors = NULL, *dtors = NULL;
    size_t nctors = 0, ndtors = 0, capc = 0, capd = 0;
    for (size_t i = 0; i < n; i++) {
        EmitModule *m = mods[i];
        for (size_t ri = 0; ri < m->num_data_relocs; ri++) {
            EmitReloc *r = &m->data_relocs[ri];
            int is_init = r->shndx == SECT_INIT_ARRAY;
            int is_fini = r->shndx == SECT_FINI_ARRAY;
            if (!is_init && !is_fini) continue;
            size_t slot = r->offset / 8;
            int prio = INIT_PRIO_DEFAULT;
            int *pt = is_init ? m->init_prio : m->fini_prio;
            size_t nslot = (is_init ? m->init_array.len : m->fini_array.len) / 8;
            if (pt && slot < nslot) prio = pt[slot];
            HookRef **arr = is_init ? &ctors : &dtors;
            size_t *cnt = is_init ? &nctors : &ndtors;
            size_t *cap = is_init ? &capc : &capd;
            if (*cnt == *cap) {
                *cap = *cap ? *cap * 2 : 4;
                *arr = xrealloc(*arr, *cap * sizeof(HookRef));
            }
            (*arr)[*cnt].prio = prio;
            (*arr)[*cnt].mod = i;
            (*arr)[*cnt].slot = slot;
            (*arr)[*cnt].sym = r->sym;
            (*cnt)++;
        }
    }
    /* Restore list order before applying priorities.  A relocation list is
     * stored in descending address order, so collecting it in order reverses
     * every constructor list -- harmless when the priorities differ, fatal
     * when they do not, which is the normal case for a list that came from a
     * toolchain that pre-sorts it.  Insertion sort on (module, slot) is
     * stable, so two hooks with the same priority keep the order the section
     * gave them. */
    for (size_t i = 1; i < nctors; i++) {
        HookRef key = ctors[i];
        size_t j = i;
        while (j > 0 && (ctors[j - 1].mod > key.mod
                      || (ctors[j - 1].mod == key.mod
                          && ctors[j - 1].slot > key.slot))) {
            ctors[j] = ctors[j - 1];
            j--;
        }
        ctors[j] = key;
    }
    for (size_t i = 1; i < nctors; i++) {
        HookRef key = ctors[i];
        size_t j = i;
        while (j > 0 && ctors[j - 1].prio > key.prio) {
            ctors[j] = ctors[j - 1];
            j--;
        }
        ctors[j] = key;
    }
    for (size_t i = 1; i < ndtors; i++) {
        HookRef key = dtors[i];
        size_t j = i;
        while (j > 0 && (dtors[j - 1].mod > key.mod
                      || (dtors[j - 1].mod == key.mod
                          && dtors[j - 1].slot > key.slot))) {
            dtors[j] = dtors[j - 1];
            j--;
        }
        dtors[j] = key;
    }
    for (size_t i = 1; i < ndtors; i++) {
        HookRef key = dtors[i];
        size_t j = i;
        while (j > 0 && dtors[j - 1].prio > key.prio) {
            dtors[j] = dtors[j - 1];
            j--;
        }
        dtors[j] = key;
    }

    int *ctor_at = nctors ? xmalloc(nctors * sizeof(int)) : NULL;
    int *dtor_at = ndtors ? xmalloc(ndtors * sizeof(int)) : NULL;
    A64Asm stub;
    a64_init(&stub);
    int main_at;
    /* Slot for "bl exit" right after main's result is in x0.  Patched to a
     * call of the C runtime's exit() (which flushes stdio) when the image
     * defines one, or NOP when it does not, leaving the raw syscall below. */
    int exit_at;
    /* Optional bl __fakecc_set_environ(envp); NOPed when the runtime does
     * not provide the hook.  Always reserve the slot so argc/argv/envp can
     * live in callee-saved registers on every image, with or without
     * constructors. */
    int envset_at;
    /* Keep argc/argv/envp across the environ hook, constructors, and main's
     * result across destructors.  Destructors run highest priority first. */
    a64_stp64(&stub, A64_FP, A64_LR, A64_SP, -48, A64_PAIR_PRE);
    a64_add_imm12(&stub, A64_FP, A64_SP, 0, 0, 1, 0);
    a64_stp64(&stub, A64_X19, A64_X20, A64_FP, 16, A64_PAIR_OFFSET);
    a64_stp64(&stub, A64_X21, A64_X22, A64_FP, 32, A64_PAIR_OFFSET);
    a64_mov_reg(&stub, A64_X19, A64_X0, 1);
    a64_mov_reg(&stub, A64_X20, A64_X1, 1);
    a64_mov_reg(&stub, A64_X21, A64_X2, 1);
    a64_mov_reg(&stub, A64_X0, A64_X21, 1);
    envset_at = (int)stub.code.len;
    a64_word(&stub, 0x94000000u);
    for (size_t i = 0; i < nctors; i++) {
        ctor_at[i] = (int)stub.code.len;
        a64_word(&stub, 0x94000000u);
    }
    a64_mov_reg(&stub, A64_X0, A64_X19, 1);
    a64_mov_reg(&stub, A64_X1, A64_X20, 1);
    a64_mov_reg(&stub, A64_X2, A64_X21, 1);
    main_at = (int)stub.code.len;
    a64_word(&stub, 0x94000000u);
    a64_mov_reg(&stub, A64_X22, A64_X0, 1);
    for (size_t i = 0; i < ndtors; i++) {
        dtor_at[i] = (int)stub.code.len;
        a64_word(&stub, 0x94000000u);
    }
    a64_mov_reg(&stub, A64_X0, A64_X22, 1);
    exit_at = (int)stub.code.len;
    a64_word(&stub, 0x94000000u);
    a64_movz(&stub, A64_X16, 1, 0, 1);
    a64_svc(&stub, 0x80);
    /* A missing weak symbol's call lands here and returns 0.  Its address
     * is rewritten to a zero register, so this stub is not that address. */
    int weak_at = (int)stub.code.len;
    a64_movz(&stub, A64_X0, 0, 0, 1);
    a64_ret(&stub, A64_LR);
    buffer_append(&out.text, stub.code.data, stub.code.len);
    a64_free(&stub);

    size_t *text_base = xmalloc(n * sizeof(size_t));
    size_t *ro_base = xmalloc(n * sizeof(size_t));
    size_t *data_base = xmalloc(n * sizeof(size_t));
    size_t *bss_base = xmalloc(n * sizeof(size_t));
    size_t *tdata_base = xmalloc(n * sizeof(size_t));
    size_t *tbss_base = xmalloc(n * sizeof(size_t));
    for (size_t i = 0; i < n; i++) {
        text_base[i] = out.text.len;
        if (mods[i]->text.len)
            buffer_append(&out.text, mods[i]->text.data, mods[i]->text.len);
        size_t ral = mods[i]->rodata_align ? mods[i]->rodata_align : 1;
        while (out.rodata.len % ral) { char z = 0; buffer_append(&out.rodata, &z, 1); }
        ro_base[i] = out.rodata.len;
        if (mods[i]->rodata.len)
            buffer_append(&out.rodata, mods[i]->rodata.data, mods[i]->rodata.len);
        size_t dal = mods[i]->data_align ? mods[i]->data_align : 1;
        while (out.data.len % dal) { char z = 0; buffer_append(&out.data, &z, 1); }
        data_base[i] = out.data.len;
        if (mods[i]->data.len)
            buffer_append(&out.data, mods[i]->data.data, mods[i]->data.len);
        size_t bal = mods[i]->bss_align ? mods[i]->bss_align : 1;
        while (out.bss_size % bal) out.bss_size++;
        bss_base[i] = out.bss_size;
        out.bss_size += mods[i]->bss_size;
        /* The thread block is every module's initialized part followed by
         * every module's zero-fill part, which is the only way a static
         * linker can build one template out of many objects. */
        size_t tdal = mods[i]->tdata_align ? mods[i]->tdata_align : 1;
        while (out.tdata.len % tdal) { char z = 0; buffer_append(&out.tdata, &z, 1); }
        tdata_base[i] = out.tdata.len;
        if (mods[i]->tdata.len)
            buffer_append(&out.tdata, mods[i]->tdata.data, mods[i]->tdata.len);
        size_t tbal = mods[i]->tbss_align ? mods[i]->tbss_align : 1;
        while (out.tbss_size % tbal) out.tbss_size++;
        tbss_base[i] = out.tbss_size;
        out.tbss_size += mods[i]->tbss_size;
        if (mods[i]->rodata_align > out.rodata_align)
            out.rodata_align = mods[i]->rodata_align;
        if (mods[i]->data_align > out.data_align)
            out.data_align = mods[i]->data_align;
        if (mods[i]->bss_align > out.bss_align)
            out.bss_align = mods[i]->bss_align;
        if (mods[i]->tdata_align > out.tdata_align)
            out.tdata_align = mods[i]->tdata_align;
        if (mods[i]->tbss_align > out.tbss_align)
            out.tbss_align = mods[i]->tbss_align;
    }

    /* from_user records that the definition currently held came from a
     * user-written TU.  Such a definition outranks one from a package, so a
     * program can replace memcpy or fputs and have its version linked --
     * which is what the C library's replaceable-symbol rule requires, and
     * what gcc's torture cases rely on when they define their own. */
    typedef struct { char *name; uint16_t sh; size_t off; size_t size;
                     uint8_t binding; int from_user; } GDef;
    GDef *gdefs = NULL;
    size_t ng = 0, capg = 0;
    typedef struct { uint16_t sh; size_t off; int defined; } Adj;
    Adj **adj = xmalloc(n * sizeof(Adj *));
    for (size_t i = 0; i < n; i++) adj[i] = NULL;
    int rc = 0;
    size_t main_off = 0;
    int have_main = 0;

    for (size_t i = 0; i < n && rc == 0; i++) {
        EmitModule *m = mods[i];
        adj[i] = xmalloc((m->num_syms ? m->num_syms : 1) * sizeof(Adj));
        for (size_t s = 0; s < m->num_syms; s++) {
            EmitSymbol *es = &m->syms[s];
            adj[i][s].defined = es->shndx != SECT_UNDEF
                             && es->shndx != SHN_COMMON;
            adj[i][s].sh = es->shndx;
            size_t base = 0;
            if (es->shndx == SECT_TEXT) base = text_base[i];
            else if (es->shndx == SECT_RODATA) base = ro_base[i];
            else if (es->shndx == SECT_DATA) base = data_base[i];
            else if (es->shndx == SECT_BSS) base = bss_base[i];
            else if (es->shndx == SECT_TDATA) base = tdata_base[i];
            else if (es->shndx == SECT_TBSS) base = tbss_base[i];
            adj[i][s].off = base + es->value;
            if (!adj[i][s].defined || !es->name || es->binding == 0) continue;
            int is_main = es->shndx == SECT_TEXT && strcmp(es->name, "main") == 0;
            int keep = 1;
            for (size_t g = 0; g < ng; g++) {
                if (strcmp(gdefs[g].name, es->name) != 0) continue;
                int old_weak = gdefs[g].binding == 2;
                int new_weak = es->binding == 2;
                /* The newcomer replaces the incumbent when it is stronger
                 * (a strong symbol over a weak one) or when the incumbent
                 * came from a package and the newcomer is a user
                 * definition.  Replacing a package symbol with a user's is
                 * how a program overrides memcpy; two user definitions, or
                 * two package ones, are a genuine conflict. */
                int new_from_user = m->is_user_tu;
                int old_from_user = gdefs[g].from_user;
                int replace_old = (new_from_user && !old_from_user)
                               || (old_weak && !new_weak);
                /* A package definition meeting a user definition of the same
                 * name steps aside -- that is the replaceable-symbol rule,
                 * and it is the case that arrives second here. */
                int old_loses = old_from_user && !new_from_user;
                if (!replace_old && !old_loses) {
                    if (!old_weak && !new_weak) {
                        fprintf(stderr, "fakecc: duplicate symbol '%s'\n",
                                es->name);
                        rc = -1;
                        break;
                    }
                } else if (replace_old) {
                    gdefs[g].sh = es->shndx;
                    gdefs[g].off = adj[i][s].off;
                    gdefs[g].size = es->size ? es->size
                                   : macho_symbol_span(m, es->shndx, es->value);
                    gdefs[g].binding = es->binding;
                    gdefs[g].from_user = new_from_user;
                    if (is_main) main_off = adj[i][s].off;
                }
                keep = 0;
                break;
            }
            if (rc) break;
            if (!keep) continue;
            if (is_main) {
                have_main = 1;
                main_off = adj[i][s].off;
            }
            if (ng == capg) {
                capg = capg ? capg * 2 : 8;
                gdefs = xrealloc(gdefs, capg * sizeof(GDef));
            }
            gdefs[ng].name = xstrdup(es->name);
            gdefs[ng].sh = es->shndx;
            gdefs[ng].off = adj[i][s].off;
            gdefs[ng].size = es->size ? es->size
                            : macho_symbol_span(m, es->shndx, es->value);
            gdefs[ng].binding = es->binding;
            gdefs[ng].from_user = m->is_user_tu;
            ng++;
        }
    }
    if (rc == 0 && !have_main) {
        fprintf(stderr, "fakecc: no 'main' function found\n");
        rc = -1;
    }

    /* Tentative definitions share one BSS slot unless a real definition
     * of the same name already won. */
    if (rc == 0) {
        typedef struct { char *name; size_t size, align, off; } Comm;
        Comm *comms = NULL;
        size_t ncomm = 0, capcomm = 0;
        for (size_t i = 0; i < n; i++) {
            EmitModule *m = mods[i];
            for (size_t s = 0; s < m->num_syms; s++) {
                EmitSymbol *es = &m->syms[s];
                if (es->shndx != SHN_COMMON || !es->name || es->binding == 0)
                    continue;
                int defined = 0;
                size_t def_sz = 0;
                for (size_t g = 0; g < ng; g++) {
                    if (strcmp(gdefs[g].name, es->name) == 0) {
                        defined = 1;
                        def_sz = gdefs[g].size;
                        break;
                    }
                }
                if (defined) {
                    /* ld64 warns and keeps the real definition.  A larger
                     * common would otherwise look like it owned the tail. */
                    if (es->size > def_sz)
                        fprintf(stderr,
                                "fakecc: tentative definition of '%s' (%zu bytes) is larger than the real definition (%zu bytes)\n",
                                es->name, es->size, def_sz);
                    continue;
                }
                size_t found = ncomm;
                for (size_t c = 0; c < ncomm; c++) {
                    if (strcmp(comms[c].name, es->name) == 0) { found = c; break; }
                }
                if (found == ncomm) {
                    if (ncomm == capcomm) {
                        capcomm = capcomm ? capcomm * 2 : 4;
                        comms = xrealloc(comms, capcomm * sizeof(Comm));
                    }
                    comms[ncomm].name = xstrdup(es->name);
                    comms[ncomm].size = es->size;
                    comms[ncomm].align = es->value ? es->value : 1;
                    comms[ncomm].off = 0;
                    ncomm++;
                } else {
                    if (es->size > comms[found].size) comms[found].size = es->size;
                    if (es->value > comms[found].align) comms[found].align = es->value;
                }
            }
        }
        for (size_t c = 0; c < ncomm; c++) {
            size_t al = comms[c].align ? comms[c].align : 1;
            while (out.bss_size % al) out.bss_size++;
            comms[c].off = out.bss_size;
            out.bss_size += comms[c].size ? comms[c].size : 1;
            if (ng == capg) {
                capg = capg ? capg * 2 : 8;
                gdefs = xrealloc(gdefs, capg * sizeof(GDef));
            }
            gdefs[ng].name = comms[c].name;
            gdefs[ng].sh = SECT_BSS;
            gdefs[ng].off = comms[c].off;
            gdefs[ng].size = comms[c].size;
            gdefs[ng].binding = 1;
            ng++;
        }
        free(comms);
    }

    /* ld64 synthesises __dso_handle in every image: a pointer the atexit
     * machinery uses to key registrations to the module that made them.  It
     * is not in libSystem, so an object from any real toolchain that runs a
     * destructor or constructor at load time references it as undefined and
     * nothing else will satisfy it.  Give it an address of its own. */
    if (rc == 0) {
        int have = 0;
        for (size_t g = 0; g < ng; g++)
            if (strcmp(gdefs[g].name, "__dso_handle") == 0) { have = 1; break; }
        int wanted = 0;
        for (size_t i = 0; i < n && !wanted; i++) {
            EmitModule *m = mods[i];
            for (size_t s = 0; s < m->num_syms; s++) {
                const EmitSymbol *es = &m->syms[s];
                if (es->shndx == SECT_UNDEF && es->name
                    && strcmp(es->name, "__dso_handle") == 0
                    && es->binding != 2) { wanted = 1; break; }
            }
        }
        if (wanted && !have) {
            while (out.data.len % 8) { char z = 0; buffer_append(&out.data, &z, 1); }
            if (out.data_align < 8) out.data_align = 8;
            if (ng == capg) {
                capg = capg ? capg * 2 : 8;
                gdefs = xrealloc(gdefs, capg * sizeof(GDef));
            }
            gdefs[ng].name = xstrdup("__dso_handle");
            gdefs[ng].sh = SECT_DATA;
            gdefs[ng].off = out.data.len;
            gdefs[ng].size = 8;
            gdefs[ng].binding = 1;
            gdefs[ng].from_user = 0;
            ng++;
            uint64_t z = 0;
            buffer_append(&out.data, (const char *)&z, 8);
        }
    }

    typedef struct { char *name; int32_t addend; int filled; } GotEnt;
    GotEnt *gots = NULL;
    size_t ngot = 0, capgot = 0;
    size_t got_base = 0;
    if (rc == 0) {
        for (size_t i = 0; i < n; i++) {
            EmitModule *m = mods[i];
            for (size_t ri = 0; ri < m->num_relocs; ri++) {
                EmitReloc *r = &m->relocs[ri];
                if (r->type != 5 || r->sym >= m->num_syms) continue;
                const char *nm = m->syms[r->sym].name;
                if (!nm) continue;
                int have = 0;
                for (size_t g = 0; g < ngot; g++) {
                    if (gots[g].addend == r->addend &&
                        strcmp(gots[g].name, nm) == 0) {
                        have = 1;
                        break;
                    }
                }
                if (have) continue;
                if (ngot == capgot) {
                    capgot = capgot ? capgot * 2 : 4;
                    gots = xrealloc(gots, capgot * sizeof(GotEnt));
                }
                gots[ngot].name = xstrdup(nm);
                gots[ngot].addend = r->addend;
                gots[ngot].filled = 0;
                ngot++;
            }
        }
        if (ngot) {
            while (out.data.len % 8) {
                char z = 0;
                buffer_append(&out.data, &z, 1);
            }
            got_base = out.data.len;
            for (size_t g = 0; g < ngot; g++) {
                char z[8] = {0};
                buffer_append(&out.data, z, 8);
            }
            if (out.data_align < 8) out.data_align = 8;
        }
    }

    /* Keyed by where the variable ends up, not by its name.  Two translation
     * units can each have their own `static __thread int x`, and those are
     * two variables that happen to share a spelling -- keying on the name
     * gives them one descriptor, one storage slot, and whichever initializer
     * was merged first silently wins in both files. */
    typedef struct { uint16_t sh; size_t off; int filled; } TlvEnt;
    TlvEnt *tlvs = NULL;
    size_t ntlv = 0, captlv = 0;
    size_t tlv_base = 0;
    if (rc == 0) {
        for (size_t i = 0; i < n; i++) {
            EmitModule *m = mods[i];
            for (size_t ri = 0; ri < m->num_relocs; ri++) {
                EmitReloc *r = &m->relocs[ri];
                if (r->type != 8 || r->sym >= m->num_syms) continue;
                const char *nm = m->syms[r->sym].name;
                if (!nm) continue;
                uint16_t sh = 0;
                size_t off = 0;
                if (adj[i][r->sym].defined && m->syms[r->sym].binding != 2) {
                    sh = adj[i][r->sym].sh;
                    off = adj[i][r->sym].off;
                } else {
                    for (size_t g = 0; g < ng; g++) {
                        if (gdefs[g].name && strcmp(gdefs[g].name, nm) == 0) {
                            sh = gdefs[g].sh;
                            off = gdefs[g].off;
                            break;
                        }
                    }
                }
                if (!sh) continue;   /* unresolved: reported with a better message below */
                int have = 0;
                for (size_t g = 0; g < ntlv; g++) {
                    if (tlvs[g].sh == sh && tlvs[g].off == off) { have = 1; break; }
                }
                if (have) continue;
                if (ntlv == captlv) {
                    captlv = captlv ? captlv * 2 : 4;
                    tlvs = xrealloc(tlvs, captlv * sizeof(TlvEnt));
                }
                tlvs[ntlv].sh = sh;
                tlvs[ntlv].off = off;
                tlvs[ntlv].filled = 0;
                ntlv++;
            }
        }
        /* The descriptors go in their own section, not __data: dyld only
         * swaps a thunk for a per-thread address for a descriptor it finds
         * there, and that swap is the whole point of the table. */
        if (ntlv) {
            while (out.thread_vars.len % 8) {
                char z = 0;
                buffer_append(&out.thread_vars, &z, 1);
            }
            tlv_base = out.thread_vars.len;
            for (size_t g = 0; g < ntlv; g++) {
                char z[24];
                memset(z, 0, sizeof z);
                buffer_append(&out.thread_vars, z, sizeof z);
            }
            if (out.thread_vars_align < 8) out.thread_vars_align = 8;
        }
    }

    MachoSectOffsets so;
    memset(&so, 0, sizeof so);
    if (rc == 0)
        macho_section_offsets(&out, out.text.len, &so);
    uint64_t ro_off = so.ro, data_off = so.data, bss_off = so.bss;

    for (size_t i = 0; i < n && rc == 0; i++) {
        EmitModule *m = mods[i];
        EmitReloc *lists[2] = { m->relocs, m->data_relocs };
        size_t lens[2] = { m->num_relocs, m->num_data_relocs };
        int is_data[2] = { 0, 1 };
        for (int pass = 0; pass < 2 && rc == 0; pass++) {
            int skip_pair = 0;
            for (size_t ri = 0; ri < lens[pass]; ri++) {
                if (skip_pair) { skip_pair = 0; continue; }
                EmitReloc *r = &lists[pass][ri];
                /* ARM64_RELOC_ADDEND carries no fixup of its own: cg64 folds
                 * it into the addend of the reloc that follows (both come
                 * from the same gfix entry), so in memory it is redundant.
                 * The .o reader has to decode it because the file format
                 * splits the two across separate entries. */
                if (r->type == 10) continue;
                if (r->sym >= m->num_syms) { rc = -1; break; }
                EmitSymbol *es = &m->syms[r->sym];
                uint16_t sh;
                size_t off;
                int use_local = adj[i][r->sym].defined && es->binding != 2;
                int found = 1;
                if (use_local) {
                    sh = adj[i][r->sym].sh;
                    off = adj[i][r->sym].off;
                } else {
                    found = 0;
                    sh = 0; off = 0;
                    for (size_t g = 0; g < ng; g++) {
                        if (es->name && strcmp(gdefs[g].name, es->name) == 0) {
                            sh = gdefs[g].sh;
                            off = gdefs[g].off;
                            found = 1;
                            break;
                        }
                    }
                    if (!found && adj[i][r->sym].defined) {
                        sh = adj[i][r->sym].sh;
                        off = adj[i][r->sym].off;
                        found = 1;
                    }
                    if (!found && es->binding == 2) {
                        sh = 0;
                        off = 0;
                        found = 2; /* weak and absent: address 0, call returns 0 */
                    }
                    /* A libSystem symbol is absent by design: nothing in the
                     * program defines it, and dyld binds it from the
                     * LC_LOAD_DYLIB the final image already carries.  It has
                     * to be reached through a GOT cell -- there is no
                     * displacement between here and a dylib to patch into a
                     * branch -- which is exactly what the reloc asks for. */
                    if (!found && es->name && emit_is_darwin_import(es->name)) {
                        sh = 0;
                        off = 0;
                        found = 3;
                    }
                    if (!found) {
                        fprintf(stderr, "fakecc: undefined symbol '%s'\n",
                                es->name ? es->name : "?");
                        rc = -1;
                        break;
                    }
                }
                if (found == 2 && !is_data[pass]) {
                    size_t site = text_base[i] + r->offset;
                    if (site + 4 > out.text.len) {
                        fprintf(stderr, "fakecc: text reloc past end of section\n");
                        rc = -1;
                        break;
                    }
                    uint64_t pc = macho_text_offset() + site;
                    uint32_t w = 0;
                    memcpy(&w, out.text.data + site, 4);
                    if (r->type == 2) {
                        uint64_t tgt = macho_text_offset() + (uint64_t)weak_at;
                        if (patch_bl(&w, pc, tgt) != 0) { rc = -1; break; }
                    } else if (r->type == 11) {
                        uint64_t tgt = macho_text_offset() + (uint64_t)weak_at;
                        if (patch_b(&w, pc, tgt) != 0) { rc = -1; break; }
                    } else if (r->type == 3) {
                        w = 0xD2800000u | (w & 31u); /* movz Xd, #0 */
                    } else if (r->type == 4) {
                        w = 0xD503201Fu; /* nop */
                    } else {
                        fprintf(stderr, "fakecc: unsupported text reloc %u\n", r->type);
                        rc = -1;
                        break;
                    }
                    memcpy(out.text.data + site, &w, 4);
                    continue;
                }
                if (found == 2)
                    continue; /* data slot already holds the absolute addend */
                uint64_t base = sh == SECT_TEXT ? macho_text_offset()
                              : sh == SECT_RODATA ? ro_off
                              : sh == SECT_DATA ? data_off
                              : sh == SECT_TDATA ? so.tdata
                              : sh == SECT_TBSS ? so.tbss
                              : bss_off;
                uint64_t tgt = base + off + (uint64_t)(int64_t)r->addend;
                if (is_data[pass] && (r->shndx == SECT_INIT_ARRAY ||
                                      r->shndx == SECT_FINI_ARRAY))
                    continue;
                if (!is_data[pass]) {
                    size_t site = text_base[i] + r->offset;
                    if (site + 4 > out.text.len) {
                        fprintf(stderr, "fakecc: text reloc past end of section\n");
                        rc = -1;
                        break;
                    }
                    uint64_t pc = macho_text_offset() + site;
                    uint32_t w = 0;
                    memcpy(&w, out.text.data + site, 4);
                    if (r->type == 5 || r->type == 6) {
                        int slot = -1;
                        for (size_t g = 0; g < ngot; g++) {
                            if (es->name && gots[g].addend == r->addend &&
                                strcmp(gots[g].name, es->name) == 0) {
                                slot = (int)g;
                                break;
                            }
                        }
                        if (slot < 0 || data_off == 0) {
                            fprintf(stderr, "fakecc: got reloc has no slot\n");
                            rc = -1;
                            break;
                        }
                        uint64_t got_va = data_off + got_base + (uint64_t)slot * 8;
                        if (!gots[slot].filled) {
                            if (found == 3)
                                emit_module_add_bind(&out, got_va, es->name);
                            else
                                emit_module_add_rebase(&out, got_va, tgt);
                            gots[slot].filled = 1;
                        }
                        if (r->type == 5) patch_adrp(&w, pc, got_va);
                        else w |= (uint32_t)(((got_va & 0xFFFu) >> 3) << 10);
                    } else if (r->type == 8 || r->type == 9) {
                        int slot = -1;
                        for (size_t g = 0; g < ntlv; g++) {
                            if (tlvs[g].sh == sh && tlvs[g].off == off) {
                                slot = (int)g;
                                break;
                            }
                        }
                        /* The descriptor table's own section is the thing
                         * that has to exist, not __data: a TU pair that has
                         * nothing but __thread storage never grows a __data
                         * section at all. */
                        if (slot < 0 || so.thread_vars == 0) {
                            fprintf(stderr, "fakecc: tlv reloc has no slot\n");
                            rc = -1;
                            break;
                        }
                        uint64_t desc = so.thread_vars + tlv_base
                                      + (uint64_t)slot * 24;
                        if (!tlvs[slot].filled) {
                            /* The thread block is every module's initialized
                             * part followed by every module's zero-fill part,
                             * so a variable's offset inside it is either its
                             * tdata offset or the tdata size rounded up to
                             * the block's own alignment plus its tbss
                             * offset.  `off` is already the offset inside the
                             * merged section. */
                            size_t max_tls_al = out.tdata_align > out.tbss_align
                                              ? out.tdata_align : out.tbss_align;
                            size_t tbss_in_block = macho_tlv_block_tbss_base(
                                (size_t)out.tdata.len, max_tls_al);
                            uint64_t blk = sh == SECT_TBSS
                                         ? (uint64_t)tbss_in_block + off : off;
                            memcpy(out.thread_vars.data + tlv_base
                                   + (size_t)slot * 24 + 16, &blk, 8);
                            emit_module_add_bind(&out, desc, "_tlv_bootstrap");
                            tlvs[slot].filled = 1;
                        }
                        if (r->type == 8) patch_adrp(&w, pc, desc);
                        else {
                            /* LDR Xt, [Xn, #imm] -> ADD Xt, Xn, #byteoff. */
                            w = 0x91000000u | ((uint32_t)(desc & 0xFFFu) << 10)
                              | (w & 0x3FFu);
                        }
                    } else if (r->type == 2) {
                        if (patch_bl(&w, pc, tgt) != 0) { rc = -1; break; }
                    } else if (r->type == 11) {
                        /* Stub tail call: b, not bl. */
                        if (patch_b(&w, pc, tgt) != 0) { rc = -1; break; }
                    } else if (r->type == 3) {
                        patch_adrp(&w, pc, tgt);
                    } else if (r->type == 4) {
                        patch_add_pageoff(&w, tgt);
                    }
                    else {
                        fprintf(stderr, "fakecc: unsupported text reloc %u\n", r->type);
                        rc = -1;
                        break;
                    }
                    memcpy(out.text.data + site, &w, 4);
                } else if (r->type == 1) {
                    if (ri + 1 >= lens[pass]) {
                        fprintf(stderr, "fakecc: subtractor reloc is not paired\n");
                        rc = -1;
                        break;
                    }
                    EmitReloc *n = &lists[pass][ri + 1];
                    if (n->type != 0 || n->offset != r->offset || n->sym >= m->num_syms) {
                        fprintf(stderr, "fakecc: subtractor reloc is not paired\n");
                        rc = -1;
                        break;
                    }
                    EmitSymbol *ns = &m->syms[n->sym];
                    uint16_t nsh = 0;
                    size_t noff = 0;
                    int nfound = 0;
                    int nlocal = adj[i][n->sym].defined && ns->binding != 2;
                    if (nlocal) {
                        nsh = adj[i][n->sym].sh;
                        noff = adj[i][n->sym].off;
                        nfound = 1;
                    } else {
                        for (size_t g = 0; g < ng; g++) {
                            if (ns->name && strcmp(gdefs[g].name, ns->name) == 0) {
                                nsh = gdefs[g].sh;
                                noff = gdefs[g].off;
                                nfound = 1;
                                break;
                            }
                        }
                        if (!nfound && adj[i][n->sym].defined) {
                            nsh = adj[i][n->sym].sh;
                            noff = adj[i][n->sym].off;
                            nfound = 1;
                        }
                    }
                    if (!nfound) {
                        fprintf(stderr, "fakecc: undefined symbol '%s'\n",
                                ns->name ? ns->name : "?");
                        rc = -1;
                        break;
                    }
                    uint64_t nbase = nsh == SECT_TEXT ? macho_text_offset()
                                   : nsh == SECT_RODATA ? ro_off
                                   : nsh == SECT_DATA ? data_off
                                   : nsh == SECT_TDATA ? so.tdata
                                   : nsh == SECT_TBSS ? so.tbss
                                   : bss_off;
                    /* The slot's own section decides which merged buffer it
                     * lives in: a pointer initializer in __thread_data has
                     * to be written there, not into __data. */
                    int slot_tdata = (r->shndx == SECT_TDATA);
                    Buffer *slotbuf = slot_tdata ? &out.tdata : &out.data;
                    size_t slot_base = slot_tdata ? tdata_base[i] : data_base[i];
                    int64_t diff = (int64_t)(nbase + noff + (uint64_t)(int64_t)n->addend)
                                 - (int64_t)(base + off);
                    size_t raw = slot_base + r->offset;
                    if (raw + 8 > slotbuf->len) {
                        fprintf(stderr, "fakecc: data reloc past end of section\n");
                        rc = -1;
                        break;
                    }
                    memcpy(slotbuf->data + raw, &diff, 8);
                    skip_pair = 1;
                } else if (r->type == 0) {
                    int slot_tdata = (r->shndx == SECT_TDATA);
                    uint64_t slot = (slot_tdata ? so.tdata : data_off)
                                  + (slot_tdata ? tdata_base[i] : data_base[i])
                                  + r->offset;
                    emit_module_add_rebase(&out, slot, tgt);
                } else {
                    fprintf(stderr, "fakecc: unsupported data reloc %u\n", r->type);
                    rc = -1;
                    break;
                }
            }
        }
    }

    if (rc == 0) {
        uint32_t w = 0;
        uint64_t pc = macho_text_offset() + (uint64_t)main_at;
        uint64_t tgt = macho_text_offset() + main_off;
        if (patch_bl(&w, pc, tgt) != 0)
            rc = -1;
        else
            memcpy(out.text.data + main_at, &w, 4);
        /* Route normal termination through the C runtime's exit() so its
         * fflush(NULL) runs; the raw exit syscall that follows in the stub
         * is the fallback for an image linked without the runtime. */
        if (rc == 0) {
            size_t exit_off = 0;
            int have_exit = 0;
            for (size_t g = 0; g < ng; g++) {
                if (gdefs[g].sh == SECT_TEXT &&
                    strcmp(gdefs[g].name, "exit") == 0) {
                    exit_off = gdefs[g].off;
                    have_exit = 1;
                    break;
                }
            }
            if (have_exit) {
                pc = macho_text_offset() + (uint64_t)exit_at;
                tgt = macho_text_offset() + exit_off;
                if (patch_bl(&w, pc, tgt) != 0)
                    rc = -1;
                else
                    memcpy(out.text.data + exit_at, &w, 4);
            } else {
                w = 0xD503201Fu; /* NOP: keep the syscall fallback */
                memcpy(out.text.data + exit_at, &w, 4);
            }
        }
        /* Wire the envp hook if the runtime defines it; otherwise NOP the
         * reserved branch (the mov x0,x21 above it stays harmless). */
        if (rc == 0) {
            size_t envset_off = 0;
            int have_envset = 0;
            for (size_t g = 0; g < ng; g++) {
                if (gdefs[g].sh == SECT_TEXT &&
                    strcmp(gdefs[g].name, "__fakecc_set_environ") == 0) {
                    envset_off = gdefs[g].off;
                    have_envset = 1;
                    break;
                }
            }
            if (have_envset) {
                pc = macho_text_offset() + (uint64_t)envset_at;
                tgt = macho_text_offset() + envset_off;
                if (patch_bl(&w, pc, tgt) != 0)
                    rc = -1;
                else
                    memcpy(out.text.data + envset_at, &w, 4);
            } else {
                w = 0xD503201Fu; /* NOP */
                memcpy(out.text.data + envset_at, &w, 4);
            }
        }
        for (size_t i = 0; i < nctors && rc == 0; i++) {
            EmitModule *m = mods[ctors[i].mod];
            uint32_t sy = ctors[i].sym;
            if (sy >= m->num_syms || !adj[ctors[i].mod] ||
                !adj[ctors[i].mod][sy].defined ||
                adj[ctors[i].mod][sy].sh != SECT_TEXT) {
                fprintf(stderr, "fakecc: constructor is not a function\n");
                rc = -1;
                break;
            }
            pc = macho_text_offset() + (uint64_t)ctor_at[i];
            tgt = macho_text_offset() + adj[ctors[i].mod][sy].off;
            if (patch_bl(&w, pc, tgt) != 0) { rc = -1; break; }
            memcpy(out.text.data + ctor_at[i], &w, 4);
        }
        for (size_t i = 0; i < ndtors && rc == 0; i++) {
            HookRef *h = &dtors[ndtors - 1 - i];
            EmitModule *m = mods[h->mod];
            if (h->sym >= m->num_syms || !adj[h->mod] ||
                !adj[h->mod][h->sym].defined ||
                adj[h->mod][h->sym].sh != SECT_TEXT) {
                fprintf(stderr, "fakecc: destructor is not a function\n");
                rc = -1;
                break;
            }
            pc = macho_text_offset() + (uint64_t)dtor_at[i];
            tgt = macho_text_offset() + adj[h->mod][h->sym].off;
            if (patch_bl(&w, pc, tgt) != 0) { rc = -1; break; }
            memcpy(out.text.data + dtor_at[i], &w, 4);
        }
        if (rc == 0)
            rc = macho_write_exec(&out, macho_text_offset(), path);
    }

    for (size_t i = 0; i < n; i++) free(adj[i]);
    free(adj);
    free(text_base); free(ro_base); free(data_base); free(bss_base);
    free(tdata_base); free(tbss_base);
    for (size_t g = 0; g < ng; g++) free(gdefs[g].name);
    free(gdefs);
    for (size_t g = 0; g < ngot; g++) free(gots[g].name);
    free(gots);
    free(tlvs);
    free(ctors);
    free(dtors);
    free(ctor_at);
    free(dtor_at);
    emit_module_free(&out);
    return rc;
}

int macho_codesign(const char *path) {
#ifndef FAKECC_SELFHOST
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fakecc: fork failed for codesign\n");
        return -1;
    }
    if (pid == 0) {
        execl("/usr/bin/codesign", "codesign", "-s", "-", "-f", path,
              (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "fakecc: ad-hoc codesign failed for '%s'\n", path);
        return -1;
    }
    return 0;
#else
    /* Selfhost images never emit a Mach-O that needs a host codesign. */
    (void)path;
    return -1;
#endif
}
