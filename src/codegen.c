#include "fakecc/codegen.h"
#include "fakecc/cg64.h"
#include "fakecc/common.h"
#include "fakecc/debug.h"
#include "fakecc/emit.h"
#include "fakecc/ir.h"
#include "fakecc/regalloc.h"
#include "fakecc/target.h"
#include "fakecc/ast.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/* x86-64 machine-code emission primitives                             */
/* ================================================================== */

static void emit_byte(Buffer *b, uint8_t val) {
    buffer_append(b, (const char *)&val, 1);
}

static void emit_int32(Buffer *b, int32_t val) {
    buffer_append(b, (const char *)&val, 4);
}

/* Emit a ModRM byte: (mod << 6) | (reg << 3) | rm — uses low 3 bits.
 * REX.R and REX.B (bit 3 of reg / rm) must be encoded in the preceding
 * REX prefix by callers via emit_rex_wrb(). */
static void emit_modrm(Buffer *b, int mod, int reg, int rm) {
    emit_byte(b, (uint8_t)(((mod & 3) << 6) | ((reg & 7) << 3) | (rm & 7)));
}
static void emit_modrm_disp(Buffer *b, int reg_field, int base, int off);
static void emit_modrm_indirect(Buffer *b, int reg_field, int base);
static void ensure_reg(Buffer *b, int v, int dst_reg, const RAResult *ra);

/* Emit REX prefix: 0x40 | (W<<3) | (R<<2) | (X<<1) | B.
 * R comes from the top bit of the ModRM.reg field's register.
 * B comes from the top bit of the ModRM.rm  field's register (or the
 * opcode-embedded register).
 * For 64-bit operations always pass W=1. */
static void emit_rex_wrb(Buffer *b, int w, int r_reg, int rm_reg) {
    int R = (r_reg >> 3) & 1;
    int B = (rm_reg >> 3) & 1;
    emit_byte(b, (uint8_t)(0x40 | (w << 3) | (R << 2) | B));
}

/* Compat shim — legacy call sites that already handle no-high-reg cases.
 * Prefer emit_rex_wrb for new code. */
static void emit_rex_w(Buffer *b) { emit_byte(b, 0x48); }

/* mov %src, %dst   →  REX 89 [ModRM: reg=src, rm=dst, mod=11] */
static void emit_mov_rr(Buffer *b, int dst, int src) {
    emit_rex_wrb(b, 1, src, dst);
    emit_byte(b, 0x89);
    emit_modrm(b, 3, src, dst);
}

/* add %src, %dst  →  REX 01 [ModRM: reg=src, rm=dst, mod=11] */
static void emit_add_rr(Buffer *b, int dst, int src) {
    emit_rex_wrb(b, 1, src, dst);
    emit_byte(b, 0x01);
    emit_modrm(b, 3, src, dst);
}

/* sub %src, %dst  →  REX 29 [ModRM: reg=src, rm=dst, mod=11] */
static void emit_sub_rr(Buffer *b, int dst, int src) {
    emit_rex_wrb(b, 1, src, dst);
    emit_byte(b, 0x29);
    emit_modrm(b, 3, src, dst);
}

/* imul %src, %dst →  REX 0F AF [ModRM: reg=dst, rm=src, mod=11] */
static void emit_imul_rr(Buffer *b, int dst, int src) {
    emit_rex_wrb(b, 1, dst, src);
    emit_byte(b, 0x0F);
    emit_byte(b, 0xAF);
    emit_modrm(b, 3, dst, src);
}

/* mov $imm32, %dst */
static void emit_mov_imm(Buffer *b, int dst_reg, int32_t imm) {
    emit_rex_wrb(b, 1, 0, dst_reg);
    emit_byte(b, 0xC7);
    emit_modrm(b, 3, 0, dst_reg);
    emit_int32(b, imm);
}

/* movabs $imm64, %dst  →  REX.W B8+rd imm64.  The imm32 form sign-extends,
 * so anything outside int32 (a large unsigned int, any wide long constant)
 * has to go through this. */
static void emit_mov_imm64(Buffer *b, int dst_reg, int64_t imm) {
    if (imm == (int64_t)(int32_t)imm) {
        emit_mov_imm(b, dst_reg, (int32_t)imm);
        return;
    }
    emit_rex_wrb(b, 1, 0, dst_reg);
    emit_byte(b, (uint8_t)(0xB8 + (dst_reg & 7)));
    for (int i = 0; i < 8; i++)
        emit_byte(b, (uint8_t)(imm >> (i * 8)));
}

/* and %src, %dst  →  REX 21 [ModRM: reg=src, rm=dst, mod=11] */
static void emit_and_rr(Buffer *b, int dst, int src) {
    emit_rex_wrb(b, 1, src, dst);
    emit_byte(b, 0x21);
    emit_modrm(b, 3, src, dst);
}

/* or %src, %dst  →  REX 09 [ModRM: reg=src, rm=dst, mod=11] */
static void emit_or_rr(Buffer *b, int dst, int src) {
    emit_rex_wrb(b, 1, src, dst);
    emit_byte(b, 0x09);
    emit_modrm(b, 3, src, dst);
}

/* xor %src, %dst  →  REX 31 [ModRM: reg=src, rm=dst, mod=11] */
static void emit_bitxor_rr(Buffer *b, int dst, int src) {
    emit_rex_wrb(b, 1, src, dst);
    emit_byte(b, 0x31);
    emit_modrm(b, 3, src, dst);
}

/* REX only when W is set or a high register is used.  gcc -O0 32-bit ops
 * on eax/ecx have no prefix (`addl %edx, %eax`). */
static void emit_rex_if(Buffer *b, int w, int r_reg, int index, int rm_reg) {
    int R = (r_reg >> 3) & 1;
    int X = (index >> 3) & 1;
    int B = (rm_reg >> 3) & 1;
    if (w || R || X || B)
        emit_byte(b, (uint8_t)(0x40 | (w << 3) | (R << 2) | (X << 1) | B));
}

static int alu_w64(int width) { return width >= 8 ? 1 : 0; }

/* rol %cl, %dst — rotate through the native width, not a 64-bit shl/shr pair.
 * width 4 uses the 32-bit encoding so bits wrap at 32 (REX.W would wrap at 64). */
static void emit_rol_rcx(Buffer *b, int dst, int width) {
    if (width == 1) {
        uint8_t rex = 0x40 | ((dst & 8) >> 3);
        emit_byte(b, rex);
        emit_byte(b, 0xD2);
        emit_modrm(b, 3, 0, dst); /* /0 = ROL r/m8 */
        return;
    }
    if (width == 2) emit_byte(b, 0x66);
    int w64 = alu_w64(width);
    emit_rex_if(b, w64, 0, 0, dst);
    emit_byte(b, 0xD3);
    emit_modrm(b, 3, 0, dst); /* /0 = ROL */
}

/* Group-1 ALU: add/or/and/sub/xor r/m, r.  op is the primary opcode
 * (0x01 add, 0x09 or, 0x21 and, 0x29 sub, 0x31 xor). */
static void emit_arith_rr_w(Buffer *b, uint8_t op, int dst, int src, int width) {
    emit_rex_if(b, alu_w64(width), src, 0, dst);
    emit_byte(b, op);
    emit_modrm(b, 3, src, dst);
}

/* Group-1 ALU with sign-extended imm8 or imm32.  ext is the /digit
 * (0 add, 1 or, 4 and, 5 sub, 6 xor). */
static void emit_arith_imm_w(Buffer *b, int ext, int dst, int32_t imm, int width) {
    emit_rex_if(b, alu_w64(width), 0, 0, dst);
    if (imm >= -128 && imm <= 127) {
        emit_byte(b, 0x83);
        emit_modrm(b, 3, ext, dst);
        emit_byte(b, (uint8_t)imm);
    } else {
        emit_byte(b, 0x81);
        emit_modrm(b, 3, ext, dst);
        emit_int32(b, imm);
    }
}

static void emit_imul_rr_w(Buffer *b, int dst, int src, int width) {
    emit_rex_if(b, alu_w64(width), dst, 0, src);
    emit_byte(b, 0x0F);
    emit_byte(b, 0xAF);
    emit_modrm(b, 3, dst, src);
}

static void emit_imul_imm_w(Buffer *b, int dst, int32_t imm, int width) {
    emit_rex_if(b, alu_w64(width), dst, 0, dst);
    if (imm >= -128 && imm <= 127) {
        emit_byte(b, 0x6B);
        emit_modrm(b, 3, dst, dst);
        emit_byte(b, (uint8_t)imm);
    } else {
        emit_byte(b, 0x69);
        emit_modrm(b, 3, dst, dst);
        emit_int32(b, imm);
    }
}

static void emit_unary_f7_w(Buffer *b, int ext, int dst, int width) {
    emit_rex_if(b, alu_w64(width), 0, 0, dst);
    emit_byte(b, 0xF7);
    emit_modrm(b, 3, ext, dst);
}

/* Shift/rotate r/m, %cl.  ext: 0 rol, 4 shl, 5 shr, 7 sar. */
static void emit_shift_cl_w(Buffer *b, int ext, int dst, int width) {
    emit_rex_if(b, alu_w64(width), 0, 0, dst);
    emit_byte(b, 0xD3);
    emit_modrm(b, 3, ext, dst);
}

static void emit_shift_imm_w(Buffer *b, int ext, int dst, int imm, int width) {
    emit_rex_if(b, alu_w64(width), 0, 0, dst);
    int mask = alu_w64(width) ? 63 : 31;
    imm &= mask;
    if (imm == 1) {
        emit_byte(b, 0xD1);
        emit_modrm(b, 3, ext, dst);
    } else {
        emit_byte(b, 0xC1);
        emit_modrm(b, 3, ext, dst);
        emit_byte(b, (uint8_t)imm);
    }
}

static void emit_cmp_rr_w(Buffer *b, int dst, int src, int width) {
    emit_rex_if(b, alu_w64(width), src, 0, dst);
    emit_byte(b, 0x39);
    emit_modrm(b, 3, src, dst);
}

static void emit_cmp_imm_w(Buffer *b, int dst, int32_t imm, int width) {
    emit_rex_if(b, alu_w64(width), 0, 0, dst);
    if (imm >= -128 && imm <= 127) {
        emit_byte(b, 0x83);
        emit_modrm(b, 3, 7, dst);
        emit_byte(b, (uint8_t)imm);
    } else {
        emit_byte(b, 0x81);
        emit_modrm(b, 3, 7, dst);
        emit_int32(b, imm);
    }
}

static void emit_test_r_w(Buffer *b, int r, int width) {
    emit_rex_if(b, alu_w64(width), r, 0, r);
    emit_byte(b, 0x85);
    emit_modrm(b, 3, r, r);
}

/* movl $imm32, %dst — 5 bytes, zero-extends to 64.  gcc -O0 form for int. */
static void emit_mov_imm32(Buffer *b, int dst, int32_t imm) {
    emit_rex_if(b, 0, 0, 0, dst);
    emit_byte(b, (uint8_t)(0xB8 + (dst & 7)));
    emit_int32(b, imm);
}

/* cqto */
static void emit_cqto(Buffer *b) { emit_rex_w(b); emit_byte(b, 0x99); }

/* idiv %rcx */
static void emit_idiv_rcx(Buffer *b) {
    emit_rex_w(b);
    emit_byte(b, 0xF7);
    emit_byte(b, 0xF9);
}

/* cmp $imm32, %reg  →  REX 81 [ModRM: /7, mod=11, rm=reg] imm32 */
static void emit_cmp_imm32(Buffer *b, int reg, int32_t imm) {
    emit_rex_wrb(b, 1, 0, reg);
    emit_byte(b, 0x81);
    emit_modrm(b, 3, 7, reg);
    emit_int32(b, imm);
}

/* add $imm32, %reg  →  REX 81 [ModRM: /0, mod=11, rm=reg] imm32 */
static void emit_add_imm32(Buffer *b, int reg, int32_t imm) {
    emit_rex_wrb(b, 1, 0, reg);
    emit_byte(b, 0x81);
    emit_modrm(b, 3, 0, reg);
    emit_int32(b, imm);
}

/* Emit `movq %fs:0, %dst` — load the thread pointer from %fs segment base.
 * Encoding: 64 (FS override) REX.W 8B ModRM(mod=00,reg=dst,rm=4) SIB(00,4,5) 00000000.
 * This is the standard way to read the TP on x86-64 Linux (glibc convention:
 * %fs:0 holds a pointer to the DTV/thread structure whose base is the TP). */
static void emit_mov_fs0_reg(Buffer *b, int dst) {
    emit_byte(b, 0x64);                  /* %fs segment override */
    emit_rex_wrb(b, 1, dst, 0);          /* REX.W [REX.R if dst>=8] */
    emit_byte(b, 0x8B);                  /* MOV r64, r/m64 */
    emit_modrm(b, 0, dst & 7, 4);        /* mod=00 reg=dst rm=4 (SIB follows) */
    emit_byte(b, 0x25);                  /* SIB: scale=00 index=4 base=5 → [disp32] */
    emit_int32(b, 0);                    /* disp32 = 0 → %fs:0 */
}

/* Emit `addq dst, [rip+0]` (RIP-relative qword add) and return the disp32
 * patch offset.  Encoding: REX.W 03 /r with mod=00 rm=5 (RIP-relative).
 * Used for TLS Initial-Exec: add the TPOFF loaded from a GOT slot. */
static size_t emit_add_rip(Buffer *b, int dst) {
    emit_rex_wrb(b, 1, dst, 0 /* B unused — rm=5 is RIP */);
    emit_byte(b, 0x03);
    emit_modrm(b, 0, dst & 7, 5);
    size_t patch = b->len;
    emit_int32(b, 0);
    return patch;
}

/* Patch a rel32 at `patch_off` to jump to `target_off`.  The rel32 is relative
 * to the byte after the 4-byte field (i.e. patch_off+4). */
static void patch_rel32(Buffer *b, size_t patch_off, size_t target_off) {
    int32_t rel = (int32_t)((int64_t)target_off - (int64_t)(patch_off + 4));
    memcpy(b->data + patch_off, &rel, 4);
}

/* test %r, %r  →  REX 85 [ModRM: reg=r, rm=r, mod=11] — sets ZF if r == 0 */
static void emit_test_rr(Buffer *b, int r) {
    emit_rex_wrb(b, 1, r, r);
    emit_byte(b, 0x85);
    emit_modrm(b, 3, r, r);
}

/* setcc %r (low 8 bits).  Encoded: 0F 9x [ModRM: mod=11, reg=0, rm=r].
 * Uses no REX-W (byte op); needs REX (no W) if r >= 4 to avoid AH/BH/CH/DH.
 * We emit REX (0x40) unconditionally for low-8 regs, and REX.B (0x41) for r8-r15. */
static void emit_setcc_r(Buffer *b, uint8_t cc_opcode, int r) {
    /* Need REX to force sil/dil/bpl/spl for regs 4-7, and REX.B for r8-r15. */
    uint8_t rex = 0x40 | ((r & 8) >> 3);
    emit_byte(b, rex);
    emit_byte(b, 0x0F);
    emit_byte(b, cc_opcode);
    emit_modrm(b, 3, 0, r & 7);
}

/* xor %r, %r  →  REX 31 [ModRM: reg=r, rm=r, mod=11] — zero r */
static void emit_xor_rr(Buffer *b, int r) {
    emit_rex_wrb(b, 1, r, r);
    emit_byte(b, 0x31);
    emit_modrm(b, 3, r, r);
}

/* ---- SSE scalar emitters ---------------------------------------- */
/* XMM register ids are 0-15 (native ModRM codes, REX.B/R extended for   */
/* xmm8-xmm15).  These mirror the GP emitters but use the F2/F3 0F      */
/* opcode space.                                                         */

/* movaps xmm_dst, xmm_src  →  0F 28 /r.  Copies all 128 bits so a
 * 16-byte vector in one XMM survives a reg-reg move. */
static void emit_sse_mov_rr(Buffer *b, int dst, int src) {
    emit_rex_wrb(b, 0, dst, src);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x28);
    emit_modrm(b, 3, dst & 7, src & 7);
}

/* VEX prefix for the 0F map, W=0, no extra vvvv operand.
 * L=1 → 256-bit (YMM), L=0 → 128-bit.  R from `xmm`, B from `base`. */
static void emit_vex_0f(Buffer *b, int xmm, int base, int L) {
    int R = (xmm >> 3) & 1;
    int B = (base >> 3) & 1;
    int vvvv = 0xF;
    int pp = 0;
    if (!B) {
        emit_byte(b, 0xC5);
        emit_byte(b, (uint8_t)((R ? 0 : 0x80) | (vvvv << 3) | ((L ? 1 : 0) << 2) | pp));
    } else {
        emit_byte(b, 0xC4);
        emit_byte(b, (uint8_t)(((R ? 0 : 1) << 7) | (1 << 6) | ((B ? 0 : 1) << 5) | 1));
        emit_byte(b, (uint8_t)((vvvv << 3) | ((L ? 1 : 0) << 2) | pp));
    }
}

static void emit_vmovups_mem(Buffer *b, int xmm, int base, int off, int is_store) {
    emit_vex_0f(b, xmm, base, 1);
    emit_byte(b, is_store ? 0x11 : 0x10);
    emit_modrm_disp(b, xmm, base, off);
}

static void emit_vmovups_via_ptr(Buffer *b, int xmm, int ptr, int is_store) {
    emit_vex_0f(b, xmm, ptr, 1);
    emit_byte(b, is_store ? 0x11 : 0x10);
    emit_modrm_indirect(b, xmm, ptr);
}

/* EVEX prefix for the 0F map, W=0, no extra vvvv operand.
 * LLL=2 → 512-bit (ZMM).  Always pair with disp32 addressing so we do
 * not depend on EVEX compressed disp8*N. */
static void emit_evex_0f(Buffer *b, int xmm, int base, int LLL) {
    int R = (xmm >> 3) & 1;
    int B = (base >> 3) & 1;
    /* P0 is R̅ X̅ B̅ R̅′ 00 mm (mm=01 → 0F).  R′=0 for xmm0–15 so R̅′=1.
     * Putting R in B̅ / B in R̅′ encoded zmm8+ and [r8+] as the low regs. */
    uint8_t p0 = (uint8_t)(((R ? 0 : 1) << 7) | (1u << 6)
                           | ((B ? 0 : 1) << 5) | (1u << 4) | 0x01);
    uint8_t p1 = (uint8_t)((0xF << 3) | (1 << 2));
    uint8_t p2 = (uint8_t)(((LLL & 3) << 5) | (1 << 3));
    emit_byte(b, 0x62);
    emit_byte(b, p0);
    emit_byte(b, p1);
    emit_byte(b, p2);
}

static void emit_evex_modrm_disp32(Buffer *b, int reg_field, int base, int off) {
    int rm = base & 7;
    if (rm == 4) {
        emit_modrm(b, 2, reg_field & 7, 4);
        emit_byte(b, 0x24);
    } else {
        emit_modrm(b, 2, reg_field & 7, rm);
    }
    emit_int32(b, off);
}

static void emit_evex_vmovups_mem(Buffer *b, int xmm, int base, int off, int is_store) {
    emit_evex_0f(b, xmm, base, 2);
    emit_byte(b, is_store ? 0x11 : 0x10);
    emit_evex_modrm_disp32(b, xmm, base, off);
}

static void emit_evex_vmovups_via_ptr(Buffer *b, int xmm, int ptr, int is_store) {
    emit_evex_0f(b, xmm, ptr, 2);
    emit_byte(b, is_store ? 0x11 : 0x10);
    emit_evex_modrm_disp32(b, xmm, ptr, 0);
}

static void emit_evex_vmovaps_rr(Buffer *b, int dst, int src) {
    emit_evex_0f(b, dst, src, 2);
    emit_byte(b, 0x28);
    emit_modrm(b, 3, dst & 7, src & 7);
}

static void emit_vmovaps_rr(Buffer *b, int dst, int src) {
    emit_vex_0f(b, dst, src, 1);
    emit_byte(b, 0x28);
    emit_modrm(b, 3, dst & 7, src & 7);
}

/* movups xmm_dst, [rbp+off]  →  0F 10 /r.  16-byte unaligned spill load. */
static void emit_sse_load_spill(Buffer *b, int dst, int off) {
    emit_rex_wrb(b, 0, dst, REG_RBP);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x10);
    if (off >= -128 && off <= 127) {
        emit_modrm(b, 1, dst & 7, REG_RBP);
        emit_byte(b, (uint8_t)(off & 0xFF));
    } else {
        emit_modrm(b, 2, dst & 7, REG_RBP);
        emit_int32(b, off);
    }
}

/* movups [rbp+off], xmm_src  →  0F 11 /r.  16-byte unaligned spill store. */
static void emit_sse_store_spill(Buffer *b, int src, int off) {
    emit_rex_wrb(b, 0, src, REG_RBP);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x11);
    if (off >= -128 && off <= 127) {
        emit_modrm(b, 1, src & 7, REG_RBP);
        emit_byte(b, (uint8_t)(off & 0xFF));
    } else {
        emit_modrm(b, 2, src & 7, REG_RBP);
        emit_int32(b, off);
    }
}

/* SSE scalar arithmetic: add/sub/mul/div.
 *   op: 0x58=add 0x5C=sub 0x59=mul 0x5E=div
 * is_float (width 4) → single-precision (F3 prefix: addss etc.);
 * else double-precision (F2 prefix: addsd etc.).
 * Encoded <prefix> 0F <op> [ModRM: reg=dst, rm=src, mod=11]. */
static void emit_sse_arith(Buffer *b, int op, int dst, int src, int is_float) {
    emit_byte(b, is_float ? 0xF3 : 0xF2);
    emit_rex_wrb(b, 0, dst, src);
    emit_byte(b, 0x0F);
    emit_byte(b, op);
    emit_modrm(b, 3, dst & 7, src & 7);
}

/* Ordered comparison of two SSE scalars.  is_float → ucomiss (0F 2E /r);
 * else ucomisd (66 0F 2E /r).  Sets ZF/CF/PF. */
static void emit_sse_ucomi(Buffer *b, int a, int b_xmm, int is_float) {
    if (!is_float) emit_byte(b, 0x66);
    emit_rex_wrb(b, 0, a, b_xmm);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x2E);
    emit_modrm(b, 3, a & 7, b_xmm & 7);
}

/* cvtsi2ss/sd xmm, r/m64  →  (F3 or F2) REX.W 0F 2A /r. */
static void emit_sse_cvtsi2s(Buffer *b, int xmm, int gp, int is_64, int is_dst_float) {
    emit_byte(b, is_dst_float ? 0xF3 : 0xF2);
    emit_rex_wrb(b, is_64 ? 1 : 0, xmm, gp);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x2A);
    emit_modrm(b, 3, xmm & 7, gp & 7);
}

/* cvttsd2si r/m64, xmm  →  F2 REX.W 0F 2C /r.  Converts a double to a
 * signed GP integer, TRUNCATING toward zero (C semantics).  Note: the
 * non-truncating variant cvtsd2si honors the MXCSR rounding mode and would
 * round 3.5 to 4; cvtt* always truncates regardless. */
static void emit_sse_cvtsd2si(Buffer *b, int gp, int xmm, int is_64) {
    emit_byte(b, 0xF2);
    emit_rex_wrb(b, is_64 ? 1 : 0, gp, xmm);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x2C);
    emit_modrm(b, 3, gp & 7, xmm & 7);
}

/* cvttss2si r/m64, xmm  →  F3 REX.W 0F 2C /r.  Converts a float to a
 * signed GP integer, TRUNCATING toward zero (C semantics).  Note: the
 * non-truncating variant cvtss2si honors the MXCSR rounding mode and would
 * round 3.5 to 4; cvtt* always truncates regardless. */
static void emit_sse_cvtss2si(Buffer *b, int gp, int xmm, int is_64) {
    emit_byte(b, 0xF3);
    emit_rex_wrb(b, is_64 ? 1 : 0, gp, xmm);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x2C);
    emit_modrm(b, 3, gp & 7, xmm & 7);
}

/* cvtss2sd xmm_dst, xmm_src  →  F3 0F 5A /r.  Widens a float (low 32 bits)
 * to a double in the low 64 bits. */
static void emit_sse_cvtss2sd(Buffer *b, int dst, int src) {
    emit_byte(b, 0xF3);
    emit_rex_wrb(b, 0, dst, src);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x5A);
    emit_modrm(b, 3, dst & 7, src & 7);
}

/* cvtsd2ss xmm_dst, xmm_src  →  F2 0F 5A /r.  Narrows a double to a float
 * in the low 32 bits. */
static void emit_sse_cvtsd2ss(Buffer *b, int dst, int src) {
    emit_byte(b, 0xF2);
    emit_rex_wrb(b, 0, dst, src);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x5A);
    emit_modrm(b, 3, dst & 7, src & 7);
}

/* movq xmm_dst, gp_src  →  66 REX.W 0F 6E /r.  Loads 64 bits from a GP
 * register into the low 64 bits of an XMM register (used to materialize
 * float/double constants: movabs rax, bits; movq xmm, rax). */
static void emit_movq_xmm_gp(Buffer *b, int xmm, int gp) {
    emit_byte(b, 0x66);
    emit_rex_wrb(b, 1, xmm, gp);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x6E);
    emit_modrm(b, 3, xmm & 7, gp & 7);
}

/* movd/movq the other way: GP gets the low 4 or 8 bytes of an XMM. */
static void emit_movd_xmm_gp(Buffer *b, int xmm, int gp) {
    emit_byte(b, 0x66);
    emit_rex_wrb(b, 0, xmm, gp);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x6E);
    emit_modrm(b, 3, xmm & 7, gp & 7);
}

static void emit_movd_gp_xmm(Buffer *b, int gp, int xmm) {
    emit_byte(b, 0x66);
    emit_rex_wrb(b, 0, xmm, gp);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x7E);
    emit_modrm(b, 3, xmm & 7, gp & 7);
}

static void emit_movq_gp_xmm(Buffer *b, int gp, int xmm) {
    emit_byte(b, 0x66);
    emit_rex_wrb(b, 1, xmm, gp);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x7E);
    emit_modrm(b, 3, xmm & 7, gp & 7);
}

/* mov %reg, [rsp+off]  →  REX.W 89 [mod reg rm=4 SIB=0x24] disp.  off must be
 * non-negative (the save area sits at/below rsp). */
static void emit_store_rsp_off(Buffer *b, int reg, int off) {
    emit_rex_wrb(b, 1, reg, REG_RSP);
    emit_byte(b, 0x89);
    if (off >= 0 && off <= 127) {
        emit_modrm(b, 1, reg, 4);
        emit_byte(b, 0x24);  /* SIB: base=rsp, index=none */
        emit_byte(b, (uint8_t)off);
    } else {
        emit_modrm(b, 2, reg, 4);
        emit_byte(b, 0x24);
        emit_int32(b, off);
    }
}

/* movsd [rsp], xmm_src  →  F2 0F 11 [mod=00 reg=src rm=4 SIB=0x24].  Used in
 * the prologue push-then-pop dance to push an XMM arg onto the stack. */
static void emit_sse_store_rsp(Buffer *b, int src) {
    emit_byte(b, 0xF2);
    emit_rex_wrb(b, 0, src, REG_RSP);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x11);
    emit_modrm(b, 0, src & 7, 4);
    emit_byte(b, 0x24);  /* SIB: base=rsp, index=none */
}

/* movups [rsp], xmm_src  →  0F 11 [mod=00 reg=src rm=4 SIB=0x24].  16-byte
 * SSE+SSEUP vector push; unaligned so the dance need not 16-align rsp. */
static void emit_movups_store_rsp(Buffer *b, int src) {
    emit_rex_wrb(b, 0, src, REG_RSP);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x11);
    emit_modrm(b, 0, src & 7, 4);
    emit_byte(b, 0x24);
}

/* movaps xmm_dst, [rsp+off]  →  0F 29 [mod dst rm=4 SIB=0x24] disp.  Saves an
 * FP arg register into the variadic save area (16-byte aligned slots). */
static void emit_sse_store_rsp_off(Buffer *b, int dst, int off) {
    emit_rex_wrb(b, 0, dst, REG_RSP);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x29);
    if (off >= 0 && off <= 127) {
        emit_modrm(b, 1, dst, 4);
        emit_byte(b, 0x24);
        emit_byte(b, (uint8_t)off);
    } else {
        emit_modrm(b, 2, dst, 4);
        emit_byte(b, 0x24);
        emit_int32(b, off);
    }
}

/* movsd xmm_dst, [base+off] (load)  →  F2 0F 10 [mod dst rm=base] disp.
 * Generic: base is any register (encoding handles [rsp]-style SIB as needed). */
static void emit_sse_load_base_off(Buffer *b, int dst, int base, int off) {
    emit_byte(b, 0xF2);
    emit_rex_wrb(b, 0, dst, base);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x10);
    if (off >= -128 && off <= 127) {
        if (base == REG_RSP) {
            emit_modrm(b, 1, dst, 4);
            emit_byte(b, 0x24);
            emit_byte(b, (uint8_t)(off & 0xFF));
        } else {
            emit_modrm(b, 1, dst, base);
            emit_byte(b, (uint8_t)(off & 0xFF));
        }
    } else {
        if (base == REG_RSP) {
            emit_modrm(b, 2, dst, 4);
            emit_byte(b, 0x24);
        } else {
            emit_modrm(b, 2, dst, base);
        }
        emit_int32(b, off);
    }
}

/* movsd xmm_dst, [rsp]  →  F2 0F 10 [mod=00 reg=dst rm=4 SIB=0x24].  Pops an
 * XMM arg off the stack in the prologue dance. */
static void emit_sse_load_rsp(Buffer *b, int dst) {
    emit_byte(b, 0xF2);
    emit_rex_wrb(b, 0, dst, REG_RSP);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x10);
    emit_modrm(b, 0, dst & 7, 4);
    emit_byte(b, 0x24);  /* SIB: base=rsp, index=none */
}

/* movups xmm_dst, [rsp]  →  0F 10 [mod=00 reg=dst rm=4 SIB=0x24]. */
static void emit_movups_load_rsp(Buffer *b, int dst) {
    emit_rex_wrb(b, 0, dst, REG_RSP);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x10);
    emit_modrm(b, 0, dst & 7, 4);
    emit_byte(b, 0x24);
}

/* jmp rel32  →  E9 rel32.  Returns offset of the rel32 field for patching. */
static size_t emit_jmp_rel32(Buffer *b) {
    emit_byte(b, 0xE9);
    size_t patch = b->len;
    emit_int32(b, 0);
    return patch;
}

/* Jcc rel32  →  0F 8x rel32.  Returns offset of the rel32 field for patching. */
static size_t emit_jcc_rel32(Buffer *b, uint8_t cc_opcode) {
    emit_byte(b, 0x0F);
    emit_byte(b, cc_opcode);
    size_t patch = b->len;
    emit_int32(b, 0);
    return patch;
}

/* push %r  →  50+r  (with REX.B for r8-r15) */
static void emit_push_r(Buffer *b, int r) {
    if (r >= 8) emit_byte(b, 0x41);  /* REX.B */
    emit_byte(b, (uint8_t)(0x50 | (r & 7)));
}

/* pop %r  →  58+r  (with REX.B for r8-r15) */
static void emit_pop_r(Buffer *b, int r) {
    if (r >= 8) emit_byte(b, 0x41);
    emit_byte(b, (uint8_t)(0x58 | (r & 7)));
}

/* sub $imm, %rsp — used for alignment padding.  imm8 form matches gcc. */
static void emit_sub_rsp_imm32(Buffer *b, int32_t imm) {
    emit_rex_w(b);
    if (imm >= -128 && imm <= 127) {
        emit_byte(b, 0x83);
        emit_byte(b, 0xEC);
        emit_byte(b, (uint8_t)imm);
    } else {
        emit_byte(b, 0x81);
        emit_byte(b, 0xEC);
        emit_int32(b, imm);
    }
}

/* add $imm, %rsp */
static void emit_add_rsp_imm32(Buffer *b, int32_t imm) {
    emit_rex_w(b);
    if (imm >= -128 && imm <= 127) {
        emit_byte(b, 0x83);
        emit_byte(b, 0xC4);
        emit_byte(b, (uint8_t)imm);
    } else {
        emit_byte(b, 0x81);
        emit_byte(b, 0xC4);
        emit_int32(b, imm);
    }
}

/* Push/pop an XMM/YMM arg in the SysV dance.  32-byte vectors use
 * vmovups ymm and a 32-byte slot; 16-byte use movups; scalars stay 8-byte. */
static void emit_xmm_push(Buffer *b, int xmm, int nbytes) {
    int n = nbytes >= 64 ? 64 : nbytes >= 32 ? 32 : nbytes >= 16 ? 16 : 8;
    emit_sub_rsp_imm32(b, n);
    if (n >= 64) emit_evex_vmovups_mem(b, xmm, REG_RSP, 0, 1);
    else if (n >= 32) emit_vmovups_mem(b, xmm, REG_RSP, 0, 1);
    else if (n >= 16) emit_movups_store_rsp(b, xmm);
    else emit_sse_store_rsp(b, xmm);
}
static void emit_xmm_pop(Buffer *b, int xmm, int nbytes) {
    int n = nbytes >= 64 ? 64 : nbytes >= 32 ? 32 : nbytes >= 16 ? 16 : 8;
    if (n >= 64) emit_evex_vmovups_mem(b, xmm, REG_RSP, 0, 0);
    else if (n >= 32) emit_vmovups_mem(b, xmm, REG_RSP, 0, 0);
    else if (n >= 16) emit_movups_load_rsp(b, xmm);
    else emit_sse_load_rsp(b, xmm);
    emit_add_rsp_imm32(b, n);
}

/* call rel32  →  E8 rel32.  Returns offset of the rel32 field for patching. */
static size_t emit_call_rel32(Buffer *b) {
    emit_byte(b, 0xE8);
    size_t patch = b->len;
    emit_int32(b, 0);
    return patch;
}

/* call *%reg  →  FF /2 [ModRM: mod=11, reg=2, rm=reg].  Indirect call through
 * a register holding a function pointer.  REX.B (0x41) for reg >= 8. */
static void emit_indirect_call(Buffer *b, int reg) {
    if (reg >= 8) emit_byte(b, 0x41);  /* REX.B */
    emit_byte(b, 0xFF);
    emit_modrm(b, 3, 2, reg & 7);
}

/* PLT emission (emit_plt0 / emit_plt_entry) has been moved to the linker
 * (src/link.c): the PLT must be built after all modules are merged, since its
 * final code-buffer offset depends on the total merged .text length.  codegen
 * now records cross-module references as relocations instead. */

/* ================================================================== */
/* x87 FPU emission — long double (80-bit extended, width 16)          */
/*                                                                      */
/* Long doubles never occupy XMM registers.  Each lives in a dedicated
 * 16-byte stack slot (ld_off[v]) and moves through x87 st0: we `fldt` a
 * slot to load into st0, `fstpt` to store st0 back and pop.  Every ld
 * operation ends with st0 empty (the `*p` pop instructions + fstpt clear
 * the x87 stack).                                                      */
/*                                                                      */
/* x87 memory operands use the `/r` encoding: opcode DB (low) with the
 * reg field of ModRM selecting the operation (0=fild, 1=fisttp, 5=fldt,
 * 7=fstpt).  To keep ModRM simple we materialize each slot's address
 * into a GP scratch register first, then encode mod=00 rm=scratch.      */
/* ================================================================== */

/* fldt m80real  →  DB /5 [mod=00 reg=5 rm=scratch].  Load 10-byte
 * extended from [scratch] into st0 (pushes the x87 stack).  `scratch` must
 * not be RSP/RBP (their mod=00 encodings are special).  RCX is used. */
static void emit_x87_fldtRCX(Buffer *b) {
    emit_byte(b, 0xDB);
    emit_modrm(b, 0, 5, REG_RCX & 7);
}

/* fstpt m80real  →  DB /7 [mod=00 reg=7 rm=scratch].  Store st0 to
 * [scratch] and pop the x87 stack. */
static void emit_x87_fstptRCX(Buffer *b) {
    emit_byte(b, 0xDB);
    emit_modrm(b, 0, 7, REG_RCX & 7);
}

/* faddp / fsubp / fmulp / fdivp  →  DE C1 / DE E1 / DE C9 / DE F9.
 * Binary x87 op: st1 = st1 op st0, pop st0.  Encoded with mod=11, rm=st0. */
static void emit_x87_arith_pop(Buffer *b, int op) {
    emit_byte(b, 0xDE);
    emit_byte(b, (uint8_t)op);
}

/* fstpt [rsp]  →  DB /7 [mod=00 reg=7 rm=4 SIB=0x24].  Store st0 to a 16-byte
 * slot at [rsp] and pop.  Used to push long-double call args. */
static void emit_x87_fstpt_rsp(Buffer *b) {
    emit_byte(b, 0xDB);
    emit_modrm(b, 0, 7, 4); /* mod=00 rm=100 => SIB follows */
    emit_byte(b, 0x24);     /* SIB: base=rsp, index=none */
}

/* fcomip st0, st1  →  DF F1.  Compare st0 to st1, pop st0, set ZF/CF.
 * Used for long-double comparisons (IR_FCMP ld). */
static void emit_x87_fcomip(Buffer *b) {
    emit_byte(b, 0xDF);
    emit_byte(b, 0xF1);
}

/* Forward declarations: these helpers live later in the file (with the other
 * SSE/GP emitters) but are referenced by the ld load/store wrappers below. */
static void emit_ld_addr(Buffer *b, int reg, int ld_off);
static void emit_store_base_off(Buffer *b, int base, int reg, int off);
static void emit_load_base_off(Buffer *b, int dst, int base, int off);

/* Materialize long-double value `v`'s slot address into RCX, then fldt it
 * into st0.  `ld_off` is the prologue-computed rbp-relative slot array. */
static void emit_ld_load(Buffer *b, int v, const int *ld_off) {
    emit_ld_addr(b, REG_RCX, ld_off[v]);
    emit_x87_fldtRCX(b);
}

/* Load st0 into long-double value `v`'s slot and pop. */
static void emit_ld_store(Buffer *b, int v, const int *ld_off) {
    emit_ld_addr(b, REG_RCX, ld_off[v]);
    emit_x87_fstptRCX(b);
}

/* lea %dst, [rsp]  →  REX 8D [ModRM: mod=00, rm=100 (SIB)] [SIB: base=rsp] */
static void emit_lea_rsp0(Buffer *b, int dst) {
    emit_rex_wrb(b, 1, dst, REG_RSP);
    emit_byte(b, 0x8D);
    emit_modrm(b, 0, dst & 7, 4);
    emit_byte(b, 0x24);
}

/* shr $imm8, %dst  →  REX C1 /5 ib */
static void emit_shr_imm8(Buffer *b, int dst, uint8_t imm) {
    emit_rex_wrb(b, 1, 0, dst);
    emit_byte(b, 0xC1);
    emit_modrm(b, 3, 5, dst);
    emit_byte(b, imm);
}

/* and $imm32, %dst  →  REX 81 /4 id */
static void emit_and_imm32(Buffer *b, int dst, int32_t imm) {
    emit_rex_wrb(b, 1, 0, dst);
    emit_byte(b, 0x81);
    emit_modrm(b, 3, 4, dst);
    emit_int32(b, imm);
}

/* Load the integer in GP register `src` into long-double value `dst`'s slot
 * via x87: stage src in the slot, fild it, fstpt the result back.
 * int→long-double (IR_SITOFP ld).  Clobbers RAX/RCX/RDX. */
static void emit_ld_from_gp_int(Buffer *b, int src, int dst, const int *ld_off,
                                int src_w, int src_u) {
    emit_ld_addr(b, REG_RCX, ld_off[dst]);
    if (src_w == 8 && src_u) {
        /* fild reads a signed integer, so a value above 2^63 would come out
         * negative.  Feed it as 2*(u>>1) + (u&1) instead: both halves fit in
         * 63 bits and the x87's 64-bit mantissa holds each exactly. */
        emit_mov_rr(b, REG_RDX, src);
        emit_shr_imm8(b, REG_RDX, 1);
        emit_store_base_off(b, REG_RCX, REG_RDX, 0);
        emit_byte(b, 0xDF); emit_modrm(b, 0, 5, REG_RCX & 7); /* fild qword */
        emit_byte(b, 0xD8); emit_byte(b, 0xC0);               /* fadd st0,st0 */
        emit_mov_rr(b, REG_RDX, src);
        emit_and_imm32(b, REG_RDX, 1);
        emit_store_base_off(b, REG_RCX, REG_RDX, 0);
        emit_byte(b, 0xDF); emit_modrm(b, 0, 5, REG_RCX & 7); /* fild qword */
        emit_byte(b, 0xDE); emit_byte(b, 0xC1);               /* faddp st1,st0 */
    } else if (src_w == 8 || src_u) {
        /* Signed 64-bit, or a narrower unsigned whose register is already
         * zero-extended: both are correct as a signed 64-bit fild. */
        emit_store_base_off(b, REG_RCX, src, 0);
        emit_byte(b, 0xDF); emit_modrm(b, 0, 5, REG_RCX & 7); /* fild qword */
    } else {
        emit_store_base_off(b, REG_RCX, src, 0);
        emit_byte(b, 0xDB); emit_modrm(b, 0, 0, REG_RCX & 7); /* fild dword */
    }
    emit_x87_fstptRCX(b); /* fstpt [rcx] — st0 (the ld) → slot, pop */
}

/* SysV AMD64: first 6 integer args in rdi, rsi, rdx, rcx, r8, r9. */
static const int SYSV_ARG_REGS[6] = {
    REG_RDI, REG_RSI, REG_RDX, REG_RCX, REG_R8, REG_R9
};

/* ================================================================== */
/* Stack-frame helpers                                                 */
/*                                                                      */
/* curr_cs_count is set once per function in codegen() to the number of */
/* callee-saved GP registers that are pushed in the prologue (0-3:      */
/* RBX, R12, R13).  Spill offsets must account for this save area       */
/* because it sits between rbp and the spill region:                    */
/*                                                                      */
/*   rbp     → saved rbp                                                */
/*   rbp-8   → saved RBX (if used)                                      */
/*   rbp-16  → saved R12 (if used)                                      */
/*   rbp-24  → saved R13 (if used)                                      */
/*   rbp-32  → spill slot 0 (prev was rbp-8 → BUG!)                     */
/*   ...                                                                 */
/* ================================================================== */

static int curr_cs_count = 0;
static const IRFunction *curr_fn = NULL;
static int curr_xmm_spill = 16;

static int spill_offset(int slot) { return -8 * (slot + 1 + curr_cs_count); }

/* mov [rbp+off], %reg */
static void emit_store_spill(Buffer *b, int reg, int off) {
    emit_rex_wrb(b, 1, reg, REG_RBP);
    emit_byte(b, 0x89);
    if (off >= -128 && off <= 127) {
        emit_modrm(b, 1, reg, REG_RBP);
        emit_byte(b, (uint8_t)(off & 0xFF));
    } else {
        emit_modrm(b, 2, reg, REG_RBP);
        emit_int32(b, off);
    }
}

/* mov [rbp+off], %reg → load from spill slot */
static void emit_load_spill(Buffer *b, int reg, int off) {
    emit_rex_wrb(b, 1, reg, REG_RBP);
    emit_byte(b, 0x8B);
    if (off >= -128 && off <= 127) {
        emit_modrm(b, 1, reg, REG_RBP);
        emit_byte(b, (uint8_t)(off & 0xFF));
    } else {
        emit_modrm(b, 2, reg, REG_RBP);
        emit_int32(b, off);
    }
}

/* lea dst_reg, [rbp+off]  →  REX 8D [ModRM: reg=dst, rm=rbp, mod=disp] */
static void emit_lea_rbp(Buffer *b, int dst, int off) {
    emit_rex_wrb(b, 1, dst, REG_RBP);
    emit_byte(b, 0x8D);
    if (off >= -128 && off <= 127) {
        emit_modrm(b, 1, dst, REG_RBP);
        emit_byte(b, (uint8_t)(off & 0xFF));
    } else {
        emit_modrm(b, 2, dst, REG_RBP);
        emit_int32(b, off);
    }
}

/* lea dst_reg, [rip + disp32]  →  REX.W 8D [ModRM: mod=00 reg=dst rm=5] disp32.
 * Returns the file offset of the rel32 slot (caller records for later patch). */
static size_t emit_lea_rip(Buffer *b, int dst) {
    emit_rex_wrb(b, 1, dst, 0 /* B doesn't matter — rm=5 encodes RIP */);
    emit_byte(b, 0x8D);
    emit_modrm(b, 0, dst & 7, 5);
    size_t patch = b->len;
    emit_int32(b, 0);
    return patch;
}

/* Emit `mov dst, [rip+0]` (RIP-relative qword load) and return the disp32
 * patch offset.  Encoding: REX.W 8B /r with mod=00 rm=5 (RIP-relative);
 * the /r reg field is the destination.  Used to load the *address* of an
 * external global from the GOT (GOTPCREL-style). */
static size_t emit_load_rip(Buffer *b, int dst) {
    emit_rex_wrb(b, 1, dst, 0 /* B unused — rm=5 is RIP */);
    emit_byte(b, 0x8B);
    emit_modrm(b, 0, dst & 7, 5);
    size_t patch = b->len;
    emit_int32(b, 0);
    return patch;
}

/* Emit ModRM for `[reg]` addressing where `reg` is a full 4-bit register id
 * (0..15).  Handles the two x86 special cases:
 *   - rbp (reg&7 == 5): mod=00 rm=5 means [disp32], not [rbp]. Use mod=01 disp8=0.
 *   - rsp (reg&7 == 4): rm=4 means SIB follows.  Emit SIB=0x24 (base=rsp, no idx, no scale).
 */
static void emit_modrm_indirect(Buffer *b, int reg_field, int base) {
    int rm = base & 7;
    if (rm == 4) {
        emit_modrm(b, 0, reg_field & 7, 4);
        emit_byte(b, 0x24);  /* SIB: scale=0, index=none(4), base=rsp(4) */
    } else if (rm == 5) {
        emit_modrm(b, 1, reg_field & 7, 5);
        emit_byte(b, 0);
    } else {
        emit_modrm(b, 0, reg_field & 7, rm);
    }
}

/* SSE load/store via pointer: movsd/movss xmm, [ptr] and [ptr], xmm.
 * is_float (width 4) → single-precision (F3 prefix, 4 bytes);
 * else double-precision (F2 prefix, 8 bytes).  Used for float loads/stores
 * through a pointer (pinned float variables). */
static void emit_sse_load_via_ptr(Buffer *b, int dst, int ptr, int is_float) {
    emit_byte(b, is_float ? 0xF3 : 0xF2);
    emit_rex_wrb(b, 0, dst, ptr);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x10);
    emit_modrm_indirect(b, dst, ptr);
}
static void emit_sse_store_via_ptr(Buffer *b, int ptr, int src, int is_float) {
    emit_byte(b, is_float ? 0xF3 : 0xF2);
    emit_rex_wrb(b, 0, src, ptr);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x11);
    emit_modrm_indirect(b, src, ptr);
}

/* movups xmm, [ptr] / [ptr], xmm — 16-byte unaligned, no alignment #GP. */
static void emit_movups_load_via_ptr(Buffer *b, int dst, int ptr) {
    emit_rex_wrb(b, 0, dst, ptr);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x10);
    emit_modrm_indirect(b, dst, ptr);
}
static void emit_movups_store_via_ptr(Buffer *b, int ptr, int src) {
    emit_rex_wrb(b, 0, src, ptr);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x11);
    emit_modrm_indirect(b, src, ptr);
}

/* movq xmm, [ptr] (F3 0F 7E) — 8-byte vector lane. */
static void emit_movq_xmm_mem(Buffer *b, int dst, int ptr) {
    emit_byte(b, 0xF3);
    emit_rex_wrb(b, 0, dst, ptr);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x7E);
    emit_modrm_indirect(b, dst, ptr);
}
static void emit_movq_mem_xmm(Buffer *b, int ptr, int src) {
    emit_byte(b, 0x66);
    emit_rex_wrb(b, 0, src, ptr);
    emit_byte(b, 0x0F);
    emit_byte(b, 0xD6);
    emit_modrm_indirect(b, src, ptr);
}

/* movd xmm, [ptr] (66 0F 6E) — 4-byte vector lane. */
static void emit_movd_xmm_mem(Buffer *b, int dst, int ptr) {
    emit_byte(b, 0x66);
    emit_rex_wrb(b, 0, dst, ptr);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x6E);
    emit_modrm_indirect(b, dst, ptr);
}
static void emit_movd_mem_xmm(Buffer *b, int ptr, int src) {
    emit_byte(b, 0x66);
    emit_rex_wrb(b, 0, src, ptr);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x7E);
    emit_modrm_indirect(b, src, ptr);
}

static void emit_vec_load_xmm(Buffer *b, int xmm, int ptr, int vec_sz) {
    if (vec_sz >= 64) emit_evex_vmovups_via_ptr(b, xmm, ptr, 0);
    else if (vec_sz >= 32) emit_vmovups_via_ptr(b, xmm, ptr, 0);
    else if (vec_sz >= 16) emit_movups_load_via_ptr(b, xmm, ptr);
    else if (vec_sz >= 8) emit_movq_xmm_mem(b, xmm, ptr);
    else emit_movd_xmm_mem(b, xmm, ptr);
}
static void emit_vec_store_xmm(Buffer *b, int ptr, int xmm, int vec_sz) {
    if (vec_sz >= 64) emit_evex_vmovups_via_ptr(b, xmm, ptr, 1);
    else if (vec_sz >= 32) emit_vmovups_via_ptr(b, xmm, ptr, 1);
    else if (vec_sz >= 16) emit_movups_store_via_ptr(b, ptr, xmm);
    else if (vec_sz >= 8) emit_movq_mem_xmm(b, ptr, xmm);
    else emit_movd_mem_xmm(b, ptr, xmm);
}

/* mov [ptr_reg], src_reg — store `width` low bytes of src to memory pointed by ptr. */
static void emit_store_via_ptr(Buffer *b, int ptr, int src, int width) {
    switch (width) {
    case 1: {
        uint8_t rex = 0x40 | ((src & 8) >> 1) | ((ptr & 8) >> 3);
        emit_byte(b, rex);
        emit_byte(b, 0x88);
        emit_modrm_indirect(b, src, ptr);
        break;
    }
    case 2:
        emit_byte(b, 0x66);
        emit_rex_wrb(b, 0, src, ptr);
        emit_byte(b, 0x89);
        emit_modrm_indirect(b, src, ptr);
        break;
    case 4:
        if (src >= 8 || ptr >= 8) emit_rex_wrb(b, 0, src, ptr);
        emit_byte(b, 0x89);
        emit_modrm_indirect(b, src, ptr);
        break;
    case 8:
    default:
        emit_rex_wrb(b, 1, src, ptr);
        emit_byte(b, 0x89);
        emit_modrm_indirect(b, src, ptr);
        break;
    }
}

/* ModRM for [base+off]. Always uses a displacement (mod=01/10) so rbp
 * (rm=5) and a zero offset stay [rbp+0] rather than the RIP-relative encoding. */
static void emit_modrm_disp(Buffer *b, int reg_field, int base, int off) {
    int rm = base & 7;
    int mod = (off >= -128 && off <= 127) ? 1 : 2;
    if (rm == 4) {
        emit_modrm(b, mod, reg_field & 7, 4);
        emit_byte(b, 0x24);
    } else {
        emit_modrm(b, mod, reg_field & 7, rm);
    }
    if (mod == 1) emit_byte(b, (uint8_t)(off & 0xFF));
    else emit_int32(b, off);
}

/* Same width/signedness as emit_load_via_ptr / emit_store_via_ptr, but
 * addressed as [base+off] instead of [ptr].  Folds alloca ADDR+load/store
 * into a gcc-O0-style `mov eax, [rbp-4]`. */
static void emit_load_disp(Buffer *b, int dst, int base, int off, int width, int is_unsigned) {
    switch (width) {
    case 1:
        emit_rex_wrb(b, 1, dst, base);
        emit_byte(b, 0x0F);
        emit_byte(b, is_unsigned ? 0xB6 : 0xBE);
        emit_modrm_disp(b, dst, base, off);
        break;
    case 2:
        emit_rex_wrb(b, 1, dst, base);
        emit_byte(b, 0x0F);
        emit_byte(b, is_unsigned ? 0xB7 : 0xBF);
        emit_modrm_disp(b, dst, base, off);
        break;
    case 4:
        /* gcc -O0 uses movl even for signed int; 32-bit ALU/cmp then
         * see a zero-extended eax, and IR_SEXT still emits movsxd. */
        emit_rex_if(b, 0, dst, 0, base);
        emit_byte(b, 0x8B);
        emit_modrm_disp(b, dst, base, off);
        break;
    case 8:
    default:
        emit_rex_wrb(b, 1, dst, base);
        emit_byte(b, 0x8B);
        emit_modrm_disp(b, dst, base, off);
        break;
    }
}

static void emit_store_disp(Buffer *b, int base, int src, int off, int width) {
    switch (width) {
    case 1: {
        uint8_t rex = 0x40 | ((src & 8) >> 1) | ((base & 8) >> 3);
        emit_byte(b, rex);
        emit_byte(b, 0x88);
        emit_modrm_disp(b, src, base, off);
        break;
    }
    case 2:
        emit_byte(b, 0x66);
        emit_rex_wrb(b, 0, src, base);
        emit_byte(b, 0x89);
        emit_modrm_disp(b, src, base, off);
        break;
    case 4:
        if (src >= 8 || base >= 8) emit_rex_wrb(b, 0, src, base);
        emit_byte(b, 0x89);
        emit_modrm_disp(b, src, base, off);
        break;
    case 8:
    default:
        emit_rex_wrb(b, 1, src, base);
        emit_byte(b, 0x89);
        emit_modrm_disp(b, src, base, off);
        break;
    }
}

static void emit_sse_load_disp(Buffer *b, int dst, int base, int off, int is_float) {
    emit_byte(b, is_float ? 0xF3 : 0xF2);
    emit_rex_wrb(b, 0, dst, base);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x10);
    emit_modrm_disp(b, dst, base, off);
}

/* movups xmm, [base+off] / [base+off], xmm — 16-byte unaligned. */
static void emit_movups_load_disp(Buffer *b, int dst, int base, int off) {
    emit_rex_wrb(b, 0, dst, base);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x10);
    emit_modrm_disp(b, dst, base, off);
}
static void emit_movups_store_disp(Buffer *b, int base, int src, int off) {
    emit_rex_wrb(b, 0, src, base);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x11);
    emit_modrm_disp(b, src, base, off);
}

static void emit_sse_store_disp(Buffer *b, int base, int src, int off, int is_float) {
    emit_byte(b, is_float ? 0xF3 : 0xF2);
    emit_rex_wrb(b, 0, src, base);
    emit_byte(b, 0x0F);
    emit_byte(b, 0x11);
    emit_modrm_disp(b, src, base, off);
}

static void emit_x87_fldt_disp(Buffer *b, int base, int off) {
    emit_byte(b, 0xDB);
    emit_modrm_disp(b, 5, base, off);
}

static void emit_x87_fstpt_disp(Buffer *b, int base, int off) {
    emit_byte(b, 0xDB);
    emit_modrm_disp(b, 7, base, off);
}

/* Load from [ptr_reg] into dst_reg. If width < 8, sign/zero-extend. */
static void emit_load_via_ptr(Buffer *b, int dst, int ptr, int width, int is_unsigned) {
    switch (width) {
    case 1:
        emit_rex_wrb(b, 1, dst, ptr);
        emit_byte(b, 0x0F);
        emit_byte(b, is_unsigned ? 0xB6 : 0xBE);
        emit_modrm_indirect(b, dst, ptr);
        break;
    case 2:
        emit_rex_wrb(b, 1, dst, ptr);
        emit_byte(b, 0x0F);
        emit_byte(b, is_unsigned ? 0xB7 : 0xBF);
        emit_modrm_indirect(b, dst, ptr);
        break;
    case 4:
        emit_rex_if(b, 0, dst, 0, ptr);
        emit_byte(b, 0x8B);
        emit_modrm_indirect(b, dst, ptr);
        break;
    case 8:
    default:
        emit_rex_wrb(b, 1, dst, ptr);
        emit_byte(b, 0x8B);
        emit_modrm_indirect(b, dst, ptr);
        break;
    }
}

/* SIB [base+index*2^scale+off].  r13/rbp (rm=5) always take a disp. */
static void emit_modrm_sib_disp(Buffer *b, int reg_field, int base, int index,
                                int scale2, int off) {
    int rm_b = base & 7;
    int mod;
    if (off == 0 && rm_b != 5) mod = 0;
    else if (off >= -128 && off <= 127) mod = 1;
    else mod = 2;
    emit_modrm(b, mod, reg_field, 4);
    emit_byte(b, (uint8_t)(((scale2 & 3) << 6) | ((index & 7) << 3) | rm_b));
    if (mod == 1) emit_byte(b, (uint8_t)(off & 0xFF));
    else if (mod == 2) emit_int32(b, off);
}

static void emit_load_sib(Buffer *b, int dst, int base, int index, int scale2,
                          int off, int width, int is_unsigned) {
    switch (width) {
    case 1:
        emit_rex_if(b, 1, dst, index, base);
        emit_byte(b, 0x0F);
        emit_byte(b, is_unsigned ? 0xB6 : 0xBE);
        emit_modrm_sib_disp(b, dst, base, index, scale2, off);
        break;
    case 2:
        emit_rex_if(b, 1, dst, index, base);
        emit_byte(b, 0x0F);
        emit_byte(b, is_unsigned ? 0xB7 : 0xBF);
        emit_modrm_sib_disp(b, dst, base, index, scale2, off);
        break;
    case 4:
        emit_rex_if(b, 0, dst, index, base);
        emit_byte(b, 0x8B);
        emit_modrm_sib_disp(b, dst, base, index, scale2, off);
        break;
    case 8:
    default:
        emit_rex_if(b, 1, dst, index, base);
        emit_byte(b, 0x8B);
        emit_modrm_sib_disp(b, dst, base, index, scale2, off);
        break;
    }
}

static void emit_store_sib(Buffer *b, int base, int index, int scale2,
                           int off, int src, int width) {
    switch (width) {
    case 1: {
        uint8_t rex = 0x40 | ((src & 8) >> 1) | ((index & 8) >> 2) | ((base & 8) >> 3);
        emit_byte(b, rex);
        emit_byte(b, 0x88);
        emit_modrm_sib_disp(b, src, base, index, scale2, off);
        break;
    }
    case 2:
        emit_byte(b, 0x66);
        emit_rex_if(b, 0, src, index, base);
        emit_byte(b, 0x89);
        emit_modrm_sib_disp(b, src, base, index, scale2, off);
        break;
    case 4:
        emit_rex_if(b, 0, src, index, base);
        emit_byte(b, 0x89);
        emit_modrm_sib_disp(b, src, base, index, scale2, off);
        break;
    case 8:
    default:
        emit_rex_if(b, 1, src, index, base);
        emit_byte(b, 0x89);
        emit_modrm_sib_disp(b, src, base, index, scale2, off);
        break;
    }
}

/* movsx reg64, reg_small — sign-extend a register's low `src_w` bytes. */
static void emit_movsx_rr(Buffer *b, int dst, int src, int src_w) {
    if (src_w == 4) {
        /* movsxd dst64, src32 */
        emit_rex_wrb(b, 1, dst, src);
        emit_byte(b, 0x63);
        emit_modrm(b, 3, dst, src);
    } else if (src_w == 2) {
        emit_rex_wrb(b, 1, dst, src);
        emit_byte(b, 0x0F);
        emit_byte(b, 0xBF);
        emit_modrm(b, 3, dst, src);
    } else if (src_w == 1) {
        emit_rex_wrb(b, 1, dst, src);
        emit_byte(b, 0x0F);
        emit_byte(b, 0xBE);
        emit_modrm(b, 3, dst, src);
    } else {
        /* No-op: already 64-bit. */
        if (dst != src) emit_mov_rr(b, dst, src);
    }
}

/* movzx reg64, reg_small — zero-extend. */
static void emit_movzx_rr(Buffer *b, int dst, int src, int src_w) {
    if (src_w == 4) {
        /* mov dst32, src32 — auto zero-extends to 64. */
        if (dst >= 8 || src >= 8) emit_rex_wrb(b, 0, src, dst);
        emit_byte(b, 0x89);
        emit_modrm(b, 3, src, dst);
    } else if (src_w == 2) {
        emit_rex_wrb(b, 1, dst, src);
        emit_byte(b, 0x0F);
        emit_byte(b, 0xB7);
        emit_modrm(b, 3, dst, src);
    } else if (src_w == 1) {
        emit_rex_wrb(b, 1, dst, src);
        emit_byte(b, 0x0F);
        emit_byte(b, 0xB6);
        emit_modrm(b, 3, dst, src);
    } else {
        if (dst != src) emit_mov_rr(b, dst, src);
    }
}

/* Mask (extend) a register's low `width` bytes to 64 bits.
 * For unsigned values this is zero-extension; for signed values it is
 * sign-extension so that subsequent 64-bit comparisons (CBR) see the
 * correct signed value.  No-op for width==8. */
static void mask_to_width(Buffer *b, int reg, int width, int is_unsigned) {
    if (width >= 8 || width <= 0) return;
    if (is_unsigned)
        emit_movzx_rr(b, reg, reg, width);
    else
        emit_movsx_rr(b, reg, reg, width);
}

/* 32-bit ops already zero-extend to 64.  Width 1/2 still need an explicit
 * extend so later compares see the right high bits. */
static void mask_after_alu(Buffer *b, int reg, int width, int is_unsigned) {
    if (width >= 4 || width <= 0) return;
    mask_to_width(b, reg, width, is_unsigned);
}

/* ================================================================== */
/* Old-style stack-slot helpers (ra == NULL fallback)                  */
/* ================================================================== */

static int old_slot(int v) { return -(8 * (v + 1)); }

static void old_load(Buffer *b, int v, int reg) {
    int off = old_slot(v);
    emit_rex_wrb(b, 1, reg, REG_RBP);
    emit_byte(b, 0x8B);
    if (off >= -128 && off <= 127) {
        emit_modrm(b, 1, reg, REG_RBP);
        emit_byte(b, (uint8_t)(off & 0xFF));
    } else {
        emit_modrm(b, 2, reg, REG_RBP);
        emit_int32(b, off);
    }
}

static void old_store(Buffer *b, int v, int reg) {
    int off = old_slot(v);
    emit_rex_wrb(b, 1, reg, REG_RBP);
    emit_byte(b, 0x89);
    if (off >= -128 && off <= 127) {
        emit_modrm(b, 1, reg, REG_RBP);
        emit_byte(b, (uint8_t)(off & 0xFF));
    } else {
        emit_modrm(b, 2, reg, REG_RBP);
        emit_int32(b, off);
    }
}

/* ================================================================== */
/* RA-aware value helpers                                              */
/* ================================================================== */

/* True if SSA value `v` is a float (TY_FLOAT) value in this function.  Used
 * to pick the XMM register file (and XMM ABI slots) for float params, args,
 * and results.  Mirrors the helper in regalloc.c. */
static int value_is_float_class(const IRFunction *fn, int v) {
    if (v < 0) return 0;
    if (!fn->value_is_float || fn->value_meta_cap <= 0) return 0;
    if (v >= fn->value_meta_cap) return 0;
    return fn->value_is_float[v];
}

/* True if SSA value `v` is a long double (TY_FLOAT width 16).  Long doubles
 * are NOT kept in XMM registers — they live in dedicated 16-byte stack slots
 * (ld_off[v]) and move through x87 st0.  This predicate gates the x87 path. */
static int value_is_ld(const IRFunction *fn, int v) {
    if (!value_is_float_class(fn, v)) return 0;
    if (!fn->value_width || fn->value_meta_cap <= 0) return 0;
    if (v >= fn->value_meta_cap) return 0;
    /* 4 = 16-byte SSE vector (one XMM).  Long double is kind 1 + width 16. */
    if (fn->value_is_float[v] == 4) return 0;
    return fn->value_width[v] == 16;
}

/* Byte width recorded for SSA value `v` (0 when unknown). */
static int value_width_of(const IRFunction *fn, int v) {
    if (v < 0) return 0;
    if (!fn->value_width || fn->value_meta_cap <= 0) return 0;
    if (v >= fn->value_meta_cap) return 0;
    return fn->value_width[v];
}

/* True if SSA value `v` is a 16-byte vector in one XMM (SSE+SSEUP). */
static int value_is_xmm16(const IRFunction *fn, int v) {
    return value_is_float_class(fn, v) && !value_is_ld(fn, v)
        && value_width_of(fn, v) == 16;
}

/* Packed SSE/AVX vector width in bytes: 16 (XMM) or 32 (YMM).  0 if scalar. */
static int value_vec_bytes(const IRFunction *fn, int v) {
    if (!value_is_float_class(fn, v) || value_is_ld(fn, v)) return 0;
    int w = value_width_of(fn, v);
    if (w == 16 || w == 32 || w == 64) return w;
    return 0;
}

static int vec_nslots(int vec_bytes, int is_ld) {
    if (is_ld) return 2;
    if (vec_bytes >= 64) return 8;
    if (vec_bytes >= 32) return 4;
    if (vec_bytes >= 16) return 2;
    return 1;
}

static void stack_idx_align(int *idx, int align) {
    if (align < 16 || !idx) return;
    int slots = align / 8;
    if (slots <= 1) return;
    if (*idx % slots)
        *idx += slots - (*idx % slots);
}

static int flags_stack_align(unsigned char flags, int vec_bytes, int align16) {
    int al = 8;
    if (flags & CALL_ARG_ALIGN64) al = 64;
    else if (flags & CALL_ARG_ALIGN32) al = 32;
    else if (flags & CALL_ARG_ALIGN16) al = 16;
    if (align16 >= 16 && align16 > al) al = align16;
    else if (align16 == 1 && al < 16) al = 16;
    if (vec_bytes >= 64 && al < 64) al = 64;
    else if (vec_bytes >= 32 && al < 32) al = 32;
    else if (vec_bytes >= 16 && al < 16) al = 16;
    return al;
}

static int param_is_mem_blob(const IRInst *pi) {
    return pi && pi->force_stack && pi->alloca_bytes > 0;
}

static int call_arg_blob_bytes(const IRInst *inst, int k) {
    if (!inst->call_arg_on_stack || !(inst->call_arg_on_stack[k] & CALL_ARG_BLOB))
        return 0;
    if (inst->call_arg_nbytes && inst->call_arg_nbytes[k] > 0)
        return inst->call_arg_nbytes[k];
    return 1;
}

/* Copy `nbytes` from SSA pointer `src_v` onto [dst_base+dst_off].
 * Saves RSI/RDI/RCX so a later SysV GP arg whose home is those
 * registers survives the copy.  DF is cleared. */
static void emit_memcpy_to_addr(Buffer *b, int dst_base, int dst_off,
                                int src_v, const RAResult *ra, int nbytes) {
    if (nbytes <= 0) return;
    emit_push_r(b, REG_RSI);
    emit_push_r(b, REG_RDI);
    emit_push_r(b, REG_RCX);
    ensure_reg(b, src_v, REG_RSI, ra);
    if (dst_base == REG_RSP)
        dst_off += 24;
    emit_rex_wrb(b, 1, REG_RDI, dst_base);
    emit_byte(b, 0x8D);
    emit_modrm_disp(b, REG_RDI, dst_base, dst_off);
    emit_mov_imm32(b, REG_RCX, nbytes);
    emit_byte(b, 0xFC);
    emit_byte(b, 0xF3);
    emit_byte(b, 0xA4);
    emit_pop_r(b, REG_RCX);
    emit_pop_r(b, REG_RDI);
    emit_pop_r(b, REG_RSI);
}

/* Materialize the address of long-double value `v`'s 16-byte slot into GP
 * register `reg`.  ld_off[v] is the rbp-relative offset assigned in the
 * prologue (negative for locals/results, positive for stack-passed params). */
static void emit_ld_addr(Buffer *b, int reg, int ld_off) {
    emit_lea_rbp(b, reg, ld_off);
}

/* Ensure value `v` is in `dst_reg`.  Emits mov or load as needed. */
static void ensure_reg(Buffer *b, int v, int dst_reg, const RAResult *ra) {
    if (!ra || v < 0 || v >= ra->num_values) {
        /* Fallback: old stack-slot path. */
        old_load(b, v, dst_reg);
        return;
    }
    int vr = ra->reg[v];
    if (vr == dst_reg) return;
    if (vr >= 0 && vr < 16) {
        emit_mov_rr(b, dst_reg, vr);
    } else {
        emit_load_spill(b, dst_reg, spill_offset(ra->spill_slot[v]));
    }
}

/* Store to value v's spill slot if v is spilled (no-op if in register). */
static void spill_if_needed(Buffer *b, int v, int src_reg, const RAResult *ra) {
    if (!ra || v < 0 || v >= ra->num_values) {
        old_store(b, v, src_reg);
        return;
    }
    if (ra->reg[v] >= 0 && ra->reg[v] < 16) return;
    emit_store_spill(b, src_reg, spill_offset(ra->spill_slot[v]));
}

static int gp_home(const RAResult *ra, IRValue v) {
    if (!ra || v < 0 || v >= ra->num_values) return -1;
    int r = ra->reg[v];
    return (r >= 0 && r < 16) ? r : -1;
}

/* Prefer dest home so we don't copy rax→dr.  Avoid rcx (holds b) and b's home. */
static int pick_alu_acc(int dr, int avoid) {
    if (dr >= 0 && dr != REG_RCX && dr != avoid)
        return dr;
    if (avoid == REG_RAX) return REG_RDX;
    return REG_RAX;
}

static void finish_gp_dst(Buffer *b, int acc, int dr, int dst, const RAResult *ra,
                          int width, int is_unsigned) {
    mask_after_alu(b, acc, width, is_unsigned);
    if (dr >= 0 && dr != acc) emit_mov_rr(b, dr, acc);
    spill_if_needed(b, dst, dr >= 0 ? dr : acc, ra);
}

static int ssa_const_imm(const IRFunction *fn, const int *def, IRValue v, int64_t *imm);
static int imm_fits_i32(int64_t imm);

/* Binary integer ALU at native width, with gcc-O0-style immediates. */
static void isel_arith(Buffer *b, const IRInst *inst, const IRFunction *fn,
                       const int *def, const RAResult *ra, int dr, int is_mul) {
    int width = inst->width > 0 ? inst->width : 8;
    int64_t imm_a = 0, imm_b = 0;
    int fb = ssa_const_imm(fn, def, inst->b, &imm_b) && imm_fits_i32(imm_b);
    int fa = !is_mul && inst->op != IR_SUB &&
             ssa_const_imm(fn, def, inst->a, &imm_a) && imm_fits_i32(imm_a);
    if (is_mul)
        fa = ssa_const_imm(fn, def, inst->a, &imm_a) && imm_fits_i32(imm_a);
    int acc;
    uint8_t rr_op = 0x01;
    int imm_ext = 0;
    if (!is_mul) {
        if (inst->op == IR_SUB) { rr_op = 0x29; imm_ext = 5; }
        else if (inst->op == IR_BAND) { rr_op = 0x21; imm_ext = 4; }
        else if (inst->op == IR_BOR) { rr_op = 0x09; imm_ext = 1; }
        else if (inst->op == IR_BXOR) { rr_op = 0x31; imm_ext = 6; }
    }
    if (fb) {
        acc = pick_alu_acc(dr, -1);
        ensure_reg(b, inst->a, acc, ra);
        if (is_mul) emit_imul_imm_w(b, acc, (int32_t)imm_b, width);
        else emit_arith_imm_w(b, imm_ext, acc, (int32_t)imm_b, width);
    } else if (fa) {
        acc = pick_alu_acc(dr, -1);
        ensure_reg(b, inst->b, acc, ra);
        if (is_mul) emit_imul_imm_w(b, acc, (int32_t)imm_a, width);
        else emit_arith_imm_w(b, imm_ext, acc, (int32_t)imm_a, width);
    } else {
        int bhome = gp_home(ra, inst->b);
        acc = pick_alu_acc(dr, bhome);
        ensure_reg(b, inst->a, acc, ra);
        ensure_reg(b, inst->b, REG_RCX, ra);
        if (is_mul) emit_imul_rr_w(b, acc, REG_RCX, width);
        else emit_arith_rr_w(b, rr_op, acc, REG_RCX, width);
    }
    finish_gp_dst(b, acc, dr, inst->dst, ra, width, inst->is_unsigned);
}

static void isel_shift(Buffer *b, const IRInst *inst, const IRFunction *fn,
                       const int *def, const RAResult *ra, int dr, int ext) {
    int width = inst->width > 0 ? inst->width : 8;
    int64_t imm = 0;
    int acc;
    if (ssa_const_imm(fn, def, inst->b, &imm) && imm >= 0 && imm < 64) {
        acc = pick_alu_acc(dr, -1);
        ensure_reg(b, inst->a, acc, ra);
        emit_shift_imm_w(b, ext, acc, (int)imm, width);
    } else {
        int ahome = gp_home(ra, inst->a);
        acc = pick_alu_acc(dr, REG_RCX);
        if (ahome == REG_RCX) {
            ensure_reg(b, inst->a, acc, ra);
            ensure_reg(b, inst->b, REG_RCX, ra);
        } else {
            ensure_reg(b, inst->b, REG_RCX, ra);
            ensure_reg(b, inst->a, acc, ra);
        }
        if (ext == 0)
            emit_rol_rcx(b, acc, width);
        else
            emit_shift_cl_w(b, ext, acc, width);
    }
    finish_gp_dst(b, acc, dr, inst->dst, ra, width, inst->is_unsigned);
}

/* XMM spill area sits further from rbp than the GP spill area, so XMM
 * spill slots don't collide with GP ones.  gp_spill_area is the total
 * byte-size of the GP spill region (ra_gp->stack_size).
 * curr_cs_count accounts for the callee-saved register save area above
 * the spill region (see spill_offset above). */
static int spill_offset_xmm(int slot, int gp_spill_area) {
    return -(gp_spill_area + 8 * curr_cs_count + curr_xmm_spill * (slot + 1));
}

static void emit_xmm_copy(Buffer *b, int dst, int src, int nbytes) {
    if (dst == src) return;
    if (nbytes >= 64) emit_evex_vmovaps_rr(b, dst, src);
    else if (nbytes >= 32) emit_vmovaps_rr(b, dst, src);
    else emit_sse_mov_rr(b, dst, src);
}

static void emit_xmm_load_spill(Buffer *b, int dst, int off, int nbytes) {
    if (nbytes >= 64) emit_evex_vmovups_mem(b, dst, REG_RBP, off, 0);
    else if (nbytes >= 32) emit_vmovups_mem(b, dst, REG_RBP, off, 0);
    else emit_sse_load_spill(b, dst, off);
}

static void emit_xmm_store_spill(Buffer *b, int src, int off, int nbytes) {
    if (nbytes >= 64) emit_evex_vmovups_mem(b, src, REG_RBP, off, 1);
    else if (nbytes >= 32) emit_vmovups_mem(b, src, REG_RBP, off, 1);
    else emit_sse_store_spill(b, src, off);
}

/* Ensure float value `v` is in `dst_xmm`.  Emits movsd or load as needed. */
static void ensure_reg_xmm(Buffer *b, int v, int dst_xmm, const RAResult *ra_xmm,
                           int gp_spill_area) {
    if (!ra_xmm || v < 0 || v >= ra_xmm->num_values) {
        fprintf(stderr, "fakecc: float value %d has no XMM home\n", v);
        exit(1);
    }
    int vr = ra_xmm->reg[v];
    int nbytes = curr_fn ? value_vec_bytes(curr_fn, v) : 0;
    if (vr == dst_xmm) return;
    if (vr >= 0 && vr < 16) {
        emit_xmm_copy(b, dst_xmm, vr, nbytes);
    } else {
        emit_xmm_load_spill(b, dst_xmm,
                            spill_offset_xmm(ra_xmm->spill_slot[v], gp_spill_area),
                            nbytes);
    }
}

/* Store to float value v's XMM spill slot if spilled (no-op if in register). */
static void spill_if_needed_xmm(Buffer *b, int v, int src_xmm,
                                const RAResult *ra_xmm, int gp_spill_area) {
    if (!ra_xmm || v < 0 || v >= ra_xmm->num_values) return;
    if (ra_xmm->reg[v] >= 0 && ra_xmm->reg[v] < 16) return;
    int nbytes = curr_fn ? value_vec_bytes(curr_fn, v) : 0;
    emit_xmm_store_spill(b, src_xmm,
                         spill_offset_xmm(ra_xmm->spill_slot[v], gp_spill_area),
                         nbytes);
}

/* Materialize a float/double constant into xmm_dst.  The IEEE-754 bit
 * pattern lives in inst->float_imm (an int64).  Load it via RAX:
 *   movabs rax, <bits>; movq xmm, rax
 * Works for both float (bits in low 32) and double (all 64 bits). */
static void emit_float_const(Buffer *b, int xmm_dst, int64_t bits) {
    /* movabs rax, imm64  →  REX.W B8+0 */
    emit_rex_w(b);
    emit_byte(b, 0xB8);
    for (int i = 0; i < 8; i++)
        emit_byte(b, (uint8_t)(bits >> (i * 8)));
    /* movq xmm_dst, rax */
    emit_movq_xmm_gp(b, xmm_dst, REG_RAX);
}

/* ================================================================== */
/* Comparison helpers                                                  */
/* ================================================================== */

/* Map an IR comparison opcode to the setcc opcode byte.
 *   signed:   sete/setne/setl/setle/setg/setge  (0x94..)
 *   unsigned: sete/setne/setb/setbe/seta/setae  (0x94/95/92/96/97/93) */
static uint8_t ir_cmp_to_setcc(int ir_op, int is_unsigned) {
    if (!is_unsigned) {
        switch (ir_op) {
        case IR_EQ: return 0x94;
        case IR_NE: return 0x95;
        case IR_LT: return 0x9C;
        case IR_LE: return 0x9E;
        case IR_GT: return 0x9F;
        case IR_GE: return 0x9D;
        default:    return 0x94;
        }
    } else {
        switch (ir_op) {
        case IR_EQ: return 0x94;
        case IR_NE: return 0x95;
        case IR_LT: return 0x92;   /* setb */
        case IR_LE: return 0x96;   /* setbe */
        case IR_GT: return 0x97;   /* seta */
        case IR_GE: return 0x93;   /* setae */
        default:    return 0x94;
        }
    }
}

/* ================================================================== */
/* IR → x86-64 codegen                                                 */
/* ================================================================== */

/* Label patch record — a rel32 to be resolved once we know the label's
 * byte offset inside this function's code. */
typedef struct {
    size_t patch_off;     /* absolute offset of the rel32 field in out->text */
    int label;            /* target label id */
    size_t after_off;     /* offset of the instruction *following* the rel32,
                             used as base for rel32 = target - after */
} Patch;

/* Cross-function call patch: the callee is referenced by name; its code
 * offset only becomes known once all functions have been laid out. */
typedef struct {
    size_t patch_off;
    char  *callee;        /* xstrdup'd */
    size_t after_off;
} CallPatch;

/* Function-address load patch: `lea r, [rip+0]` where the target is a function
 * (e.g. `&add` or a function lvalue).  Resolved against the symbol table. */
typedef struct {
    size_t patch_off;
    char  *fn_name;       /* xstrdup'd function name */
} FnAddrPatch;

/* An IR_DBG_VALUE marker seen at code offset `pc`: from there on, source
 * variable `var` (index into fn->dbg_vars) lives wherever SSA value `value`
 * was allocated.  `value` is -1 once the definition has been optimized away. */
typedef struct {
    int    var;
    int    value;
    size_t pc;
} DbgObs;

/* Compute which callee-saved registers this function actually uses.
 * Sets used_bit for each of REG_RBX / REG_R12 / REG_R13 (0 or 1). */
static void collect_callee_saved(const RAResult *ra, int used[3]) {
    used[0] = used[1] = used[2] = 0;
    if (!ra) return;
    for (int v = 0; v < ra->num_values; v++) {
        int r = ra->reg[v];
        if (r == REG_RBX) used[0] = 1;
        else if (r == REG_R12) used[1] = 1;
        else if (r == REG_R13) used[2] = 1;
    }
}

/* mov [base+off], %reg  →  REX.W 89 [mod=01/10 reg=src rm=base] disp. */
static void emit_store_base_off(Buffer *b, int base, int reg, int off) {
    emit_rex_wrb(b, 1, reg, base);
    emit_byte(b, 0x89);
    if (off >= -128 && off <= 127) {
        emit_modrm(b, 1, reg, base);
        emit_byte(b, (uint8_t)(off & 0xFF));
    } else {
        emit_modrm(b, 2, reg, base);
        emit_int32(b, off);
    }
}

/* mov %reg, [base+off] (load)  →  REX.W 8B [mod=01/10 reg=dst rm=base] disp. */
static void emit_load_base_off(Buffer *b, int dst, int base, int off) {
    emit_rex_wrb(b, 1, dst, base);
    emit_byte(b, 0x8B);
    if (off >= -128 && off <= 127) {
        if ((base & 7) == 4) {
            emit_modrm(b, 1, dst, 4);
            emit_byte(b, 0x24);
        } else {
            emit_modrm(b, 1, dst, base);
        }
        emit_byte(b, (uint8_t)(off & 0xFF));
    } else {
        if ((base & 7) == 4) {
            emit_modrm(b, 2, dst, 4);
            emit_byte(b, 0x24);
        } else {
            emit_modrm(b, 2, dst, base);
        }
        emit_int32(b, off);
    }
}

/* mov [base+off], %reg (32-bit) → 89 [mod=01/10 reg=src rm=base] disp (no REX.W). */
static void emit_store_base_off32(Buffer *b, int base, int reg, int off) {
    emit_rex_wrb(b, 0, reg, base);
    emit_byte(b, 0x89);
    if (off >= -128 && off <= 127) {
        if ((base & 7) == 4) {
            emit_modrm(b, 1, reg, 4);
            emit_byte(b, 0x24);
        } else {
            emit_modrm(b, 1, reg, base);
        }
        emit_byte(b, (uint8_t)(off & 0xFF));
    } else {
        if ((base & 7) == 4) {
            emit_modrm(b, 2, reg, 4);
            emit_byte(b, 0x24);
        } else {
            emit_modrm(b, 2, reg, base);
        }
        emit_int32(b, off);
    }
}

/* mov reg, [base+off] (32-bit load) → 8B [mod=01/10 reg=dst rm=base] disp (no REX.W). */
static void emit_load_base_off32(Buffer *b, int dst, int base, int off) {
    emit_rex_wrb(b, 0, dst, base);
    emit_byte(b, 0x8B);
    if (off >= -128 && off <= 127) {
        if ((base & 7) == 4) {
            emit_modrm(b, 1, dst, 4);
            emit_byte(b, 0x24);
        } else {
            emit_modrm(b, 1, dst, base);
        }
        emit_byte(b, (uint8_t)(off & 0xFF));
    } else {
        if ((base & 7) == 4) {
            emit_modrm(b, 2, dst, 4);
            emit_byte(b, 0x24);
        } else {
            emit_modrm(b, 2, dst, base);
        }
        emit_int32(b, off);
    }
}


static void emit_epilogue(Buffer *b, int stack_size, const int cs_used[3], int has_dyn_alloca);
/* GNU __builtin_apply_args / __builtin_apply block layout for x86-64 SysV,
 * matching gcc/builtins.cc apply_args_size() + ix86_function_arg_regno_p:
 *
 *   +0    incoming stack-arg pointer (rbp+16)
 *   +8    rax  +16 rdx  +24 rcx  +32 rsi  +40 rdi  +48 r8  +56 r9
 *   +64   xmm0..xmm7 as 16-byte slots (TImode / V2DFmode)
 *   total 192
 *
 * Result block (apply_result_size / ix86_function_value_regno_p):
 *   +0 rax  +8 rdx  +16 rsi  +24 rdi  +32 xmm0  +48 xmm1
 *   total 64
 */
#define APPLY_ARGS_SIZE    192
#define APPLY_RESULT_SIZE  64
#define APPLY_ARGS_RAX     8
#define APPLY_ARGS_RDX     16
#define APPLY_ARGS_RCX     24
#define APPLY_ARGS_RSI     32
#define APPLY_ARGS_RDI     40
#define APPLY_ARGS_R8      48
#define APPLY_ARGS_R9      56
#define APPLY_ARGS_XMM     64
#define APPLY_RES_RAX      0
#define APPLY_RES_RDX      8
#define APPLY_RES_RSI      16
#define APPLY_RES_RDI      24
#define APPLY_RES_XMM0     32
#define APPLY_RES_XMM1     48

/* movaps xmm <-> [base+off].  store=1 writes memory.  Address must be
 * 16-byte aligned (GCC uses GET_MODE_ALIGNMENT of the SSE mode). */
static void emit_sse_movaps_base_off(Buffer *b, int xmm, int base, int off, int store) {
    emit_rex_wrb(b, 0, xmm, base);
    emit_byte(b, 0x0F);
    emit_byte(b, store ? 0x29 : 0x28);
    int need_sib = ((base & 7) == 4);
    if (off >= -128 && off <= 127) {
        emit_modrm(b, 1, xmm, need_sib ? 4 : base);
        if (need_sib) emit_byte(b, 0x24);
        emit_byte(b, (uint8_t)(off & 0xFF));
    } else {
        emit_modrm(b, 2, xmm, need_sib ? 4 : base);
        if (need_sib) emit_byte(b, 0x24);
        emit_int32(b, off);
    }
}

/* Prologue: copy incoming arg registers into the apply_args block at
 * [rsp+base], before the param dance clobbers them.  GCC emits this at
 * function entry (expand_builtin_apply_args). */
static void emit_fill_apply_args(Buffer *b, int base) {
    emit_store_rsp_off(b, REG_RAX, base + APPLY_ARGS_RAX);
    emit_store_rsp_off(b, REG_RDX, base + APPLY_ARGS_RDX);
    emit_store_rsp_off(b, REG_RCX, base + APPLY_ARGS_RCX);
    emit_store_rsp_off(b, REG_RSI, base + APPLY_ARGS_RSI);
    emit_store_rsp_off(b, REG_RDI, base + APPLY_ARGS_RDI);
    emit_store_rsp_off(b, REG_R8,  base + APPLY_ARGS_R8);
    emit_store_rsp_off(b, REG_R9,  base + APPLY_ARGS_R9);
    for (int x = 0; x < 8; x++)
        emit_sse_store_rsp_off(b, x, base + APPLY_ARGS_XMM + 16 * x);
    /* Incoming stack args: after push %rbp / mov %rbp,%rsp they live at
     * [rbp+16] (return address at +8). */
    emit_lea_rbp(b, REG_RCX, 16);
    emit_store_rsp_off(b, REG_RCX, base + 0);
}

static void emit_builtin_apply_args(Buffer *b, const IRInst *inst, const RAResult *ra,
                                    int dr, int apply_args_rbp_off) {
    int target = (dr >= 0) ? dr : REG_RAX;
    emit_lea_rbp(b, target, apply_args_rbp_off);
    if (dr < 0)
        spill_if_needed(b, inst->dst, REG_RAX, ra);
}

static void emit_builtin_apply(Buffer *b, const IRInst *inst, const RAResult *ra,
                               int dr, int result_rbp_off, int saved_sp_rbp_off) {
    /* Stash operands in the result slot first so later ensure_reg moves
     * cannot clobber an SSA home that aliases r10/r11/rcx. */
    ensure_reg(b, inst->call_args[0], REG_RAX, ra);
    emit_store_spill(b, REG_RAX, result_rbp_off + 0);
    ensure_reg(b, inst->call_args[1], REG_RAX, ra);
    emit_store_spill(b, REG_RAX, result_rbp_off + 8);
    ensure_reg(b, inst->call_args[2], REG_RAX, ra);
    emit_store_spill(b, REG_RAX, result_rbp_off + 16);

    emit_store_spill(b, REG_RSP, saved_sp_rbp_off);

    emit_load_spill(b, REG_R11, result_rbp_off + 0);  /* function */
    emit_load_spill(b, REG_R10, result_rbp_off + 8);  /* arguments */
    emit_load_spill(b, REG_RCX, result_rbp_off + 16); /* size */

    emit_mov_rr(b, REG_RDX, REG_RCX);
    emit_add_imm32(b, REG_RCX, 15);
    emit_and_imm32(b, REG_RCX, (int32_t)-16);
    emit_sub_rr(b, REG_RSP, REG_RCX);

    /* Copy `size` bytes of stack arguments onto the outgoing stack.
     * GCC: allocate_dynamic_stack_space + emit_block_move from the saved
     * incoming arg pointer (block[0]). */
    emit_load_base_off(b, REG_RSI, REG_R10, 0);
    emit_mov_rr(b, REG_RDI, REG_RSP);
    emit_mov_rr(b, REG_RCX, REG_RDX);
    emit_byte(b, 0xFC); /* cld — SysV requires DF clear at calls */
    emit_byte(b, 0xF3);
    emit_byte(b, 0xA4); /* rep movsb */

    emit_load_base_off(b, REG_RAX, REG_R10, APPLY_ARGS_RAX);
    emit_load_base_off(b, REG_RDX, REG_R10, APPLY_ARGS_RDX);
    emit_load_base_off(b, REG_RCX, REG_R10, APPLY_ARGS_RCX);
    emit_load_base_off(b, REG_RSI, REG_R10, APPLY_ARGS_RSI);
    emit_load_base_off(b, REG_RDI, REG_R10, APPLY_ARGS_RDI);
    emit_load_base_off(b, REG_R8,  REG_R10, APPLY_ARGS_R8);
    emit_load_base_off(b, REG_R9,  REG_R10, APPLY_ARGS_R9);
    for (int x = 0; x < 8; x++)
        emit_sse_movaps_base_off(b, x, REG_R10, APPLY_ARGS_XMM + 16 * x, 0);

    emit_indirect_call(b, REG_R11);

    emit_store_spill(b, REG_RAX, result_rbp_off + APPLY_RES_RAX);
    emit_store_spill(b, REG_RDX, result_rbp_off + APPLY_RES_RDX);
    emit_store_spill(b, REG_RSI, result_rbp_off + APPLY_RES_RSI);
    emit_store_spill(b, REG_RDI, result_rbp_off + APPLY_RES_RDI);
    emit_sse_movaps_base_off(b, 0, REG_RBP, result_rbp_off + APPLY_RES_XMM0, 1);
    emit_sse_movaps_base_off(b, 1, REG_RBP, result_rbp_off + APPLY_RES_XMM1, 1);

    emit_load_spill(b, REG_RSP, saved_sp_rbp_off);

    int target = (dr >= 0) ? dr : REG_RAX;
    emit_lea_rbp(b, target, result_rbp_off);
    if (dr < 0)
        spill_if_needed(b, inst->dst, REG_RAX, ra);
}

static void emit_builtin_return(Buffer *b, const IRInst *inst, const RAResult *ra,
                                int stack_size, const int cs_used[3], int has_dyn_alloca) {
    ensure_reg(b, inst->call_args[0], REG_R10, ra);
    emit_load_base_off(b, REG_RAX, REG_R10, APPLY_RES_RAX);
    emit_load_base_off(b, REG_RDX, REG_R10, APPLY_RES_RDX);
    emit_load_base_off(b, REG_RSI, REG_R10, APPLY_RES_RSI);
    emit_load_base_off(b, REG_RDI, REG_R10, APPLY_RES_RDI);
    emit_sse_movaps_base_off(b, 0, REG_R10, APPLY_RES_XMM0, 0);
    emit_sse_movaps_base_off(b, 1, REG_R10, APPLY_RES_XMM1, 0);
    emit_epilogue(b, stack_size, cs_used, has_dyn_alloca);
}

/* va_list field offsets matching the SysV AMD64 __va_list_tag layout
 * (defined in ast.c).  gp_offset and fp_offset are 4-byte unsigned ints;
 * overflow_arg_area and reg_save_area are 8-byte pointers. */
#define VA_GP_OFF   0
#define VA_FP_OFF   4
#define VA_OV_OFF   8
#define VA_REG_OFF  16

/* Implement `va_start(ap, last)`: fill the four va_list fields.  The initial
 * offsets (gp_offset, fp_offset, overflow_arg_off) are passed in; they are
 * computed once per function from its named-arg layout.  reg_save_area is
 * the 176-byte save area at the bottom of the frame, addressed as
 * [rbp + reg_save_rbp_off] so it stays valid after IR_DYN_ALLOCA moves rsp. */
static void emit_va_start(Buffer *b, const IRInst *inst, const RAResult *ra,
                          int gp_offset, int fp_offset, int overflow_off,
                          int reg_save_rbp_off) {
    int ap = inst->call_args[0];
    ensure_reg(b, ap, REG_RAX, ra);
    int ap_reg = REG_RAX;
    /* gp_offset @0 (4-byte unsigned int) */
    emit_mov_imm(b, REG_RCX, gp_offset);
    emit_store_base_off32(b, ap_reg, REG_RCX, VA_GP_OFF);
    /* fp_offset @4 (4-byte unsigned int) */
    emit_mov_imm(b, REG_RCX, fp_offset);
    emit_store_base_off32(b, ap_reg, REG_RCX, VA_FP_OFF);
    /* overflow_arg_area @8 = rbp + first stack-passed arg offset (8-byte ptr) */
    emit_lea_rbp(b, REG_RCX, overflow_off);
    emit_store_base_off(b, ap_reg, REG_RCX, VA_OV_OFF);
    /* reg_save_area @16 = bottom of frame (8-byte ptr) */
    emit_lea_rbp(b, REG_RCX, reg_save_rbp_off);
    emit_store_base_off(b, ap_reg, REG_RCX, VA_REG_OFF);
}

/* Load a GP value from [base+index] into dst without SIB addressing: computes
 * base+index into a scratch via add, then loads from [scratch+0].  Clobbers
 * R11 and the index reg must not be dst. */
static void emit_load_gp_viabase(Buffer *b, int dst, int base, int index) {
    emit_add_rr(b, base, index); /* base += index */
    emit_load_base_off(b, dst, base, 0);
}

/* Implement `va_arg(ap, T)`.  Walks the register-save area while the per-class
 * offset lasts, then falls back to the overflow (stack) area.  Branches on the
 * requested type class (GP vs FP) using inline-patched forward branches.
 *
 * Layout of the save area (addressed as rsp + byte offset, rsp constant):
 *   [rsp+0..+40]   rdi,rsi,rdx,rcx,r8,r9   (8 bytes each, GP regs)
 *   [rsp+48..+176] xmm0-xmm7               (16 bytes each, FP regs)
 * reg_save_area = rsp is filled by va_start. */
static void emit_va_arg(Buffer *b, const IRFunction *fn, const IRInst *inst,
                        const RAResult *ra, const RAResult *ra_xmm,
                        int gp_spill_area, const int *ld_off) {
    int ap = inst->call_args[0];
    ensure_reg(b, ap, REG_RAX, ra);
    int ap_reg = REG_RAX;
    int dst = inst->dst;
    int is_float = inst->is_float;

    if (dst >= 0 && value_is_ld(fn, dst)) {
        /* long double (X87 class): always passed on the stack. */
        emit_load_base_off(b, REG_R11, ap_reg, VA_OV_OFF); /* r11 = overflow_arg_area */
        emit_add_imm32(b, REG_R11, 15);
        emit_rex_wrb(b, 1, 0, REG_R11);
        emit_byte(b, 0x83);
        emit_modrm(b, 3, 4, REG_R11 & 7); /* and $~15, %r11 */
        emit_byte(b, 0xF0);

        /* Load 2 qwords from [R11] into [rbp + ld_off[dst]] */
        emit_load_base_off(b, REG_RDX, REG_R11, 0);
        emit_store_spill(b, REG_RDX, ld_off[dst]);
        emit_load_base_off(b, REG_RDX, REG_R11, 8);
        emit_store_spill(b, REG_RDX, ld_off[dst] + 8);

        /* Advance overflow_arg_area by 16 and save */
        emit_add_imm32(b, REG_R11, 16);
        emit_store_base_off(b, ap_reg, REG_R11, VA_OV_OFF);
        return;
    }

    /* INTEGER-class aggregate: copy `imm` bytes from the GP save area (or
     * overflow) into the caller-provided slot in call_args[1], then return
     * that pointer. */
    if (!is_float && inst->imm > 0 && inst->call_nargs >= 2
        && inst->call_args[1] >= 0) {
        int nbytes = (int)inst->imm;
        int n8 = (nbytes + 7) / 8;
        if (n8 < 1) n8 = 1;
        int destv = inst->call_args[1];
        ensure_reg(b, destv, REG_RSI, ra);
        int ov_step = (nbytes + 7) & ~7;
        if (ov_step < 8) ov_step = 8;
        int ov_align = 8;
        if (inst->align16 >= 16) ov_align = inst->align16;
        else if (inst->align16) ov_align = 16;
        /* Do not bump overflow alignment from size: `struct { char x[32]; }`
         * is MEMORY with align 1.  type_stack_align already reports 32/64
         * for real YMM/ZMM / aligned(32) types. */

        /* YMM (SSE class, one register) always lives in the overflow area —
         * the GP/XMM save area is only 16 bytes per slot.  32-byte INTEGER
         * MEMORY structs must not take this path (va_arg_22 A32). */
        if (!inst->force_stack && (inst->float_imm & 0x10)
            && (nbytes == 32 || nbytes == 64)) {
            emit_load_base_off(b, REG_R11, ap_reg, VA_OV_OFF);
            emit_add_imm32(b, REG_R11, ov_align - 1);
            emit_and_imm32(b, REG_R11, (int32_t)-ov_align);
            if (nbytes == 64 && host_has_avx512f()) {
                emit_evex_vmovups_mem(b, 0, REG_R11, 0, 0);
                emit_evex_vmovups_mem(b, 0, REG_RSI, 0, 1);
            } else {
                emit_vmovups_mem(b, 0, REG_R11, 0, 0);
                emit_vmovups_mem(b, 0, REG_RSI, 0, 1);
                if (nbytes == 64) {
                    emit_vmovups_mem(b, 0, REG_R11, 32, 0);
                    emit_vmovups_mem(b, 0, REG_RSI, 32, 1);
                }
            }
            emit_add_imm32(b, REG_R11, nbytes);
            emit_store_base_off(b, ap_reg, REG_R11, VA_OV_OFF);
            if (dst >= 0) {
                if (ra && dst < ra->num_values && ra->reg[dst] >= 0
                    && ra->reg[dst] < 16) {
                    int dr = ra->reg[dst];
                    if (dr != REG_RSI) emit_mov_rr(b, dr, REG_RSI);
                } else {
                    spill_if_needed(b, dst, REG_RSI, ra);
                }
            }
            return;
        }

        /* SSE+SSEUP aggregate (`struct { __m128 x; }`): one XMM, 16 bytes.
         * `{double; long}` is SSE+INTEGER (float_imm==1, no 0x10). */
        if (!inst->force_stack && (inst->float_imm & 0x10) && nbytes == 16) {
            emit_load_base_off32(b, REG_RCX, ap_reg, VA_FP_OFF);
            emit_cmp_imm32(b, REG_RCX, 176 - 16);
            size_t jae_ov = emit_jcc_rel32(b, 0x87); /* JA overflow */
            emit_load_base_off(b, REG_R11, ap_reg, VA_REG_OFF);
            emit_add_rr(b, REG_R11, REG_RCX);
            emit_movups_load_disp(b, 0, REG_R11, 0);
            emit_movups_store_disp(b, REG_RSI, 0, 0);
            emit_add_imm32(b, REG_RCX, 16);
            emit_store_base_off32(b, ap_reg, REG_RCX, VA_FP_OFF);
            size_t jmp_end = emit_jmp_rel32(b);
            size_t ov_off = b->len;
            patch_rel32(b, jae_ov, ov_off);
            emit_load_base_off(b, REG_R11, ap_reg, VA_OV_OFF);
            emit_add_imm32(b, REG_R11, 15);
            emit_and_imm32(b, REG_R11, (int32_t)-16);
            emit_movups_load_disp(b, 0, REG_R11, 0);
            emit_movups_store_disp(b, REG_RSI, 0, 0);
            emit_add_imm32(b, REG_R11, 16);
            emit_store_base_off(b, ap_reg, REG_R11, VA_OV_OFF);
            patch_rel32(b, jmp_end, b->len);
            if (dst >= 0) {
                if (ra && dst < ra->num_values && ra->reg[dst] >= 0
                    && ra->reg[dst] < 16) {
                    int dr = ra->reg[dst];
                    if (dr != REG_RSI) emit_mov_rr(b, dr, REG_RSI);
                } else {
                    spill_if_needed(b, dst, REG_RSI, ra);
                }
            }
            return;
        }

        int num_gp = 0, num_fp = 0;
        for (int i = 0; i < n8; i++) {
            if ((inst->float_imm >> i) & 1) num_fp++;
            else num_gp++;
        }

        if (!inst->force_stack && n8 <= 2) {
            size_t jae_gp = 0, jae_fp = 0;
            if (num_gp > 0) {
                emit_load_base_off32(b, REG_RCX, ap_reg, VA_GP_OFF);
                emit_cmp_imm32(b, REG_RCX, 48 - 8 * num_gp);
                jae_gp = emit_jcc_rel32(b, 0x87); /* JA overflow */
            }
            if (num_fp > 0) {
                emit_load_base_off32(b, REG_RCX, ap_reg, VA_FP_OFF);
                emit_cmp_imm32(b, REG_RCX, 176 - 16 * num_fp);
                jae_fp = emit_jcc_rel32(b, 0x87); /* JA overflow */
            }
            for (int i = 0; i < n8; i++) {
                int is_sse = (inst->float_imm >> i) & 1;
                int off_field = is_sse ? VA_FP_OFF : VA_GP_OFF;
                emit_load_base_off32(b, REG_RCX, ap_reg, off_field);
                emit_load_base_off(b, REG_R11, ap_reg, VA_REG_OFF);
                emit_add_rr(b, REG_R11, REG_RCX);
                emit_load_base_off(b, REG_RDX, REG_R11, 0);
                emit_store_base_off(b, REG_RSI, REG_RDX, i * 8);
                emit_add_imm32(b, REG_RCX, is_sse ? 16 : 8);
                emit_store_base_off32(b, ap_reg, REG_RCX, off_field);
            }
            size_t jmp_end = emit_jmp_rel32(b);
            size_t ov_off = b->len;
            if (num_gp > 0) patch_rel32(b, jae_gp, ov_off);
            if (num_fp > 0) patch_rel32(b, jae_fp, ov_off);
            emit_load_base_off(b, REG_R11, ap_reg, VA_OV_OFF);
            if (ov_align >= 16) {
                emit_add_imm32(b, REG_R11, ov_align - 1);
                emit_and_imm32(b, REG_R11, (int32_t)-ov_align);
            }
            for (int i = 0; i < n8; i++) {
                emit_load_base_off(b, REG_RDX, REG_R11, i * 8);
                emit_store_base_off(b, REG_RSI, REG_RDX, i * 8);
            }
            emit_add_imm32(b, REG_R11, ov_step);
            emit_store_base_off(b, ap_reg, REG_R11, VA_OV_OFF);
            patch_rel32(b, jmp_end, b->len);
        } else {
            /* MEMORY-class struct: SysV copies eightbytes from the overflow
             * stack (including aggregates larger than 16 bytes). */
            emit_load_base_off(b, REG_R11, ap_reg, VA_OV_OFF);
            if (ov_align >= 16) {
                emit_add_imm32(b, REG_R11, ov_align - 1);
                emit_and_imm32(b, REG_R11, (int32_t)-ov_align);
            }
            for (int i = 0; i < n8; i++) {
                emit_load_base_off(b, REG_RDX, REG_R11, i * 8);
                emit_store_base_off(b, REG_RSI, REG_RDX, i * 8);
            }
            emit_add_imm32(b, REG_R11, ov_step);
            emit_store_base_off(b, ap_reg, REG_R11, VA_OV_OFF);
        }
        if (dst >= 0) {
            if (ra && dst < ra->num_values && ra->reg[dst] >= 0
                && ra->reg[dst] < 16) {
                int dr = ra->reg[dst];
                if (dr != REG_RSI) emit_mov_rr(b, dr, REG_RSI);
            } else {
                spill_if_needed(b, dst, REG_RSI, ra);
            }
        }
        return;
    }

    if (!is_float) {
        /* ---------------- GP class ---------------- */
        emit_load_base_off32(b, REG_RCX, ap_reg, VA_GP_OFF); /* ecx = gp_offset */
        emit_cmp_imm32(b, REG_RCX, 48);
        size_t jae_ov = emit_jcc_rel32(b, 0x83); /* JAE overflow_path */
        /* register path: load from [reg_save_area + gp_offset] */
        emit_load_base_off(b, REG_R11, ap_reg, VA_REG_OFF); /* r11 = save_area */
        emit_load_gp_viabase(b, REG_RDX, REG_R11, REG_RCX);  /* rdx = *[save+gp] */
        emit_load_base_off32(b, REG_RCX, ap_reg, VA_GP_OFF);
        emit_add_imm32(b, REG_RCX, 8);
        emit_store_base_off32(b, ap_reg, REG_RCX, VA_GP_OFF);
        size_t jmp_end = emit_jmp_rel32(b);
        /* overflow_path: load from [overflow_arg_area], advance by 8 */
        size_t ov_off = b->len;
        patch_rel32(b, jae_ov, ov_off);
        emit_load_base_off(b, REG_R11, ap_reg, VA_OV_OFF); /* r11 = overflow */
        emit_load_base_off(b, REG_RDX, REG_R11, 0);         /* rdx = *[overflow] */
        emit_add_imm32(b, REG_R11, 8);
        emit_store_base_off(b, ap_reg, REG_R11, VA_OV_OFF);
        /* end: */
        patch_rel32(b, jmp_end, b->len);
        /* move loaded value (RDX) into dst home / spill */
        if (dst >= 0) {
            if (ra && dst < ra->num_values && ra->reg[dst] >= 0
                && ra->reg[dst] < 16) {
                int dr = ra->reg[dst];
                if (dr != REG_RDX) emit_mov_rr(b, dr, REG_RDX);
            } else {
                spill_if_needed(b, dst, REG_RDX, ra);
            }
        }
    } else {
        /* ---------------- FP class ---------------- */
        int w16 = dst >= 0 && value_is_xmm16(fn, dst);
        emit_load_base_off32(b, REG_RCX, ap_reg, VA_FP_OFF); /* ecx = fp_offset */
        emit_cmp_imm32(b, REG_RCX, 176);
        size_t jae_ov = emit_jcc_rel32(b, 0x83); /* JAE overflow_path */
        /* register path: load FP from [reg_save_area + fp_offset] */
        emit_load_base_off(b, REG_R11, ap_reg, VA_REG_OFF); /* r11 = save_area */
        emit_add_rr(b, REG_R11, REG_RCX); /* r11 = save_area + fp_offset */
        if (w16) emit_movups_load_disp(b, 0, REG_R11, 0);
        else emit_sse_load_base_off(b, 0, REG_R11, 0); /* xmm0 = *[r11] (movsd) */
        emit_load_base_off32(b, REG_RCX, ap_reg, VA_FP_OFF);
        emit_add_imm32(b, REG_RCX, 16);
        emit_store_base_off32(b, ap_reg, REG_RCX, VA_FP_OFF);
        size_t jmp_end = emit_jmp_rel32(b);
        /* overflow_path: 16-byte SSE is 16-aligned and consumes 16 bytes;
         * scalar float/double consume 8. */
        size_t ov_off = b->len;
        patch_rel32(b, jae_ov, ov_off);
        emit_load_base_off(b, REG_R11, ap_reg, VA_OV_OFF); /* r11 = overflow */
        if (w16) {
            emit_add_imm32(b, REG_R11, 15);
            emit_and_imm32(b, REG_R11, (int32_t)-16);
            emit_movups_load_disp(b, 0, REG_R11, 0);
            emit_add_imm32(b, REG_R11, 16);
        } else {
            emit_sse_load_base_off(b, 0, REG_R11, 0); /* xmm0 = *[overflow] */
            emit_add_imm32(b, REG_R11, 8);
        }
        emit_store_base_off(b, ap_reg, REG_R11, VA_OV_OFF);
        /* end: */
        patch_rel32(b, jmp_end, b->len);
        if (dst >= 0) {
            if (ra_xmm && dst < ra_xmm->num_values && ra_xmm->reg[dst] >= 0
                && ra_xmm->reg[dst] < 16) {
                int dr = ra_xmm->reg[dst];
                if (dr != 0) emit_sse_mov_rr(b, dr, 0);
            } else {
                spill_if_needed_xmm(b, dst, 0, ra_xmm, gp_spill_area);
            }
        }
    }
}

/* Map codegen Reg enum → DWARF x86-64 register number. */
static int reg_to_dwarf(int r) {
    static const int map[16] = {
        /* RAX RCX RDX RBX RSP RBP RSI RDI */
           0,   2,   1,   3,   7,   6,   4,   5,
        /* R8  R9  R10 R11 R12 R13 R14 R15 */
           8,   9,  10,  11,  12,  13,  14,  15
    };
    if (r >= 0 && r < 16) return map[r];
    return -1;
}

/* Where did register allocation put SSA value `v`?  Fills a DWARF location
 * description: a machine register, or the rbp-relative spill slot it was
 * assigned.  Yields DBG_LOC_NONE when `v` has no home — the value's defining
 * instruction was optimized away, so the variable is unavailable here. */
static void value_home(const IRFunction *fn, int v, int gp_spill_area,
                       DebugLocKind *kind, int *rbp_offset, int *dwarf_reg) {
    *kind = DBG_LOC_NONE;
    *rbp_offset = 0;
    *dwarf_reg = -1;
    if (v < 0) return;

    if (value_is_float_class(fn, v)) {
        const RAResult *ra_xmm = (const RAResult *)fn->ra_xmm;
        if (!ra_xmm || v >= ra_xmm->num_values) return;
        if (ra_xmm->reg[v] >= 0) {
            *kind = DBG_LOC_REG;
            *dwarf_reg = 17 + ra_xmm->reg[v];   /* DWARF xmm0 == 17 */
        } else {
            *kind = DBG_LOC_FBREG;
            *rbp_offset = spill_offset_xmm(ra_xmm->spill_slot[v], gp_spill_area);
        }
        return;
    }

    const RAResult *ra = (const RAResult *)fn->ra;
    if (!ra || v >= ra->num_values) return;
    if (ra->reg[v] >= 0) {
        *kind = DBG_LOC_REG;
        *dwarf_reg = reg_to_dwarf(ra->reg[v]);
        if (*dwarf_reg < 0) *kind = DBG_LOC_NONE;
    } else {
        *kind = DBG_LOC_FBREG;
        *rbp_offset = spill_offset(ra->spill_slot[v]);
    }
}

/* Append one location-list entry to a debug variable. */
static void dbg_var_add_range(DebugVar *dv, size_t pc_start, size_t pc_end,
                              DebugLocKind kind, int rbp_offset, int dwarf_reg) {
    if (kind == DBG_LOC_NONE || pc_end <= pc_start) return;
    if (dv->num_ranges >= dv->cap_ranges) {
        dv->cap_ranges = dv->cap_ranges ? dv->cap_ranges * 2 : 4;
        dv->ranges = xrealloc(dv->ranges,
                              dv->cap_ranges * sizeof(DebugLocRange));
    }
    DebugLocRange *r = &dv->ranges[dv->num_ranges++];
    r->pc_start = pc_start;
    r->pc_end = pc_end;
    r->loc_kind = kind;
    r->rbp_offset = rbp_offset;
    r->dwarf_reg = dwarf_reg;
}

/* DWARF expression builders for DW_AT_call_value. */
static void cv_u8(Buffer *b, uint8_t v) { buffer_append(b, (const char *)&v, 1); }
static void cv_uleb(Buffer *b, uint64_t v) {
    do {
        uint8_t byte = v & 0x7f;
        v >>= 7;
        if (v) byte |= 0x80;
        cv_u8(b, byte);
    } while (v);
}
static void cv_sleb(Buffer *b, int64_t v) {
    int more = 1;
    while (more) {
        uint8_t byte = v & 0x7f;
        v >>= 7;
        int sign = (byte & 0x40) != 0;
        if ((v == 0 && !sign) || (v == -1 && sign)) more = 0;
        else byte |= 0x80;
        cv_u8(b, byte);
    }
}

#define CV_OP_consts       0x11
#define CV_OP_plus         0x22
#define CV_OP_minus        0x1c
#define CV_OP_neg          0x29
#define CV_OP_lit0         0x30
#define CV_OP_reg0         0x50
#define CV_OP_entry_value  0xa3

static void cv_entry_reg(Buffer *b, int dwarf_reg) {
    /* DW_OP_entry_value's inner expression describes where the value lives at
     * the call site. gdb 8.2 only accepts a SINGLE DW_OP_reg* op there —
     * "DW_OP_entry_value is supported only for single DW_OP_reg*". For
     * registers 0..31 a single DW_OP_regN (0x50+n) satisfies that; for higher
     * (XMM) registers we must use DW_OP_regx, which gdb rejects inside
     * entry_value — but those are rare as call args, so we fall back to
     * regx and accept the gdb limitation for XMM args. */
    Buffer inner; buffer_init(&inner);
    if (dwarf_reg >= 0 && dwarf_reg <= 31)
        cv_u8(&inner, (uint8_t)(CV_OP_reg0 + dwarf_reg));
    else {
        cv_u8(&inner, 0x90); /* DW_OP_regx */
        cv_uleb(&inner, (uint64_t)dwarf_reg);
    }
    cv_u8(b, CV_OP_entry_value);
    cv_uleb(b, inner.len);
    buffer_append(b, inner.data, inner.len);
    buffer_free(&inner);
}

/* Is SSA `v` the live value of formal parameter with SysV index `param_idx`
 * just before instruction index `at`? */
/* Resolve the alloca slot an SSA value points to: if the value is an
 * IR_ADDR, return its alloca_slot operand; if it is an IR_Load from a
 * slot whose defining STORE used an IR_ADDR, resolve transitively. */
static int resolve_alloca_slot(const IRFunction *fn, IRValue v, size_t at) {
    if (v < 0) return -1;
    for (size_t i = at; i > 0; ) {
        i--;
        const IRInst *ins = &fn->insts.data[i];
        if (ins->dst != v) continue;
        if (ins->op == IR_ADDR) return ins->a;
        if (ins->op == IR_LOAD) return resolve_alloca_slot(fn, ins->a, i);
        return -1;
    }
    return -1;
}

static int ssa_is_param(const IRFunction *fn, IRValue v, int param_idx, size_t at) {
    if (v < 0 || param_idx < 0) return 0;
    int dbg_i = -1;
    for (size_t i = 0; i < fn->num_dbg_vars; i++) {
        if (fn->dbg_vars[i].kind == IR_DBG_PARAM
            && fn->dbg_vars[i].param_idx == param_idx) {
            dbg_i = (int)i;
            break;
        }
    }
    if (dbg_i < 0) {
        for (size_t i = 0; i < at && i < fn->insts.len; i++) {
            const IRInst *ins = &fn->insts.data[i];
            if (ins->op == IR_PARAM && (int)ins->imm == param_idx && ins->dst == v)
                return 1;
        }
        return 0;
    }
    /* Walk backwards from `at`. The value is the parameter's if it traces
     * back to the IR_PARAM def, following IR_DBG_VALUE markers AND the
     * spill/reload cycle (IR_STORE to alloca slot, IR_LOAD from it) that -O0
     * emits for pinned parameters. */
    for (size_t i = at; i > 0; ) {
        i--;
        const IRInst *ins = &fn->insts.data[i];
        if (ins->op == IR_DBG_VALUE && (int)ins->imm == dbg_i)
            return ins->a == v;
        if (ins->op == IR_PARAM && (int)ins->imm == param_idx && ins->dst == v)
            return 1;
        /* IR_LOAD dst, slot / IR_LOAD_PTR dst, ptr — dst is a reload of what
         * was stored at an alloca slot. Resolve the slot both values refer
         * to (following IR_ADDR) and check if a STORE to that slot wrote the
         * parameter value. */
        if ((ins->op == IR_LOAD || ins->op == IR_LOAD_PTR) && ins->dst == v) {
            int slot = resolve_alloca_slot(fn, ins->a, i);
            if (slot < 0) return 0;
            /* Find the last STORE/STORE_PTR whose address resolves to this slot. */
            for (size_t j = i; j > 0; ) {
                j--;
                const IRInst *s = &fn->insts.data[j];
                if ((s->op == IR_STORE || s->op == IR_STORE_PTR)
                    && resolve_alloca_slot(fn, s->a, j) == slot)
                    return ssa_is_param(fn, s->b, param_idx, j);
            }
            return 0;
        }
    }
    return 0;
}

static int find_param_for_ssa(const IRFunction *fn, IRValue v, size_t at) {
    for (size_t i = 0; i < fn->num_dbg_vars; i++) {
        if (fn->dbg_vars[i].kind != IR_DBG_PARAM) continue;
        if (ssa_is_param(fn, v, fn->dbg_vars[i].param_idx, at))
            return fn->dbg_vars[i].param_idx;
    }
    /* Fall back: IR_PARAM result itself. */
    for (size_t i = 0; i < at && i < fn->insts.len; i++) {
        const IRInst *ins = &fn->insts.data[i];
        if (ins->op == IR_PARAM && ins->dst == v) return (int)ins->imm;
    }
    return -1;
}

static const IRInst *find_def(const IRFunction *fn, IRValue v, size_t at) {
    if (v < 0) return NULL;
    for (size_t i = at; i > 0; ) {
        i--;
        if (fn->insts.data[i].dst == v) return &fn->insts.data[i];
    }
    return NULL;
}

/* Synthesize a DW_AT_call_value expression for SSA `v`.  Prefer describing
 * it in terms of DW_OP_entry_value of formal parameters (so outer frames
 * survive caller-saved clobbers), falling back to a literal when possible.
 * Returns 1 on success. */
static int synth_call_value(const IRFunction *fn, IRValue v, size_t at,
                            const int *param_entry_dwarf, int nparams,
                            Buffer *out, int depth) {
    if (depth > 16 || v < 0) return 0;
    int pidx = find_param_for_ssa(fn, v, at);
    if (pidx >= 0 && pidx < nparams && param_entry_dwarf[pidx] >= 0) {
        cv_entry_reg(out, param_entry_dwarf[pidx]);
        return 1;
    }
    const IRInst *def = find_def(fn, v, at);
    if (!def) return 0;
    switch (def->op) {
    case IR_CONST:
        if (def->imm >= 0 && def->imm <= 31) {
            cv_u8(out, (uint8_t)(CV_OP_lit0 + def->imm));
        } else {
            cv_u8(out, CV_OP_consts);
            cv_sleb(out, (int64_t)def->imm);
        }
        return 1;
    case IR_LOAD: {
        /* -O0 reload from a stack slot — trace what was stored there. */
        int slot = def->a;
        for (size_t i = at; i > 0; ) {
            i--;
            const IRInst *s = &fn->insts.data[i];
            if (s->op == IR_STORE && s->a == slot)
                return synth_call_value(fn, s->b, i, param_entry_dwarf,
                                        nparams, out, depth + 1);
        }
        return 0;
    }
    case IR_COPY:
    case IR_SEXT:
    case IR_ZEXT:
    case IR_TRUNC:
        return synth_call_value(fn, def->a, at, param_entry_dwarf, nparams,
                                out, depth + 1);
    case IR_NEG: {
        if (!synth_call_value(fn, def->a, at, param_entry_dwarf, nparams,
                              out, depth + 1))
            return 0;
        cv_u8(out, CV_OP_neg);
        return 1;
    }
    case IR_ADD:
    case IR_SUB: {
        if (!synth_call_value(fn, def->a, at, param_entry_dwarf, nparams,
                              out, depth + 1))
            return 0;
        if (!synth_call_value(fn, def->b, at, param_entry_dwarf, nparams,
                              out, depth + 1))
            return 0;
        cv_u8(out, def->op == IR_ADD ? CV_OP_plus : CV_OP_minus);
        return 1;
    }
    default:
        return 0;
    }
}

static DebugTypeTag ir_type_tag(int type_kind, int is_bool) {
    if (is_bool) return DBG_TY_BOOL;
    switch (type_kind) {
    case TY_VOID: return DBG_TY_VOID;
    case TY_INT: return DBG_TY_INT;
    case TY_FLOAT: return DBG_TY_FLOAT;
    case TY_PTR: return DBG_TY_PTR;
    case TY_ARRAY: return DBG_TY_ARRAY;
    case TY_STRUCT: return DBG_TY_STRUCT;
    default: return DBG_TY_INT;
    }
}

static void emit_epilogue(Buffer *b, int stack_size, const int cs_used[3], int has_dyn_alloca) {
    if (has_dyn_alloca) {
        int cs_count = (cs_used[0]?1:0) + (cs_used[1]?1:0) + (cs_used[2]?1:0);
        int cs_bytes = cs_count * 8;
        if (cs_bytes > 0) {
            /* lea -cs_bytes(%rbp), %rsp: 48 8D 65 (256-cs_bytes) */
            emit_byte(b, 0x48);
            emit_byte(b, 0x8D);
            emit_byte(b, 0x65);
            emit_byte(b, (uint8_t)(-cs_bytes));
        } else {
            /* mov %rbp, %rsp: 48 89 EC */
            emit_byte(b, 0x48);
            emit_byte(b, 0x89);
            emit_byte(b, 0xEC);
        }
    } else {
        emit_add_rsp_imm32(b, stack_size);
    }
    if (cs_used[2]) emit_pop_r(b, REG_R13);
    if (cs_used[1]) emit_pop_r(b, REG_R12);
    if (cs_used[0]) emit_pop_r(b, REG_RBX);
    emit_byte(b, 0x5D);   /* pop %rbp */
    emit_byte(b, 0xC3);   /* ret */
}

/* Def index of each SSA value, or -1. */
static int *codegen_build_defs(const IRFunction *fn) {
    int nv = fn->next_value_id;
    int *def = xmalloc((size_t)(nv > 0 ? nv : 1) * sizeof(int));
    for (int i = 0; i < nv; i++) def[i] = -1;
    for (size_t i = 0; i < fn->insts.len; i++) {
        int d = fn->insts.data[i].dst;
        if (d >= 0 && d < nv) def[d] = (int)i;
    }
    return def;
}

/* If `v` is &pinned_alloca or (&pinned_alloca)+const, write rbp offset and
 * return 1.  This is the addressing fold gcc -O0 does for stack slots.
 * Allocas with IR_ALLOCA.imm >= 32 are realigned at IR_ADDR, so a static
 * rbp offset would miss the runtime `and $-align`. */
static int alloca_dyn_align(const IRFunction *fn, const int *def, IRValue slot) {
    if (slot < 0 || slot >= fn->next_value_id || !def || def[slot] < 0) return 0;
    const IRInst *al = &fn->insts.data[def[slot]];
    return al->op == IR_ALLOCA && al->imm >= 32;
}

static int fold_ptr_off(const IRFunction *fn, const int *def, const int *alloca_off,
                        IRValue v, int *out_off, int depth) {
    if (depth > 8 || v < 0 || v >= fn->next_value_id || !def || !alloca_off) return 0;
    int di = def[v];
    if (di < 0) return 0;
    const IRInst *d = &fn->insts.data[di];
    if (d->op == IR_ADDR && d->a >= 0 && d->a < fn->next_value_id && alloca_off[d->a] != 0) {
        if (alloca_dyn_align(fn, def, d->a)) return 0;
        *out_off = alloca_off[d->a];
        return 1;
    }
    if (d->op == IR_ADD) {
        int base_off = 0;
        if (d->b >= 0 && d->b < fn->next_value_id && def[d->b] >= 0) {
            const IRInst *imm = &fn->insts.data[def[d->b]];
            if (imm->op == IR_CONST && !imm->is_float
                && fold_ptr_off(fn, def, alloca_off, d->a, &base_off, depth + 1)) {
                *out_off = base_off + (int)imm->imm;
                return 1;
            }
        }
        if (d->a >= 0 && d->a < fn->next_value_id && def[d->a] >= 0) {
            const IRInst *imm = &fn->insts.data[def[d->a]];
            if (imm->op == IR_CONST && !imm->is_float
                && fold_ptr_off(fn, def, alloca_off, d->b, &base_off, depth + 1)) {
                *out_off = base_off + (int)imm->imm;
                return 1;
            }
        }
    }
    return 0;
}

static int ssa_const_imm(const IRFunction *fn, const int *def, IRValue v, int64_t *imm) {
    if (!fn || !def || v < 0 || v >= fn->next_value_id || def[v] < 0) return 0;
    const IRInst *d = &fn->insts.data[def[v]];
    if (d->op != IR_CONST || d->is_float) return 0;
    *imm = d->imm;
    return 1;
}

static int imm_fits_i32(int64_t imm) {
    return imm == (int64_t)(int32_t)imm;
}

/* True if v is a non-address integer offset: peel COPY/SEXT/ZEXT, then
 * reject CONST (those GEPs stay as add+const) and address-producing ops. */
static int isel_ok_index(const IRFunction *fn, const int *def, IRValue v) {
    int depth = 0;
    while (v >= 0 && v < fn->next_value_id && def[v] >= 0 && depth++ < 8) {
        int op = fn->insts.data[def[v]].op;
        if (op == IR_COPY || op == IR_SEXT || op == IR_ZEXT) {
            v = fn->insts.data[def[v]].a;
            continue;
        }
        return op != IR_CONST && op != IR_ADDR && op != IR_GADDR
            && op != IR_GADDR_TLS && op != IR_FADDR;
    }
    return 0;
}

/* ADD of a pinned alloca address and an integer offset → [rbp+off+index]. */
static int fold_rbp_index(const IRFunction *fn, const int *def, const int *alloca_off,
                          IRValue v, int *off, IRValue *index) {
    if (v < 0 || v >= fn->next_value_id || def[v] < 0) return 0;
    const IRInst *d = &fn->insts.data[def[v]];
    if (d->op != IR_ADD) return 0;
    int64_t imm;
    int base_off;
    if (!ssa_const_imm(fn, def, d->b, &imm)
        && isel_ok_index(fn, def, d->b)
        && fold_ptr_off(fn, def, alloca_off, d->a, &base_off, 0)) {
        *off = base_off;
        *index = d->b;
        return 1;
    }
    if (!ssa_const_imm(fn, def, d->a, &imm)
        && isel_ok_index(fn, def, d->a)
        && fold_ptr_off(fn, def, alloca_off, d->b, &base_off, 0)) {
        *off = base_off;
        *index = d->a;
        return 1;
    }
    return 0;
}

/* SIB encodable scale: x86 supports *1,*2,*4,*8. */
static int sib_scale_log2(int64_t s) {
    if (s == 1) return 0;
    if (s == 2) return 1;
    if (s == 4) return 2;
    if (s == 8) return 3;
    return -1;
}

/* v's definition is MUL(idx, C) with C a power of two in {1,2,4,8} — the
 * scaled GEP stride emit_pointee_stride produces for p[i].  The MUL result
 * itself is returned as the index (used with SIB scale 1): it is live up to
 * the address ADD right next to the dereference, whereas the MUL's inner
 * operand's register may already have been recycled at that point. */
static int gep_scaled_index(const IRFunction *fn, const int *def, IRValue v,
                            IRValue *idx, int *scale2) {
    if (v < 0 || v >= fn->next_value_id || def[v] < 0) return 0;
    const IRInst *d = &fn->insts.data[def[v]];
    if (d->op != IR_MUL) return 0;
    int64_t ca, cb;
    if ((ssa_const_imm(fn, def, d->b, &cb) && sib_scale_log2(cb) >= 0)
        || (ssa_const_imm(fn, def, d->a, &ca) && sib_scale_log2(ca) >= 0)) {
        *scale2 = 0;
        *idx = v;
        return 1;
    }
    return 0;
}

/* Decompose the address feeding a LOAD_PTR/STORE_PTR into
 *     base + index * 2^scale2 + disp
 * where the index*scale term comes from a MUL(idx, 1|2|4|8) GEP stride.
 * The base is any pointer SSA (param, loaded pointer, GADDR, call result…);
 * const-only struct GEPs accumulate in disp.  A fully constant index folds
 * into disp and comes back with *idx_out == -1 (plain [base+disp]).
 *
 * Register liveness rule (the caller only guarantees the dereference sits
 * directly after v's ADD): a register reached by peeling constants is safe
 * to reference only while the peeled ADDs occupy CONSECUTIVE IR positions
 * back from v, so no intervening instruction can have recycled its home.
 * A pinned alloca address ([rbp+off]) is a frame value and stays safe
 * regardless of position. */
static int fold_gep(const IRFunction *fn, const int *def,
                    const int *alloca_off, IRValue v,
                    IRValue *base_out, IRValue *idx_out, int *scale2_out,
                    int *disp_out) {
    IRValue cur = v;
    int disp = 0;
    int expect = def[v];
    for (int depth = 0; depth < 8; depth++) {
        if (cur < 0 || cur >= fn->next_value_id || def[cur] < 0) return 0;
        int cp = def[cur];
        if (cp != expect) return 0; /* gap in the address chain */
        const IRInst *d = &fn->insts.data[cp];
        if (d->op != IR_ADD) return 0;
        IRValue raw_idx = -1, other = -1;
        int sc = -1;
        if (gep_scaled_index(fn, def, d->a, &raw_idx, &sc))
            other = d->b;
        else if (gep_scaled_index(fn, def, d->b, &raw_idx, &sc))
            other = d->a;
        if (raw_idx >= 0) {
            int bdisp = 0;
            IRValue base = other;
            int is_frame = 0;
            int boff = 0;
            if (base >= 0 && base < fn->next_value_id && def[base] >= 0
                && fn->insts.data[def[base]].op == IR_ADD) {
                /* Peel ADD(base, const) layers, demanding consecutive
                 * positions; a non-consecutive layer is usable only when it
                 * resolves to a pinned frame address. */
                int bexpect = cp - 1;
                for (;;) {
                    if (base < 0 || base >= fn->next_value_id
                        || def[base] < 0)
                        return 0;
                    int bp = def[base];
                    const IRInst *bd = &fn->insts.data[bp];
                    if (bd->op != IR_ADD) break;
                    if (bp != bexpect) {
                        if (fold_ptr_off(fn, def, alloca_off, base,
                                         &boff, 0)) {
                            is_frame = 1;
                            break;
                        }
                        return 0;
                    }
                    int64_t bc;
                    if (ssa_const_imm(fn, def, bd->b, &bc)) {
                        base = bd->a;
                    } else if (ssa_const_imm(fn, def, bd->a, &bc)) {
                        base = bd->b;
                    } else {
                        break; /* non-const ADD: usable register base */
                    }
                    bdisp += (int)bc;
                    bexpect = bp - 1;
                }
            }
            if (!is_frame) {
                int off = 0;
                if (base < 0 || base >= fn->next_value_id || def[base] < 0)
                    return 0;
                if (fold_ptr_off(fn, def, alloca_off, base, &off, 0)) {
                    is_frame = 1;
                    boff = off;
                }
            }
            int64_t total = (int64_t)disp + bdisp + boff;
            IRValue idx = raw_idx;
            int64_t ci;
            if (ssa_const_imm(fn, def, raw_idx, &ci)) {
                total += ci << sc;
                idx = -1;
            }
            if (!imm_fits_i32(total)) return 0;
            /* Register bases must be real pointer values, not constants or
             * other scaled indices (plain integer arithmetic stays
             * unfolded). */
            if (!is_frame) {
                int bop = fn->insts.data[def[base]].op;
                if (bop == IR_CONST || bop == IR_MUL) return 0;
            }
            *base_out = is_frame ? -1 : base;
            *idx_out = idx;
            *scale2_out = sc;
            *disp_out = (int)total;
            return 1;
        }
        int64_t c;
        IRValue inner;
        if (ssa_const_imm(fn, def, d->b, &c))
            inner = d->a;
        else if (ssa_const_imm(fn, def, d->a, &c))
            inner = d->b;
        else
            return 0;
        int off = 0;
        if (inner >= 0 && inner < fn->next_value_id
            && fold_ptr_off(fn, def, alloca_off, inner, &off, 0)) {
            /* Pinned frame base reached through one last const ADD: emit
             * plain [rbp+off+disp+c] with no index. */
            int64_t total = (int64_t)disp + c + off;
            if (!imm_fits_i32(total)) return 0;
            *base_out = -1;
            *idx_out = -1;
            *scale2_out = 0;
            *disp_out = (int)total;
            return 1;
        }
        if (inner < 0 || inner >= fn->next_value_id || def[inner] < 0)
            return 0;
        if (def[inner] != cp - 1) return 0; /* gap: register may be recycled */
        disp += (int)c;
        cur = inner;
        expect = cp - 1;
    }
    return 0;
}

enum { AF_NONE = 0, AF_RBP_IDX = 1, AF_SIB = 2 };

/* Classify a dereferenced address for -O0 addressing-mode folding.
 *   AF_RBP_IDX: [rbp + idx*2^scale2 + disp]  (idx may be -1)
 *   AF_SIB:     [base + idx*2^scale2 + disp] (idx may be -1)
 * Only called after the plain [rbp+disp] fold_ptr_off case failed and for
 * ordinary 1/2/4/8-byte GP accesses. */
static int classify_ptr_addr(const IRFunction *fn, const int *def,
                             const int *alloca_off, IRValue v,
                             IRValue *base_out, IRValue *idx_out,
                             int *scale2_out, int *disp_out) {
    IRValue base = -1, idx = -1;
    int sc = 0, disp = 0;
    if (fold_gep(fn, def, alloca_off, v, &base, &idx, &sc, &disp)) {
        if (base < 0) {
            *base_out = -1;
            *idx_out = idx;
            *scale2_out = sc;
            *disp_out = disp;
            return AF_RBP_IDX;
        }
        *base_out = base;
        *idx_out = idx;
        *scale2_out = sc;
        *disp_out = disp;
        return AF_SIB;
    }
    int off = 0;
    IRValue iv = -1;
    if (fold_rbp_index(fn, def, alloca_off, v, &off, &iv)) {
        *base_out = -1;
        *idx_out = iv;
        *scale2_out = 0;
        *disp_out = off;
        return AF_RBP_IDX;
    }
    return AF_NONE;
}

/* Operand of inst can be encoded as an immediate (gcc -O0 does this). */
static int alu_folds_imm(const IRInst *inst, int which, const IRFunction *fn,
                         const int *def) {
    IRValue v = which ? inst->b : inst->a;
    int64_t imm;
    if (!ssa_const_imm(fn, def, v, &imm) || !imm_fits_i32(imm)) return 0;
    switch (inst->op) {
    case IR_ADD:
    case IR_BAND:
    case IR_BOR:
    case IR_BXOR:
    case IR_MUL:
        return 1;
    case IR_SUB:
        return which == 1;
    case IR_SHL:
    case IR_SHR:
    case IR_ROL:
        return which == 1 && imm >= 0 && imm < 64;
    case IR_EQ:
    case IR_NE:
    case IR_LT:
    case IR_LE:
    case IR_GT:
    case IR_GE:
        return which == 1;
    default:
        return 0;
    }
}

static void mark_reg(char *needed, int *uses, int nv, IRValue v) {
    if (v >= 0 && v < nv) {
        needed[v] = 1;
        uses[v]++;
    }
}

static void mark_inst_uses_needed(const IRInst *inst, char *needed,
                                int *uses, int nv) {
    if (inst->op == IR_LABEL || inst->op == IR_BR || inst->op == IR_DBG_VALUE) return;
    if (inst->op == IR_ADDR) return; /* alloca id in `a` is not an SSA value use */
    mark_reg(needed, uses, nv, inst->a);
    if (inst->op != IR_CBR && inst->op != IR_CALL)
        mark_reg(needed, uses, nv, inst->b);
    if (inst->op == IR_CALL) {
        if (inst->call_callee >= 0) mark_reg(needed, uses, nv, inst->call_callee);
        for (int k = 0; k < inst->call_nargs; k++)
            mark_reg(needed, uses, nv, inst->call_args[k]);
    }
}

/* Mark the SSA operands one instruction actually requires at -O0, taking
 * addressing-mode folds into account (a folded dereference needs base/index,
 * not the intermediate address/Gep value). */
static void codegen_mark_inst_roots(char *needed, int *uses,
                                    const int *raw_uses, int nv,
                                    const IRFunction *fn,
                                    const int *def, const int *alloca_off,
                                    const IRInst *inst, size_t ii) {
    int dummy;
    if (inst->op == IR_LOAD_PTR) {
            IRValue ab = -1, ai = -1;
            int asc = 0, adisp = 0;
            if (fold_ptr_off(fn, def, alloca_off, inst->a, &dummy, 0))
                return;
            /* Fold a fresh GEP only when the address ADD is immediately
             * before the dereference AND the address value is not used
             * anywhere else: a dereference folded into [base+index*s]
             * relies on base/index registers surviving the materialized
             * ADD; if another instruction (e.g. the matching STORE_PTR
             * of a pre-increment) also uses the address, the ADD is
             * emitted and coalesces with its base register, so folding
             * would read [addr+idx] twice. */
            int adjacent = inst->a >= 0 && inst->a < fn->next_value_id
                           && def[inst->a] >= 0 && (size_t)def[inst->a] + 1 == ii;
            int sole_use = inst->a < 0 || raw_uses[inst->a] <= 1;
            if (!value_is_float_class(fn, inst->dst) && !value_is_ld(fn, inst->dst)
                && (inst->width == 1 || inst->width == 2 || inst->width == 4
                    || inst->width == 8)
                && adjacent && sole_use
                && classify_ptr_addr(fn, def, alloca_off, inst->a,
                                     &ab, &ai, &asc, &adisp) != AF_NONE) {
            if (ab >= 0) mark_reg(needed, uses, nv, ab);
            if (ai >= 0) mark_reg(needed, uses, nv, ai);
            return;
        }
        mark_reg(needed, uses, nv, inst->a);
        return;
    }
    if (inst->op == IR_STORE_PTR) {
            IRValue ab = -1, ai = -1;
            int asc = 0, adisp = 0;
            if (fold_ptr_off(fn, def, alloca_off, inst->a, &dummy, 0)) {
                mark_reg(needed, uses, nv, inst->b);
                return;
            }
            int adjacent = inst->a >= 0 && inst->a < fn->next_value_id
                           && def[inst->a] >= 0 && (size_t)def[inst->a] + 1 == ii;
            int sole_use = inst->a < 0 || raw_uses[inst->a] <= 1;
            if (!value_is_float_class(fn, inst->b) && !value_is_ld(fn, inst->b)
                && (inst->width == 1 || inst->width == 2 || inst->width == 4
                    || inst->width == 8)
                && adjacent && sole_use
                && classify_ptr_addr(fn, def, alloca_off, inst->a,
                                     &ab, &ai, &asc, &adisp) != AF_NONE) {
            if (ab >= 0) mark_reg(needed, uses, nv, ab);
            if (ai >= 0) mark_reg(needed, uses, nv, ai);
            mark_reg(needed, uses, nv, inst->b);
            return;
        }
        mark_reg(needed, uses, nv, inst->a);
        mark_reg(needed, uses, nv, inst->b);
        return;
    }
    if (alu_folds_imm(inst, 1, fn, def)) {
        mark_reg(needed, uses, nv, inst->a);
        return;
    }
    if (alu_folds_imm(inst, 0, fn, def)) {
        mark_reg(needed, uses, nv, inst->b);
        return;
    }
    mark_inst_uses_needed(inst, needed, uses, nv);
}

/* Values that must materialize in a register (escaped pointers, arithmetic
 * operands, call args).  Every instruction marks the operands it actually
 * requires; LOAD_PTR/STORE_PTR mark base/index instead of the intermediate
 * address SSA when the dereference folds into [base+index*scale+disp].
 *
 * Marking is a plain forward sweep over all instructions rather than a
 * use-def closure: post-mem2reg IR is not strictly SSA — a value id can be
 * defined more than once (loop variables), so def[id] only names the last
 * definition and a closure would drop the operands of the earlier ones. */
static void codegen_mark_needed_and_uses(const IRFunction *fn, const int *def,
                                 const int *alloca_off, const char *skip_body,
                                 char *needed, int *ssa_uses,
                                 const int *raw_uses) {
    int nv = fn->next_value_id > 0 ? fn->next_value_id : 1;
    for (size_t i = 0; i < fn->insts.len; i++) {
        if (skip_body && skip_body[i]) continue;
        codegen_mark_inst_roots(needed, ssa_uses, raw_uses, nv, fn, def,
                                alloca_off, &fn->insts.data[i], i);
    }
}

void codegen(const IRModule *ir, EmitModule *out, int want_debug) {
    /* Backend dispatch point: everything below this function is the
     * x86-64 instruction selector; the arm64 selector is codegen64(). */
    if (target_current()->arch == TARGET_ARCH_ARM64) {
        codegen64(ir, out, want_debug);
        return;
    }
    /* Emit globals into the appropriate section (.rodata for read-only,
     * .data for initialized mutable, .bss for zero-initialized) and register
     * each as a defined symbol with its linkage binding. */
    size_t *global_off = NULL;
    uint16_t *global_shndx = NULL;
    if (ir->globals.len > 0) {
        global_off = malloc(ir->globals.len * sizeof(size_t));
        global_shndx = malloc(ir->globals.len * sizeof(uint16_t));
        if (!global_off || !global_shndx) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
    }
    for (size_t gi = 0; gi < ir->globals.len; gi++) {
        const IRGlobal *g = &ir->globals.data[gi];
        uint8_t binding = g->is_static ? 0 /* STB_LOCAL */ : 1 /* STB_GLOBAL */;
        uint16_t shndx;
        size_t off;
        size_t al = g->align > 0 ? (size_t)g->align : 8;
        if (al < 1) al = 1;
        if (g->is_tls) {
            /* __thread globals live in .tdata (initialized) or .tbss
             * (zero-init).  The linker assembles them into the TLS
             * template referenced by PT_TLS, and accesses resolve
             * via %fs:[TPOFF64]. */
            if (g->init_bytes) {
                shndx = SECT_TDATA;
                while (out->tdata.len % al) {
                    char z = 0; buffer_append(&out->tdata, &z, 1);
                }
                off = out->tdata.len;
                buffer_append(&out->tdata, g->init_bytes, g->size);
                if (al > out->tdata_align) out->tdata_align = al;
            } else {
                shndx = SECT_TBSS;
                while (out->tbss_size % al) out->tbss_size++;
                off = out->tbss_size;
                out->tbss_size += g->size;
                if (al > out->tbss_align) out->tbss_align = al;
            }
        } else if (g->is_readonly) {
            shndx = SECT_RODATA;
            while (out->rodata.len % al) {
                char z = 0; buffer_append(&out->rodata, &z, 1);
            }
            off = out->rodata.len;
            buffer_append(&out->rodata, g->init_bytes, g->size);
            if (al > out->rodata_align) out->rodata_align = al;
        } else if (g->init_bytes) {
            shndx = SECT_DATA;
            while (out->data.len % al) {
                char z = 0; buffer_append(&out->data, &z, 1);
            }
            off = out->data.len;
            buffer_append(&out->data, g->init_bytes, g->size);
            if (al > out->data_align) out->data_align = al;
        } else {
            shndx = SECT_BSS;
            while (out->bss_size % al) out->bss_size++;
            off = out->bss_size;
            out->bss_size += g->size;
            if (al > out->bss_align) out->bss_align = al;
        }
        uint8_t st_type = g->is_tls ? 6 /* STT_TLS */ : 1 /* STT_OBJECT */;
        emit_module_add_symbol(out, g->name, binding, st_type,
                               shndx, off, g->size);
        global_off[gi] = off;
        global_shndx[gi] = shndx;
        if (want_debug && !g->is_readonly) {
            /* Skip anonymous string literals (readonly); emit mutable globals. */
            DebugVar gv;
            memset(&gv, 0, sizeof(gv));
            gv.name = g->name;
            gv.file = (char *)g->loc.file;
            gv.line = g->loc.line;
            gv.kind = DBG_VAR_GLOBAL;
            gv.type_tag = DBG_TY_INT;
            gv.width = g->size;
            gv.is_unsigned = 0;
            gv.loc_kind = DBG_LOC_ADDR;
            gv.sym_name = g->name;
            gv.alloca_ssa = -1;
            gv.param_idx = -1;
            emit_module_add_dbg_global(out, &gv);
        }
    }

    /* Cross-function call patches (accumulated across every function). */
    CallPatch *call_patches = NULL;
    size_t num_call_patches = 0, cap_call_patches = 0;
    /* Function-address load patches (`&func` / function lvalue). */
    FnAddrPatch *fnaddr_patches = NULL;
    size_t num_fnaddr_patches = 0, cap_fnaddr_patches = 0;

    for (size_t i = 0; i < ir->functions.len; i++) {
        const IRFunction *fn = &ir->functions.data[i];
        const RAResult *ra = (const RAResult *)fn->ra;
        const RAResult *ra_xmm = (const RAResult *)fn->ra_xmm;
        curr_fn = fn;
        /* XMM spill stride follows -mavx512f (ZMM) vs default YMM. */
        curr_xmm_spill = host_has_avx512f() ? 64 : 32;
        size_t start_offset = out->text.len;
        int dbg_func_idx = -1;
        size_t prologue_end_pc = start_offset;
        DbgObs *dbg_obs = NULL;
        size_t num_dbg_obs = 0, cap_dbg_obs = 0;
        if (want_debug) {
            dbg_func_idx = emit_module_add_dbg_func(out, fn->name, fn->loc.file,
                                                    fn->loc.line, start_offset);
            /* A row for the function's own line, so the entry address maps to
             * a source location.  Without it a debugger that stops inside the
             * prologue reports a bare address and cannot show the frame. */
            if (fn->loc.file && fn->loc.line > 0)
                emit_module_add_dbg_line(out, fn->loc.file, fn->loc.line,
                                         fn->loc.col, start_offset);
        }

        /* ---- Prologue ---- */
        int cs_used[3];
        collect_callee_saved(ra, cs_used);
        int cs_count = cs_used[0] + cs_used[1] + cs_used[2];
        curr_cs_count = cs_count;
        int cs_save_area = 8 * cs_count;  /* bytes occupied by callee-saved reg pushes */

        /* Compute pinned-alloca frame area: byte-offset from rbp for each
         * IR_ALLOCA with alloca_bytes > 0.  Offsets are placed above the
         * spill areas (further from rbp, i.e. more negative).  The frame
         * layout below rbp is: [cs saves][GP spills][XMM spills][pinned allocas]. */
        int *alloca_off = xmalloc(fn->next_value_id * sizeof(int));
        for (int i = 0; i < fn->next_value_id; i++) alloca_off[i] = 0;
        int gp_spill_area = ra ? ra->stack_size : 0;
        int xmm_spill_area = ra_xmm ? ra_xmm->stack_size : 0;
        int pinned_area = 0;
        for (size_t j = 0; j < fn->insts.len; j++) {
            const IRInst *inst = &fn->insts.data[j];
            if (inst->op == IR_ALLOCA && inst->alloca_bytes > 0 && inst->dst >= 0) {
                int bytes = inst->alloca_bytes;
                if (bytes % 8 != 0) bytes += 8 - (bytes % 8);
                int aln = (inst->imm >= 16) ? (int)inst->imm : 8;
                if (aln < 8) aln = 8;
                pinned_area += bytes;
                int abs_off = cs_save_area + gp_spill_area + xmm_spill_area + pinned_area;
                if (aln > 1 && (abs_off % aln) != 0)
                    abs_off += aln - (abs_off % aln);
                pinned_area = abs_off - (cs_save_area + gp_spill_area + xmm_spill_area);
                alloca_off[inst->dst] = -abs_off;
            }
        }

        /* Long-double (width 16) slot assignment.  ld values never live in
         * registers — each gets a fixed 16-byte slot addressed rbp-relative
         * via ld_off[v].  Locals reuse their pinned-alloca offset; operation
         * results and constants are packed into a dedicated ld area below the
         * pinned area.  ld params are handled in the param-binding loop
         * (positive stack offset).  Unassigned ld values keep ld_off = 0 and
         * are treated as slot 0 (only valid once every ld value is assigned). */
        int *ld_off = xmalloc(fn->next_value_id * sizeof(int));
        for (int i = 0; i < fn->next_value_id; i++) ld_off[i] = 0;
        int ld_area = 0;
        for (size_t j = 0; j < fn->insts.len; j++) {
            const IRInst *inst = &fn->insts.data[j];
            if (!value_is_ld(fn, inst->dst)) continue;
            if (inst->op == IR_ALLOCA && inst->alloca_bytes > 0) {
                /* ld local: reuse its pinned-alloca slot (16-byte aligned). */
                ld_off[inst->dst] = alloca_off[inst->dst];
            } else if (inst->op != IR_PARAM) {
                /* ld operation result or constant: fresh 16-byte slot. */
                ld_area += 16;
                ld_off[inst->dst] = -(cs_save_area + gp_spill_area + xmm_spill_area + pinned_area + ld_area);
            }
        }

        int stack_size = gp_spill_area + xmm_spill_area + pinned_area + ld_area;
        if (!ra && stack_size == 0) stack_size = 8 * fn->next_value_id;
        if (stack_size % 16 != 0) stack_size += 16 - (stack_size % 16);

        /* Variadic save area stays at the bottom of the frame (rsp after
         * `sub $stack_size`) so va_start can recover it as [rbp - frame_down]
         * even if a later VLA moves rsp.  apply_args/result follow it. */
        int apply_args_rsp_off = -1, apply_result_rsp_off = -1, apply_saved_sp_rsp_off = -1;
        int aligned_call_saved_sp_rsp_off = -1;
        int bottom_extra = 0;
        if (fn->is_variadic) bottom_extra = 176;
        if (fn->needs_apply_args) {
            apply_args_rsp_off = bottom_extra;
            bottom_extra += APPLY_ARGS_SIZE;
        }
        if (fn->needs_apply) {
            apply_result_rsp_off = bottom_extra;
            bottom_extra += APPLY_RESULT_SIZE;
            apply_saved_sp_rsp_off = bottom_extra;
            bottom_extra += 8;
        }
        /* aligned_call used to lea rsp from the fixed frame, which lands on
         * a live VLA.  Keep a rbp-relative copy of the current rsp so we
         * can sub/and from it and restore after the call. */
        if (fn->has_dyn_alloca) {
            aligned_call_saved_sp_rsp_off = bottom_extra;
            bottom_extra += 8;
        }
        stack_size += bottom_extra;
        if (stack_size % 16 != 0) stack_size += 16 - (stack_size % 16);

        /* Alignment invariant: on entry rsp % 16 == 8 (call pushed ret).
         * After "push rbp" rsp % 16 == 0.  Each callee-saved push adds 8;
         * if odd count, we need one extra bump so that rsp (the save-area base)
         * ends up 16-aligned for the movaps stores. */
        curr_cs_count = cs_count;
        if (((cs_count) & 1) != 0) stack_size += 8;

        int frame_down = cs_save_area + stack_size;
        int apply_args_rbp_off = (apply_args_rsp_off >= 0)
            ? -(frame_down - apply_args_rsp_off) : 0;
        int apply_result_rbp_off = (apply_result_rsp_off >= 0)
            ? -(frame_down - apply_result_rsp_off) : 0;
        int apply_saved_sp_rbp_off = (apply_saved_sp_rsp_off >= 0)
            ? -(frame_down - apply_saved_sp_rsp_off) : 0;
        int aligned_call_saved_sp_rbp_off = (aligned_call_saved_sp_rsp_off >= 0)
            ? -(frame_down - aligned_call_saved_sp_rsp_off) : 0;

        emit_byte(&out->text, 0x55);              /* pushq %rbp */
        size_t after_push_rbp_pc = out->text.len;
        emit_rex_w(&out->text);
        emit_byte(&out->text, 0x89);
        emit_byte(&out->text, 0xE5);              /* movq %rsp, %rbp */
        size_t after_mov_rbp_pc = out->text.len;
        if (want_debug && dbg_func_idx >= 0)
            emit_module_dbg_func_frame(out, dbg_func_idx, after_push_rbp_pc,
                                       after_mov_rbp_pc);

        if (cs_used[0]) emit_push_r(&out->text, REG_RBX);
        if (cs_used[1]) emit_push_r(&out->text, REG_R12);
        if (cs_used[2]) emit_push_r(&out->text, REG_R13);

        /* Materialize incoming SysV arg registers into their allocated
         * homes.  SysV AMD64 passes the first 6 INTEGER-class args in
         * RDI,RSI,RDX,RCX,R8,R9 and the first 8 SSE-class (float/double)
         * args in XMM0-XMM7, with the two counters independent.  Args that
         * don't fit in their class's registers are passed on the stack at
         * [rbp + 16 + 8*k].
         *
         * We first decide, per param, how it arrives (GP reg / XMM reg /
         * stack).  Then a push-then-pop dance (pushing every used arg
         * register, GP via `push`, XMM via `sub rsp,8; movsd [rsp],xmm`)
         * in reverse param order guarantees no clobber can lose data even
         * when a param's home is another param's source register. */
        /* Per-param arrival info.  arrive_reg = native register code, or -1
         * for stack-passed.  arrive_is_xmm tells which file arrive_reg is
         * in.  stack_off is the [rbp+..] offset for stack-passed args.
         * PARAMs are contiguous at function start (see ir.c lowering). */
        int nparams = 0;
        for (size_t j = 0; j < fn->insts.len; j++) {
            if (fn->insts.data[j].op == IR_PARAM) nparams++;
            else break;
        }
        /* Parser caps params at MAX_PARAMS (common.h). */
        if (nparams > MAX_PARAMS) nparams = MAX_PARAMS;
        int *arrive_reg = NULL;
        int *arrive_is_xmm = NULL;
        int *stack_off = NULL;
        if (nparams > 0) {
            arrive_reg = malloc((size_t)nparams * sizeof(int));
            arrive_is_xmm = malloc((size_t)nparams * sizeof(int));
            stack_off = malloc((size_t)nparams * sizeof(int));
            if (!arrive_reg || !arrive_is_xmm || !stack_off) {
                fprintf(stderr, "fakecc: OOM\n");
                exit(1);
            }
        }
        int gp_reg_idx = 0, xmm_reg_idx = 0, stack_arg_idx = 0;
        for (int p = 0; p < nparams; p++) {
            const IRInst *pi = &fn->insts.data[p];
            int is_ld = (pi->dst >= 0 && pi->dst < fn->next_value_id &&
                         value_is_ld(fn, pi->dst));
            int is_float = !is_ld && (pi->dst >= 0 && pi->dst < fn->next_value_id &&
                            value_is_float_class(fn, pi->dst));
            int force_stack = pi->force_stack || is_ld;
            int vecb = is_float ? value_vec_bytes(fn, pi->dst) : 0;
            if (!vecb && is_float && (pi->width == 16 || pi->width == 32 || pi->width == 64))
                vecb = pi->width;
            int pal = flags_stack_align(0, vecb, pi->align16);
            if (is_ld && pal < 16) pal = 16;
            int nslots = vec_nslots(vecb, is_ld);
            int blob = param_is_mem_blob(pi);
            if (blob) {
                nslots = (pi->alloca_bytes + 7) / 8;
                if (nslots < 1) nslots = 1;
            }
            if (force_stack) {
                /* long double or MEMORY-class eightbyte: stack only. */
                stack_idx_align(&stack_arg_idx, pal);
                arrive_reg[p] = -1;
                arrive_is_xmm[p] = 0;
                stack_off[p] = 16 + 8 * stack_arg_idx;
                stack_arg_idx += nslots;
                if (is_ld)
                    ld_off[pi->dst] = stack_off[p];
            } else if (is_float) {
                if (xmm_reg_idx < 8) {
                    arrive_reg[p] = xmm_reg_idx; /* native XMM code 0..7 */
                    arrive_is_xmm[p] = 1;
                    xmm_reg_idx++;
                } else {
                    arrive_reg[p] = -1;
                    arrive_is_xmm[p] = 0;
                    stack_idx_align(&stack_arg_idx, pal);
                    stack_off[p] = 16 + 8 * stack_arg_idx;
                    stack_arg_idx += nslots;
                }
            } else {
                if (gp_reg_idx < 6) {
                    arrive_reg[p] = SYSV_ARG_REGS[gp_reg_idx++];
                    arrive_is_xmm[p] = 0;
                } else {
                    arrive_reg[p] = -1;
                    arrive_is_xmm[p] = 0;
                    stack_off[p] = 16 + 8 * stack_arg_idx++;
                }
            }
        }

        int *ssa_def = codegen_build_defs(fn);
        char *skip_body = xmalloc(fn->insts.len ? fn->insts.len : 1);
        memset(skip_body, 0, fn->insts.len ? fn->insts.len : 1);
        int *param_home_off = NULL;
        if (nparams > 0) {
            param_home_off = xmalloc((size_t)nparams * sizeof(int));
            memset(param_home_off, 0, (size_t)nparams * sizeof(int));
        }
        /* Scalar -O0: PARAM is stored once into a same-sized pinned alloca.
         * Write the incoming register into that slot in the prologue and skip
         * the later ADDR+STORE_PTR.  Reject ADDR+const / wider allocas so
         * __int128 and struct eightbytes keep their multi-store sequence. */
        for (int p = 0; p < nparams; p++) {
            const IRInst *pi = &fn->insts.data[p];
            IRValue v = pi->dst;
            if (v < 0 || value_is_ld(fn, v)) continue;
            if (param_is_mem_blob(pi)) continue;
            int store_i = -1, nstore = 0, other = 0;
            for (size_t j = 0; j < fn->insts.len; j++) {
                const IRInst *in = &fn->insts.data[j];
                if (in->op == IR_DBG_VALUE || in->op == IR_PARAM ||
                    in->op == IR_LABEL || in->op == IR_BR)
                    continue;
                if (in->op == IR_STORE_PTR && in->b == v) {
                    nstore++;
                    store_i = (int)j;
                    continue;
                }
                if (in->a == v) { other = 1; break; }
                if (in->op != IR_CBR && in->op != IR_CALL && in->b == v) {
                    other = 1;
                    break;
                }
                if (in->op == IR_CALL) {
                    if (in->call_callee == v) { other = 1; break; }
                    for (int k = 0; k < in->call_nargs; k++) {
                        if (in->call_args[k] == v) { other = 1; break; }
                    }
                    if (other) break;
                }
            }
            if (other || nstore != 1 || store_i < 0) continue;
            const IRInst *st = &fn->insts.data[store_i];
            IRValue ptr = st->a;
            if (ptr < 0 || ptr >= fn->next_value_id || ssa_def[ptr] < 0) continue;
            const IRInst *ad = &fn->insts.data[ssa_def[ptr]];
            if (ad->op != IR_ADDR) continue;
            int slot = ad->a;
            if (slot < 0 || slot >= fn->next_value_id || ssa_def[slot] < 0) continue;
            const IRInst *al = &fn->insts.data[ssa_def[slot]];
            if (al->op != IR_ALLOCA) continue;
            int store_w = st->width ? st->width : 4;
            int param_w = pi->width ? pi->width : 4;
            if (al->alloca_bytes != store_w || store_w != param_w) continue;
            int off = alloca_off[slot];
            if (off == 0) continue;
            param_home_off[p] = off;
            skip_body[store_i] = 1;
        }
        int nvals_f = fn->next_value_id > 0 ? fn->next_value_id : 1;
        int *ssa_uses_f = xmalloc((size_t)nvals_f * sizeof(int));
        memset(ssa_uses_f, 0, (size_t)nvals_f * sizeof(int));
        char *needed = xmalloc((size_t)nvals_f);
        memset(needed, 0, (size_t)nvals_f);
        /* Raw-IR operand use counts (no addressing folds): gates whether a
         * materialized address can be re-folded into [base+index] at the
         * only adjacent dereference. */
        int *raw_uses = xmalloc((size_t)nvals_f * sizeof(int));
        memset(raw_uses, 0, (size_t)nvals_f * sizeof(int));
        for (size_t uj = 0; uj < fn->insts.len; uj++) {
            if (skip_body[uj]) continue;
            const IRInst *u = &fn->insts.data[uj];
            if (u->op == IR_LABEL || u->op == IR_BR || u->op == IR_DBG_VALUE)
                continue;
            if (u->op != IR_ADDR && u->a >= 0)
                raw_uses[u->a]++;
            if (u->op != IR_CBR && u->op != IR_CALL && u->b >= 0)
                raw_uses[u->b]++;
            if (u->op == IR_CALL) {
                if (u->call_callee >= 0) raw_uses[u->call_callee]++;
                for (int uk = 0; uk < u->call_nargs; uk++)
                    if (u->call_args[uk] >= 0) raw_uses[u->call_args[uk]]++;
            }
        }
        /* Single root-marking pass: fills both the "must have a register"
         * set and last-use counts, taking addressing-mode folds into account
         * (a folded dereference reuses base/index SSA roots, not the
         * intermediate address value). */
        codegen_mark_needed_and_uses(fn, ssa_def, alloca_off, skip_body,
                                    needed, ssa_uses_f, raw_uses);

        /* ---- Fused integer-compare → conditional branch ----
         * gcc -O0 emits `cmp; j<cc>` for `if (a < b)`.  Without fusion the
         * comparison is materialized as 0/1 (xor/setcc/mov/test) and the
         * CBR tests that value.  When the compare feeds ONLY the adjacent
         * branch (optionally through one ==0 / !=0 wrapper, i.e. `!(a<b)`),
         * skip both value-materializing instructions and branch on flags. */
        size_t ninst_f = fn->insts.len ? fn->insts.len : 1;
        char *fused_skip = xmalloc(ninst_f);
        int *fused_cmp_at = xmalloc(ninst_f * sizeof(int));
        char *fused_inv_at = xmalloc(ninst_f);
        memset(fused_skip, 0, ninst_f);
        for (size_t uj = 0; uj < ninst_f; uj++) fused_cmp_at[uj] = -1;
        memset(fused_inv_at, 0, ninst_f);
        for (size_t uj = 0; uj < fn->insts.len; uj++) {
            const IRInst *cb = &fn->insts.data[uj];
            if (cb->op != IR_CBR || skip_body[uj]) continue;
            if (cb->a < 0 || cb->a >= fn->next_value_id || ssa_def[cb->a] < 0)
                continue;
            int wi = ssa_def[cb->a];
            const IRInst *w = &fn->insts.data[wi];
            int is_int_cmp = (w->op == IR_EQ || w->op == IR_NE || w->op == IR_LT
                              || w->op == IR_LE || w->op == IR_GT || w->op == IR_GE);
            int ci = -1, invert = 0;
            if (is_int_cmp && (size_t)wi + 1 == uj && ssa_uses_f[w->dst] == 1) {
                ci = wi;
            } else if ((w->op == IR_EQ || w->op == IR_NE)
                       && (size_t)wi + 1 == uj && ssa_uses_f[w->dst] == 1
                       && w->a >= 0 && w->a < fn->next_value_id
                       && ssa_def[w->a] >= 0) {
                int64_t wz;
                int pj = ssa_def[w->a];
                const IRInst *p = &fn->insts.data[pj];
                int p_cmp = (p->op == IR_EQ || p->op == IR_NE || p->op == IR_LT
                             || p->op == IR_LE || p->op == IR_GT || p->op == IR_GE);
                if (p_cmp && (size_t)wi == (size_t)pj + 1
                    && ssa_uses_f[p->dst] == 1
                    && ssa_const_imm(fn, ssa_def, w->b, &wz) && wz == 0) {
                    ci = pj;
                    invert = (w->op == IR_EQ);
                }
            }
            if (ci < 0) continue;
            const IRInst *cmp = &fn->insts.data[ci];
            if (skip_body[ci]) continue;
            fused_cmp_at[uj] = ci;
            fused_inv_at[uj] = (char)invert;
            fused_skip[ci] = 1;
            if (wi != ci) fused_skip[wi] = 1;
            mark_reg(needed, ssa_uses_f, nvals_f, cmp->a);
            int64_t cimm;
            if (!(ssa_const_imm(fn, ssa_def, cmp->b, &cimm) && imm_fits_i32(cimm)))
                mark_reg(needed, ssa_uses_f, nvals_f, cmp->b);
        }

        /* Variadic: compute the initial va_list field values.  The named args
         * consume the first gp_reg_idx GP and xmm_reg_idx FP register slots, so
         * the first variadic arg begins at those offsets in the save area.  The
         * overflow (stack) area starts after the named stack-passed args. */
        int va_gp_offset = 0, va_fp_offset = 0, va_overflow_off = 16;
        if (fn->is_variadic) {
            va_gp_offset = 8 * gp_reg_idx;
            va_fp_offset = 48 + 16 * xmm_reg_idx;
            va_overflow_off = 16 + 8 * stack_arg_idx;
        }

        /* Allocate the frame.  For variadic functions this also reserves the
         * 176-byte register-save area at the bottom. */
        emit_sub_rsp_imm32(&out->text, stack_size); /* sub $N, %rsp */

        /* Variadic: save the incoming argument registers into the save area at
         * the bottom of the frame BEFORE the param dance clobbers them.  The
         * dance (below) pushes/pops arg regs to move them into their homes,
         * which would destroy the values we need for the save area.  rsp is
         * constant from here on, so [rsp+off] addresses the save area stably. */
        if (fn->is_variadic) {
            /* GP regs rdi,rsi,rdx,rcx,r8,r9 → [rsp+0..+40]. */
            emit_store_rsp_off(&out->text, REG_RDI, 0);
            emit_store_rsp_off(&out->text, REG_RSI, 8);
            emit_store_rsp_off(&out->text, REG_RDX, 16);
            emit_store_rsp_off(&out->text, REG_RCX, 24);
            emit_store_rsp_off(&out->text, REG_R8, 32);
            emit_store_rsp_off(&out->text, REG_R9, 40);
            /* FP regs xmm0-xmm7 → [rsp+48..+176] via movaps (16-byte slots). */
            emit_sse_store_rsp_off(&out->text, 0, 48);
            emit_sse_store_rsp_off(&out->text, 1, 64);
            emit_sse_store_rsp_off(&out->text, 2, 80);
            emit_sse_store_rsp_off(&out->text, 3, 96);
            emit_sse_store_rsp_off(&out->text, 4, 112);
            emit_sse_store_rsp_off(&out->text, 5, 128);
            emit_sse_store_rsp_off(&out->text, 6, 144);
            emit_sse_store_rsp_off(&out->text, 7, 160);
        }
        if (fn->needs_apply_args)
            emit_fill_apply_args(&out->text, apply_args_rsp_off);

        /* Incoming scalar arg → pinned slot while SysV regs still hold
         * their original values.  Stores do not clobber other arg regs. */
        for (int p = 0; p < nparams; p++) {
            if (!param_home_off || param_home_off[p] == 0) continue;
            const IRInst *pi = &fn->insts.data[p];
            int w = pi->width ? pi->width : 4;
            int is_float = value_is_float_class(fn, pi->dst);
            int vecb = is_float ? value_vec_bytes(fn, pi->dst) : 0;
            if (!vecb && is_float && (w == 16 || w == 32 || w == 64)) vecb = w;
            if (arrive_reg[p] < 0) {
                if (is_float) {
                    if (vecb >= 16) {
                        emit_xmm_load_spill(&out->text, XMM_SCRATCH0, stack_off[p], vecb);
                        emit_xmm_store_spill(&out->text, XMM_SCRATCH0,
                                             param_home_off[p], vecb);
                    } else {
                        emit_sse_load_disp(&out->text, XMM_SCRATCH0, REG_RBP,
                                           stack_off[p], w == 4);
                        emit_sse_store_disp(&out->text, REG_RBP, XMM_SCRATCH0,
                                            param_home_off[p], w == 4);
                    }
                } else {
                    emit_load_disp(&out->text, REG_RAX, REG_RBP, stack_off[p],
                                   w, pi->is_unsigned);
                    emit_store_disp(&out->text, REG_RBP, REG_RAX,
                                    param_home_off[p], w);
                }
            } else if (arrive_is_xmm[p]) {
                if (vecb >= 16)
                    emit_xmm_store_spill(&out->text, arrive_reg[p],
                                         param_home_off[p], vecb);
                else
                    emit_sse_store_disp(&out->text, REG_RBP, arrive_reg[p],
                                        param_home_off[p], w == 4);
            } else {
                emit_store_disp(&out->text, REG_RBP, arrive_reg[p],
                                param_home_off[p], w);
            }
        }

        /* Push every used arg register in REVERSE param order. */
        for (int p = nparams - 1; p >= 0; p--) {
            if (param_home_off && param_home_off[p] != 0) continue;
            if (arrive_reg[p] < 0) continue;
            if (arrive_is_xmm[p]) {
                const IRInst *pi = &fn->insts.data[p];
                int vb = value_vec_bytes(fn, pi->dst);
                if (!vb && (pi->width == 16 || pi->width == 32 || pi->width == 64))
                    vb = pi->width;
                emit_xmm_push(&out->text, arrive_reg[p], vb);
            } else {
                emit_push_r(&out->text, arrive_reg[p]);
            }
        }
        /* Pop into each param's allocated home (or spill slot) in order. */
        for (int p = 0; p < nparams; p++) {
            if (param_home_off && param_home_off[p] != 0) continue;
            const IRInst *pi = &fn->insts.data[p];
            int is_ld = (pi->dst >= 0 && pi->dst < fn->next_value_id &&
                         value_is_ld(fn, pi->dst));
            int is_float = (pi->dst >= 0 && pi->dst < fn->next_value_id &&
                            value_is_float_class(fn, pi->dst));
            if (arrive_reg[p] < 0) {
                /* Stack-passed arg: load from [rbp + stack_off] into home. */
                if (param_is_mem_blob(pi)) {
                    int pdr = (ra && pi->dst >= 0 && pi->dst < ra->num_values)
                              ? ra->reg[pi->dst] : -1;
                    if (pdr >= 0) {
                        emit_lea_rbp(&out->text, pdr, stack_off[p]);
                    } else {
                        emit_lea_rbp(&out->text, REG_RAX, stack_off[p]);
                        spill_if_needed(&out->text, pi->dst, REG_RAX, ra);
                    }
                    continue;
                }
                if (is_ld) {
                    /* long double: already in its 16-byte stack slot (ld_off set
                     * in the param loop); it lives in memory, not XMM — nothing
                     * to load. */
                    continue;
                }
                if (is_float) {
                    int vb = value_vec_bytes(fn, pi->dst);
                    if (!vb && (pi->width == 16 || pi->width == 32 || pi->width == 64))
                        vb = pi->width;
                    int pdr_xmm = (ra_xmm && pi->dst >= 0 &&
                                   pi->dst < ra_xmm->num_values)
                                  ? ra_xmm->reg[pi->dst] : -1;
                    if (pdr_xmm >= 0) {
                        emit_xmm_load_spill(&out->text, pdr_xmm, stack_off[p], vb);
                    } else {
                        emit_xmm_load_spill(&out->text, XMM_SCRATCH0, stack_off[p], vb);
                        spill_if_needed_xmm(&out->text, pi->dst, XMM_SCRATCH0,
                                             ra_xmm, gp_spill_area);
                    }
                } else {
                    int pdr = (ra && pi->dst >= 0 && pi->dst < ra->num_values)
                              ? ra->reg[pi->dst] : -1;
                    if (pdr >= 0) {
                        emit_load_spill(&out->text, pdr, stack_off[p]);
                    } else {
                        emit_load_spill(&out->text, REG_RAX, stack_off[p]);
                        spill_if_needed(&out->text, pi->dst, REG_RAX, ra);
                    }
                }
                continue;
            }
            if (is_float) {
                int vb = value_vec_bytes(fn, pi->dst);
                if (!vb && (pi->width == 16 || pi->width == 32 || pi->width == 64))
                    vb = pi->width;
                int pdr_xmm = (ra_xmm && pi->dst >= 0 &&
                               pi->dst < ra_xmm->num_values)
                              ? ra_xmm->reg[pi->dst] : -1;
                if (pdr_xmm >= 0) {
                    emit_xmm_pop(&out->text, pdr_xmm, vb);
                } else {
                    emit_xmm_pop(&out->text, XMM_SCRATCH0, vb);
                    spill_if_needed_xmm(&out->text, pi->dst, XMM_SCRATCH0,
                                         ra_xmm, gp_spill_area);
                }
            } else {
                int pdr = (ra && pi->dst >= 0 && pi->dst < ra->num_values)
                          ? ra->reg[pi->dst] : -1;
                if (pdr >= 0) {
                    emit_pop_r(&out->text, pdr);
                } else {
                    emit_pop_r(&out->text, REG_RAX);
                    spill_if_needed(&out->text, pi->dst, REG_RAX, ra);
                }
            }
        }

        /* Provisional: refined below to the first instruction that belongs to
         * the body rather than to the declaration line.  At -O0 the parameters
         * are spilled to their stack slots by instructions carrying the
         * declaration's line, and those are still prologue as far as a
         * debugger is concerned — stopping before them shows uninitialized
         * parameters. */
        prologue_end_pc = out->text.len;
        int prologue_end_found = 0;

        /* ---- Per-function label + patch tables ---- */
        /* label_off[label_id] = absolute code offset where the label lands,
         * or (size_t)-1 if not yet seen. */
        int nlabels = fn->next_label_id;
        size_t *label_off = NULL;
        if (nlabels > 0) {
            label_off = xmalloc(nlabels * sizeof(size_t));
            for (int L = 0; L < nlabels; L++) label_off[L] = (size_t)-1;
        }
        Patch *patches = NULL;
        size_t num_patches = 0, cap_patches = 0;

        #define ADD_PATCH(POFF, LBL, AFT) do { \
            if (num_patches >= cap_patches) { \
                cap_patches = cap_patches ? cap_patches * 2 : 8; \
                patches = xrealloc(patches, cap_patches * sizeof(Patch)); \
            } \
            patches[num_patches].patch_off = (POFF); \
            patches[num_patches].label = (LBL); \
            patches[num_patches].after_off = (AFT); \
            num_patches++; \
        } while (0)

        /* ---- Instruction loop ---- */
        for (size_t j = 0; j < fn->insts.len; j++) {
            const IRInst *inst = &fn->insts.data[j];
            if (inst->op == IR_DBG_VALUE) {
                /* Emits no code — it only records where a source variable
                 * lives from this point in the instruction stream on. */
                if (want_debug && inst->imm >= 0 &&
                    (size_t)inst->imm < fn->num_dbg_vars) {
                    if (num_dbg_obs >= cap_dbg_obs) {
                        cap_dbg_obs = cap_dbg_obs ? cap_dbg_obs * 2 : 8;
                        dbg_obs = xrealloc(dbg_obs, cap_dbg_obs * sizeof(DbgObs));
                    }
                    dbg_obs[num_dbg_obs].var = inst->imm;
                    dbg_obs[num_dbg_obs].value = inst->a;
                    dbg_obs[num_dbg_obs].pc = out->text.len;
                    num_dbg_obs++;
                }
                continue;
            }
            if (skip_body[j] || fused_skip[j]) continue;
            {
                int fold_off = 0;
                IRValue dummy_b = -1, dummy_idx = -1;
                /* Skip pure values that never need a register: folded
                 * [rbp+off] addresses and immediates encoded in ALU.  Do
                 * not skip COPY: mem2reg pointer selects are copies. */
                if (inst->dst >= 0 && inst->dst < fn->next_value_id && !needed[inst->dst]) {
                    if (inst->op == IR_ADDR
                        && fold_ptr_off(fn, ssa_def, alloca_off, inst->dst, &fold_off, 0))
                        continue;
                    if (inst->op == IR_CONST && !inst->is_float && !value_is_ld(fn, inst->dst))
                        continue;
                    /* A GEP stride multiply consumed by [base+index*scale]. */
                    if (inst->op == IR_MUL)
                        continue;
                    if (inst->op == IR_ADD
                        && (fold_ptr_off(fn, ssa_def, alloca_off, inst->dst, &fold_off, 0)
                            || classify_ptr_addr(fn, ssa_def, alloca_off, inst->dst,
                                                 &dummy_b, &dummy_idx,
                                                 &fold_off, &fold_off) != AF_NONE))
                        continue;
                }
            }
            if (want_debug && inst->loc.file && inst->loc.line > 0 &&
                inst->op != IR_PARAM && inst->op != IR_ALLOCA) {
                if (!prologue_end_found && inst->loc.line != fn->loc.line) {
                    prologue_end_pc = out->text.len;
                    prologue_end_found = 1;
                }
                emit_module_add_dbg_line(out, inst->loc.file, inst->loc.line,
                                         inst->loc.col, out->text.len);
            }
            /* Pick the register file by class: float values live in XMM,
             * everything else in the GP file.  `dr` is the destination's home
             * register (GP code or XMM code) or -1 if spilled. */
            int dr;
            if (value_is_float_class(fn, inst->dst)) {
                dr = (ra_xmm && inst->dst >= 0 && inst->dst < ra_xmm->num_values)
                     ? ra_xmm->reg[inst->dst] : -1;
            } else {
                dr = (ra && inst->dst >= 0 && inst->dst < ra->num_values)
                     ? ra->reg[inst->dst] : -1;
            }

            switch (inst->op) {

            case IR_CONST: {
                if (value_is_ld(fn, inst->dst)) {
                    /* long double constant: 80-bit value lives in a rodata
                     * global (name in inst->call_name).  lea rcx,[rip+0] with a
                     * PC32 reloc targeting the global symbol, fldt [rcx],
                     * fstpt into dst's slot.  Clobbers RCX. */
                    size_t patch = emit_lea_rip(&out->text, REG_RCX);
                    int gsym = emit_module_find_symbol(out, inst->call_name);
                    if (gsym < 0)
                        gsym = emit_module_add_undefined(out, inst->call_name);
                    emit_module_add_reloc(out, patch, R_X86_64_PC32, gsym, -4);
                    emit_x87_fldtRCX(&out->text); /* fldt [rcx] */
                    emit_ld_store(&out->text, inst->dst, ld_off);
                } else if (inst->is_float) {
                    /* Float constant: bit pattern in float_imm.  Materialize via
                     * RAX (movabs) then movq into the XMM file. */
                    if (dr >= 0) {
                        emit_float_const(&out->text, dr, inst->float_imm);
                    } else {
                        emit_float_const(&out->text, XMM_SCRATCH0, inst->float_imm);
                        spill_if_needed_xmm(&out->text, inst->dst, XMM_SCRATCH0,
                                            ra_xmm, gp_spill_area);
                    }
                } else if (dr >= 0) {
                    if (inst->width > 0 && inst->width <= 4)
                        emit_mov_imm32(&out->text, dr, (int32_t)inst->imm);
                    else
                        emit_mov_imm64(&out->text, dr, inst->imm);
                } else {
                    if (inst->width > 0 && inst->width <= 4)
                        emit_mov_imm32(&out->text, REG_RAX, (int32_t)inst->imm);
                    else
                        emit_mov_imm64(&out->text, REG_RAX, inst->imm);
                    spill_if_needed(&out->text, inst->dst, REG_RAX, ra);
                }
                break;
            }

            case IR_ADD:
            case IR_SUB:
            case IR_BAND:
            case IR_BOR:
            case IR_BXOR:
                isel_arith(&out->text, inst, fn, ssa_def, ra, dr, 0);
                break;

            case IR_MUL:
                isel_arith(&out->text, inst, fn, ssa_def, ra, dr, 1);
                break;

            case IR_DIV:
            case IR_MOD: {
                int width = inst->width > 0 ? inst->width : 8;
                int w64 = alu_w64(width);
                ensure_reg(&out->text, inst->a, REG_RAX, ra);
                ensure_reg(&out->text, inst->b, REG_RCX, ra);
                if (inst->is_unsigned) {
                    emit_xor_rr(&out->text, REG_RDX);
                    emit_rex_if(&out->text, w64, 0, 0, REG_RCX);
                    emit_byte(&out->text, 0xF7);
                    emit_modrm(&out->text, 3, 6, REG_RCX);
                } else if (w64) {
                    emit_cqto(&out->text);
                    emit_idiv_rcx(&out->text);
                } else {
                    emit_byte(&out->text, 0x99); /* cdq */
                    emit_rex_if(&out->text, 0, 0, 0, REG_RCX);
                    emit_byte(&out->text, 0xF7);
                    emit_modrm(&out->text, 3, 7, REG_RCX);
                }
                if (inst->op == IR_DIV) {
                    mask_after_alu(&out->text, REG_RAX, width, inst->is_unsigned);
                    if (dr >= 0 && dr != REG_RAX)
                        emit_mov_rr(&out->text, dr, REG_RAX);
                    spill_if_needed(&out->text, inst->dst,
                                    dr >= 0 ? dr : REG_RAX, ra);
                } else {
                    mask_after_alu(&out->text, REG_RDX, width, inst->is_unsigned);
                    if (dr >= 0 && dr != REG_RDX)
                        emit_mov_rr(&out->text, dr, REG_RDX);
                    spill_if_needed(&out->text, inst->dst,
                                    dr >= 0 ? dr : REG_RDX, ra);
                }
                break;
            }

            case IR_NEG: {
                int width = inst->width > 0 ? inst->width : 8;
                int acc = pick_alu_acc(dr, -1);
                ensure_reg(&out->text, inst->a, acc, ra);
                emit_unary_f7_w(&out->text, 3, acc, width);
                finish_gp_dst(&out->text, acc, dr, inst->dst, ra, width,
                              inst->is_unsigned);
                break;
            }

            case IR_BNOT: {
                int width = inst->width > 0 ? inst->width : 8;
                int acc = pick_alu_acc(dr, -1);
                ensure_reg(&out->text, inst->a, acc, ra);
                emit_unary_f7_w(&out->text, 2, acc, width);
                finish_gp_dst(&out->text, acc, dr, inst->dst, ra, width,
                              inst->is_unsigned);
                break;
            }

            case IR_SHL:
                isel_shift(&out->text, inst, fn, ssa_def, ra, dr, 4);
                break;
            case IR_SHR:
                isel_shift(&out->text, inst, fn, ssa_def, ra, dr,
                           inst->is_unsigned ? 5 : 7);
                break;
            case IR_ROL:
                isel_shift(&out->text, inst, fn, ssa_def, ra, dr, 0);
                break;

            case IR_EQ:
            case IR_NE:
            case IR_LT:
            case IR_LE:
            case IR_GT:
            case IR_GE: {
                int width = inst->width > 0 ? inst->width : 8;
                int64_t imm = 0;
                int fb = ssa_const_imm(fn, ssa_def, inst->b, &imm) && imm_fits_i32(imm);
                ensure_reg(&out->text, inst->a, REG_RAX, ra);
                if (!fb)
                    ensure_reg(&out->text, inst->b, REG_RCX, ra);
                uint8_t cc = ir_cmp_to_setcc(inst->op, inst->is_unsigned);
                int cc_dst = (dr >= 0 && dr != REG_RAX && dr != REG_RCX)
                             ? dr : REG_RDX;
                emit_xor_rr(&out->text, cc_dst);
                if (fb && imm == 0)
                    emit_test_r_w(&out->text, REG_RAX, width);
                else if (fb)
                    emit_cmp_imm_w(&out->text, REG_RAX, (int32_t)imm, width);
                else
                    emit_cmp_rr_w(&out->text, REG_RAX, REG_RCX, width);
                emit_setcc_r(&out->text, cc, cc_dst);
                if (dr >= 0) {
                    if (dr != cc_dst) emit_mov_rr(&out->text, dr, cc_dst);
                } else {
                    spill_if_needed(&out->text, inst->dst, cc_dst, ra);
                }
                break;
            }

            case IR_ALLOCA:
                break;

            case IR_COPY: {
                if (value_is_ld(fn, inst->dst) || (inst->a >= 0 && value_is_ld(fn, inst->a))) {
                    emit_ld_load(&out->text, inst->a, ld_off);
                    emit_ld_store(&out->text, inst->dst, ld_off);
                    break;
                }
                if (value_is_float_class(fn, inst->dst) || (inst->a >= 0 && value_is_float_class(fn, inst->a))) {
                    int dr_xmm = (ra_xmm && inst->dst >= 0 && inst->dst < ra_xmm->num_values)
                                 ? ra_xmm->reg[inst->dst] : -1;
                    int ar_xmm = (ra_xmm && inst->a >= 0 && inst->a < ra_xmm->num_values)
                                 ? ra_xmm->reg[inst->a] : -1;
                    if (dr_xmm >= 0 && ar_xmm >= 0 && dr_xmm == ar_xmm) {
                        break;
                    }
                    ensure_reg_xmm(&out->text, inst->a, XMM_SCRATCH0, ra_xmm, gp_spill_area);
                    if (dr_xmm >= 0 && dr_xmm != XMM_SCRATCH0) {
                        emit_xmm_copy(&out->text, dr_xmm, XMM_SCRATCH0,
                                      value_vec_bytes(fn, inst->dst));
                    }
                    spill_if_needed_xmm(&out->text, inst->dst, dr_xmm >= 0 ? dr_xmm : XMM_SCRATCH0, ra_xmm, gp_spill_area);
                    break;
                }
                if (ra && inst->a >= 0 && inst->a < ra->num_values &&
                    inst->dst >= 0 && inst->dst < ra->num_values &&
                    ra->reg[inst->dst] == ra->reg[inst->a] &&
                    ra->reg[inst->dst] >= 0) {
                    break;
                }
                /* Materialize the source via ensure_reg (handles register-alloc
                 * OR spilled sources); old_load would read the wrong slot when
                 * `a` lives in a register.  Then move to dr or spill dst. */
                ensure_reg(&out->text, inst->a, REG_RAX, ra);
                if (dr >= 0) {
                    if (dr != REG_RAX) emit_mov_rr(&out->text, dr, REG_RAX);
                } else {
                    spill_if_needed(&out->text, inst->dst, REG_RAX, ra);
                }
                break;
            }

            case IR_SEXT:
            case IR_ZEXT: {
                /* imm = source width */
                int src_w = inst->imm;
                ensure_reg(&out->text, inst->a, REG_RAX, ra);
                if (inst->op == IR_SEXT)
                    emit_movsx_rr(&out->text, REG_RAX, REG_RAX, src_w);
                else
                    emit_movzx_rr(&out->text, REG_RAX, REG_RAX, src_w);
                if (dr >= 0 && dr != REG_RAX) emit_mov_rr(&out->text, dr, REG_RAX);
                spill_if_needed(&out->text, inst->dst,
                                dr >= 0 ? dr : REG_RAX, ra);
                break;
            }

            case IR_TRUNC: {
                /* Narrow `a` to inst->width and re-extend it to 64 bits, so
                 * the result upholds the invariant every other opcode relies
                 * on: a value of width W is correctly extended in its 64-bit
                 * register.  Skipping the mask (treating TRUNC as a pure
                 * register-level no-op) leaves the source's high bits behind,
                 * and the comparison opcodes issue a 64-bit `cmp` without
                 * re-masking — so `(unsigned)wide64 == 1` compared false
                 * whenever the discarded half was non-zero. */
                if (ra && inst->a >= 0 && inst->a < ra->num_values &&
                    inst->dst >= 0 && inst->dst < ra->num_values &&
                    ra->reg[inst->dst] == ra->reg[inst->a] &&
                    ra->reg[inst->dst] >= 0) {
                    mask_to_width(&out->text, ra->reg[inst->dst],
                                  inst->width, inst->is_unsigned);
                    break;
                }
                if (dr >= 0) {
                    ensure_reg(&out->text, inst->a, dr, ra);
                    mask_to_width(&out->text, dr, inst->width,
                                  inst->is_unsigned);
                } else {
                    /* Materialize the source with ensure_reg, not old_load:
                     * old_load addresses `a` at the pre-regalloc slot
                     * -8*(a+1), which under register allocation is usually
                     * outside the frame entirely (the frame is sized for the
                     * spill slots, not one slot per SSA value). */
                    ensure_reg(&out->text, inst->a, REG_RAX, ra);
                    mask_to_width(&out->text, REG_RAX, inst->width,
                                  inst->is_unsigned);
                    spill_if_needed(&out->text, inst->dst, REG_RAX, ra);
                }
                break;
            }

            case IR_LOAD: {
                if (value_is_ld(fn, inst->dst) || (inst->a >= 0 && value_is_ld(fn, inst->a))) {
                    emit_ld_load(&out->text, inst->a, ld_off);
                    emit_ld_store(&out->text, inst->dst, ld_off);
                    break;
                }
                if (value_is_float_class(fn, inst->dst) || (inst->a >= 0 && value_is_float_class(fn, inst->a))) {
                    int dr_xmm = (ra_xmm && inst->dst >= 0 && inst->dst < ra_xmm->num_values)
                                 ? ra_xmm->reg[inst->dst] : -1;
                    ensure_reg_xmm(&out->text, inst->a, XMM_SCRATCH0, ra_xmm, gp_spill_area);
                    if (dr_xmm >= 0 && dr_xmm != XMM_SCRATCH0) {
                        emit_xmm_copy(&out->text, dr_xmm, XMM_SCRATCH0,
                                      value_vec_bytes(fn, inst->dst));
                    }
                    spill_if_needed_xmm(&out->text, inst->dst, dr_xmm >= 0 ? dr_xmm : XMM_SCRATCH0, ra_xmm, gp_spill_area);
                    break;
                }
                /* Rarely reached: mem2reg usually eliminates LOAD.  If a LOAD
                 * survives, it copies the SSA value `a` into `dst`.  With
                 * regalloc, `a` may live in a register or a spill slot — both
                 * must be handled, so materialize via ensure_reg (NOT old_load,
                 * which assumes the pre-regalloc slot layout and would read
                 * garbage when `a` is register-allocated).  Then move to dr or
                 * spill dst. */
                ensure_reg(&out->text, inst->a, REG_RAX, ra);
                if (dr >= 0) {
                    if (dr != REG_RAX) emit_mov_rr(&out->text, dr, REG_RAX);
                } else {
                    spill_if_needed(&out->text, inst->dst, REG_RAX, ra);
                }
                break;
            }

            case IR_STORE: {
                if ((inst->a >= 0 && value_is_ld(fn, inst->a)) || (inst->b >= 0 && value_is_ld(fn, inst->b))) {
                    emit_ld_load(&out->text, inst->b, ld_off);
                    emit_ld_store(&out->text, inst->a, ld_off);
                    break;
                }
                if ((inst->a >= 0 && value_is_float_class(fn, inst->a)) || (inst->b >= 0 && value_is_float_class(fn, inst->b))) {
                    int ar_xmm = (ra_xmm && inst->a >= 0 && inst->a < ra_xmm->num_values)
                                 ? ra_xmm->reg[inst->a] : -1;
                    if (ar_xmm >= 0) {
                        ensure_reg_xmm(&out->text, inst->b, ar_xmm, ra_xmm, gp_spill_area);
                    } else {
                        ensure_reg_xmm(&out->text, inst->b, XMM_SCRATCH0, ra_xmm, gp_spill_area);
                        spill_if_needed_xmm(&out->text, inst->a, XMM_SCRATCH0, ra_xmm, gp_spill_area);
                    }
                    break;
                }
                /* [a] = b.  Rarely reached — mem2reg normally removes it.
                 * Route both operands through ensure_reg / spill_if_needed so
                 * each is addressed at its real home; old_load/old_store
                 * assume the pre-regalloc "one slot per SSA value" layout and
                 * read or write outside the frame once regalloc is on. */
                int ar = (ra && inst->a >= 0 && inst->a < ra->num_values)
                         ? ra->reg[inst->a] : -1;
                if (ar >= 0) {
                    ensure_reg(&out->text, inst->b, ar, ra);
                } else {
                    ensure_reg(&out->text, inst->b, REG_RAX, ra);
                    spill_if_needed(&out->text, inst->a, REG_RAX, ra);
                }
                break;
            }

            case IR_ADDR: {
                /* dst = &alloca_slot.  Get alloca's byte-offset from rbp. */
                int off = 0;
                if (inst->a >= 0 && inst->a < fn->next_value_id)
                    off = alloca_off[inst->a];
                if (off == 0) {
                    /* Alloca wasn't laid out as pinned — this is a bug in IR-gen
                     * (should have set alloca_bytes>0). */
                    fprintf(stderr, "fakecc: IR_ADDR on non-pinned alloca %d\n", inst->a);
                    exit(1);
                }
                int target = dr >= 0 ? dr : REG_RAX;
                emit_lea_rbp(&out->text, target, off);
                /* Alignments > 16: incoming rbp is only 16-aligned, so bump. */
                if (inst->a >= 0 && inst->a < fn->next_value_id && ssa_def[inst->a] >= 0) {
                    const IRInst *al = &fn->insts.data[ssa_def[inst->a]];
                    int aln = (al->op == IR_ALLOCA) ? (int)al->imm : 0;
                    if (aln >= 32) {
                        emit_add_imm32(&out->text, target, aln - 1);
                        emit_and_imm32(&out->text, target, (int32_t)-aln);
                    }
                }
                if (dr < 0)
                    spill_if_needed(&out->text, inst->dst, REG_RAX, ra);
                break;
            }

            case IR_GADDR: {
                /* dst = &global; target name in inst->call_name.  If the
                 * global is defined in THIS module, its address is a link-time
                 * constant — emit `lea r, [rip+0]` + PC32 reloc as before.
                 * Otherwise (cross-module global or external libc variable) the
                 * address is only known via the GOT: emit `mov r, [rip+0]`
                 * (a qword load) + a GOTPCREL reloc.  The linker fills the GOT
                 * entry with the symbol's address, so `r` ends up holding
                 * `&global` in both cases.  This is what makes external data
                 * (e.g. `stderr`) loadable — a plain PC32 would resolve to the
                 * PLT stub, which is wrong for variables. */
                int target = dr >= 0 ? dr : REG_RAX;
                int gsym = emit_module_find_symbol(out, inst->call_name);
                int defined_here = (gsym >= 0 && out->syms[gsym].shndx != SECT_UNDEF);
                if (defined_here) {
                    size_t patch = emit_lea_rip(&out->text, target);
                    emit_module_add_reloc(out, patch, R_X86_64_PC32, gsym, -4);
                } else {
                    if (gsym < 0)
                        gsym = emit_module_add_undefined(out, inst->call_name);
                    size_t patch = emit_load_rip(&out->text, target);
                    emit_module_add_reloc(out, patch, R_X86_64_GOTPCREL, gsym, -4);
                }
                if (dr < 0)
                    spill_if_needed(&out->text, inst->dst, REG_RAX, ra);
                break;
            }

            case IR_GADDR_TLS: {
                /* dst = &__thread-global; target name in inst->call_name.
                 *
                 * Emit the x86-64 Initial-Exec (IE) TLS address sequence so the
                 * same .o works in a static executable and in a DSO:
                 *
                 *   movq %fs:0, %rdst           # load thread pointer from %fs
                 *   addq x@gottpoff(%rip), %rdst # add TPOFF from a GOT slot
                 *
                 * The linker patches the RIP-relative disp with R_X86_64_GOTTPOFF.
                 * The GOT slot is filled with TPOFF (S - tls_end) at static link,
                 * or with a dynamic R_X86_64_TPOFF64 in a shared object.
                 * Local-Exec TPOFF32 is rejected by ld.so as reloc type 0x17.
                 *
                 * NOTE: `lea %fs:[rip+disp32]` is NOT equivalent — on x86-64 the
                 * `lea` instruction ignores segment overrides entirely, so the %fs
                 * prefix is a no-op.  The correct TP read is `movq %fs:0, %reg`. */
                int target = dr >= 0 ? dr : REG_RAX;
                int gsym = emit_module_find_symbol(out, inst->call_name);
                if (gsym < 0)
                    gsym = emit_module_add_undefined_type(out, inst->call_name,
                                                         6 /* STT_TLS */);
                else if (out->syms[gsym].shndx == SECT_UNDEF
                         && out->syms[gsym].type == 0)
                    out->syms[gsym].type = 6 /* STT_TLS */;
                emit_mov_fs0_reg(&out->text, target);
                size_t patch = emit_add_rip(&out->text, target);
                emit_module_add_reloc(out, patch, R_X86_64_GOTTPOFF, gsym, -4);
                if (dr < 0)
                    spill_if_needed(&out->text, inst->dst, REG_RAX, ra);
                break;
            }

            case IR_LADDR: {
                /* dst = &&label — emit `lea r, [rip+0]` and record a label patch.
                 * imm = label_id.  The patch resolution (label_off[id] - after_off)
                 * gives the RIP-relative displacement to the label's code offset. */
                int target = dr >= 0 ? dr : REG_RAX;
                size_t patch = emit_lea_rip(&out->text, target);
                size_t after = out->text.len;
                ADD_PATCH(patch, inst->imm, after);
                if (dr < 0)
                    spill_if_needed(&out->text, inst->dst, REG_RAX, ra);
                break;
            }

            case IR_JMP_PTR: {
                /* goto *ptr — emit `jmp *r11` (FF /4 with ModRM 0xE3).
                 * We load the pointer into R11 first. */
                ensure_reg(&out->text, inst->a, REG_R11, ra);
                /* REX.B for R11 */
                emit_byte(&out->text, 0x41);
                emit_byte(&out->text, 0xFF);
                emit_byte(&out->text, 0xE3);  /* ModRM: mod=11 reg=4(/4) rm=011 → 11100011 = 0xE3 */
                break;
            }

            case IR_FRAME_ADDR: {
                int target = dr >= 0 ? dr : REG_RAX;
                emit_mov_rr(&out->text, target, REG_RBP);
                int level = (int)inst->imm;
                for (int i = 0; i < level; i++) {
                    emit_load_base_off(&out->text, target, target, 0);
                }
                if (dr < 0)
                    spill_if_needed(&out->text, inst->dst, REG_RAX, ra);
                break;
            }

            case IR_RETURN_ADDR: {
                int target = dr >= 0 ? dr : REG_RAX;
                emit_mov_rr(&out->text, target, REG_RBP);
                int level = (int)inst->imm;
                for (int i = 0; i < level; i++) {
                    emit_load_base_off(&out->text, target, target, 0);
                }
                emit_load_base_off(&out->text, target, target, 8);
                if (dr < 0)
                    spill_if_needed(&out->text, inst->dst, REG_RAX, ra);
                break;
            }

            case IR_DYN_ALLOCA: {
                /* Dynamic stack allocation: size in inst->a.
                 * Load size into RAX, align to 16, sub from rsp, mov rsp to target. */
                ensure_reg(&out->text, inst->a, REG_RAX, ra);
                /* add $15, %rax: 48 83 C0 0F */
                emit_byte(&out->text, 0x48);
                emit_byte(&out->text, 0x83);
                emit_byte(&out->text, 0xC0);
                emit_byte(&out->text, 0x0F);
                /* and $-16, %rax: 48 83 E0 F0 */
                emit_byte(&out->text, 0x48);
                emit_byte(&out->text, 0x83);
                emit_byte(&out->text, 0xE0);
                emit_byte(&out->text, 0xF0);
                /* sub %rax, %rsp: 48 29 C4 */
                emit_byte(&out->text, 0x48);
                emit_byte(&out->text, 0x29);
                emit_byte(&out->text, 0xC4);
                /* result is %rsp */
                int target = dr >= 0 ? dr : REG_RAX;
                emit_mov_rr(&out->text, target, REG_RSP);
                if (dr < 0)
                    spill_if_needed(&out->text, inst->dst, REG_RAX, ra);
                break;
            }

            case IR_STACK_SAVE: {
                int target = dr >= 0 ? dr : REG_RAX;
                emit_mov_rr(&out->text, target, REG_RSP);
                if (dr < 0)
                    spill_if_needed(&out->text, inst->dst, REG_RAX, ra);
                break;
            }

            case IR_STACK_RESTORE: {
                ensure_reg(&out->text, inst->a, REG_RAX, ra);
                emit_mov_rr(&out->text, REG_RSP, REG_RAX);
                break;
            }

            case IR_LONGJMP: {
                ensure_reg(&out->text, inst->a, REG_RDI, ra);
                emit_byte(&out->text, 0x48); emit_byte(&out->text, 0x8B); emit_byte(&out->text, 0x6F); emit_byte(&out->text, 0x00);
                emit_byte(&out->text, 0x48); emit_byte(&out->text, 0x8B); emit_byte(&out->text, 0x67); emit_byte(&out->text, 0x10);
                emit_byte(&out->text, 0x48); emit_byte(&out->text, 0x8B); emit_byte(&out->text, 0x47); emit_byte(&out->text, 0x08);
                emit_byte(&out->text, 0xFF); emit_byte(&out->text, 0xE0);
                break;
            }

            case IR_FADDR: {
                /* dst = &function; function name in inst->call_name.  Emit
                 * `lea r, [rip+0]` and record an FnAddrPatch resolved against
                 * the code symbol table (functions live in .code, not .data). */
                int target = dr >= 0 ? dr : REG_RAX;
                size_t patch = emit_lea_rip(&out->text, target);
                if (num_fnaddr_patches >= cap_fnaddr_patches) {
                    cap_fnaddr_patches = cap_fnaddr_patches ? cap_fnaddr_patches * 2 : 8;
                    fnaddr_patches = xrealloc(fnaddr_patches,
                                               cap_fnaddr_patches * sizeof(FnAddrPatch));
                }
                fnaddr_patches[num_fnaddr_patches].patch_off = patch;
                fnaddr_patches[num_fnaddr_patches].fn_name = xstrdup(inst->call_name);
                num_fnaddr_patches++;
                if (dr < 0)
                    spill_if_needed(&out->text, inst->dst, REG_RAX, ra);
                break;
            }

            case IR_LOAD_PTR: {
                /* dst = *ptr.  ptr = inst->a. */
                int ptr_off = 0;
                int folded = fold_ptr_off(fn, ssa_def, alloca_off, inst->a, &ptr_off, 0);
                IRValue af_base = -1, af_idx = -1;
                int af_sc = 0, af_disp = 0, af_kind = AF_NONE;
                int af_adj = inst->a >= 0 && inst->a < fn->next_value_id
                             && ssa_def[inst->a] >= 0
                             && (size_t)ssa_def[inst->a] + 1 == j
                             && (inst->a < 0 || raw_uses[inst->a] <= 1);
                if (!folded && af_adj
                    && !value_is_float_class(fn, inst->dst) && !value_is_ld(fn, inst->dst)
                    && (inst->width == 1 || inst->width == 2 || inst->width == 4
                        || inst->width == 8))
                    af_kind = classify_ptr_addr(fn, ssa_def, alloca_off, inst->a,
                                                &af_base, &af_idx, &af_sc, &af_disp);
                if (!folded && af_kind == AF_NONE)
                    ensure_reg(&out->text, inst->a, REG_RCX, ra);
                if (value_is_ld(fn, inst->dst)) {
                    if (folded)
                        emit_x87_fldt_disp(&out->text, REG_RBP, ptr_off);
                    else {
                        emit_byte(&out->text, 0xDB);
                        emit_modrm(&out->text, 0, 5, REG_RCX & 7);
                    }
                    emit_ld_store(&out->text, inst->dst, ld_off);
                } else if (value_is_float_class(fn, inst->dst)) {
                    int dst_xmm = dr >= 0 ? dr : XMM_SCRATCH0;
                    int vw = inst->width;
                    if (vw >= 64) {
                        if (folded)
                            emit_evex_vmovups_mem(&out->text, dst_xmm, REG_RBP, ptr_off, 0);
                        else
                            emit_evex_vmovups_via_ptr(&out->text, dst_xmm, REG_RCX, 0);
                    } else if (vw >= 32) {
                        if (folded)
                            emit_vmovups_mem(&out->text, dst_xmm, REG_RBP, ptr_off, 0);
                        else
                            emit_vmovups_via_ptr(&out->text, dst_xmm, REG_RCX, 0);
                    } else if (vw == 16) {
                        if (folded) {
                            emit_rex_wrb(&out->text, 0, dst_xmm, REG_RBP);
                            emit_byte(&out->text, 0x0F);
                            emit_byte(&out->text, 0x10);
                            emit_modrm_disp(&out->text, dst_xmm, REG_RBP, ptr_off);
                        } else
                            emit_movups_load_via_ptr(&out->text, dst_xmm, REG_RCX);
                    } else {
                        int is_float = (inst->width == 4);
                        if (folded)
                            emit_sse_load_disp(&out->text, dst_xmm, REG_RBP, ptr_off, is_float);
                        else
                            emit_sse_load_via_ptr(&out->text, dst_xmm, REG_RCX, is_float);
                    }
                    if (dr < 0)
                        spill_if_needed_xmm(&out->text, inst->dst, XMM_SCRATCH0,
                                            ra_xmm, gp_spill_area);
                } else {
                    int dst_gp = dr >= 0 ? dr : REG_RAX;
                    if (folded)
                        emit_load_disp(&out->text, dst_gp, REG_RBP, ptr_off,
                                       inst->width, inst->is_unsigned);
                    else if (af_kind == AF_RBP_IDX) {
                        if (af_idx >= 0)
                            ensure_reg(&out->text, af_idx, REG_RCX, ra);
                        if (af_idx >= 0)
                            emit_load_sib(&out->text, dst_gp, REG_RBP, REG_RCX,
                                          af_sc, af_disp, inst->width, inst->is_unsigned);
                        else
                            emit_load_disp(&out->text, dst_gp, REG_RBP, af_disp,
                                           inst->width, inst->is_unsigned);
                    } else if (af_kind == AF_SIB) {
                        if (af_idx >= 0)
                            ensure_reg(&out->text, af_idx, REG_RCX, ra);
                        ensure_reg(&out->text, af_base, REG_RAX, ra);
                        if (af_idx >= 0)
                            emit_load_sib(&out->text, dst_gp, REG_RAX, REG_RCX,
                                          af_sc, af_disp, inst->width, inst->is_unsigned);
                        else
                            emit_load_disp(&out->text, dst_gp, REG_RAX, af_disp,
                                           inst->width, inst->is_unsigned);
                    } else
                        emit_load_via_ptr(&out->text, dst_gp, REG_RCX,
                                          inst->width, inst->is_unsigned);
                    if (dr < 0)
                        spill_if_needed(&out->text, inst->dst, REG_RAX, ra);
                }
                break;
            }

            case IR_STORE_PTR: {
                int ptr_off = 0;
                int folded = fold_ptr_off(fn, ssa_def, alloca_off, inst->a, &ptr_off, 0);
                IRValue af_base = -1, af_idx = -1;
                int af_sc = 0, af_disp = 0, af_kind = AF_NONE;
                int af_adj = inst->a >= 0 && inst->a < fn->next_value_id
                             && ssa_def[inst->a] >= 0
                             && (size_t)ssa_def[inst->a] + 1 == j
                             && (inst->a < 0 || raw_uses[inst->a] <= 1);
                if (!folded && af_adj
                    && !value_is_float_class(fn, inst->b) && !value_is_ld(fn, inst->b)
                    && (inst->width == 1 || inst->width == 2 || inst->width == 4
                        || inst->width == 8))
                    af_kind = classify_ptr_addr(fn, ssa_def, alloca_off, inst->a,
                                                &af_base, &af_idx, &af_sc, &af_disp);
                if (!folded && af_kind == AF_NONE)
                    ensure_reg(&out->text, inst->a, REG_RCX, ra);
                if (value_is_ld(fn, inst->b)) {
                    emit_ld_load(&out->text, inst->b, ld_off);
                    if (folded)
                        emit_x87_fstpt_disp(&out->text, REG_RBP, ptr_off);
                    else {
                        ensure_reg(&out->text, inst->a, REG_RCX, ra);
                        emit_byte(&out->text, 0xDB);
                        emit_modrm(&out->text, 0, 7, REG_RCX & 7);
                    }
                } else if (value_is_float_class(fn, inst->b)) {
                    ensure_reg_xmm(&out->text, inst->b, XMM_SCRATCH0, ra_xmm,
                                   gp_spill_area);
                    if (inst->width >= 64) {
                        if (folded)
                            emit_evex_vmovups_mem(&out->text, XMM_SCRATCH0, REG_RBP, ptr_off, 1);
                        else
                            emit_evex_vmovups_via_ptr(&out->text, XMM_SCRATCH0, REG_RCX, 1);
                    } else if (inst->width >= 32) {
                        if (folded)
                            emit_vmovups_mem(&out->text, XMM_SCRATCH0, REG_RBP, ptr_off, 1);
                        else
                            emit_vmovups_via_ptr(&out->text, XMM_SCRATCH0, REG_RCX, 1);
                    } else if (inst->width == 16) {
                        if (folded) {
                            emit_rex_wrb(&out->text, 0, XMM_SCRATCH0, REG_RBP);
                            emit_byte(&out->text, 0x0F);
                            emit_byte(&out->text, 0x11);
                            emit_modrm_disp(&out->text, XMM_SCRATCH0, REG_RBP, ptr_off);
                        } else
                            emit_movups_store_via_ptr(&out->text, REG_RCX, XMM_SCRATCH0);
                    } else {
                        int is_float = (inst->width == 4);
                        if (folded)
                            emit_sse_store_disp(&out->text, REG_RBP, XMM_SCRATCH0,
                                                ptr_off, is_float);
                        else
                            emit_sse_store_via_ptr(&out->text, REG_RCX, XMM_SCRATCH0,
                                                   is_float);
                    }
                } else if (af_kind == AF_RBP_IDX) {
                    if (af_idx >= 0)
                        ensure_reg(&out->text, af_idx, REG_RCX, ra);
                    ensure_reg(&out->text, inst->b, REG_RAX, ra);
                    if (af_idx >= 0)
                        emit_store_sib(&out->text, REG_RBP, REG_RCX, af_sc, af_disp,
                                       REG_RAX, inst->width);
                    else
                        emit_store_disp(&out->text, REG_RBP, REG_RAX, af_disp,
                                        inst->width);
                } else if (af_kind == AF_SIB) {
                    if (af_idx >= 0)
                        ensure_reg(&out->text, af_idx, REG_RCX, ra);
                    ensure_reg(&out->text, inst->b, REG_RDX, ra);
                    ensure_reg(&out->text, af_base, REG_RAX, ra);
                    if (af_idx >= 0)
                        emit_store_sib(&out->text, REG_RAX, REG_RCX, af_sc, af_disp,
                                       REG_RDX, inst->width);
                    else
                        emit_store_disp(&out->text, REG_RAX, REG_RDX, af_disp,
                                        inst->width);
                } else {
                    ensure_reg(&out->text, inst->b, REG_RAX, ra);
                    if (folded)
                        emit_store_disp(&out->text, REG_RBP, REG_RAX, ptr_off,
                                        inst->width);
                    else
                        emit_store_via_ptr(&out->text, REG_RCX, REG_RAX, inst->width);
                }
                break;
            }

            case IR_VADD:
            case IR_VSUB:
            case IR_VMUL:
            case IR_VDIV:
            case IR_VBAND:
            case IR_VBOR:
            case IR_VBXOR: {
                /* Scratch is XMM14/XMM15 and RAX/RCX/RDX — never RDI/R8/R9
                 * or XMM0-13, which the allocator hands out to live SSA. */
                int vec_sz = inst->width;
                int elem_sz = (int)inst->imm;
                int is_float = inst->is_float;
                ensure_reg(&out->text, inst->a, REG_RAX, ra);
                ensure_reg(&out->text, inst->b, REG_RCX, ra);
                if (inst->op == IR_VDIV
                    || (inst->op == IR_VMUL && !is_float
                        && (elem_sz == 1 || elem_sz >= 8))) {
                    /* SSE has no packed integer div, no packed byte mul, and
                     * no packed 64-bit mul of both lanes.  Scalarize using
                     * only reserved GP scratches, spilling the three
                     * pointers onto the stack across the imul/div. */
                    int n = elem_sz > 0 ? vec_sz / elem_sz : 0;
                    ensure_reg(&out->text, inst->dst, REG_RDX, ra);
                    emit_push_r(&out->text, REG_RAX); /* src_a */
                    emit_push_r(&out->text, REG_RCX); /* src_b */
                    emit_push_r(&out->text, REG_RDX); /* dst  */
                    for (int ei = 0; ei < n; ei++) {
                        int off = ei * elem_sz;
                        emit_load_disp(&out->text, REG_RAX, REG_RSP, 16, 8, 1);
                        if (off) emit_add_imm32(&out->text, REG_RAX, off);
                        emit_load_via_ptr(&out->text, REG_RAX, REG_RAX, elem_sz,
                                          inst->is_unsigned);
                        emit_load_disp(&out->text, REG_RCX, REG_RSP, 8, 8, 1);
                        if (off) emit_add_imm32(&out->text, REG_RCX, off);
                        emit_load_via_ptr(&out->text, REG_RCX, REG_RCX, elem_sz,
                                          inst->is_unsigned);
                        if (inst->op == IR_VMUL) {
                            emit_imul_rr(&out->text, REG_RAX, REG_RCX);
                            mask_to_width(&out->text, REG_RAX, elem_sz,
                                          inst->is_unsigned);
                        } else if (inst->is_unsigned) {
                            emit_xor_rr(&out->text, REG_RDX);
                            emit_rex_w(&out->text);
                            emit_byte(&out->text, 0xF7);
                            emit_byte(&out->text, 0xF1);
                        } else {
                            emit_cqto(&out->text);
                            emit_idiv_rcx(&out->text);
                        }
                        emit_load_disp(&out->text, REG_RCX, REG_RSP, 0, 8, 1);
                        if (off) emit_add_imm32(&out->text, REG_RCX, off);
                        emit_store_via_ptr(&out->text, REG_RCX, REG_RAX, elem_sz);
                    }
                    emit_add_rsp_imm32(&out->text, 24);
                    break;
                }
                emit_vec_load_xmm(&out->text, XMM_SCRATCH0, REG_RAX, vec_sz);
                emit_vec_load_xmm(&out->text, XMM_SCRATCH1, REG_RCX, vec_sz);
                /* Packed op: XMM14 = XMM14 op XMM15 */
                if (is_float) {
                    if (elem_sz >= 8) emit_byte(&out->text, 0x66);
                    emit_rex_wrb(&out->text, 0, XMM_SCRATCH0, XMM_SCRATCH1);
                    emit_byte(&out->text, 0x0F);
                    if (inst->op == IR_VADD) emit_byte(&out->text, 0x58);
                    else if (inst->op == IR_VSUB) emit_byte(&out->text, 0x5C);
                    else if (inst->op == IR_VMUL) emit_byte(&out->text, 0x59);
                    else if (inst->op == IR_VDIV) emit_byte(&out->text, 0x5E);
                    else if (inst->op == IR_VBAND) emit_byte(&out->text, 0x54);
                    else if (inst->op == IR_VBOR)  emit_byte(&out->text, 0x56);
                    else emit_byte(&out->text, 0x57);
                    emit_modrm(&out->text, 3, XMM_SCRATCH0, XMM_SCRATCH1);
                } else {
                    emit_byte(&out->text, 0x66);
                    emit_rex_wrb(&out->text, 0, XMM_SCRATCH0, XMM_SCRATCH1);
                    emit_byte(&out->text, 0x0F);
                    if (inst->op == IR_VBAND) {
                        emit_byte(&out->text, 0xDB);
                    } else if (inst->op == IR_VBOR) {
                        emit_byte(&out->text, 0xEB);
                    } else if (inst->op == IR_VBXOR) {
                        emit_byte(&out->text, 0xEF);
                    } else if (elem_sz == 1) {
                        emit_byte(&out->text,
                                  inst->op == IR_VSUB ? 0xF8 : 0xFC);
                    } else if (elem_sz == 2) {
                        if (inst->op == IR_VADD) emit_byte(&out->text, 0xFD);
                        else if (inst->op == IR_VSUB) emit_byte(&out->text, 0xF9);
                        else emit_byte(&out->text, 0xD5); /* pmullw */
                    } else if (elem_sz == 4) {
                        if (inst->op == IR_VMUL) {
                            emit_byte(&out->text, 0x38);
                            emit_byte(&out->text, 0x40);
                        } else {
                            emit_byte(&out->text, (inst->op == IR_VSUB) ? 0xFA : 0xFE);
                        }
                    } else {
                        emit_byte(&out->text, (inst->op == IR_VSUB) ? 0xFB : 0xD4);
                    }
                    emit_modrm(&out->text, 3, XMM_SCRATCH0, XMM_SCRATCH1);
                }
                ensure_reg(&out->text, inst->dst, REG_RAX, ra);
                emit_vec_store_xmm(&out->text, REG_RAX, XMM_SCRATCH0, vec_sz);
                break;
            }

            case IR_LABEL: {
                if (inst->imm >= 0 && inst->imm < nlabels) {
                    label_off[inst->imm] = out->text.len;
                }
                break;
            }

            case IR_BR: {
                size_t patch = emit_jmp_rel32(&out->text);
                size_t after = out->text.len;
                ADD_PATCH(patch, inst->imm, after);
                break;
            }

            case IR_CBR: {
                /* CBR: a = cond, imm = true_label, b = false_label.
                 * Fused compare: `cmp a,b; j<cc> true; jmp false`.
                 * Otherwise test the materialized 0/1 condition. */
                uint8_t cc = 0x85; /* jne */
                if (fused_cmp_at[j] >= 0) {
                    const IRInst *cmp = &fn->insts.data[fused_cmp_at[j]];
                    int cwidth = cmp->width > 0 ? cmp->width : 8;
                    int64_t cimmv = 0;
                    int cfb = ssa_const_imm(fn, ssa_def, cmp->b, &cimmv)
                              && imm_fits_i32(cimmv);
                    ensure_reg(&out->text, cmp->a, REG_RAX, ra);
                    if (!cfb)
                        ensure_reg(&out->text, cmp->b, REG_RCX, ra);
                    /* ir_cmp_to_setcc yields the SETcc opcode (0x90+cc);
                     * jcc rel32 wants the near-branch opcode (0x80+cc). */
                    cc = (uint8_t)(0x80 | (ir_cmp_to_setcc(cmp->op, cmp->is_unsigned)
                                            & 0x0F));
                    if (fused_inv_at[j])
                        cc ^= 1;
                    if (cfb && cimmv == 0)
                        emit_test_r_w(&out->text, REG_RAX, cwidth);
                    else if (cfb)
                        emit_cmp_imm_w(&out->text, REG_RAX, (int32_t)cimmv, cwidth);
                    else
                        emit_cmp_rr_w(&out->text, REG_RAX, REG_RCX, cwidth);
                } else {
                    ensure_reg(&out->text, inst->a, REG_RAX, ra);
                    emit_test_rr(&out->text, REG_RAX);
                }
                /* j<cc> true_label */
                size_t p1 = emit_jcc_rel32(&out->text, cc);
                size_t a1 = out->text.len;
                ADD_PATCH(p1, inst->imm, a1);
                /* jmp false_label */
                size_t p2 = emit_jmp_rel32(&out->text);
                size_t a2 = out->text.len;
                ADD_PATCH(p2, inst->b, a2);
                break;
            }

            case IR_PARAM:
                /* Already handled during prologue (push-then-pop dance).
                 * Skip here.  */
                break;

            case IR_CALL: {
                /* Arm64 memory barrier.  x86 keeps the historical no-op. */
                if (inst->call_name && strcmp(inst->call_name, "__fakecc_dmb") == 0)
                    break;
                /* Arm64 uses SWP.  x86 keeps a plain swap of the same width. */
                if (inst->call_name && strcmp(inst->call_name, "__fakecc_swp") == 0
                    && inst->call_nargs >= 2) {
                    int w = inst->width;
                    if (w != 1 && w != 2 && w != 4 && w != 8) w = 4;
                    int fp = (w == 4 || w == 8) && inst->call_args[1] >= 0
                             && value_is_float_class(fn, inst->call_args[1]) == 1;
                    if (fp) {
                        ensure_reg(&out->text, inst->call_args[0], REG_RCX, ra);
                        emit_push_r(&out->text, REG_RCX);
                        ensure_reg_xmm(&out->text, inst->call_args[1], 0,
                                       ra_xmm, gp_spill_area);
                        if (w == 8) emit_movq_gp_xmm(&out->text, REG_RDX, 0);
                        else emit_movd_gp_xmm(&out->text, REG_RDX, 0);
                        emit_pop_r(&out->text, REG_RCX);
                        emit_load_via_ptr(&out->text, REG_RAX, REG_RCX, w, 1);
                        emit_store_via_ptr(&out->text, REG_RCX, REG_RDX, w);
                        if (w == 8) emit_movq_xmm_gp(&out->text, 0, REG_RAX);
                        else emit_movd_xmm_gp(&out->text, 0, REG_RAX);
                        if (inst->dst >= 0 && value_is_float_class(fn, inst->dst) == 1) {
                            int nbytes = w;
                            if (dr >= 0 && dr != 0)
                                emit_xmm_copy(&out->text, dr, 0, nbytes);
                            else if (dr < 0)
                                spill_if_needed_xmm(&out->text, inst->dst, 0,
                                                    ra_xmm, gp_spill_area);
                        }
                        break;
                    }
                    ensure_reg(&out->text, inst->call_args[0], REG_RCX, ra);
                    emit_push_r(&out->text, REG_RCX);
                    ensure_reg(&out->text, inst->call_args[1], REG_RAX, ra);
                    emit_push_r(&out->text, REG_RAX);
                    emit_pop_r(&out->text, REG_RDX);
                    emit_pop_r(&out->text, REG_RCX);
                    emit_load_via_ptr(&out->text, REG_RAX, REG_RCX, w, inst->is_unsigned);
                    emit_store_via_ptr(&out->text, REG_RCX, REG_RDX, w);
                    if (inst->dst >= 0) {
                        if (dr >= 0) {
                            if (dr != REG_RAX) emit_mov_rr(&out->text, dr, REG_RAX);
                        } else {
                            spill_if_needed(&out->text, inst->dst, REG_RAX, ra);
                        }
                    }
                    break;
                }
                /* Arm64 uses LDADD.  x86 adds in a register and stores the low bytes. */
                if (inst->call_name && inst->call_nargs >= 2 &&
                    (strcmp(inst->call_name, "__fakecc_ldadd") == 0 ||
                     strcmp(inst->call_name, "__fakecc_ldclr") == 0 ||
                     strcmp(inst->call_name, "__fakecc_ldeor") == 0 ||
                     strcmp(inst->call_name, "__fakecc_ldset") == 0)) {
                    int w = inst->width;
                    if (w != 1 && w != 2 && w != 4 && w != 8) w = 4;
                    ensure_reg(&out->text, inst->call_args[0], REG_RCX, ra);
                    emit_push_r(&out->text, REG_RCX);
                    ensure_reg(&out->text, inst->call_args[1], REG_RAX, ra);
                    emit_push_r(&out->text, REG_RAX);
                    emit_pop_r(&out->text, REG_RDX);
                    emit_pop_r(&out->text, REG_RCX);
                    emit_load_via_ptr(&out->text, REG_RAX, REG_RCX, w, inst->is_unsigned);
                    if (strcmp(inst->call_name, "__fakecc_ldclr") == 0)
                        emit_and_rr(&out->text, REG_RDX, REG_RAX);
                    else if (strcmp(inst->call_name, "__fakecc_ldeor") == 0)
                        emit_bitxor_rr(&out->text, REG_RDX, REG_RAX);
                    else if (strcmp(inst->call_name, "__fakecc_ldset") == 0)
                        emit_or_rr(&out->text, REG_RDX, REG_RAX);
                    else
                        emit_add_rr(&out->text, REG_RDX, REG_RAX);
                    emit_store_via_ptr(&out->text, REG_RCX, REG_RDX, w);
                    if (inst->dst >= 0) {
                        if (dr >= 0) {
                            if (dr != REG_RAX) emit_mov_rr(&out->text, dr, REG_RAX);
                        } else {
                            spill_if_needed(&out->text, inst->dst, REG_RAX, ra);
                        }
                    }
                    break;
                }
                /* Float add/sub retries with CAS on arm64.  x86 does one
                 * plain load, add, and store so a single thread matches. */
                if (inst->call_name && strcmp(inst->call_name, "__fakecc_fadd") == 0
                    && inst->call_nargs >= 2) {
                    int w = inst->width;
                    if (w != 4 && w != 8) w = 4;
                    int sub = (inst->imm & 4) != 0;
                    int ret_new = (inst->imm & 8) != 0;
                    ensure_reg(&out->text, inst->call_args[0], REG_RCX, ra);
                    emit_push_r(&out->text, REG_RCX);
                    ensure_reg_xmm(&out->text, inst->call_args[1], 1,
                                   ra_xmm, gp_spill_area);
                    emit_pop_r(&out->text, REG_RCX);
                    emit_load_via_ptr(&out->text, REG_RAX, REG_RCX, w, 1);
                    emit_mov_rr(&out->text, REG_RDX, REG_RAX);
                    if (w == 8) emit_movq_xmm_gp(&out->text, 0, REG_RAX);
                    else emit_movd_xmm_gp(&out->text, 0, REG_RAX);
                    emit_sse_arith(&out->text, sub ? 0x5C : 0x58, 0, 1, w == 4);
                    if (w == 8) emit_movq_gp_xmm(&out->text, REG_RAX, 0);
                    else emit_movd_gp_xmm(&out->text, REG_RAX, 0);
                    emit_store_via_ptr(&out->text, REG_RCX, REG_RAX, w);
                    if (!ret_new) emit_mov_rr(&out->text, REG_RAX, REG_RDX);
                    if (w == 8) emit_movq_xmm_gp(&out->text, 0, REG_RAX);
                    else emit_movd_xmm_gp(&out->text, 0, REG_RAX);
                    if (inst->dst >= 0 && value_is_float_class(fn, inst->dst) == 1) {
                        if (dr >= 0 && dr != 0)
                            emit_xmm_copy(&out->text, dr, 0, w);
                        else if (dr < 0)
                            spill_if_needed_xmm(&out->text, inst->dst, 0,
                                                ra_xmm, gp_spill_area);
                    }
                    break;
                }
                /* Arm64 uses CAS.  x86 compares and stores the low bytes. */
                if (inst->call_name && strcmp(inst->call_name, "__fakecc_cas") == 0
                    && inst->call_nargs >= 3) {
                    int w = inst->width;
                    if (w != 1 && w != 2 && w != 4 && w != 8) w = 4;
                    ensure_reg(&out->text, inst->call_args[0], REG_RCX, ra);
                    emit_push_r(&out->text, REG_RCX);
                    ensure_reg(&out->text, inst->call_args[1], REG_RCX, ra);
                    emit_push_r(&out->text, REG_RCX);
                    if ((w == 4 || w == 8) && inst->call_args[2] >= 0
                        && value_is_float_class(fn, inst->call_args[2]) == 1) {
                        ensure_reg_xmm(&out->text, inst->call_args[2], 0,
                                       ra_xmm, gp_spill_area);
                        if (w == 8) emit_movq_gp_xmm(&out->text, REG_RAX, 0);
                        else emit_movd_gp_xmm(&out->text, REG_RAX, 0);
                    } else {
                        ensure_reg(&out->text, inst->call_args[2], REG_RAX, ra);
                    }
                    emit_push_r(&out->text, REG_RAX);
                    emit_pop_r(&out->text, REG_RDX);
                    emit_pop_r(&out->text, REG_RSI);
                    emit_pop_r(&out->text, REG_RCX);
                    emit_load_via_ptr(&out->text, REG_RAX, REG_RSI, w, 1);
                    emit_push_r(&out->text, REG_RAX);
                    emit_load_via_ptr(&out->text, REG_RAX, REG_RCX, w, 1);
                    emit_pop_r(&out->text, REG_R8);
                    emit_cmp_rr_w(&out->text, REG_RAX, REG_R8, w >= 8 ? 8 : 4);
                    size_t je = emit_jcc_rel32(&out->text, 0x84);
                    emit_store_via_ptr(&out->text, REG_RSI, REG_RAX, w);
                    emit_xor_rr(&out->text, REG_RAX);
                    size_t jmp = emit_jmp_rel32(&out->text);
                    patch_rel32(&out->text, je, out->text.len);
                    emit_store_via_ptr(&out->text, REG_RCX, REG_RDX, w);
                    emit_mov_imm(&out->text, REG_RAX, 1);
                    patch_rel32(&out->text, jmp, out->text.len);
                    if (inst->dst >= 0) {
                        if (dr >= 0) {
                            if (dr != REG_RAX) emit_mov_rr(&out->text, dr, REG_RAX);
                        } else {
                            spill_if_needed(&out->text, inst->dst, REG_RAX, ra);
                        }
                    }
                    break;
                }
                /* Expected is a value.  The result is the previous memory
                 * contents; a match stores the desired value. */
                if (inst->call_name && strcmp(inst->call_name, "__fakecc_cas_old") == 0
                    && inst->call_nargs >= 3) {
                    int w = inst->width;
                    if (w != 1 && w != 2 && w != 4 && w != 8) w = 4;
                    ensure_reg(&out->text, inst->call_args[0], REG_RCX, ra);
                    emit_push_r(&out->text, REG_RCX);
                    ensure_reg(&out->text, inst->call_args[1], REG_RCX, ra);
                    emit_push_r(&out->text, REG_RCX);
                    ensure_reg(&out->text, inst->call_args[2], REG_RAX, ra);
                    emit_push_r(&out->text, REG_RAX);
                    emit_pop_r(&out->text, REG_RDX);
                    emit_pop_r(&out->text, REG_RSI);
                    emit_pop_r(&out->text, REG_RCX);
                    emit_load_via_ptr(&out->text, REG_RAX, REG_RCX, w, 1);
                    emit_cmp_rr_w(&out->text, REG_RAX, REG_RSI, w >= 8 ? 8 : 4);
                    size_t je = emit_jcc_rel32(&out->text, 0x84);
                    size_t jmp = emit_jmp_rel32(&out->text);
                    patch_rel32(&out->text, je, out->text.len);
                    emit_store_via_ptr(&out->text, REG_RCX, REG_RDX, w);
                    patch_rel32(&out->text, jmp, out->text.len);
                    if (inst->dst >= 0) {
                        if (dr >= 0) {
                            if (dr != REG_RAX) emit_mov_rr(&out->text, dr, REG_RAX);
                        } else {
                            spill_if_needed(&out->text, inst->dst, REG_RAX, ra);
                        }
                    }
                    break;
                }
                /* __syscall(num, a0..a5) — emit a raw `syscall` instruction.
                 * Linux x86-64 syscall ABI: rax = num, args in rdi/rsi/rdx/r10/r8/r9.
                 * We use the same push-then-pop dance to load args safely. */
                if (inst->call_name && strcmp(inst->call_name, "__syscall") == 0) {
                    static const int SYS_ARG_REGS[7] = {
                        REG_RAX, REG_RDI, REG_RSI, REG_RDX,
                        REG_R10, REG_R8, REG_R9
                    };
                    int nargs = inst->call_nargs;
                    /* Push args in reverse order */
                    for (int k = 0; k < nargs; k++) {
                        ensure_reg(&out->text, inst->call_args[k], REG_RCX, ra);
                        emit_push_r(&out->text, REG_RCX);
                    }
                    /* Pop into syscall arg regs in reverse (arg 0 = num → RAX) */
                    for (int k = nargs - 1; k >= 0; k--) {
                        emit_pop_r(&out->text, SYS_ARG_REGS[k]);
                    }
                    /* Emit `syscall` — 0F 05 */
                    emit_byte(&out->text, 0x0F);
                    emit_byte(&out->text, 0x05);
                    /* Result in RAX; move to dst or spill. */
                    if (dr >= 0) {
                        if (dr != REG_RAX) emit_mov_rr(&out->text, dr, REG_RAX);
                    } else {
                        spill_if_needed(&out->text, inst->dst, REG_RAX, ra);
                    }
                    break;
                }
                if (inst->call_name && strcmp(inst->call_name, "__clone") == 0) {
                    static const int CLONE_ARG_REGS[6] = {
                        REG_RDI, /* fn */
                        REG_RSI, /* child_stack */
                        REG_RDX, /* flags */
                        REG_RCX, /* arg */
                        REG_R8,  /* tcb */
                        REG_R9   /* ctid */
                    };
                    int nargs = inst->call_nargs;
                    for (int k = 0; k < nargs && k < 6; k++) {
                        ensure_reg(&out->text, inst->call_args[k], REG_RCX, ra);
                        emit_push_r(&out->text, REG_RCX);
                    }
                    for (int k = (nargs < 6 ? nargs : 6) - 1; k >= 0; k--) {
                        emit_pop_r(&out->text, CLONE_ARG_REGS[k]);
                    }

                    /* 1. and $-16, %rsi */
                    emit_byte(&out->text, 0x48); emit_byte(&out->text, 0x83);
                    emit_byte(&out->text, 0xe6); emit_byte(&out->text, 0xf0);

                    /* 2. sub $16, %rsi */
                    emit_byte(&out->text, 0x48); emit_byte(&out->text, 0x83);
                    emit_byte(&out->text, 0xee); emit_byte(&out->text, 0x10);

                    /* 3. mov %rcx, (%rsi) -- save arg */
                    emit_byte(&out->text, 0x48); emit_byte(&out->text, 0x89);
                    emit_byte(&out->text, 0x0e);

                    /* 4. mov %rdi, 8(%rsi) -- save fn */
                    emit_byte(&out->text, 0x48); emit_byte(&out->text, 0x89);
                    emit_byte(&out->text, 0x7e); emit_byte(&out->text, 0x08);

                    /* 5. mov %rdx, %rdi -- flags to %rdi */
                    emit_byte(&out->text, 0x48); emit_byte(&out->text, 0x89);
                    emit_byte(&out->text, 0xd7);

                    /* 6. mov %r9, %rdx -- parent_tidptr = ctid */
                    emit_byte(&out->text, 0x4c); emit_byte(&out->text, 0x89);
                    emit_byte(&out->text, 0xca);

                    /* 7. mov %r9, %r10 -- child_tidptr = ctid */
                    emit_byte(&out->text, 0x4d); emit_byte(&out->text, 0x89);
                    emit_byte(&out->text, 0xca);

                    /* 8. mov $56, %eax -- sys_clone */
                    emit_byte(&out->text, 0xb8);
                    emit_byte(&out->text, 0x38); emit_byte(&out->text, 0x00);
                    emit_byte(&out->text, 0x00); emit_byte(&out->text, 0x00);

                    /* 9. syscall */
                    emit_byte(&out->text, 0x0f); emit_byte(&out->text, 0x05);

                    /* 10. test %rax, %rax */
                    emit_byte(&out->text, 0x48); emit_byte(&out->text, 0x85);
                    emit_byte(&out->text, 0xc0);

                    /* 11. jnz +15 (jump over child trampoline) */
                    emit_byte(&out->text, 0x75); emit_byte(&out->text, 0x0f);

                    /* --- Child trampoline (15 bytes) --- */
                    /* pop %rdi (arg) */
                    emit_byte(&out->text, 0x5f);
                    /* pop %rax (fn) */
                    emit_byte(&out->text, 0x58);
                    /* call *%rax */
                    emit_byte(&out->text, 0xff); emit_byte(&out->text, 0xd0);
                    /* mov %rax, %rdi (exit code) */
                    emit_byte(&out->text, 0x48); emit_byte(&out->text, 0x89);
                    emit_byte(&out->text, 0xc7);
                    /* mov $60, %eax (sys_exit) */
                    emit_byte(&out->text, 0xb8);
                    emit_byte(&out->text, 0x3c); emit_byte(&out->text, 0x00);
                    emit_byte(&out->text, 0x00); emit_byte(&out->text, 0x00);
                    /* syscall */
                    emit_byte(&out->text, 0x0f); emit_byte(&out->text, 0x05);
                    /* hlt */
                    emit_byte(&out->text, 0xf4);

                    /* --- Parent continuation --- */
                    if (dr >= 0) {
                        if (dr != REG_RAX) emit_mov_rr(&out->text, dr, REG_RAX);
                    } else {
                        spill_if_needed(&out->text, inst->dst, REG_RAX, ra);
                    }
                    break;
                }
                /* GCC integer bit builtins: keep the full name so codegen
                 * can emit lzcnt/tzcnt/popcnt/bswap instead of a libc call. */
                if (inst->call_name && strncmp(inst->call_name, "__builtin_", 10) == 0
                    && (strstr(inst->call_name, "clz") || strstr(inst->call_name, "ctz")
                        || strstr(inst->call_name, "ffs") || strstr(inst->call_name, "popcount")
                        || strstr(inst->call_name, "parity") || strstr(inst->call_name, "clrsb")
                        || strstr(inst->call_name, "bswap"))
                    /* These all read call_args[0]; `__builtin_clz()` with no
                     * argument would be a NULL dereference.  Such a call falls
                     * through to the generic path and fails at link time. */
                    && inst->call_args && inst->call_nargs > 0) {
                    const char *bn = inst->call_name;
                    int w64 = 1;
                    if (strstr(bn, "bswap16")) w64 = 0;
                    else if (strstr(bn, "bswap32")) w64 = 0;
                    else if (strstr(bn, "bswap64")) w64 = 1;
                    else {
                        size_t L = strlen(bn);
                        if (!(L >= 2 && bn[L-2] == 'l' && bn[L-1] == 'l')
                            && !(L >= 1 && bn[L-1] == 'l'))
                            w64 = 0; /* 32-bit int variants */
                    }
                    ensure_reg(&out->text, inst->call_args[0], REG_RCX, ra);
                    if (!w64 && !strstr(bn, "bswap16")) {
                        /* 32-bit operand: zero-extend so high bits cannot affect lzcnt/tzcnt. */
                        emit_rex_wrb(&out->text, 0, REG_RCX, REG_RCX);
                        emit_byte(&out->text, 0x89);
                        emit_modrm(&out->text, 3, REG_RCX & 7, REG_RCX & 7);
                    }
                    if (strstr(bn, "bswap")) {
                        if (strstr(bn, "16")) {
                            emit_mov_rr(&out->text, REG_RAX, REG_RCX);
                            emit_and_imm32(&out->text, REG_RAX, 0xFFFF);
                            /* rorw $8, %ax */
                            emit_byte(&out->text, 0x66);
                            emit_byte(&out->text, 0xC1);
                            emit_modrm(&out->text, 3, 1, REG_RAX);
                            emit_byte(&out->text, 8);
                        } else {
                            emit_mov_rr(&out->text, REG_RAX, REG_RCX);
                            if (w64) emit_rex_wrb(&out->text, 1, 0, REG_RAX);
                            emit_byte(&out->text, 0x0F);
                            emit_byte(&out->text, (uint8_t)(0xC8 + (REG_RAX & 7)));
                        }
                    } else if (strstr(bn, "clrsb")) {
                        emit_mov_rr(&out->text, REG_R11, REG_RCX);
                        emit_mov_imm(&out->text, REG_RCX, w64 ? 63 : 31);
                        emit_mov_rr(&out->text, REG_RAX, REG_R11);
                        emit_rex_wrb(&out->text, w64, 0, REG_RAX);
                        emit_byte(&out->text, 0xD3);
                        emit_modrm(&out->text, 3, 7, REG_RAX & 7);
                        emit_bitxor_rr(&out->text, REG_RAX, REG_R11);
                        emit_byte(&out->text, 0xF3);
                        emit_rex_wrb(&out->text, w64, REG_RAX, REG_RAX);
                        emit_byte(&out->text, 0x0F);
                        emit_byte(&out->text, 0xBD);
                        emit_modrm(&out->text, 3, REG_RAX & 7, REG_RAX & 7);
                        emit_add_imm32(&out->text, REG_RAX, -1);
                    } else if (strstr(bn, "ffs")) {
                        emit_xor_rr(&out->text, REG_RAX);
                        emit_byte(&out->text, 0xF3);
                        emit_rex_wrb(&out->text, w64, REG_RDX, REG_RCX);
                        emit_byte(&out->text, 0x0F);
                        emit_byte(&out->text, 0xBC);
                        emit_modrm(&out->text, 3, REG_RDX & 7, REG_RCX & 7);
                        emit_add_imm32(&out->text, REG_RDX, 1);
                        emit_test_rr(&out->text, REG_RCX);
                        emit_rex_wrb(&out->text, 1, REG_RAX, REG_RDX);
                        emit_byte(&out->text, 0x0F);
                        emit_byte(&out->text, 0x45); /* cmovnz rax, rdx */
                        emit_modrm(&out->text, 3, REG_RAX & 7, REG_RDX & 7);
                    } else {
                        uint8_t opc = 0xBD; /* lzcnt */
                        if (strstr(bn, "ctz")) opc = 0xBC;
                        else if (strstr(bn, "popcount") || strstr(bn, "parity")) opc = 0xB8;
                        emit_byte(&out->text, 0xF3);
                        emit_rex_wrb(&out->text, w64, REG_RAX, REG_RCX);
                        emit_byte(&out->text, 0x0F);
                        emit_byte(&out->text, opc);
                        emit_modrm(&out->text, 3, REG_RAX & 7, REG_RCX & 7);
                        if (strstr(bn, "parity"))
                            emit_and_imm32(&out->text, REG_RAX, 1);
                    }
                    if (inst->dst >= 0) {
                        if (dr >= 0) {
                            if (dr != REG_RAX)
                                emit_mov_rr(&out->text, dr, REG_RAX);
                        } else {
                            spill_if_needed(&out->text, inst->dst, REG_RAX, ra);
                        }
                    }
                    break;
                }
                /* __builtin_ctzll(x) — count trailing zeros of a nonzero uint64.
                 * Emit `bsf rax, src` (0F BC /r): index of least significant set
                 * bit == ctz for nonzero input.  The source guarantee (_w &&)
                 * means the input is never zero, so bsf's ZF=1/undefined-dst
                 * case never occurs.  Result lands in RAX; move it to dst's
                 * home register (mirroring the generic GP result handling). */
                if (inst->call_name
                    && strcmp(inst->call_name, "__builtin_ctzll") == 0) {
                    ensure_reg(&out->text, inst->call_args[0], REG_RCX, ra);
                    emit_rex_wrb(&out->text, 1, REG_RAX, REG_RCX);
                    emit_byte(&out->text, 0x0F);
                    emit_byte(&out->text, 0xBC);
                    emit_modrm(&out->text, 3, REG_RAX & 7, REG_RCX & 7);
                    if (inst->dst >= 0) {
                        if (dr >= 0) {
                            if (dr != REG_RAX)
                                emit_mov_rr(&out->text, dr, REG_RAX);
                        } else {
                            spill_if_needed(&out->text, inst->dst, REG_RAX, ra);
                        }
                    }
                    break;
                }
                if (inst->call_name && strcmp(inst->call_name, "__builtin_apply_args") == 0) {
                    emit_builtin_apply_args(&out->text, inst, ra, dr, apply_args_rbp_off);
                    break;
                }
                if (inst->call_name && strcmp(inst->call_name, "__builtin_apply") == 0) {
                    emit_builtin_apply(&out->text, inst, ra, dr,
                                       apply_result_rbp_off, apply_saved_sp_rbp_off);
                    break;
                }
                if (inst->call_name && strcmp(inst->call_name, "__builtin_return") == 0) {
                    emit_builtin_return(&out->text, inst, ra, stack_size, cs_used,
                                        fn->has_dyn_alloca);
                    break;
                }
                /* va_start / va_arg / va_end — variadic builtins.  They are named
                 * calls (call_name set) and operate on the va_list pointer that
                 * was passed as call_args[0].  Dispatch by name. */
                if (inst->call_name && strcmp(inst->call_name, "va_start") == 0) {
                    emit_va_start(&out->text, inst, ra, va_gp_offset,
                                  va_fp_offset, va_overflow_off, -frame_down);
                    break;
                }
                if (inst->call_name && strcmp(inst->call_name, "va_end") == 0) {
                    /* va_end is a no-op for our ABI. */
                    break;
                }
                if (inst->call_name && strcmp(inst->call_name, "va_arg") == 0) {
                    emit_va_arg(&out->text, fn, inst, ra, ra_xmm, gp_spill_area, ld_off);
                    break;
                }
                int nargs = inst->call_nargs;
                /* SysV AMD64 assigns the first 6 INTEGER-class args to
                 * RDI,RSI,RDX,RCX,R8,R9 and the first 8 SSE-class (float/
                 * double) args to XMM0-XMM7, the two counters independent.
                 * Args that don't fit in their class's registers are passed
                 * on the stack at [rsp+8], [rsp+16], ...  Decide each arg's
                 * target up front. */
                /* call_nargs is bounded by IR_CALL_MAX_ARGS (ir.h:81); clamp as
                 * a guard so a raised cap can't overflow these stack arrays. */
                if (nargs > IR_CALL_MAX_ARGS) nargs = IR_CALL_MAX_ARGS;
                int *target_reg = NULL;
                int *target_is_xmm = NULL;
                if (nargs > 0) {
                    target_reg = malloc((size_t)nargs * sizeof(int));
                    target_is_xmm = malloc((size_t)nargs * sizeof(int));
                    if (!target_reg || !target_is_xmm) {
                        fprintf(stderr, "fakecc: OOM\n");
                        exit(1);
                    }
                }
                int n_gp = 0, n_xmm = 0, n_stack = 0;
                int *arg_slot = NULL;
                int *arg_nslots = NULL;
                if (nargs > 0) {
                    arg_slot = malloc((size_t)nargs * sizeof(int));
                    arg_nslots = malloc((size_t)nargs * sizeof(int));
                    if (!arg_slot || !arg_nslots) {
                        fprintf(stderr, "fakecc: OOM\n");
                        exit(1);
                    }
                }
                int max_al = 16;
                for (int k = 0; k < nargs; k++) {
                    int is_ld = value_is_ld(fn, inst->call_args[k]);
                    int is_float = !is_ld && value_is_float_class(fn, inst->call_args[k]);
                    int vecb = is_float ? value_vec_bytes(fn, inst->call_args[k]) : 0;
                    unsigned char flags = (inst->call_arg_on_stack && inst->call_arg_on_stack[k])
                                          ? inst->call_arg_on_stack[k] : 0;
                    int on_stack = flags & CALL_ARG_STACK;
                    int force_stack = on_stack || is_ld;
                    int pal = flags_stack_align(flags, vecb, is_ld ? 16 : 0);
                    int blob_n = call_arg_blob_bytes(inst, k);
                    arg_slot[k] = -1;
                    arg_nslots[k] = 0;
                    if (force_stack) {
                        int nslots = blob_n ? (blob_n + 7) / 8 : vec_nslots(vecb, is_ld);
                        if (nslots < 1) nslots = 1;
                        stack_idx_align(&n_stack, pal);
                        if (pal > max_al) max_al = pal;
                        target_reg[k] = -1;
                        target_is_xmm[k] = 0;
                        arg_slot[k] = n_stack;
                        arg_nslots[k] = nslots;
                        n_stack += nslots;
                    } else if (is_float) {
                        if (n_xmm < 8) {
                            target_reg[k] = n_xmm; /* XMM0..7 */
                            target_is_xmm[k] = 1;
                            n_xmm++;
                        } else {
                            int nslots = vec_nslots(vecb, 0);
                            stack_idx_align(&n_stack, pal);
                            if (pal > max_al) max_al = pal;
                            target_reg[k] = -1;
                            target_is_xmm[k] = 0;
                            arg_slot[k] = n_stack;
                            arg_nslots[k] = nslots;
                            n_stack += nslots;
                        }
                    } else {
                        if (n_gp < 6) {
                            target_reg[k] = SYSV_ARG_REGS[n_gp++];
                            target_is_xmm[k] = 0;
                        } else {
                            target_reg[k] = -1;
                            target_is_xmm[k] = 0;
                            arg_slot[k] = n_stack;
                            arg_nslots[k] = 1;
                            n_stack++;
                        }
                    }
                }
                int need_pad = 0;
                int aligned_call = (max_al >= 32 && n_stack > 0);
                int stack_bytes = n_stack * 8;
                if (aligned_call) {
                    stack_idx_align(&n_stack, max_al);
                    stack_bytes = n_stack * 8;
                    int slack = max_al;
                    /* Realign via rbp so we do not clobber GP arg homes
                     * (R11 previously held an outgoing eightbyte).  Restore
                     * the same way after the call.  A live VLA/alloca sits
                     * below the fixed frame, so save the current rsp and
                     * allocate from there instead of lea from rbp. */
                    if (fn->has_dyn_alloca) {
                        emit_store_spill(&out->text, REG_RSP,
                                         aligned_call_saved_sp_rbp_off);
                        emit_sub_rsp_imm32(&out->text, stack_bytes + slack);
                        emit_and_imm32(&out->text, REG_RSP, (int32_t)-max_al);
                    } else {
                        emit_lea_rbp(&out->text, REG_RSP,
                                     -(frame_down + stack_bytes + slack));
                        emit_and_imm32(&out->text, REG_RSP, (int32_t)-max_al);
                    }
                    for (int k = 0; k < nargs; k++) {
                        if (target_reg[k] >= 0) continue;
                        int off = 8 * arg_slot[k];
                        if (value_is_ld(fn, inst->call_args[k])) {
                            emit_ld_load(&out->text, inst->call_args[k], ld_off);
                            emit_x87_fstpt_disp(&out->text, REG_RSP, off);
                        } else if (call_arg_blob_bytes(inst, k)) {
                            emit_memcpy_to_addr(&out->text, REG_RSP, off,
                                                inst->call_args[k], ra,
                                                call_arg_blob_bytes(inst, k));
                        } else if (value_is_float_class(fn, inst->call_args[k])) {
                            int vb = value_vec_bytes(fn, inst->call_args[k]);
                            ensure_reg_xmm(&out->text, inst->call_args[k],
                                           XMM_SCRATCH0, ra_xmm, gp_spill_area);
                            if (vb >= 64)
                                emit_evex_vmovups_mem(&out->text, XMM_SCRATCH0,
                                                      REG_RSP, off, 1);
                            else if (vb >= 32)
                                emit_vmovups_mem(&out->text, XMM_SCRATCH0,
                                                 REG_RSP, off, 1);
                            else if (vb >= 16)
                                emit_movups_store_disp(&out->text, REG_RSP,
                                                       XMM_SCRATCH0, off);
                            else
                                emit_sse_store_disp(&out->text, REG_RSP,
                                                    XMM_SCRATCH0, off, 0);
                        } else {
                            ensure_reg(&out->text, inst->call_args[k],
                                       REG_RAX, ra);
                            emit_store_rsp_off(&out->text, REG_RAX, off);
                        }
                    }
                } else {
                /* Alignment: at call time rsp must be 16-aligned.  The reg-arg
                 * dance is balanced (pushes == pops), so only the nstack
                 * pushes shift alignment.  Pad iff nstack is odd. */
                need_pad = (n_stack & 1);
                if (need_pad) emit_sub_rsp_imm32(&out->text, 8);

                /* Push stack-passed args right-to-left, inserting 8-byte
                 * holes so 16-aligned types sit on 16-byte boundaries. */
                for (int slot = n_stack - 1; slot >= 0; ) {
                    int found = -1;
                    for (int k = 0; k < nargs; k++) {
                        if (target_reg[k] >= 0) continue;
                        if (arg_slot[k] <= slot
                            && slot < arg_slot[k] + arg_nslots[k]) {
                            found = k;
                            break;
                        }
                    }
                    if (found < 0) {
                        emit_sub_rsp_imm32(&out->text, 8);
                        slot--;
                        continue;
                    }
                    if (slot != arg_slot[found] + arg_nslots[found] - 1) {
                        emit_sub_rsp_imm32(&out->text, 8);
                        slot--;
                        continue;
                    }
                    if (value_is_ld(fn, inst->call_args[found])) {
                        emit_ld_load(&out->text, inst->call_args[found], ld_off);
                        emit_sub_rsp_imm32(&out->text, 16);
                        emit_x87_fstpt_rsp(&out->text);
                        slot -= 2;
                    } else if (call_arg_blob_bytes(inst, found)) {
                        int bn = call_arg_blob_bytes(inst, found);
                        int ns = arg_nslots[found];
                        emit_sub_rsp_imm32(&out->text, ns * 8);
                        emit_memcpy_to_addr(&out->text, REG_RSP, 0,
                                            inst->call_args[found], ra, bn);
                        slot -= ns;
                    } else if (value_is_float_class(fn, inst->call_args[found])) {
                        int vb = value_vec_bytes(fn, inst->call_args[found]);
                        ensure_reg_xmm(&out->text, inst->call_args[found],
                                       XMM_SCRATCH0, ra_xmm, gp_spill_area);
                        emit_xmm_push(&out->text, XMM_SCRATCH0, vb);
                        slot -= vec_nslots(vb, 0);
                    } else {
                        ensure_reg(&out->text, inst->call_args[found], REG_RAX, ra);
                        emit_push_r(&out->text, REG_RAX);
                        slot--;
                    }
                }
                }
                free(arg_slot);
                free(arg_nslots);

                /* Place register arguments straight into their ABI homes.
                 * gcc -O0 emits one mov per arg; the old code push/pop-danced
                 * EVERY register arg whenever ANY two homes formed a rotate.
                 * Schedule the moves as a register permutation: a mov is
                 * emitted when its target holds no other pending source;
                 * genuine cycles get one push/pop each.
                 *
                 * An indirect callee is an extra GP node targeting R11:
                 * R11 is in the allocatable register set, so the callee can
                 * be homed in an arg target (and an arg can be homed in R11).
                 * Scheduling it with the args makes both clobber hazards
                 * explicit instead of loading it up front. */
                {
                    int has_callee = (inst->call_name == NULL);
                    int nn = 0;
                    for (int k = 0; k < nargs; k++)
                        if (target_reg[k] >= 0) nn++;
                    if (has_callee) nn++;
                    int *node_tgt  = xmalloc((size_t)(nn > 0 ? nn : 1) * sizeof(int));
                    char *node_xmm = xmalloc((size_t)(nn > 0 ? nn : 1));
                    int *node_val  = xmalloc((size_t)(nn > 0 ? nn : 1) * sizeof(int));
                    /* home: >=0 register, -1 spilled (reload into target),
                     * -2 pushed by cycle break (pop into target), -3 done.
                     * The allocator marks spills with a negative color (-2,
                     * not just REG_NONE), so normalize anything <0 to -1. */
                    int *node_home = xmalloc((size_t)(nn > 0 ? nn : 1) * sizeof(int));
                    int n = 0;
                    for (int k = 0; k < nargs; k++) {
                        if (target_reg[k] < 0) continue; /* stack-passed */
                        node_tgt[n] = target_reg[k];
                        node_xmm[n] = (char)target_is_xmm[k];
                        node_val[n] = inst->call_args[k];
                        if (target_is_xmm[k]) {
                            int h = (ra_xmm && inst->call_args[k] >= 0
                                     && inst->call_args[k] < ra_xmm->num_values)
                                    ? ra_xmm->reg[inst->call_args[k]] : -1;
                            node_home[n] = (h >= 0 && h < 16) ? h : -1;
                        } else {
                            node_home[n] = gp_home(ra, inst->call_args[k]);
                        }
                        n++;
                    }
                    if (has_callee) {
                        node_tgt[n] = REG_R11;
                        node_xmm[n] = 0;
                        node_val[n] = inst->call_callee;
                        node_home[n] = gp_home(ra, inst->call_callee);
                        n++;
                    }
                    int npending = n;
                    while (npending > 0) {
                        int moved = 0;
                        for (int k = 0; k < n && !moved; k++) {
                            if (node_home[k] == -3) continue;
                            int t = node_tgt[k];
                            int blocked = 0;
                            for (int m = 0; m < n; m++) {
                                if (m == k || node_home[m] == -3
                                    || node_xmm[m] != node_xmm[k])
                                    continue;
                                if (node_home[m] >= 0 && node_home[m] == t) {
                                    blocked = 1;
                                    break;
                                }
                            }
                            if (blocked) continue;
                            if (node_home[k] == -1) {
                                if (node_xmm[k])
                                    ensure_reg_xmm(&out->text, node_val[k], t,
                                                   ra_xmm, gp_spill_area);
                                else
                                    ensure_reg(&out->text, node_val[k], t, ra);
                            } else if (node_home[k] == -2) {
                                if (node_xmm[k])
                                    emit_xmm_pop(&out->text, t,
                                                 value_vec_bytes(fn, node_val[k]));
                                else
                                    emit_pop_r(&out->text, t);
                            } else if (node_home[k] != t) {
                                if (node_xmm[k])
                                    emit_xmm_copy(&out->text, t, node_home[k],
                                                  value_vec_bytes(fn, node_val[k]));
                                else
                                    emit_mov_rr(&out->text, t, node_home[k]);
                            }
                            node_home[k] = -3;
                            npending--;
                            moved = 1;
                        }
                        if (moved) continue;
                        /* Pure permutation cycle: save one blocked source and
                         * let the greedy pass drain the rest of the cycle. */
                        int picked = -1;
                        for (int k = 0; k < n; k++) {
                            if (node_home[k] >= 0) { picked = k; break; }
                        }
                        if (picked < 0) break; /* defensive: no infinite loop */
                        if (node_xmm[picked])
                            emit_xmm_push(&out->text, node_home[picked],
                                          value_vec_bytes(fn, node_val[picked]));
                        else
                            emit_push_r(&out->text, node_home[picked]);
                        node_home[picked] = -2;
                    }
                    free(node_tgt);
                    free(node_xmm);
                    free(node_val);
                    free(node_home);
                }
                /* AL = number of vector registers used (ABI requirement for
                 * variadic callees; harmless otherwise). */
                emit_byte(&out->text, 0xB0 | REG_RAX); /* mov al, imm8 */
                emit_byte(&out->text, (uint8_t)n_xmm);

                if (inst->call_name) {
                    /* Direct named call: emit call rel32 with a cross-function patch. */
                    size_t poff = emit_call_rel32(&out->text);
                    size_t aft = out->text.len;
                    if (num_call_patches >= cap_call_patches) {
                        cap_call_patches = cap_call_patches ? cap_call_patches * 2 : 8;
                        call_patches = xrealloc(call_patches,
                                                 cap_call_patches * sizeof(CallPatch));
                    }
                    call_patches[num_call_patches].patch_off = poff;
                    call_patches[num_call_patches].callee = xstrdup(inst->call_name);
                    call_patches[num_call_patches].after_off = aft;
                    num_call_patches++;
                    if (want_debug && dbg_func_idx >= 0) {
                        /* Record DW_TAG_call_site so DW_OP_entry_value can
                         * recover outer-frame parameter values. */
                        int entry_dwarf[64];
                        for (int p = 0; p < nparams && p < 64; p++) {
                            if (arrive_reg[p] < 0) entry_dwarf[p] = -1;
                            else if (arrive_is_xmm[p])
                                entry_dwarf[p] = 17 + arrive_reg[p];
                            else
                                entry_dwarf[p] = reg_to_dwarf(arrive_reg[p]);
                        }
                        DebugCallSite cs;
                        memset(&cs, 0, sizeof(cs));
                        cs.call_pc = aft - 5; /* E8 + rel32 */
                        cs.return_pc = aft;
                        cs.callee_name = inst->call_name
                                         ? xstrdup(inst->call_name) : NULL;
                        /* Count register-passed GP/XMM args only. */
                        int ncs = 0;
                        for (int k = 0; k < nargs; k++)
                            if (target_reg[k] >= 0) ncs++;
                        if (ncs > 0) {
                            cs.params = xmalloc((size_t)ncs * sizeof(DebugCallSiteParam));
                            cs.num_params = (size_t)ncs;
                            int pi = 0;
                            for (int k = 0; k < nargs; k++) {
                                if (target_reg[k] < 0) continue;
                                DebugCallSiteParam *cp = &cs.params[pi++];
                                memset(cp, 0, sizeof(*cp));
                                if (target_is_xmm[k])
                                    cp->dwarf_reg = 17 + target_reg[k];
                                else
                                    cp->dwarf_reg = reg_to_dwarf(target_reg[k]);
                                Buffer cv; buffer_init(&cv);
                                if (!synth_call_value(fn, inst->call_args[k], j,
                                                      entry_dwarf, nparams,
                                                      &cv, 0)
                                    || cv.len == 0) {
                                    /* Fallback: the ABI register at the call
                                     * (best-effort; may fail for outer frames
                                     * when the reg is caller-saved). gdb 8.2
                                     * requires DW_OP_reg in a call-value
                                     * expression to carry DW_OP_piece. */
                                    if (cp->dwarf_reg >= 0 && cp->dwarf_reg <= 31) {
                                        cv_u8(&cv, (uint8_t)(CV_OP_reg0 + cp->dwarf_reg));
                                        cv_u8(&cv, 0x93); /* DW_OP_piece */
                                        cv_uleb(&cv, 4);  /* int is 4 bytes */
                                    }
                                }
                                cp->value_expr_len = cv.len;
                                if (cv.len) {
                                    cp->value_expr = xmalloc(cv.len);
                                    memcpy(cp->value_expr, cv.data, cv.len);
                                }
                                buffer_free(&cv);
                            }
                        }
                        emit_module_add_dbg_call_site(out, dbg_func_idx, &cs);
                        debug_call_site_release(&cs);
                    }
                } else {
                    /* Indirect call: the callee is already in R11.  Emit
                     * `call *%r11` (FF /2). */
                    size_t call_pc = out->text.len;
                    emit_indirect_call(&out->text, REG_R11);
                    size_t aft = out->text.len;
                    if (want_debug && dbg_func_idx >= 0) {
                        int entry_dwarf[64];
                        for (int p = 0; p < nparams && p < 64; p++) {
                            if (arrive_reg[p] < 0) entry_dwarf[p] = -1;
                            else if (arrive_is_xmm[p])
                                entry_dwarf[p] = 17 + arrive_reg[p];
                            else
                                entry_dwarf[p] = reg_to_dwarf(arrive_reg[p]);
                        }
                        DebugCallSite cs;
                        memset(&cs, 0, sizeof(cs));
                        cs.call_pc = call_pc;
                        cs.return_pc = aft;
                        cs.callee_name = NULL;
                        int ncs = 0;
                        for (int k = 0; k < nargs; k++)
                            if (target_reg[k] >= 0) ncs++;
                        if (ncs > 0) {
                            cs.params = xmalloc((size_t)ncs * sizeof(DebugCallSiteParam));
                            cs.num_params = (size_t)ncs;
                            int pi = 0;
                            for (int k = 0; k < nargs; k++) {
                                if (target_reg[k] < 0) continue;
                                DebugCallSiteParam *cp = &cs.params[pi++];
                                memset(cp, 0, sizeof(*cp));
                                if (target_is_xmm[k])
                                    cp->dwarf_reg = 17 + target_reg[k];
                                else
                                    cp->dwarf_reg = reg_to_dwarf(target_reg[k]);
                                Buffer cv; buffer_init(&cv);
                                if (!synth_call_value(fn, inst->call_args[k], j,
                                                      entry_dwarf, nparams,
                                                      &cv, 0)
                                    || cv.len == 0) {
                                    /* Fallback: the ABI register. gdb 8.2
                                     * requires DW_OP_reg in a call-value
                                     * expression to carry DW_OP_piece. */
                                    if (cp->dwarf_reg >= 0 && cp->dwarf_reg <= 31) {
                                        cv_u8(&cv, (uint8_t)(CV_OP_reg0 + cp->dwarf_reg));
                                        cv_u8(&cv, 0x93); /* DW_OP_piece */
                                        cv_uleb(&cv, 4);  /* int is 4 bytes */
                                    }
                                }
                                cp->value_expr_len = cv.len;
                                if (cv.len) {
                                    cp->value_expr = xmalloc(cv.len);
                                    memcpy(cp->value_expr, cv.data, cv.len);
                                }
                                buffer_free(&cv);
                            }
                        }
                        emit_module_add_dbg_call_site(out, dbg_func_idx, &cs);
                        debug_call_site_release(&cs);
                    }
                }

                /* Tear down stack args + padding. */
                if (aligned_call) {
                    if (fn->has_dyn_alloca)
                        emit_load_spill(&out->text, REG_RSP,
                                        aligned_call_saved_sp_rbp_off);
                    else
                        emit_lea_rbp(&out->text, REG_RSP, -frame_down);
                } else {
                    int cleanup = n_stack * 8 + (need_pad ? 8 : 0);
                    if (cleanup > 0) emit_add_rsp_imm32(&out->text, cleanup);
                }

                if (inst->x87_pair && inst->a >= 0) {
                    /* X87 aggregate: st0 (16-byte) or st0/st1 (32-byte
                     * `_Complex long double`).  Store into the caller slot. */
                    ensure_reg(&out->text, inst->a, REG_RCX, ra);
                    emit_x87_fstptRCX(&out->text);
                    if (inst->imm != 16) {
                        emit_add_imm32(&out->text, REG_RCX, 16);
                        emit_x87_fstptRCX(&out->text);
                    }
                } else {

                /* Multi-eightbyte aggregate return: SysV assigns INTEGER
                 * eightbytes to RAX then RDX and SSE eightbytes to XMM0
                 * then XMM1, independently.  Save hi first so capturing lo
                 * cannot clobber it. */
                int ret_lo_f = (inst->dst >= 0) && value_is_float_class(fn, inst->dst);
                int ret_hi_f = (inst->b >= 0) && value_is_float_class(fn, inst->b);
                int hi_src_gp = -1, hi_src_xmm = -1;
                if (inst->b >= 0) {
                    if (ret_lo_f && ret_hi_f) hi_src_xmm = 1;      /* XMM1 */
                    else if (!ret_lo_f && ret_hi_f) hi_src_xmm = 0; /* XMM0 */
                    else if (ret_lo_f && !ret_hi_f) hi_src_gp = REG_RAX;
                    else hi_src_gp = REG_RDX;
                }

                if (inst->b >= 0) {
                    if (hi_src_xmm >= 0) {
                        emit_sub_rsp_imm32(&out->text, 8);
                        emit_sse_store_rsp(&out->text, hi_src_xmm);
                    } else {
                        emit_push_r(&out->text, hi_src_gp);
                    }
                }

                if (inst->dst >= 0 && value_is_ld(fn, inst->dst)) {
                    /* long double result comes back in st0 (SysV ld ABI).  fstpt
                     * it into dst's slot.  Clobbers RCX.  Void: dst == -1. */
                    emit_ld_store(&out->text, inst->dst, ld_off);
                } else if (inst->dst >= 0 && value_is_float_class(fn, inst->dst)) {
                    /* Float result comes back in XMM0 (SysV float ABI).  Move
                     * it to dst's XMM home, or spill.  Void: dst == -1. */
                    if (dr >= 0 && dr != 0)
                        emit_xmm_copy(&out->text, dr, 0, value_vec_bytes(fn, inst->dst));
                    else if (dr < 0)
                        spill_if_needed_xmm(&out->text, inst->dst, 0,
                                            ra_xmm, gp_spill_area);
                } else if (inst->dst >= 0) {
                    /* Result is in RAX; move to dst home (or spill).  A void
                     * call has dst == -1 (no result) — leave RAX alone. */
                    if (dr >= 0) {
                        if (dr != REG_RAX) emit_mov_rr(&out->text, dr, REG_RAX);
                    } else {
                        spill_if_needed(&out->text, inst->dst, REG_RAX, ra);
                    }
                }

                if (inst->b >= 0) {
                    if (hi_src_xmm >= 0) {
                        int br = (ra_xmm && inst->b < ra_xmm->num_values)
                                 ? ra_xmm->reg[inst->b] : -1;
                        if (br >= 0) {
                            emit_sse_load_rsp(&out->text, br);
                            emit_add_rsp_imm32(&out->text, 8);
                        } else {
                            emit_sse_load_rsp(&out->text, XMM_SCRATCH0);
                            emit_add_rsp_imm32(&out->text, 8);
                            spill_if_needed_xmm(&out->text, inst->b,
                                                XMM_SCRATCH0, ra_xmm,
                                                gp_spill_area);
                        }
                    } else {
                        int br = (ra && inst->b < ra->num_values)
                                 ? ra->reg[inst->b] : -1;
                        if (br >= 0)
                            emit_pop_r(&out->text, br);
                        else {
                            emit_pop_r(&out->text, REG_RCX);
                            spill_if_needed(&out->text, inst->b, REG_RCX, ra);
                        }
                    }
                }
                } /* !x87_pair */
                free(target_reg);
                free(target_is_xmm);
                break;
            }

            case IR_RETURN: {
                /* Void function: bare `return;` — no value in a.
                 * SysV multi-eightbyte returns: INTEGER eightbytes go to
                 * RAX then RDX; SSE eightbytes go to XMM0 then XMM1. */
                if (inst->x87_pair && inst->a >= 0) {
                    /* 16-byte X87: fldt once into st0.  32-byte complex:
                     * load imag first so the subsequent fldt of real
                     * leaves st0=real, st1=imag. */
                    ensure_reg(&out->text, inst->a, REG_RCX, ra);
                    if (inst->imm == 16) {
                        emit_x87_fldtRCX(&out->text);
                    } else {
                        emit_add_imm32(&out->text, REG_RCX, 16);
                        emit_x87_fldtRCX(&out->text);
                        emit_add_imm32(&out->text, REG_RCX, -16);
                        emit_x87_fldtRCX(&out->text);
                    }
                } else if (inst->a != -1 && inst->b != -1) {
                    int a_f = value_is_float_class(fn, inst->a);
                    int b_f = value_is_float_class(fn, inst->b);
                    if (!a_f && !b_f) {
                        /* Both INTEGER: stage hi in R11 so loading lo into
                         * RAX cannot clobber a value that lived in RDX. */
                        ensure_reg(&out->text, inst->b, REG_R11, ra);
                        ensure_reg(&out->text, inst->a, REG_RAX, ra);
                        if (REG_R11 != REG_RDX)
                            emit_mov_rr(&out->text, REG_RDX, REG_R11);
                    } else if (a_f && b_f) {
                        ensure_reg_xmm(&out->text, inst->b, XMM_SCRATCH0,
                                       ra_xmm, gp_spill_area);
                        ensure_reg_xmm(&out->text, inst->a, 0, ra_xmm,
                                       gp_spill_area); /* XMM0 */
                        if (XMM_SCRATCH0 != 1)
                            emit_sse_mov_rr(&out->text, 1, XMM_SCRATCH0);
                    } else if (a_f) {
                        /* lo SSE, hi INTEGER — hi is the first INTEGER (RAX). */
                        ensure_reg(&out->text, inst->b, REG_RAX, ra);
                        ensure_reg_xmm(&out->text, inst->a, 0, ra_xmm,
                                       gp_spill_area);
                    } else {
                        /* lo INTEGER, hi SSE — hi is the first SSE (XMM0). */
                        ensure_reg_xmm(&out->text, inst->b, 0, ra_xmm,
                                       gp_spill_area); /* XMM0 */
                        ensure_reg(&out->text, inst->a, REG_RAX, ra);
                    }
                } else if (inst->a != -1) {
                    if (value_is_ld(fn, inst->a))
                        emit_ld_load(&out->text, inst->a, ld_off); /* → st0 */
                    else if (value_is_float_class(fn, inst->a))
                        ensure_reg_xmm(&out->text, inst->a, 0, ra_xmm,
                                       gp_spill_area); /* XMM0 */
                    else
                        ensure_reg(&out->text, inst->a, REG_RAX, ra);
                }
                emit_epilogue(&out->text, stack_size, cs_used, fn->has_dyn_alloca);
                break;
            }

            case IR_FADD:
            case IR_FSUB:
            case IR_FMUL:
            case IR_FDIV: {
                if (value_is_ld(fn, inst->dst)) {
                    /* long double arithmetic: ld a, ld b, f**p, fstpt dst.
                     * x87 stack ends empty (the `*p` pop + fstpt clear st0). */
                    emit_ld_load(&out->text, inst->a, ld_off);
                    emit_ld_load(&out->text, inst->b, ld_off);
                    int op;
                    switch (inst->op) {
                    /* load a then b (st0=b,st1=a); the pop-op does st1 = st1 op st0
                     * (faddp/fmulp) or st1 = st0 op st1 (fsubrp/fdivp) — chosen
                     * so the result is a+b, a-b, a*b, a/b respectively. */
                    case IR_FADD: op = 0xC1; break; /* DE C1: faddp */
                    case IR_FSUB: op = 0xE9; break; /* DE E9: fsubrp */
                    case IR_FMUL: op = 0xC9; break; /* DE C9: fmulp */
                    default:      op = 0xF9; break; /* DE F9: fdivp */
                    }
                    emit_x87_arith_pop(&out->text, op);
                    emit_ld_store(&out->text, inst->dst, ld_off);
                    break;
                }
                /* dst = a op b, all float.  Stage both operands into XMM
                 * scratch regs (never-allocated scratch0/1), compute into
                 * scratch0, then move to dst home or spill. */
                int is_float = (inst->width == 4);
                ensure_reg_xmm(&out->text, inst->a, XMM_SCRATCH0, ra_xmm,
                               gp_spill_area);
                ensure_reg_xmm(&out->text, inst->b, XMM_SCRATCH1, ra_xmm,
                               gp_spill_area);
                int op;
                switch (inst->op) {
                case IR_FADD: op = 0x58; break;
                case IR_FSUB: op = 0x5C; break;
                case IR_FMUL: op = 0x59; break;
                default:      op = 0x5E; break; /* FDIV */
                }
                emit_sse_arith(&out->text, op, XMM_SCRATCH0, XMM_SCRATCH1,
                               is_float);
                if (dr >= 0 && dr != XMM_SCRATCH0)
                    emit_sse_mov_rr(&out->text, dr, XMM_SCRATCH0);
                spill_if_needed_xmm(&out->text, inst->dst, XMM_SCRATCH0,
                                    ra_xmm, gp_spill_area);
                break;
            }

            case IR_FCMP: {
                if (value_is_ld(fn, inst->a) || value_is_ld(fn, inst->b)) {
                    /* long double comparison — matches GCC's fcomip sequence.
                     * fcomip computes st0-st1, sets flags, pops st0.  We always
                     * read with seta/setae/sete/setne and pick direction via
                     * load order (LT/LE load a first; GT/GE load b first): see
                     * GCC cmplt/cmpgt.  Zero RDX before (xor clobbers flags). */
                    int is_lt_le = (inst->is_unsigned == 0 || inst->is_unsigned == 1);
                    emit_xor_rr(&out->text, REG_RDX);
                    if (is_lt_le) {
                        emit_ld_load(&out->text, inst->a, ld_off); /* st0=a */
                        emit_ld_load(&out->text, inst->b, ld_off); /* st0=b,st1=a */
                    } else {
                        emit_ld_load(&out->text, inst->b, ld_off); /* st0=b */
                        emit_ld_load(&out->text, inst->a, ld_off); /* st0=a,st1=b */
                    }
                    if (inst->is_unsigned >= 4) emit_xor_rr(&out->text, REG_RAX);
                    emit_x87_fcomip(&out->text);
                    emit_byte(&out->text, 0xDD); emit_byte(&out->text, 0xD8); /* fstp %st(0) */
                    uint8_t cc;
                    switch (inst->is_unsigned) {
                    case 0: cc = 0x97; break; /* LT: a<b ↔ b>a → seta */
                    case 1: cc = 0x93; break; /* LE: a<=b ↔ b>=a → setae */
                    case 2: cc = 0x97; break; /* GT: a>b → seta */
                    case 3: cc = 0x93; break; /* GE: a>=b → setae */
                    case 4: cc = 0x94; break; /* EQ → sete */
                    default: cc = 0x95; break; /* NE → setne */
                    }
                    emit_setcc_r(&out->text, cc, REG_RDX);
                    /* Unordered (either operand NaN) sets ZF=PF=CF=1, which
                     * reads as equal.  The ordered relations already come out
                     * false there, but equality has to exclude PF explicitly:
                     * a == b is ZF && !PF, a != b is !ZF || PF. */
                    if (inst->is_unsigned == 4) {
                        emit_setcc_r(&out->text, 0x9B, REG_RAX); /* setnp */
                        emit_and_rr(&out->text, REG_RDX, REG_RAX);
                    } else if (inst->is_unsigned == 5) {
                        emit_setcc_r(&out->text, 0x9A, REG_RAX); /* setp */
                        emit_or_rr(&out->text, REG_RDX, REG_RAX);
                    }
                    int dr_gp = (ra && inst->dst >= 0 && inst->dst < ra->num_values)
                                ? ra->reg[inst->dst] : -1;
                    if (dr_gp >= 0 && dr_gp != REG_RDX)
                        emit_mov_rr(&out->text, dr_gp, REG_RDX);
                    spill_if_needed(&out->text, inst->dst,
                                    dr_gp >= 0 ? dr_gp : REG_RDX, ra);
                    break;
                }
                /* dst (int) = (a op b) ? 1 : 0, a/b float.  The comparison is
                 * encoded in inst->is_unsigned: 0=LT 1=LE 2=GT 3=GE 4=EQ
                 * 5=NE.  ucomi sets flags; setcc produces the 0/1 result. */
                int is_float = (inst->width == 4);
                ensure_reg_xmm(&out->text, inst->a, XMM_SCRATCH0, ra_xmm,
                               gp_spill_area);
                ensure_reg_xmm(&out->text, inst->b, XMM_SCRATCH1, ra_xmm,
                               gp_spill_area);
                /* Map the FCMP ordering to the matching setcc byte.  Only
                 * seta/setae read false when the compare is unordered (CF and
                 * ZF are both set then), so a<b and a<=b are evaluated as
                 * b>a and b>=a with the ucomi operands swapped rather than
                 * with setb/setbe, which NaN would satisfy. */
                uint8_t cc;
                int swap = 0;
                switch (inst->is_unsigned) {
                case 0: cc = 0x97; swap = 1; break; /* LT: a<b ↔ b>a → seta */
                case 1: cc = 0x93; swap = 1; break; /* LE: a<=b ↔ b>=a → setae */
                case 2: cc = 0x97; break; /* GT → seta */
                case 3: cc = 0x93; break; /* GE → setae */
                case 4: cc = 0x94; break; /* EQ → sete */
                default: cc = 0x95; break; /* NE → setne */
                }
                /* setcc needs RDX (a GP scratch not used by the SSE staging).
                 * Result is int (width 4); dr is a GP register.  Zero RDX BEFORE
                 * the compare: ucomi sets the flags, and setcc reads them — a
                 * post-compare xor would clobber ZF/CF and break the result. */
                emit_xor_rr(&out->text, REG_RDX);
                if (inst->is_unsigned >= 4) emit_xor_rr(&out->text, REG_RAX);
                if (swap)
                    emit_sse_ucomi(&out->text, XMM_SCRATCH1, XMM_SCRATCH0,
                                   is_float);
                else
                    emit_sse_ucomi(&out->text, XMM_SCRATCH0, XMM_SCRATCH1,
                                   is_float);
                emit_setcc_r(&out->text, cc, REG_RDX);
                /* Unordered (either operand NaN) sets ZF=PF=CF=1, which reads
                 * as equal.  The ordered relations already come out false
                 * there, but equality has to exclude PF explicitly: a == b is
                 * ZF && !PF, a != b is !ZF || PF. */
                if (inst->is_unsigned == 4) {
                    emit_setcc_r(&out->text, 0x9B, REG_RAX); /* setnp */
                    emit_and_rr(&out->text, REG_RDX, REG_RAX);
                } else if (inst->is_unsigned == 5) {
                    emit_setcc_r(&out->text, 0x9A, REG_RAX); /* setp */
                    emit_or_rr(&out->text, REG_RDX, REG_RAX);
                }
                if (dr >= 0 && dr != REG_RDX)
                    emit_mov_rr(&out->text, dr, REG_RDX);
                spill_if_needed(&out->text, inst->dst,
                                dr >= 0 ? dr : REG_RDX, ra);
                break;
            }

            case IR_SITOFP: {
                /* imm carries the source integer width and is_unsigned its
                 * signedness; inst->width is the destination float width. */
                int src_w = inst->imm ? (int)inst->imm : 4;
                int src_u = inst->is_unsigned;
                int dst_is_f = (inst->width == 4);
                if (value_is_ld(fn, inst->dst)) {
                    /* int→long double.  Load int into a GP reg, store into dst's
                     * slot, fild from the slot (pushes ld to st0), fstpt the
                     * result back into dst's slot.  Clobbers RAX/RCX/RDX. */
                    /* ensure_reg handles register / spill / out-of-range
                     * operands; indexing ra->spill_slot[inst->a] here read
                     * past the map when the operand has no slot. */
                    if (ra && (inst->a < 0 || inst->a >= ra->num_values))
                        fprintf(stderr, "PROBE: SITOFP a=%d num_values=%d ra=%p\n", inst->a, ra->num_values, (void*)ra);
                    ensure_reg(&out->text, inst->a, REG_RAX, ra);
                    emit_ld_from_gp_int(&out->text, REG_RAX, inst->dst, ld_off,
                                        src_w, src_u);
                    break;
                }
                /* dst (float) = (float)a.  Load a into a GP scratch. */
                ensure_reg(&out->text, inst->a, REG_RAX, ra);
                if (src_u && src_w == 8) {
                    /* cvtsi2s is signed.  Above 2^63 convert u/2 (rounding
                     * the odd bit into it so no low bit is lost) and double
                     * the result. */
                    emit_test_rr(&out->text, REG_RAX);
                    size_t j_big = emit_jcc_rel32(&out->text, 0x88); /* js */
                    emit_sse_cvtsi2s(&out->text, XMM_SCRATCH0, REG_RAX, 1, dst_is_f);
                    size_t j_done = emit_jmp_rel32(&out->text);
                    patch_rel32(&out->text, j_big, out->text.len);
                    emit_mov_rr(&out->text, REG_RDX, REG_RAX);
                    emit_and_imm32(&out->text, REG_RDX, 1);
                    emit_shr_imm8(&out->text, REG_RAX, 1);
                    emit_or_rr(&out->text, REG_RAX, REG_RDX);
                    emit_sse_cvtsi2s(&out->text, XMM_SCRATCH0, REG_RAX, 1, dst_is_f);
                    emit_sse_arith(&out->text, 0x58 /* add */, XMM_SCRATCH0,
                                   XMM_SCRATCH0, dst_is_f);
                    patch_rel32(&out->text, j_done, out->text.len);
                } else {
                    if (src_u && src_w < 8)
                        mask_to_width(&out->text, REG_RAX, src_w, 1);
                    emit_sse_cvtsi2s(&out->text, XMM_SCRATCH0, REG_RAX,
                                      src_w == 8 || src_u, dst_is_f);
                }
                if (dr >= 0 && dr != XMM_SCRATCH0)
                    emit_sse_mov_rr(&out->text, dr, XMM_SCRATCH0);
                spill_if_needed_xmm(&out->text, inst->dst, XMM_SCRATCH0,
                                    ra_xmm, gp_spill_area);
                break;
            }

            case IR_FPTOSI: {
                if (value_is_ld(fn, inst->a) && inst->is_unsigned
                    && inst->width == 8) {
                    /* long double→unsigned long.  fisttp is a signed store and
                     * yields the integer indefinite above 2^63, so split the
                     * range the same way the SSE path does.  2^63 is built as
                     * 2^62+2^62 because no immediate can hold it. */
                    int dr_gp = (ra && inst->dst >= 0 && inst->dst < ra->num_values)
                                ? ra->reg[inst->dst] : -1;
                    int dst_reg = (dr_gp >= 0) ? dr_gp : REG_RAX;
                    emit_sub_rsp_imm32(&out->text, 16);
                    emit_mov_imm64(&out->text, REG_RAX,
                                   (int64_t)0x4000000000000000LL);
                    emit_store_rsp_off(&out->text, REG_RAX, 0);
                    emit_lea_rsp0(&out->text, REG_RCX);
                    emit_byte(&out->text, 0xDF);
                    emit_modrm(&out->text, 0, 5, REG_RCX & 7); /* fild qword */
                    emit_byte(&out->text, 0xD8); emit_byte(&out->text, 0xC0); /* fadd st0,st0 */
                    emit_ld_load(&out->text, inst->a, ld_off); /* st0=a, st1=2^63 */
                    emit_byte(&out->text, 0xDB); emit_byte(&out->text, 0xF1); /* fcomi st0,st1 */
                    size_t j_small = emit_jcc_rel32(&out->text, 0x82); /* jb */
                    emit_byte(&out->text, 0xD8); emit_byte(&out->text, 0xE1); /* fsub st0,st1 */
                    emit_lea_rsp0(&out->text, REG_RCX);
                    emit_byte(&out->text, 0xDD);
                    emit_modrm(&out->text, 0, 1, REG_RCX & 7); /* fisttp qword */
                    emit_load_base_off(&out->text, dst_reg, REG_RCX, 0);
                    emit_mov_imm64(&out->text, REG_RDX,
                                   (int64_t)0x8000000000000000ULL);
                    emit_bitxor_rr(&out->text, dst_reg, REG_RDX);
                    size_t j_done = emit_jmp_rel32(&out->text);
                    patch_rel32(&out->text, j_small, out->text.len);
                    emit_lea_rsp0(&out->text, REG_RCX);
                    emit_byte(&out->text, 0xDD);
                    emit_modrm(&out->text, 0, 1, REG_RCX & 7); /* fisttp qword */
                    emit_load_base_off(&out->text, dst_reg, REG_RCX, 0);
                    patch_rel32(&out->text, j_done, out->text.len);
                    emit_byte(&out->text, 0xDD); emit_byte(&out->text, 0xD8); /* fstp st0 (drop 2^63) */
                    emit_add_rsp_imm32(&out->text, 16);
                    if (dr_gp >= 0 && dr_gp != dst_reg)
                        emit_mov_rr(&out->text, dr_gp, dst_reg);
                    spill_if_needed(&out->text, inst->dst, dst_reg, ra);
                    break;
                }
                if (value_is_ld(fn, inst->a)) {
                    /* long double→int.  Load ld a into st0, sub a small scratch
                     * on the stack, fisttp dword to it, read into a GP reg.  Uses
                     * 8 bytes below rsp (SysV red zone is safe here).  Clobbers
                     * RAX/RCX/RDX. */
                    emit_ld_load(&out->text, inst->a, ld_off); /* fldt [a] → st0 */
                    emit_sub_rsp_imm32(&out->text, 8);
                    /* lea rcx, [rsp] */
                    emit_byte(&out->text, 0x48); emit_byte(&out->text, 0x8D);
                    emit_modrm(&out->text, 0, REG_RCX & 7, 4);
                    emit_byte(&out->text, 0x24);
                    /* GCC: signed/narrow uses fisttp m32 (indefinite 0x80000000
                     * out of range); unsigned 32/64 uses m64 so 3e9L fits. */
                    int use64 = inst->is_unsigned ? (inst->width >= 4)
                                                  : (inst->width >= 8);
                    if (use64) {
                        emit_byte(&out->text, 0xDD);
                        emit_modrm(&out->text, 0, 1, REG_RCX & 7); /* fisttp m64 */
                    } else {
                        emit_byte(&out->text, 0xDB);
                        emit_modrm(&out->text, 0, 1, REG_RCX & 7); /* fisttp m32 */
                    }
                    int dr_gp = (ra && inst->dst >= 0 && inst->dst < ra->num_values)
                                ? ra->reg[inst->dst] : -1;
                    int dst_reg = (dr_gp >= 0) ? dr_gp : REG_RAX;
                    emit_rex_wrb(&out->text, use64 ? 1 : 0, dst_reg, REG_RSP);
                    emit_byte(&out->text, 0x8B);
                    emit_modrm(&out->text, 0, dst_reg & 7, 4);
                    emit_byte(&out->text, 0x24);
                    emit_add_rsp_imm32(&out->text, 8);
                    if (inst->width < 8)
                        mask_to_width(&out->text, dst_reg, inst->width,
                                      inst->is_unsigned);
                    if (dr_gp >= 0 && dr_gp != dst_reg)
                        emit_mov_rr(&out->text, dr_gp, dst_reg);
                    spill_if_needed(&out->text, inst->dst,
                                    dr_gp >= 0 ? dr_gp : REG_RAX, ra);
                    break;
                }
                /* dst (int) = (int)a, a = float.  The source float width is
                 * inst->imm (inst->width is the target int width).  Load a
                 * into XMM scratch, cvtsd2si/cvtss2si into a GP scratch. */
                ensure_reg_xmm(&out->text, inst->a, XMM_SCRATCH0, ra_xmm,
                               gp_spill_area);
                int src_float = (inst->imm == 4);
                if (inst->is_unsigned && inst->width == 8) {
                    /* unsigned long: cvttss/sd2si is a *signed* conversion and
                     * saturates to 0x8000000000000000 above 2^63, so split the
                     * range: below 2^63 convert directly, at or above it
                     * subtract 2^63, convert, and put the top bit back. */
                    int64_t bias = src_float ? (int64_t)0x5F000000
                                             : (int64_t)0x43E0000000000000LL;
                    emit_float_const(&out->text, XMM_SCRATCH1, bias);
                    emit_sse_ucomi(&out->text, XMM_SCRATCH0, XMM_SCRATCH1, src_float);
                    size_t j_big = emit_jcc_rel32(&out->text, 0x83); /* jae */
                    if (src_float)
                        emit_sse_cvtss2si(&out->text, REG_RAX, XMM_SCRATCH0, 1);
                    else
                        emit_sse_cvtsd2si(&out->text, REG_RAX, XMM_SCRATCH0, 1);
                    size_t j_done = emit_jmp_rel32(&out->text);
                    patch_rel32(&out->text, j_big, out->text.len);
                    emit_sse_arith(&out->text, 0x5C /* sub */, XMM_SCRATCH0,
                                   XMM_SCRATCH1, src_float);
                    if (src_float)
                        emit_sse_cvtss2si(&out->text, REG_RAX, XMM_SCRATCH0, 1);
                    else
                        emit_sse_cvtsd2si(&out->text, REG_RAX, XMM_SCRATCH0, 1);
                    emit_mov_imm64(&out->text, REG_RCX, (int64_t)0x8000000000000000ULL);
                    emit_bitxor_rr(&out->text, REG_RAX, REG_RCX);
                    patch_rel32(&out->text, j_done, out->text.len);
                } else {
                    /* GCC x86: signed (and narrow unsigned) float→int uses
                     * 32-bit cvtt*, which yields the integer indefinite
                     * 0x80000000 for out-of-range values.  Unsigned 32-bit
                     * uses a 64-bit convert so 3e9f survives. */
                    int to64 = inst->is_unsigned ? (inst->width >= 4)
                                                 : (inst->width >= 8);
                    if (src_float)
                        emit_sse_cvtss2si(&out->text, REG_RAX, XMM_SCRATCH0, to64);
                    else
                        emit_sse_cvtsd2si(&out->text, REG_RAX, XMM_SCRATCH0, to64);
                    mask_to_width(&out->text, REG_RAX, inst->width,
                                  inst->is_unsigned);
                }
                if (dr >= 0 && dr != REG_RAX)
                    emit_mov_rr(&out->text, dr, REG_RAX);
                spill_if_needed(&out->text, inst->dst,
                                dr >= 0 ? dr : REG_RAX, ra);
                break;
            }

            case IR_FPEXT: {
                if (value_is_ld(fn, inst->dst)) {
                    /* double (or float)→long double.  Load the SSE value into
                     * XMM scratch, store to dst's slot, fld qword/dword from
                     * the slot (pushes ld to st0), fstpt back to dst's slot.
                     * The store and the fld must agree on the source width:
                     * a float source occupies only the low 4 bytes. */
                    int src_is_f32 = value_width_of(fn, inst->a) == 4;
                    ensure_reg_xmm(&out->text, inst->a, XMM_SCRATCH0, ra_xmm,
                                   gp_spill_area);
                    emit_ld_addr(&out->text, REG_RCX, ld_off[inst->dst]);
                    emit_sse_store_via_ptr(&out->text, REG_RCX, XMM_SCRATCH0,
                                           src_is_f32);
                    /* fld dword/qword [rcx] */
                    emit_byte(&out->text, src_is_f32 ? 0xD9 : 0xDD);
                    emit_modrm(&out->text, 0, 0, REG_RCX & 7);
                    emit_x87_fstptRCX(&out->text); /* fstpt [rcx] */
                    break;
                }
                /* dst (double) = (double)(float)a.  Widen float→double. */
                ensure_reg_xmm(&out->text, inst->a, XMM_SCRATCH0, ra_xmm,
                               gp_spill_area);
                emit_sse_cvtss2sd(&out->text, XMM_SCRATCH0, XMM_SCRATCH0);
                if (dr >= 0 && dr != XMM_SCRATCH0)
                    emit_sse_mov_rr(&out->text, dr, XMM_SCRATCH0);
                spill_if_needed_xmm(&out->text, inst->dst, XMM_SCRATCH0,
                                    ra_xmm, gp_spill_area);
                break;
            }

            case IR_FPTRUNC: {
                if (value_is_ld(fn, inst->a)) {
                    /* long double→double (or float).  fldt ld source slot into
                     * st0, fstp qword into the source slot's first 8 bytes (the
                     * source is consumed, so reusing it as scratch is safe),
                     * load into XMM scratch → dst home. */
                    int dst_is_f32 = inst->width == 4;
                    emit_ld_load(&out->text, inst->a, ld_off); /* fldt → st0 */
                    emit_ld_addr(&out->text, REG_RCX, ld_off[inst->a]);
                    /* fstp dword/qword [rcx] — narrowing happens here, so the
                     * store size must match the destination's float width. */
                    emit_byte(&out->text, dst_is_f32 ? 0xD9 : 0xDD);
                    emit_modrm(&out->text, 0, 3, REG_RCX & 7);
                    emit_sse_load_via_ptr(&out->text, XMM_SCRATCH0, REG_RCX,
                                          dst_is_f32);
                    if (dr >= 0 && dr != XMM_SCRATCH0)
                        emit_sse_mov_rr(&out->text, dr, XMM_SCRATCH0);
                    spill_if_needed_xmm(&out->text, inst->dst, XMM_SCRATCH0,
                                        ra_xmm, gp_spill_area);
                    break;
                }
                /* dst (float) = (float)(double)a.  Narrow double→float. */
                ensure_reg_xmm(&out->text, inst->a, XMM_SCRATCH0, ra_xmm,
                               gp_spill_area);
                emit_sse_cvtsd2ss(&out->text, XMM_SCRATCH0, XMM_SCRATCH0);
                if (dr >= 0 && dr != XMM_SCRATCH0)
                    emit_sse_mov_rr(&out->text, dr, XMM_SCRATCH0);
                spill_if_needed_xmm(&out->text, inst->dst, XMM_SCRATCH0,
                                    ra_xmm, gp_spill_area);
                break;
            }

            case IR_DBG_VALUE:
                /* Handled before the switch; produces no machine code. */
                break;

            } /* switch */
        } /* for insts */

        /* Fallthrough guard: if the function body can fall off the end without
         * hitting a terminator (return or branch), append an epilogue + ret so
         * execution does not tumble into the next function.  This happens for
         * void functions with no explicit `return;` (e.g. `void f() {}`), and
         * for empty-bodied functions whose IR carries no instructions at all. */
        int needs_ret = 0;
        if (fn->insts.len == 0) {
            needs_ret = 1;
        } else {
            IROpcode last = fn->insts.data[fn->insts.len - 1].op;
            if (last != IR_RETURN && last != IR_BR && last != IR_CBR)
                needs_ret = 1;
        }
        if (needs_ret)
            emit_epilogue(&out->text, stack_size, cs_used, fn->has_dyn_alloca);

        if (want_debug && dbg_func_idx >= 0) {
            size_t fn_end_pc = out->text.len;
            emit_module_dbg_func_end(out, dbg_func_idx, fn_end_pc,
                                     prologue_end_pc);
            for (size_t di = 0; di < fn->num_dbg_vars; di++) {
                const IRDebugVar *idv = &fn->dbg_vars[di];
                DebugVar dv;
                memset(&dv, 0, sizeof(dv));
                dv.name = idv->name;
                dv.file = (char *)idv->loc.file;
                dv.line = idv->loc.line;
                dv.kind = idv->kind == IR_DBG_PARAM ? DBG_VAR_PARAM : DBG_VAR_LOCAL;
                dv.type_tag = ir_type_tag(idv->type_kind, idv->is_bool);
                dv.width = idv->width;
                dv.is_unsigned = idv->is_unsigned;
                dv.array_len = idv->array_len;
                dv.type_name = idv->struct_tag;
                dv.struct_size = idv->struct_size;
                if (idv->num_members > 0) {
                    dv.members = xmalloc((size_t)idv->num_members *
                                         sizeof(DebugMember));
                    dv.num_members = (size_t)idv->num_members;
                    for (int mi = 0; mi < idv->num_members; mi++) {
                        const IRDebugMember *im = &idv->members[mi];
                        DebugMember *dm = &dv.members[mi];
                        memset(dm, 0, sizeof(*dm));
                        dm->name = im->name;
                        dm->offset = im->offset;
                        dm->bit_width = im->bit_width;
                        dm->bit_offset = im->bit_offset;
                        dm->type_tag = ir_type_tag(im->type_kind, im->is_bool);
                        dm->width = im->width;
                        dm->is_unsigned = im->is_unsigned;
                    }
                }
                dv.alloca_ssa = idv->alloca_ssa;
                dv.param_idx = idv->param_idx;
                dv.entry_dwarf_reg = -1;
                dv.loc_kind = DBG_LOC_NONE;

                if (idv->alloca_ssa >= 0 && idv->alloca_ssa < fn->next_value_id
                    && alloca_off[idv->alloca_ssa] != 0) {
                    /* Still in memory (array, address-taken, or -O0). */
                    if (idv->kind == IR_DBG_PARAM && idv->param_idx >= 0
                        && idv->param_idx < nparams
                        && arrive_reg[idv->param_idx] >= 0) {
                        /* -O0: the parameter arrives in a register but is
                         * spilled to the stack slot during the prologue.
                         * A debugger can stop inside the prologue (before the
                         * spill), so emit a location list — register during
                         * the prologue, stack slot from prologue_end_pc on. */
                        int p = idv->param_idx;
                        int dreg = arrive_is_xmm[p] ? (17 + arrive_reg[p])
                                                     : reg_to_dwarf(arrive_reg[p]);
                        dbg_var_add_range(&dv, start_offset, prologue_end_pc,
                                          DBG_LOC_REG, 0, dreg);
                        dbg_var_add_range(&dv, prologue_end_pc, fn_end_pc,
                                          DBG_LOC_FBREG,
                                          alloca_off[idv->alloca_ssa], -1);
                    } else {
                        /* Arrives on the stack (or not a param): one stack
                         * slot for the whole lifetime. */
                        dv.loc_kind = DBG_LOC_FBREG;
                        dv.rbp_offset = alloca_off[idv->alloca_ssa];
                    }
                } else if (idv->kind == IR_DBG_PARAM && idv->param_idx >= 0
                           && idv->param_idx < nparams) {
                    int p = idv->param_idx;
                    if (arrive_reg[p] < 0) {
                        dv.loc_kind = DBG_LOC_FBREG;
                        dv.rbp_offset = stack_off[p];
                        dv.entry_dwarf_reg = -1;
                    } else if (arrive_is_xmm[p]) {
                        dv.loc_kind = DBG_LOC_REG;
                        dv.dwarf_reg = 17 + arrive_reg[p];
                        dv.entry_dwarf_reg = dv.dwarf_reg;
                    } else {
                        dv.loc_kind = DBG_LOC_REG;
                        dv.dwarf_reg = reg_to_dwarf(arrive_reg[p]);
                        dv.entry_dwarf_reg = dv.dwarf_reg;
                    }
                }

                /* Promoted into SSA: the variable has no single home, so turn
                 * the markers left by mem2reg into a location list.  Each
                 * marker's location holds until the next marker for the same
                 * variable, or the end of the function.  A parameter starts
                 * out in its ABI arrival register, before any marker. */
                if (dv.loc_kind == DBG_LOC_REG && idv->kind == IR_DBG_PARAM) {
                    size_t first = fn_end_pc;
                    for (size_t k = 0; k < num_dbg_obs; k++) {
                        if (dbg_obs[k].var == (int)di) { first = dbg_obs[k].pc; break; }
                    }
                    if (first > start_offset && num_dbg_obs > 0)
                        dbg_var_add_range(&dv, start_offset, first, dv.loc_kind,
                                          dv.rbp_offset, dv.dwarf_reg);
                }
                for (size_t k = 0; k < num_dbg_obs; k++) {
                    if (dbg_obs[k].var != (int)di) continue;
                    size_t end = fn_end_pc;
                    for (size_t k2 = k + 1; k2 < num_dbg_obs; k2++) {
                        if (dbg_obs[k2].var == (int)di) { end = dbg_obs[k2].pc; break; }
                    }
                    DebugLocKind lk;
                    int off, dreg;
                    value_home(fn, dbg_obs[k].value, gp_spill_area,
                               &lk, &off, &dreg);
                    dbg_var_add_range(&dv, dbg_obs[k].pc, end, lk, off, dreg);
                }
                emit_module_add_dbg_var(out, dbg_func_idx, &dv);
                free(dv.ranges);
                free(dv.members);
            }
        }
        free(dbg_obs);

        /* ---- Apply label patches ---- */
        for (size_t pi = 0; pi < num_patches; pi++) {
            Patch *p = &patches[pi];
            if (p->label < 0 || p->label >= nlabels ||
                label_off[p->label] == (size_t)-1) {
                fprintf(stderr, "fakecc: unresolved label %d in codegen\n", p->label);
                exit(1);
            }
            int64_t rel = (int64_t)label_off[p->label] - (int64_t)p->after_off;
            if (rel < INT32_MIN || rel > INT32_MAX) {
                fprintf(stderr, "fakecc: branch displacement out of range\n");
                exit(1);
            }
            int32_t rel32 = (int32_t)rel;
            memcpy(out->text.data + p->patch_off, &rel32, 4);
        }
        free(patches);
        free(label_off);
        free(alloca_off);
        free(ld_off);
        free(arrive_reg);
        free(arrive_is_xmm);
        free(stack_off);
        free(ssa_def);
        free(skip_body);
        free(param_home_off);
        free(needed);

        size_t fn_size = out->text.len - start_offset;
        uint8_t binding = fn->is_static ? 0 /* STB_LOCAL */ : 1 /* STB_GLOBAL */;
        emit_module_add_symbol(out, fn->name, binding, 2 /* STT_FUNC */,
                               (uint16_t)SECT_TEXT, start_offset, fn_size);
        if (fn->is_constructor) {
            int fsym = emit_module_find_symbol(out, fn->name);
            size_t slot = out->init_array.len;
            uint64_t z = 0;
            buffer_append(&out->init_array, (const char *)&z, 8);
            emit_module_add_data_reloc(out, slot, R_X86_64_64, fsym, 0);
            out->data_relocs[out->num_data_relocs - 1].shndx = SECT_INIT_ARRAY;
            size_t n = out->init_array.len / 8;
            out->init_prio = realloc(out->init_prio, n * sizeof(int));
            if (!out->init_prio) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
            out->init_prio[n - 1] = fn->ctor_prio ? fn->ctor_prio : INIT_PRIO_DEFAULT;
        }
        if (fn->is_destructor) {
            int fsym = emit_module_find_symbol(out, fn->name);
            size_t slot = out->fini_array.len;
            uint64_t z = 0;
            buffer_append(&out->fini_array, (const char *)&z, 8);
            emit_module_add_data_reloc(out, slot, R_X86_64_64, fsym, 0);
            out->data_relocs[out->num_data_relocs - 1].shndx = SECT_FINI_ARRAY;
            size_t n = out->fini_array.len / 8;
            out->fini_prio = realloc(out->fini_prio, n * sizeof(int));
            if (!out->fini_prio) { fprintf(stderr, "fakecc: OOM\n"); exit(1); }
            out->fini_prio[n - 1] = fn->dtor_prio ? fn->dtor_prio : INIT_PRIO_DEFAULT;
        }
    }

    /* Pointer slots that must hold the address of another global or function
     * become R_X86_64_64 relocations in .rela.data. This runs after all globals
     * and functions are emitted so both are present in the symbol table. */
    for (size_t gi = 0; gi < ir->globals.len; gi++) {
        const IRGlobal *g = &ir->globals.data[gi];
        for (int fi = 0; fi < g->num_fixups; fi++) {
            int tsym = emit_module_find_symbol(out, g->fixups[fi].sym);
            if (tsym < 0)
                tsym = emit_module_add_undefined(out, g->fixups[fi].sym);
            emit_module_add_data_reloc(out, global_off[gi] + g->fixups[fi].offset,
                                       R_X86_64_64, tsym, g->fixups[fi].addend);
            out->data_relocs[out->num_data_relocs - 1].shndx = global_shndx[gi];
        }
    }
    free(global_off);
    free(global_shndx);

    /* ---- Resolve cross-function call patches ---- */
    for (size_t pi = 0; pi < num_call_patches; pi++) {
        CallPatch *cp = &call_patches[pi];
        /* Look up the callee among DEFINED symbols in this module.  A match
         * against an undefined symbol (forward declaration) must NOT resolve
         * here — it needs a relocation so the linker can bind it across TUs. */
        size_t target = (size_t)-1;
        for (size_t si = 0; si < out->num_syms; si++) {
            if (out->syms[si].name && strcmp(out->syms[si].name, cp->callee) == 0
                && out->syms[si].shndx != SECT_UNDEF) {
                target = out->syms[si].value;
                break;
            }
        }
        if (target == (size_t)-1) {
            /* Not a defined function — external (`extern`) call.  Record a
             * PLT32 relocation targeting the (undefined) symbol; the linker
             * resolves it to the symbol's address or a PLT entry. */
            int esym = emit_module_find_symbol(out, cp->callee);
            if (esym < 0)
                esym = emit_module_add_undefined(out, cp->callee);
            emit_module_add_reloc(out, cp->patch_off, R_X86_64_PLT32, esym, -4);
            free(cp->callee);
            continue;
        }
        int64_t rel = (int64_t)target - (int64_t)cp->after_off;
        if (rel < INT32_MIN || rel > INT32_MAX) {
            fprintf(stderr, "fakecc: call displacement out of range\n");
            exit(1);
        }
        int32_t rel32 = (int32_t)rel;
        memcpy(out->text.data + cp->patch_off, &rel32, 4);
        free(cp->callee);
    }
    free(call_patches);

    /* ---- Resolve function-address load patches ---- */
    for (size_t pi = 0; pi < num_fnaddr_patches; pi++) {
        FnAddrPatch *fp = &fnaddr_patches[pi];
        /* Look up the function in the symbol table. */
        size_t target = (size_t)-1;
        for (size_t si = 0; si < out->num_syms; si++) {
            if (out->syms[si].name && strcmp(out->syms[si].name, fp->fn_name) == 0
                && out->syms[si].shndx != SECT_UNDEF) {
                target = out->syms[si].value;
                break;
            }
        }
        if (target == (size_t)-1) {
            /* External function address — record a PC32 relocation targeting
             * the undefined symbol; the linker resolves it. */
            int esym = emit_module_find_symbol(out, fp->fn_name);
            if (esym < 0)
                esym = emit_module_add_undefined(out, fp->fn_name);
            emit_module_add_reloc(out, fp->patch_off, R_X86_64_PC32, esym, -4);
            free(fp->fn_name);
            continue;
        }
        /* lea r, [rip+disp32]: disp32 = target - (rip_next) where both are
         * absolute addresses.  target = ELF_BASE + symbol value;
         * rip_next = ELF_BASE + patch_off + 4.  The ELF_BASE cancels: */
        int64_t rel = (int64_t)target - (int64_t)(fp->patch_off + 4);
        if (rel < INT32_MIN || rel > INT32_MAX) {
            fprintf(stderr, "fakecc: function address displacement out of range\n");
            exit(1);
        }
        int32_t rel32 = (int32_t)rel;
        memcpy(out->text.data + fp->patch_off, &rel32, 4);
        free(fp->fn_name);
    }
    free(fnaddr_patches);

    /* ---- Resolve module aliases (variables and functions) ---- */
    for (size_t ai = 0; ai < ir->aliases.len; ai++) {
        const IRAlias *al = &ir->aliases.data[ai];
        int tsym = emit_module_find_symbol(out, al->target);
        if (tsym >= 0) {
            EmitSymbol target_sym = out->syms[tsym];
            int existing = emit_module_find_symbol(out, al->name);
            if (existing >= 0 && out->syms[existing].shndx == SECT_UNDEF) {
                out->syms[existing].binding = al->is_static ? 0 : 1;
                out->syms[existing].type = target_sym.type;
                out->syms[existing].shndx = target_sym.shndx;
                out->syms[existing].value = target_sym.value;
                out->syms[existing].size = target_sym.size;
            } else {
                emit_module_add_symbol(out, al->name, al->is_static ? 0 : 1,
                                       target_sym.type, target_sym.shndx,
                                       target_sym.value, target_sym.size);
            }
        }
    }
}
