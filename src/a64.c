#include "fakecc/a64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/* Local label assembler                                               */
/* ================================================================== */

void a64_init(A64Asm *a) {
    buffer_init(&a->code);
    a->labels = NULL;
    a->nlabels = a->cap_labels = 0;
    a->fixups = NULL;
    a->nfixups = a->cap_fixups = 0;
    a->base_off = 0;
}

void a64_free(A64Asm *a) {
    buffer_free(&a->code);
    free(a->labels);
    free(a->fixups);
    a->labels = NULL;
    a->fixups = NULL;
}

void a64_word(A64Asm *a, uint32_t w) {
    buffer_append(&a->code, (const char *)&w, 4);
}

int a64_new_label(A64Asm *a) {
    if (a->nlabels == a->cap_labels) {
        a->cap_labels = a->cap_labels ? a->cap_labels * 2 : 8;
        a->labels = xrealloc(a->labels, a->cap_labels * sizeof(A64LabelSlot));
    }
    int id = (int)a->nlabels++;
    a->labels[id].pos = 0;
    a->labels[id].bound = 0;
    return id;
}

void a64_bind(A64Asm *a, int label) {
    a->labels[label].pos = (uint32_t)a->code.len;
    a->labels[label].bound = 1;
}

/* Record a fixup for the word just emitted at byte offset `at`. */
static void add_fixup_at(A64Asm *a, int kind, int rd, int label, uint32_t at) {
    if (a->nfixups == a->cap_fixups) {
        a->cap_fixups = a->cap_fixups ? a->cap_fixups * 2 : 16;
        a->fixups = xrealloc(a->fixups, a->cap_fixups * sizeof(A64Fixup));
    }
    A64Fixup *f = &a->fixups[a->nfixups++];
    f->kind = kind;
    f->at = at;
    f->label = label;
    f->rd = rd;
}

/* Emit a word and record a fixup against that word's own offset. */
static void word_with_fixup(A64Asm *a, uint32_t w, int kind, int rd, int label) {
    uint32_t at = (uint32_t)a->code.len;
    a64_word(a, w);
    add_fixup_at(a, kind, rd, label, at);
}

static uint32_t read_word_at(A64Asm *a, uint32_t off) {
    uint32_t w;
    memcpy(&w, a->code.data + off, 4);
    return w;
}

static void write_word_at(A64Asm *a, uint32_t off, uint32_t w) {
    memcpy(a->code.data + off, &w, 4);
}

int a64_resolve(A64Asm *a) {
    int rc = 0;
    for (size_t i = 0; i < a->nfixups; i++) {
        A64Fixup *f = &a->fixups[i];
        if (!a->labels[f->label].bound) {
            fprintf(stderr, "fakecc: a64 label %d unbound\n", f->label);
            rc = -1;
            continue;
        }
        uint32_t tgt = a->labels[f->label].pos;
        uint32_t w = read_word_at(a, f->at);
        if (f->kind == A64_FIX_B26) {
            int64_t d = (int64_t)tgt - (int64_t)f->at;
            if ((d & 3) || d < -(1LL << 27) || d >= (1LL << 27)) {
                fprintf(stderr, "fakecc: a64 b/bl out of range\n");
                rc = -1;
                continue;
            }
            w |= (uint32_t)((d >> 2) & 0x3FFFFFFLL);
        } else if (f->kind == A64_FIX_B19) {
            int64_t d = (int64_t)tgt - (int64_t)f->at;
            if ((d & 3) || d < -(1LL << 20) || d >= (1LL << 20)) {
                fprintf(stderr, "fakecc: a64 conditional branch out of range\n");
                rc = -1;
                continue;
            }
            w |= (uint32_t)(((d >> 2) & 0x7FFFFLL) << 5);
        } else { /* A64_FIX_ADRP / A64_FIX_ADRP_ADD */
            int64_t pc = (int64_t)f->at + (int64_t)a->base_off;
            int64_t tv = (int64_t)tgt + (int64_t)a->base_off;
            int64_t pc_page = pc & ~(int64_t)0xFFF;
            int64_t t_page  = tv & ~(int64_t)0xFFF;
            int64_t pages = (t_page - pc_page) >> 12;
            if (pages < -(1LL << 20) || pages >= (1LL << 20)) {
                fprintf(stderr, "fakecc: a64 adrp out of range\n");
                rc = -1;
                continue;
            }
            w |= (uint32_t)((pages & 3) << 29);
            w |= (uint32_t)(((pages >> 2) & 0x7FFFFLL) << 5);
            write_word_at(a, f->at, w);
            if (f->kind == A64_FIX_ADRP_ADD) {
                /* Patch the following ADD (imm12, shift 0) with the target's
                 * in-page offset. */
                uint32_t aw = read_word_at(a, f->at + 4);
                aw |= (uint32_t)(((uint64_t)tv) & 0xFFFu) << 10;
                write_word_at(a, f->at + 4, aw);
            }
            continue;
        }
        write_word_at(a, f->at, w);
    }
    /* Fixups are one-shot; reset so a reused buffer stays clean. */
    a->nfixups = 0;
    return rc;
}

/* ================================================================== */
/* Field helpers                                                       */
/* ================================================================== */

#define RD(r)   ((r) & 31)
#define RN(r)   (((r) & 31) << 5)
#define RM(r)   (((r) & 31) << 16)

/* ================================================================== */
/* Moves / constants                                                   */
/* ================================================================== */

void a64_mov_reg(A64Asm *a, int rd, int rm, int is64) {
    /* ORR (shifted register), shift 0, Rn = ZR(31). */
    uint32_t base = is64 ? 0xAA0003E0u : 0x2A0003E0u;
    a64_word(a, base | RM(rm) | RD(rd));
}

void a64_movz(A64Asm *a, int rd, unsigned imm16, unsigned hw, int is64) {
    uint32_t base = is64 ? 0xD2800000u : 0x52800000u;
    a64_word(a, base | ((hw & 3) << 21) | ((imm16 & 0xFFFF) << 5) | RD(rd));
}

void a64_movk(A64Asm *a, int rd, unsigned imm16, unsigned hw, int is64) {
    uint32_t base = is64 ? 0xF2800000u : 0x72800000u;
    a64_word(a, base | ((hw & 3) << 21) | ((imm16 & 0xFFFF) << 5) | RD(rd));
}

void a64_movn(A64Asm *a, int rd, unsigned imm16, unsigned hw, int is64) {
    uint32_t base = is64 ? 0x92800000u : 0x12800000u;
    a64_word(a, base | ((hw & 3) << 21) | ((imm16 & 0xFFFF) << 5) | RD(rd));
}

void a64_mov_imm64(A64Asm *a, int rd, uint64_t v) {
    /* movz at the lowest non-zero halfword, movk for the remaining three. */
    int first = -1;
    for (int i = 0; i < 4; i++)
        if ((v >> (16 * i)) & 0xFFFF) { first = i; break; }
    if (first < 0) {
        a64_movz(a, rd, 0, 0, 1);
        return;
    }
    a64_movz(a, rd, (unsigned)((v >> (16 * first)) & 0xFFFF),
             (unsigned)first, 1);
    for (int i = first + 1; i < 4; i++) {
        unsigned h = (unsigned)((v >> (16 * i)) & 0xFFFF);
        if (h)
            a64_movk(a, rd, h, (unsigned)i, 1);
    }
}

/* ================================================================== */
/* Integer arithmetic                                                  */
/* ================================================================== */

static uint32_t addsub_imm_base(int is64, int is_sub, int setflags) {
    return (is64 ? 0x80000000u : 0)
         | (is_sub ? 0x40000000u : 0)
         | (setflags ? 0x20000000u : 0)
         | 0x11000000u;
}

void a64_add_imm12(A64Asm *a, int rd, int rn, unsigned imm12,
                   int shift12, int is64, int setflags) {
    uint32_t w = addsub_imm_base(is64, 0, setflags)
               | (shift12 ? (1u << 22) : 0)
               | ((imm12 & 0xFFF) << 10) | RN(rn) | RD(rd);
    a64_word(a, w);
}

void a64_sub_imm12(A64Asm *a, int rd, int rn, unsigned imm12,
                   int shift12, int is64, int setflags) {
    uint32_t w = addsub_imm_base(is64, 1, setflags)
               | (shift12 ? (1u << 22) : 0)
               | ((imm12 & 0xFFF) << 10) | RN(rn) | RD(rd);
    a64_word(a, w);
}

static uint32_t addsub_reg_base(int is64, int is_sub, int setflags) {
    return (is64 ? 0x80000000u : 0)
         | (is_sub ? 0x40000000u : 0)
         | (setflags ? 0x20000000u : 0)
         | 0x0B000000u;
}

void a64_add_reg(A64Asm *a, int rd, int rn, int rm,
                 int shift, unsigned amt, int is64, int setflags) {
    uint32_t w = addsub_reg_base(is64, 0, setflags)
               | ((shift & 3) << 22) | ((amt & 0x3F) << 10)
               | RM(rm) | RN(rn) | RD(rd);
    a64_word(a, w);
}

void a64_sub_reg(A64Asm *a, int rd, int rn, int rm,
                 int shift, unsigned amt, int is64, int setflags) {
    uint32_t w = addsub_reg_base(is64, 1, setflags)
               | ((shift & 3) << 22) | ((amt & 0x3F) << 10)
               | RM(rm) | RN(rn) | RD(rd);
    a64_word(a, w);
}

void a64_neg(A64Asm *a, int rd, int rm, int is64) {
    /* SUB Rd, ZR, Rm. */
    a64_sub_reg(a, rd, 31, rm, A64_LSL, 0, is64, 0);
}

/* Encode a logical bitmask immediate (ARM ARM EncodeBitMasks).
 * Returns 0 and fills N/immr/imms if v is representable, else -1. */
static int encode_limm(uint64_t v, int is64,
                       unsigned *out_n, unsigned *out_immr,
                       unsigned *out_imms) {
    int datasize = is64 ? 64 : 32;
    uint64_t vmask = is64 ? ~0ULL : 0xFFFFFFFFULL;
    v &= vmask;

    static const int esizes[6] = {64, 32, 16, 8, 4, 2};
    for (int len = 0; len < 6; len++) {
        int esize = esizes[len];
        if (esize > datasize) continue;
        uint64_t emask = esize == 64 ? ~0ULL : ((1ULL << esize) - 1);
        uint64_t pat = v & emask;
        uint64_t rep = 0;
        for (int e = 0; e < datasize / esize; e++)
            rep |= pat << (e * esize);
        rep &= vmask;
        if (rep != v) continue;
        /* Find a rotation that turns pat into a run of S+1 ones
         * (welem = 2^(S+1)-1).  The decoder applies ROR(welem, R), so
         * the field R is the complement of pat's own rotation. */
        for (int r = 0; r < esize; r++) {
            uint64_t w = ((pat >> r) | (pat << (esize - r))) & emask;
            if (w == 0) continue;
            if ((w & (w + 1)) != 0) continue;
            /* w = (1 << k) - 1; S = k - 1. */
            int k = 0;
            while (k < esize && ((w >> k) & 1)) k++;
            int s = k - 1;
            *out_n = is64 ? 1u : 0u;
            *out_immr = (unsigned)((esize - r) % esize);
            *out_imms = (unsigned)s;
            return 0;
        }
    }
    return -1;
}

static void logical_imm(A64Asm *a, int opc, int rd, int rn,
                        uint64_t v, int is64, int *ok) {
    unsigned n, immr, imms;
    if (encode_limm(v, is64, &n, &immr, &imms) != 0) {
        if (ok) *ok = 0;
        return;
    }
    if (ok) *ok = 1;
    uint32_t w = (is64 ? 0x80000000u : 0)
               | ((opc & 3) << 29)
               | (0x24u << 23)             /* 100100 fixed field */
               | (n << 22) | (immr << 16) | (imms << 10)
               | RN(rn) | RD(rd);
    a64_word(a, w);
}

int a64_and_imm(A64Asm *a, int rd, int rn, uint64_t v, int is64) {
    int ok = 1;
    logical_imm(a, 0, rd, rn, v, is64, &ok);
    return ok ? 0 : -1;
}

int a64_or_imm(A64Asm *a, int rd, int rn, uint64_t v, int is64) {
    int ok = 1;
    logical_imm(a, 1, rd, rn, v, is64, &ok);
    return ok ? 0 : -1;
}

int a64_eor_imm(A64Asm *a, int rd, int rn, uint64_t v, int is64) {
    int ok = 1;
    logical_imm(a, 2, rd, rn, v, is64, &ok);
    return ok ? 0 : -1;
}

static uint32_t logical_reg_base(int is64, int opc) {
    return (is64 ? 0x80000000u : 0) | ((opc & 3) << 29) | 0x0A000000u;
}

static void logical_reg(A64Asm *a, int opc, int rd, int rn, int rm, int is64) {
    uint32_t w = logical_reg_base(is64, opc) | RM(rm) | RN(rn) | RD(rd);
    a64_word(a, w);
}

void a64_and_reg(A64Asm *a, int rd, int rn, int rm, int is64) {
    logical_reg(a, 0, rd, rn, rm, is64);
}
void a64_or_reg(A64Asm *a, int rd, int rn, int rm, int is64) {
    logical_reg(a, 1, rd, rn, rm, is64);
}
void a64_eor_reg(A64Asm *a, int rd, int rn, int rm, int is64) {
    logical_reg(a, 2, rd, rn, rm, is64);
}

/* UBFM/SBFM shift-amount aliases. */
static void bitfield(A64Asm *a, int is_signed, int rd, int rn,
                     unsigned immr, unsigned imms, int is64) {
    uint32_t base = (is64 ? (0x80000000u | 0x00400000u) : 0)   /* sf + N */
                  | (is_signed ? 0x13000000u : 0x53000000u);
    a64_word(a, base | (immr << 16) | (imms << 10) | RN(rn) | RD(rd));
}

void a64_lsl_imm(A64Asm *a, int rd, int rn, unsigned amt, int is64) {
    unsigned w = is64 ? 64 : 32;
    bitfield(a, 0, rd, rn, (w - amt) & (w - 1), w - 1 - amt, is64);
}

void a64_lsr_imm(A64Asm *a, int rd, int rn, unsigned amt, int is64) {
    /* LSR is UBFM with imms fixed to the top element bit. */
    unsigned w = is64 ? 64 : 32;
    bitfield(a, 0, rd, rn, amt, w - 1, is64);
}

void a64_asr_imm(A64Asm *a, int rd, int rn, unsigned amt, int is64) {
    unsigned w = is64 ? 64 : 32;
    bitfield(a, 1, rd, rn, amt, w - 1, is64);
}

void a64_mul(A64Asm *a, int rd, int rn, int rm, int is64) {
    /* MADD with Ra = ZR. */
    uint32_t w = (is64 ? 0x80000000u : 0) | 0x1B000000u
               | RM(rm) | (31u << 10) | RN(rn) | RD(rd);
    a64_word(a, w);
}

void a64_msub(A64Asm *a, int rd, int rn, int rm, int ra, int is64) {
    /* MSUB: MADD encoding with o0 = 1 at bit 15 (not 21). */
    uint32_t w = (is64 ? 0x80000000u : 0) | 0x1B000000u | (1u << 15)
               | RM(rm) | ((ra & 31) << 10) | RN(rn) | RD(rd);
    a64_word(a, w);
}

void a64_udiv(A64Asm *a, int rd, int rn, int rm, int is64) {
    uint32_t w = (is64 ? 0x80000000u : 0) | 0x1AC00800u
               | RM(rm) | RN(rn) | RD(rd);
    a64_word(a, w);
}

void a64_sdiv(A64Asm *a, int rd, int rn, int rm, int is64) {
    uint32_t w = (is64 ? 0x80000000u : 0) | 0x1AC00C00u
               | RM(rm) | RN(rn) | RD(rd);
    a64_word(a, w);
}

/* Variable shifts: the hardware masks the count to 0..63 / 0..31. */
static void var_shift(A64Asm *a, uint32_t base64, int rd, int rn, int rm,
                      int is64) {
    uint32_t w = (is64 ? base64 : base64 & ~0x80000000u)
               | RM(rm) | RN(rn) | RD(rd);
    a64_word(a, w);
}
void a64_lslv(A64Asm *a, int rd, int rn, int rm, int is64) {
    var_shift(a, 0x9AC02000u, rd, rn, rm, is64);
}
void a64_lsrv(A64Asm *a, int rd, int rn, int rm, int is64) {
    var_shift(a, 0x9AC02400u, rd, rn, rm, is64);
}
void a64_asrv(A64Asm *a, int rd, int rn, int rm, int is64) {
    var_shift(a, 0x9AC02800u, rd, rn, rm, is64);
}
void a64_rorv(A64Asm *a, int rd, int rn, int rm, int is64) {
    var_shift(a, 0x9AC02C00u, rd, rn, rm, is64);
}

/* CSET: CSINC Xd, XZR, XZR, invert(cond); the inverse condition is
 * cond XOR 1 (e.g. LT↔GE), NOT a bitwise complement. */
void a64_cset(A64Asm *a, int rd, int cond, int is64) {
    uint32_t w = (is64 ? 0x9A9F07E0u : 0x1A9F07E0u)
               | (((unsigned)cond ^ 1u) << 12) | RD(rd);
    a64_word(a, w);
}

/* MVN: ORN Xd, XZR, Xm. */
void a64_mvn(A64Asm *a, int rd, int rm, int is64) {
    uint32_t w = (is64 ? 0xAA2003E0u : 0x2A2003E0u) | RM(rm) | RD(rd);
    a64_word(a, w);
}

/* Sign/zero-extending aliases over SBFM/UBFM. */
void a64_sxt(A64Asm *a, int rd, int rn, unsigned from_bytes, int is64) {
    /* sxtb (imms 7), sxth (15), sxtw (31, 64-bit destination only). */
    unsigned imms = from_bytes <= 1 ? 7u : from_bytes == 2 ? 15u : 31u;
    bitfield(a, 1, rd, rn, 0, imms, is64);
}
void a64_uxt(A64Asm *a, int rd, int rn, unsigned from_bytes, int is64) {
    unsigned imms = from_bytes <= 1 ? 7u : from_bytes == 2 ? 15u : 31u;
    bitfield(a, 0, rd, rn, 0, imms, is64);
}

/* ================================================================== */
/* Compare                                                             */
/* ================================================================== */

void a64_cmp_imm12(A64Asm *a, int rn, unsigned imm12, int shift12, int is64) {
    /* SUBS XZR, Rn, #imm. */
    uint32_t w = addsub_imm_base(is64, 1, 1)
               | (shift12 ? (1u << 22) : 0)
               | ((imm12 & 0xFFF) << 10) | RN(rn) | 31u;
    a64_word(a, w);
}

void a64_cmp_reg(A64Asm *a, int rn, int rm, int is64) {
    /* SUBS XZR, Rn, Rm. */
    uint32_t w = addsub_reg_base(is64, 1, 1) | RM(rm) | RN(rn) | 31u;
    a64_word(a, w);
}

/* ================================================================== */
/* Loads / stores — unsigned immediate offset                          */
/* ================================================================== */

static void ls_imm(A64Asm *a, uint32_t base, int rt, int rn,
                   int off, int scale) {
    if (off < 0 || (off & (scale - 1))) {
        fprintf(stderr, "fakecc: a64 bad load/store offset %d\n", off);
        exit(1);
    }
    unsigned pimm = (unsigned)(off / scale) & 0xFFF;
    a64_word(a, base | (pimm << 10) | RN(rn) | RD(rt));
}

/* Unscaled signed-offset pairs (LDUR/STUR): imm9 range -256..255.
 * Used for fp-relative locals below the frame pointer (A64's scaled
 * unsigned-offset forms cannot encode negative displacements). */
static void ls_unscaled(A64Asm *a, uint32_t base, int rt, int rn, int off) {
    if (off < -256 || off > 255) {
        fprintf(stderr, "fakecc: a64 unscaled offset %d out of imm9\n", off);
        exit(1);
    }
    uint32_t pimm = (uint32_t)(off & 0x1FF);
    a64_word(a, base | (pimm << 12) | RN(rn) | RD(rt));
}
void a64_stur64(A64Asm *a, int rt, int rn, int off) { ls_unscaled(a, 0xF8000000u, rt, rn, off); }
void a64_ldur64(A64Asm *a, int rt, int rn, int off) { ls_unscaled(a, 0xF8400000u, rt, rn, off); }
void a64_stur32(A64Asm *a, int rt, int rn, int off) { ls_unscaled(a, 0xB8000000u, rt, rn, off); }
void a64_ldur32(A64Asm *a, int rt, int rn, int off) { ls_unscaled(a, 0xB8400000u, rt, rn, off); }
void a64_stur16(A64Asm *a, int rt, int rn, int off) { ls_unscaled(a, 0x78000000u, rt, rn, off); }
void a64_ldur16(A64Asm *a, int rt, int rn, int off) { ls_unscaled(a, 0x78400000u, rt, rn, off); }
void a64_stur8 (A64Asm *a, int rt, int rn, int off) { ls_unscaled(a, 0x38000000u, rt, rn, off); }
void a64_ldur8 (A64Asm *a, int rt, int rn, int off) { ls_unscaled(a, 0x38400000u, rt, rn, off); }
void a64_ldursb64(A64Asm *a, int rt, int rn, int off) { ls_unscaled(a, 0x38800000u, rt, rn, off); }
void a64_ldursh64(A64Asm *a, int rt, int rn, int off) { ls_unscaled(a, 0x78800000u, rt, rn, off); }
void a64_ldursw  (A64Asm *a, int rt, int rn, int off) { ls_unscaled(a, 0xB8800000u, rt, rn, off); }

void a64_str64(A64Asm *a, int rt, int rn, int off) { ls_imm(a, 0xF9000000u, rt, rn, off, 8); }
void a64_ldr64(A64Asm *a, int rt, int rn, int off) { ls_imm(a, 0xF9400000u, rt, rn, off, 8); }
void a64_str32(A64Asm *a, int rt, int rn, int off) { ls_imm(a, 0xB9000000u, rt, rn, off, 4); }
void a64_ldr32(A64Asm *a, int rt, int rn, int off) { ls_imm(a, 0xB9400000u, rt, rn, off, 4); }
void a64_str16(A64Asm *a, int rt, int rn, int off) { ls_imm(a, 0x79000000u, rt, rn, off, 2); }
void a64_ldr16(A64Asm *a, int rt, int rn, int off) { ls_imm(a, 0x79400000u, rt, rn, off, 2); }
void a64_str8 (A64Asm *a, int rt, int rn, int off) { ls_imm(a, 0x39000000u, rt, rn, off, 1); }
void a64_ldr8 (A64Asm *a, int rt, int rn, int off) { ls_imm(a, 0x39400000u, rt, rn, off, 1); }
void a64_ldrsw  (A64Asm *a, int rt, int rn, int off) { ls_imm(a, 0xB9800000u, rt, rn, off, 4); }
void a64_ldrsb64(A64Asm *a, int rt, int rn, int off) { ls_imm(a, 0x39800000u, rt, rn, off, 1); }
void a64_ldrsh64(A64Asm *a, int rt, int rn, int off) { ls_imm(a, 0x79800000u, rt, rn, off, 2); }

/* ================================================================== */
/* Loads / stores — register offset                                    */
/* ================================================================== */

static void ls_reg(A64Asm *a, uint32_t base, int rt, int rn, int rm) {
    a64_word(a, base | RM(rm) | RN(rn) | RD(rt));
}

void a64_ldr64_reg(A64Asm *a, int rt, int rn, int rm) { ls_reg(a, 0xF8606800u, rt, rn, rm); }
void a64_str64_reg(A64Asm *a, int rt, int rn, int rm) { ls_reg(a, 0xF8206800u, rt, rn, rm); }
void a64_ldr16_reg(A64Asm *a, int rt, int rn, int rm) { ls_reg(a, 0x78606800u, rt, rn, rm); }
void a64_ldrsb64_reg(A64Asm *a, int rt, int rn, int rm) { ls_reg(a, 0x38A06800u, rt, rn, rm); }

/* ================================================================== */
/* Pair load/store                                                     */
/* ================================================================== */

/* Address-mode bits [25:23] relative to the signed-offset opcode:
 *   offset 010 (base as given), post-index 001, pre-index 011.
 * Pairs of 32-bit words share the same pattern with base 0x29x00000. */
static void pair_op(A64Asm *a, uint32_t base, int rt, int rt2,
                    int rn, int scaled_off, A64PairMode mode) {
    /* mode_adjust: OFFSET +0x00000000; POST clear bit24 set bit23;
     * PRE set bit23. */
    uint32_t w = base;
    if (mode == A64_PAIR_POST)
        w = (w & ~0x01000000u) | 0x00800000u;
    else if (mode == A64_PAIR_PRE)
        w |= 0x00800000u;
    w |= (uint32_t)((scaled_off & 0x7F) << 15);
    w |= ((rt2 & 31) << 10) | RN(rn) | RD(rt);
    a64_word(a, w);
}

void a64_stp64(A64Asm *a, int rt, int rt2, int rn, int off, A64PairMode mode) {
    pair_op(a, 0xA9000000u, rt, rt2, rn, off / 8, mode);
}
void a64_ldp64(A64Asm *a, int rt, int rt2, int rn, int off, A64PairMode mode) {
    pair_op(a, 0xA9400000u, rt, rt2, rn, off / 8, mode);
}
void a64_stp32(A64Asm *a, int rt, int rt2, int rn, int off, A64PairMode mode) {
    pair_op(a, 0x29000000u, rt, rt2, rn, off / 4, mode);
}
void a64_ldp32(A64Asm *a, int rt, int rt2, int rn, int off, A64PairMode mode) {
    pair_op(a, 0x29400000u, rt, rt2, rn, off / 4, mode);
}

/* ================================================================== */
/* Branches / system / page addressing                                 */
/* ================================================================== */

void a64_b(A64Asm *a, int label) {
    word_with_fixup(a, 0x14000000u, A64_FIX_B26, 0, label);
}

void a64_bl(A64Asm *a, int label) {
    word_with_fixup(a, 0x94000000u, A64_FIX_B26, 0, label);
}

void a64_bcond(A64Asm *a, int cond, int label) {
    word_with_fixup(a, 0x54000000u | (cond & 15), A64_FIX_B19, 0, label);
}

void a64_cbz(A64Asm *a, int rt, int label, int is64) {
    word_with_fixup(a, (is64 ? 0xB4000000u : 0x34000000u) | RD(rt),
                    A64_FIX_B19, 0, label);
}

void a64_cbnz(A64Asm *a, int rt, int label, int is64) {
    word_with_fixup(a, (is64 ? 0xB5000000u : 0x35000000u) | RD(rt),
                    A64_FIX_B19, 0, label);
}

void a64_ret(A64Asm *a, int rn) {
    a64_word(a, 0xD65F0000u | RN(rn));
}

void a64_svc(A64Asm *a, unsigned imm16) {
    a64_word(a, 0xD4000001u | ((imm16 & 0xFFFF) << 5));
}

void a64_adrp_label(A64Asm *a, int rd, int label) {
    word_with_fixup(a, 0x90000000u | RD(rd), A64_FIX_ADRP, rd, label);
}

/* ADRP + ADD pair resolving a label's runtime address into rd (PIE-safe;
 * the ADD low12 word is patched by a64_resolve). */
void a64_adrp_add_label(A64Asm *a, int rd, int label) {
    uint32_t at = (uint32_t)a->code.len;
    a64_word(a, 0x90000000u | RD(rd));                       /* adrp rd, page */
    a64_add_imm12(a, rd, rd, 0, 0, 1, 0);                    /* add rd,rd,#lo */
    add_fixup_at(a, A64_FIX_ADRP_ADD, rd, label, at);
}

/* Indirect branch / branch-with-link through a register. */
void a64_br_reg(A64Asm *a, int rn) {
    a64_word(a, 0xD61F0000u | RN(rn));
}
void a64_blr(A64Asm *a, int rn) {
    a64_word(a, 0xD63F0000u | RN(rn));
}

/* Pointer authentication.  These are HINT-space encodings -- the CPU treats
 * them as no-ops unless the process opted in at exec time -- so emitting them
 * unconditionally is safe and keeps the generated code the same shape the
 * rest of the system uses.
 *
 * The encodings below were read out of an assembled reference rather than
 * written down from the ARM manual: a plausible-looking guess here is worse
 * than no support, because a wrong word is either an illegal instruction or
 * an unrelated operation.  `ret` is in the same space (0xd65f0..00 is the
 * authenticated-return half of RET) and is already emitted as a plain ret,
 * which the hardware also accepts. */
void a64_paciasp(A64Asm *a) {
    a64_word(a, 0xD503233Fu);
}

void a64_pacia(A64Asm *a, int rt) {
    a64_word(a, 0xDAC10000u | RD(rt));
}

void a64_autda(A64Asm *a, int rt, int rn) {
    a64_word(a, 0xDAC10000u | RD(rt) | RN(rn));
}

void a64_retab(A64Asm *a) {
    a64_word(a, 0xD65F0FFFu);
}

/* ================================================================== */
/* Scalar floating point                                               */
/* ================================================================== */

void a64_fmov_reg(A64Asm *a, int rd, int rm, int is_double) {
    /* ORR Vd.16B, Vn.16B, Vm.16B: the source is placed in Rn with
     * Rm = ZERO (00000), which assembles/disassembles as FMOV Vd, Vn. */
    uint32_t base = is_double ? 0x1E604000u : 0x1E204000u;
    a64_word(a, base | RN(rm) | RD(rd));
}

void a64_fmov_gp(A64Asm *a, int rd, int rn, int to_vec, int is64) {
    /* FMOV (general), scalar.  Checked against clang -arch arm64:
     *   fmov x0, d0 = 9e660000    fmov d1, x1 = 9e670021
     *   fmov w2, s2 = 1e260042    fmov s3, w3 = 1e270063 */
    uint32_t base = is64
        ? (to_vec ? 0x9E670000u : 0x9E660000u)
        : (to_vec ? 0x1E270000u : 0x1E260000u);
    a64_word(a, base | RN(rn) | RD(rd));
}

static void fp_op(A64Asm *a, uint32_t base_d, uint32_t base_s,
                  int rd, int rn, int rm, int is_double) {
    uint32_t w = (is_double ? base_d : base_s) | RM(rm) | RN(rn) | RD(rd);
    a64_word(a, w);
}

void a64_fadd(A64Asm *a, int rd, int rn, int rm, int is_double) {
    fp_op(a, 0x1E602800u, 0x1E202800u, rd, rn, rm, is_double);
}
void a64_fsub(A64Asm *a, int rd, int rn, int rm, int is_double) {
    fp_op(a, 0x1E603800u, 0x1E203800u, rd, rn, rm, is_double);
}
void a64_fmul(A64Asm *a, int rd, int rn, int rm, int is_double) {
    fp_op(a, 0x1E600800u, 0x1E200800u, rd, rn, rm, is_double);
}
void a64_fdiv(A64Asm *a, int rd, int rn, int rm, int is_double) {
    fp_op(a, 0x1E601800u, 0x1E201800u, rd, rn, rm, is_double);
}
void a64_fcmp(A64Asm *a, int rn, int rm, int is_double) {
    fp_op(a, 0x1E602000u, 0x1E202000u, 0, rn, rm, is_double);
}
void a64_scvtf(A64Asm *a, int rd, int rn, int from64) {
    /* D from X: 0x9E620000; S from W: 0x1E220000. */
    uint32_t w = (from64 ? 0x9E620000u : 0x1E220000u) | RN(rn) | RD(rd);
    a64_word(a, w);
}
void a64_fcvtzs(A64Asm *a, int rd, int rn, int to64) {
    /* X from D: 0x9E780000; W from S: 0x1E380000. */
    uint32_t w = (to64 ? 0x9E780000u : 0x1E380000u) | RN(rn) | RD(rd);
    a64_word(a, w);
}

void a64_str_d(A64Asm *a, int rt, int rn, int off) { ls_imm(a, 0xFD000000u, rt, rn, off, 8); }
void a64_ldr_d(A64Asm *a, int rt, int rn, int off) { ls_imm(a, 0xFD400000u, rt, rn, off, 8); }
void a64_str_s(A64Asm *a, int rt, int rn, int off) { ls_imm(a, 0xBD000000u, rt, rn, off, 4); }
void a64_ldr_s(A64Asm *a, int rt, int rn, int off) { ls_imm(a, 0xBD400000u, rt, rn, off, 4); }
