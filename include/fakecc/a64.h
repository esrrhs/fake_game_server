#ifndef FAKECC_A64_H
#define FAKECC_A64_H

/* Minimal A64 (AArch64) instruction encoder with local labels.
 *
 * Every instruction is one 32-bit word emitted little-endian.  Register
 * numbers are the hardware fields 0..31 (see reg_arm64.h; XZR/WZR and SP
 * share code 31, selected by the instruction).  Branch/page fixups are
 * resolved inside one assembler buffer (a function body); cross-function
 * and cross-section references are emitted as ARM64 relocations by the
 * code generator/linker tasks.
 *
 * Encoding field splits follow the ARM Architecture Reference Manual
 * (C6 xx); words were cross-checked byte-for-byte against
 * `clang -arch arm64` (see test/unit/test_a64.c golden table). */

#include "fakecc/common.h"

#include <stdint.h>

/* Condition codes (bits 0-3 of b.cond). */
enum {
    A64_EQ = 0,  A64_NE = 1,  A64_CS = 2,  A64_CC = 3,
    A64_MI = 4,  A64_PL = 5,  A64_VS = 6,  A64_VC = 7,
    A64_HI = 8,  A64_LS = 9,  A64_GE = 10, A64_LT = 11,
    A64_GT = 12, A64_LE = 13, A64_AL = 14,
};

/* Shift types for register-operand ALU instructions. */
enum { A64_LSL = 0, A64_LSR = 1, A64_ASR = 2 };

typedef struct {
    uint32_t pos;      /* bound byte offset */
    int      bound;
} A64LabelSlot;

typedef struct {
    int      kind;     /* A64_FIX_* */
    uint32_t at;       /* byte offset of the instruction word */
    int      label;
    int      rd;       /* ADRP only */
} A64Fixup;

enum {
    A64_FIX_B26 = 1,   /* b/bl: signed 26-bit word displacement */
    A64_FIX_B19,       /* b.cond, cbz, cbnz: signed 19-bit word displacement */
    A64_FIX_ADRP,      /* adrp to a label's page: 21-bit page displacement */
    A64_FIX_ADRP_ADD,  /* adrp at `at` + add low12 at `at+4`, same label */
};

typedef struct {
    Buffer        code;
    A64LabelSlot *labels;
    size_t        nlabels, cap_labels;
    A64Fixup     *fixups;
    size_t        nfixups, cap_fixups;
    /* File/VA offset at which the buffer's first word lands in the final
     * image.  Zero for raw buffers; Mach-O uses 512 (header pad) so ADRP
     * page math produces runtime addresses.  Relative B/BL deltas are
     * unaffected. */
    uint32_t      base_off;
} A64Asm;

void a64_init(A64Asm *a);
void a64_free(A64Asm *a);
static inline void a64_set_base(A64Asm *a, uint32_t off) { a->base_off = off; }

int  a64_new_label(A64Asm *a);
void a64_bind(A64Asm *a, int label);   /* label = current emit offset */
/* Resolve all local fixups.  Returns 0 on success, -1 if a branch is out
 * of range (message printed); callers should then use a long branch. */
int  a64_resolve(A64Asm *a);

/* Raw word emit (little-endian). */
void a64_word(A64Asm *a, uint32_t w);

/* ── Moves / constants ─────────────────────────────────────────────── */
void a64_mov_reg(A64Asm *a, int rd, int rm, int is64);
void a64_movz(A64Asm *a, int rd, unsigned imm16, unsigned hw, int is64);
void a64_movk(A64Asm *a, int rd, unsigned imm16, unsigned hw, int is64);
void a64_movn(A64Asm *a, int rd, unsigned imm16, unsigned hw, int is64);
/* Materialize any 32/64-bit constant in 1-4 movz/movk words. */
void a64_mov_imm64(A64Asm *a, int rd, uint64_t v);

/* ── Integer arithmetic ────────────────────────────────────────────── */
/* add/sub immediate: imm12 is 0..4095; shift12 selects <<12 scaling. */
void a64_add_imm12(A64Asm *a, int rd, int rn, unsigned imm12,
                   int shift12, int is64, int setflags);
void a64_sub_imm12(A64Asm *a, int rd, int rn, unsigned imm12,
                   int shift12, int is64, int setflags);
/* add/sub shifted-register; shift A64_LSL/LSR/ASR, amount 0..63 (0..31 w). */
void a64_add_reg(A64Asm *a, int rd, int rn, int rm,
                 int shift, unsigned amt, int is64, int setflags);
void a64_sub_reg(A64Asm *a, int rd, int rn, int rm,
                 int shift, unsigned amt, int is64, int setflags);
void a64_neg(A64Asm *a, int rd, int rm, int is64);
/* Bitmask-immediate logical ops; return -1 if v is not encodable. */
int  a64_and_imm(A64Asm *a, int rd, int rn, uint64_t v, int is64);
int  a64_or_imm(A64Asm *a, int rd, int rn, uint64_t v, int is64);
int  a64_eor_imm(A64Asm *a, int rd, int rn, uint64_t v, int is64);
/* Logical register. */
void a64_and_reg(A64Asm *a, int rd, int rn, int rm, int is64);
void a64_or_reg(A64Asm *a, int rd, int rn, int rm, int is64);
void a64_eor_reg(A64Asm *a, int rd, int rn, int rm, int is64);
/* Shift by immediate (UBFM/SBFM aliases). */
void a64_lsl_imm(A64Asm *a, int rd, int rn, unsigned amt, int is64);
void a64_lsr_imm(A64Asm *a, int rd, int rn, unsigned amt, int is64);
void a64_asr_imm(A64Asm *a, int rd, int rn, unsigned amt, int is64);
void a64_mul(A64Asm *a, int rd, int rn, int rm, int is64);
void a64_msub(A64Asm *a, int rd, int rn, int rm, int ra, int is64);
void a64_udiv(A64Asm *a, int rd, int rn, int rm, int is64);
void a64_sdiv(A64Asm *a, int rd, int rn, int rm, int is64);
/* Variable-count shifts/rotate (count is a register; hardware masks). */
void a64_lslv(A64Asm *a, int rd, int rn, int rm, int is64);
void a64_lsrv(A64Asm *a, int rd, int rn, int rm, int is64);
void a64_asrv(A64Asm *a, int rd, int rn, int rm, int is64);
void a64_rorv(A64Asm *a, int rd, int rn, int rm, int is64);
/* cset rd, cond (CSINC from ZZR); mvn rd, rm (ORN from ZR). */
void a64_cset(A64Asm *a, int rd, int cond, int is64);
void a64_mvn(A64Asm *a, int rd, int rm, int is64);
/* Sign/zero extension: from_bytes 1/2/4 (sxtb/sxth/sxtw & ux*). */
void a64_sxt(A64Asm *a, int rd, int rn, unsigned from_bytes, int is64);
void a64_uxt(A64Asm *a, int rd, int rn, unsigned from_bytes, int is64);

/* ── Compare (sets NZCV) ────────────────────────────────────────────── */
void a64_cmp_imm12(A64Asm *a, int rn, unsigned imm12, int shift12, int is64);
void a64_cmp_reg(A64Asm *a, int rn, int rm, int is64);

/* ── Loads / stores (unsigned immediate offset) ─────────────────────── */
/* Unscaled signed-offset (LDUR/STUR): off in -256..255; for negative
 * fp-relative locals. */
void a64_stur64(A64Asm *a, int rt, int rn, int off);
void a64_ldur64(A64Asm *a, int rt, int rn, int off);
void a64_stur32(A64Asm *a, int rt, int rn, int off);
void a64_ldur32(A64Asm *a, int rt, int rn, int off);
void a64_stur16(A64Asm *a, int rt, int rn, int off);
void a64_ldur16(A64Asm *a, int rt, int rn, int off);
void a64_stur8(A64Asm *a, int rt, int rn, int off);
void a64_ldur8(A64Asm *a, int rt, int rn, int off);
void a64_ldursb64(A64Asm *a, int rt, int rn, int off);
void a64_ldursh64(A64Asm *a, int rt, int rn, int off);
void a64_ldursw(A64Asm *a, int rt, int rn, int off);
void a64_str64(A64Asm *a, int rt, int rn, int off);   /* off multiple of 8 */
void a64_ldr64(A64Asm *a, int rt, int rn, int off);
void a64_str32(A64Asm *a, int rt, int rn, int off);   /* off multiple of 4 */
void a64_ldr32(A64Asm *a, int rt, int rn, int off);
void a64_str16(A64Asm *a, int rt, int rn, int off);   /* off multiple of 2 */
void a64_ldr16(A64Asm *a, int rt, int rn, int off);
void a64_str8(A64Asm *a, int rt, int rn, int off);
void a64_ldr8(A64Asm *a, int rt, int rn, int off);
void a64_ldrsw(A64Asm *a, int rt, int rn, int off);   /* 32-bit -> 64-bit */
void a64_ldrsb64(A64Asm *a, int rt, int rn, int off); /* signed byte -> X */
void a64_ldrsh64(A64Asm *a, int rt, int rn, int off); /* signed half -> X */
/* Register-offset variants (offset register shifted LSL by access size). */
void a64_ldr64_reg(A64Asm *a, int rt, int rn, int rm);
void a64_str64_reg(A64Asm *a, int rt, int rn, int rm);
void a64_ldr16_reg(A64Asm *a, int rt, int rn, int rm);
void a64_ldrsb64_reg(A64Asm *a, int rt, int rn, int rm);

/* ── Pair load/store ───────────────────────────────────────────────── */
/* off is a multiple of the element size (8 for 64, 4 for 32). */
typedef enum {
    A64_PAIR_OFFSET = 0,  /* [rn, #off]      — no writeback            */
    A64_PAIR_POST   = 1,  /* [rn], #off      — writeback AFTER transfer */
    A64_PAIR_PRE    = 2,  /* [rn, #off]!     — writeback BEFORE transfer */
} A64PairMode;
void a64_stp64(A64Asm *a, int rt, int rt2, int rn, int off, A64PairMode mode);
void a64_ldp64(A64Asm *a, int rt, int rt2, int rn, int off, A64PairMode mode);
void a64_stp32(A64Asm *a, int rt, int rt2, int rn, int off, A64PairMode mode);
void a64_ldp32(A64Asm *a, int rt, int rt2, int rn, int off, A64PairMode mode);

/* ── Branches / system / page addressing ───────────────────────────── */
void a64_b(A64Asm *a, int label);
void a64_bl(A64Asm *a, int label);
void a64_bcond(A64Asm *a, int cond, int label);
void a64_cbz(A64Asm *a, int rt, int label, int is64);
void a64_cbnz(A64Asm *a, int rt, int label, int is64);
void a64_ret(A64Asm *a, int rn);          /* rn = A64_LR normally */
void a64_svc(A64Asm *a, unsigned imm16);
void a64_adrp_label(A64Asm *a, int rd, int label);
/* ADRP+ADD pair: materialize a label's PC-relative runtime address. */
void a64_adrp_add_label(A64Asm *a, int rd, int label);
void a64_br_reg(A64Asm *a, int rn);       /* indirect branch */
void a64_blr(A64Asm *a, int rn);          /* indirect call */

/* Pointer authentication (HINT-space; a no-op unless the process opted in). */
void a64_paciasp(A64Asm *a);             /* paciasp */
void a64_pacia(A64Asm *a, int rt);       /* pacia rt */
void a64_autda(A64Asm *a, int rt, int rn);/* autda rt, rn */
void a64_retab(A64Asm *a);               /* retab */

/* ── Scalar floating point (S = 32-bit, D = 64-bit) ───────────────── */
void a64_fmov_reg(A64Asm *a, int rd, int rm, int is_double);
/* FMOV between a GP register and a scalar SIMD register.
 * to_vec=1: FMOV Dd/Sd, Xn/Wn.  to_vec=0: FMOV Xd/Wd, Dn/Sn.
 * is64 selects the 64-bit form (D/X); otherwise the low 32 bits (S/W). */
void a64_fmov_gp(A64Asm *a, int rd, int rn, int to_vec, int is64);
void a64_fadd(A64Asm *a, int rd, int rn, int rm, int is_double);
void a64_fsub(A64Asm *a, int rd, int rn, int rm, int is_double);
void a64_fmul(A64Asm *a, int rd, int rn, int rm, int is_double);
void a64_fdiv(A64Asm *a, int rd, int rn, int rm, int is_double);
void a64_fcmp(A64Asm *a, int rn, int rm, int is_double);
void a64_scvtf(A64Asm *a, int rd, int rn, int from64);  /* int -> float */
void a64_fcvtzs(A64Asm *a, int rd, int rn, int to64);   /* float -> int */
void a64_str_d(A64Asm *a, int rt, int rn, int off);     /* D, off % 8 == 0 */
void a64_ldr_d(A64Asm *a, int rt, int rn, int off);
void a64_str_s(A64Asm *a, int rt, int rn, int off);     /* S, off % 4 == 0 */
void a64_ldr_s(A64Asm *a, int rt, int rn, int off);

#endif /* FAKECC_A64_H */
