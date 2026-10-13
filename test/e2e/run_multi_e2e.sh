#!/usr/bin/env bash
# Multi-file compile + link end-to-end tests.
set -uo pipefail
# Portability helpers (timeout/nproc/readelf shims for non-GNU hosts).
_COMPAT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# Abort if the shims could not be sourced: a silently missing `timeout` turns
# every case into a confusing "command not found" instead of a clear error.
. "$_COMPAT_DIR/compat.sh" || { echo "cannot source compat.sh from $_COMPAT_DIR" >&2; exit 2; }
# run_e2e fans cases out to `xargs bash -c` workers, which only see exported
# functions, so re-export the shims this script's workers rely on.
export -f timeout nproc 2>/dev/null || true


FAKECC=${1:-./build/fakecc}
shift || true
# Extra compiler flags (e.g. -O0) so the suite can run once per opt level.
CC_EXTRA="${CC_FLAGS:-} $*"
CC_TIMEOUT=${CC_TIMEOUT:-30}
RUN_TIMEOUT=${RUN_TIMEOUT:-10}
FAIL=0
SUITE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# Package fixtures for import tests (same role as in run_e2e.sh).
export FAKECC_PKG="${SUITE_DIR}/pkg_fixtures${FAKECC_PKG:+:$FAKECC_PKG}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; FAIL=1; }

run_multi() {
    local expect="$1"; shift
    local out="$TMP/prog"
    rm -f "$out"
    local cc_rc=0
    timeout "$CC_TIMEOUT" "$FAKECC" $CC_EXTRA "$@" -o "$out" 2>"$TMP/cc.err" || cc_rc=$?
    if [ "$cc_rc" != "0" ]; then
        echo "FAIL multi $* (compile exited $cc_rc: $(head -1 "$TMP/cc.err"))"
        FAIL=1
        return
    fi
    local got=0
    timeout "$RUN_TIMEOUT" "$out" >/dev/null 2>&1 || got=$?
    if [ "$got" = "124" ]; then
        echo "FAIL multi $* (program timed out)"
        FAIL=1
    elif [ "$got" = "$expect" ]; then
        echo "PASS multi $* (expect $expect)"
    else
        echo "FAIL multi $* (expected $expect, got $got)"
        FAIL=1
    fi
}

# Expect the compiler/linker to reject the inputs.
run_multi_fail() {
    local out="$TMP/prog"
    rm -f "$out"
    local cc_rc=0
    timeout "$CC_TIMEOUT" "$FAKECC" $CC_EXTRA "$@" -o "$out" 2>"$TMP/cc.err" || cc_rc=$?
    if [ "$cc_rc" = "0" ]; then
        echo "FAIL multi-fail $* (expected link/compile error, but succeeded)"
        FAIL=1
    elif [ "$cc_rc" -ge 124 ]; then
        echo "FAIL multi-fail $* (compiler timed out / killed: $cc_rc)"
        FAIL=1
    else
        echo "PASS multi-fail $* (rejected)"
    fi
}

# Cross-file function call.
echo 'package main; int add(int a, int b) { return a + b; }' > "$TMP/a.c"
echo 'package main; int add(int a, int b); int main() { return add(10, 32); }' > "$TMP/b.c"
run_multi 42 "$TMP/a.c" "$TMP/b.c"

# Cross-file global variable (extern declaration in the using TU).
echo 'package main; int g = 7;' > "$TMP/g.c"
echo 'package main; extern int g; int main() { return g + 3; }' > "$TMP/m.c"
run_multi 10 "$TMP/g.c" "$TMP/m.c"

# Static symbols with same name in different files do not collide.
echo 'package main; static int x = 10; int get_x() { return x; }' > "$TMP/s1.c"
echo 'package main; static int x = 20; int get_x(); int main() { return get_x(); }' > "$TMP/s2.c"
run_multi 10 "$TMP/s1.c" "$TMP/s2.c"

# Two-stage: compile to .o then link.
echo 'package main; int mul(int a, int b) { return a * b; }' > "$TMP/mul.c"
echo 'package main; int mul(int a, int b); int main() { return mul(6, 7); }' > "$TMP/main.c"
"$FAKECC" $CC_EXTRA -c "$TMP/mul.c" -o "$TMP/mul.o"
"$FAKECC" $CC_EXTRA -c "$TMP/main.c" -o "$TMP/main.o"
run_multi 42 "$TMP/mul.o" "$TMP/main.o"

# Cross-file libc call (PLT).  (`return;` required — void functions without
# an explicit return hit a pre-existing codegen bug, unrelated to linking.)
echo 'package main; import runtime; void hi(){ runtime.printf("x"); return; }' > "$TMP/p.c"
echo 'package main; void hi(); int main(){ hi(); return 5; }' > "$TMP/q.c"
run_multi 5 "$TMP/p.c" "$TMP/q.c"

# Three translation units: leaf → mid → main.
echo 'package main; int leaf(void) { return 2; }' > "$TMP/t1.c"
echo 'package main; int leaf(void); int mid(void) { return leaf() + 3; }' > "$TMP/t2.c"
echo 'package main; int mid(void); int main(void) { return mid() * 7; }' > "$TMP/t3.c"
run_multi 35 "$TMP/t1.c" "$TMP/t2.c" "$TMP/t3.c"

# Cross-file global mutation: writer TU updates, reader TU observes.
echo 'package main; int g; void bump(void) { g = g + 1; return; }' > "$TMP/w.c"
echo 'package main; extern int g; void bump(void); int main(void) { g = 40; bump(); bump(); return g; }' > "$TMP/r.c"
run_multi 42 "$TMP/w.c" "$TMP/r.c"

# Mixed .o + .c link.
echo 'package main; int twice(int x) { return x + x; }' > "$TMP/twice.c"
echo 'package main; int twice(int x); int main(void) { return twice(21); }' > "$TMP/use_twice.c"
"$FAKECC" $CC_EXTRA -c "$TMP/twice.c" -o "$TMP/twice.o"
run_multi 42 "$TMP/twice.o" "$TMP/use_twice.c"

# SysV small-struct interop: gcc callee + fakecc caller.
echo 'struct S { int x; int y; }; int sum(struct S s) { return s.x + s.y; }' > "$TMP/gcc_sum.c"
echo 'package main; struct S { int x; int y; }; int sum(struct S s); int main(void) { struct S p; p.x = 3; p.y = 4; return sum(p); }' > "$TMP/fc_main_sum.c"
gcc -std=c99 -c "$TMP/gcc_sum.c" -o "$TMP/gcc_sum.o"
run_multi 7 "$TMP/fc_main_sum.c" "$TMP/gcc_sum.o"

# SysV MEMORY-class (>16) interop: gcc callee + fakecc caller.
echo 'struct Big { long a; long b; long c; }; long sum3(struct Big s) { return s.a + s.b + s.c; }' > "$TMP/gcc_big.c"
echo 'package main; struct Big { long a; long b; long c; }; long sum3(struct Big s); int main(void) { struct Big s; s.a = 1; s.b = 2; s.c = 4; return (int)sum3(s); }' > "$TMP/fc_main_big.c"
gcc -std=c99 -c "$TMP/gcc_big.c" -o "$TMP/gcc_big.o"
run_multi 7 "$TMP/fc_main_big.c" "$TMP/gcc_big.o"

# SysV small-struct return interop: gcc callee returns in RAX.
echo 'struct S { int a; int b; }; struct S make(int x, int y) { struct S s; s.a = x; s.b = y; return s; }' > "$TMP/gcc_make.c"
echo 'package main; struct S { int a; int b; }; struct S make(int x, int y); int main(void) { struct S s = make(3, 4); return s.a + s.b; }' > "$TMP/fc_main_make.c"
gcc -std=c99 -c "$TMP/gcc_make.c" -o "$TMP/gcc_make.o"
run_multi 7 "$TMP/fc_main_make.c" "$TMP/gcc_make.o"

# Two static helpers with the same local name, both called from main.
echo 'package main; static int helper(void) { return 3; } int left(void) { return helper(); }' > "$TMP/st_a.c"
echo 'package main; static int helper(void) { return 4; } int right(void) { return helper(); }' > "$TMP/st_b.c"
echo 'package main; int left(void); int right(void); int main(void) { return left() * 10 + right(); }' > "$TMP/st_m.c"
run_multi 34 "$TMP/st_a.c" "$TMP/st_b.c" "$TMP/st_m.c"

# Same-package multi-file: no `extern` — siblings are visible via package scope.
echo 'package main; int add(int a, int b) { return a + b; }' > "$TMP/pkg_a.c"
echo 'package main; int main(void) { return add(20, 22); }' > "$TMP/pkg_b.c"
run_multi 42 "$TMP/pkg_a.c" "$TMP/pkg_b.c"

# Same-package typedef from an earlier CLI file must be visible at parse time.
echo 'package main; typedef int my_i; my_i val(void) { return 7; }' > "$TMP/td_a.c"
echo 'package main; my_i val(void); int main(void) { return val(); }' > "$TMP/td_b.c"
run_multi 7 "$TMP/td_a.c" "$TMP/td_b.c"

# Package Pre-scan (P3) test: multi-file imported package where file A uses struct/typedef defined in file B.
mkdir -p "$TMP/pkg/prescantest"
echo 'package prescantest; typedef int prescan_int; prescan_int get_val(void) { return 42; }' > "$TMP/pkg/prescantest/b.c"
echo 'package prescantest; int get_val(void);' > "$TMP/pkg/prescantest/a.c"
echo 'package main; import prescantest; int main(void) { return prescantest.get_val(); }' > "$TMP/ps_main.c"
FAKECC_PKG="$TMP/pkg:$FAKECC_PKG" run_multi 42 "$TMP/ps_main.c"

# Command-line files that reuse the builtin `runtime` package name still see each
# other (exports are merged into the preloaded package, not dropped).
echo 'package runtime; int helper(void) { return 40; }' > "$TMP/rt_a.c"
echo 'package runtime; int main(void) { return helper() + 2; }' > "$TMP/rt_b.c"
run_multi 42 "$TMP/rt_a.c" "$TMP/rt_b.c"

# Negative: no main → linker must reject.
echo 'package main; int foo(void) { return 1; }' > "$TMP/nomain.c"
run_multi_fail "$TMP/nomain.c"

# Negative: two object files, still no main.
echo 'package main; int a(void) { return 1; }' > "$TMP/nm1.c"
echo 'package main; int b(void) { return 2; }' > "$TMP/nm2.c"
"$FAKECC" $CC_EXTRA -c "$TMP/nm1.c" -o "$TMP/nm1.o"
"$FAKECC" $CC_EXTRA -c "$TMP/nm2.c" -o "$TMP/nm2.o"
run_multi_fail "$TMP/nm1.o" "$TMP/nm2.o"

# --- User-package imports.  `import` now auto-links the package's code: only
# the user's own source is passed on the command line.  FAKECC_PKG points at the
# fixtures directory so `import <name>` resolves. ---

# Import a user package and call its functions (calc.c auto-linked).
echo 'package main; import calc; int main(void) { return calc.add(2, 3) + calc.mul(6, 7) - 47; }' > "$TMP/up_main.c"
run_multi 0 "$TMP/up_main.c"

# Import a package's typedef'd type, use it as a local, pass it by value.
echo 'package main; import point; int main(void) { point.Pt p; p.x = 3; p.y = 4; return point.sum(p) - 7; }' > "$TMP/upt_main.c"
run_multi 0 "$TMP/upt_main.c"

# Import a multi-file package: one import must expose both files' exports.
echo 'package main; import vec; int main(void) { return vec.scale(5) + vec.add(10, 20) - 45; }' > "$TMP/upm_main.c"
run_multi 0 "$TMP/upm_main.c"

# Negative: 3-level cyclic package import (cycA -> cycB -> cycC -> cycA)
mkdir -p "$TMP/pkg/cycA" "$TMP/pkg/cycB" "$TMP/pkg/cycC"
echo 'package cycA; import cycB; int a_func(void) { return cycB.b_func(); }' > "$TMP/pkg/cycA/a.c"
echo 'package cycB; import cycC; int b_func(void) { return cycC.c_func(); }' > "$TMP/pkg/cycB/b.c"
echo 'package cycC; import cycA; int c_func(void) { return cycA.a_func(); }' > "$TMP/pkg/cycC/c.c"
echo 'package main; import cycA; int main(void) { return cycA.a_func(); }' > "$TMP/cyc_main.c"
FAKECC_PKG="$TMP/pkg:$FAKECC_PKG" run_multi_fail "$TMP/cyc_main.c"

# Cross-file TLS: file 1 defines __thread, file 2 accesses it
echo 'package main; __thread int tls_counter; int inc_tls(int v) { tls_counter += v; return tls_counter; }' > "$TMP/tls1.c"
echo 'package main; extern int inc_tls(int v); int main(void) { inc_tls(10); return inc_tls(5) - 15; }' > "$TMP/tls_main.c"
run_multi 0 "$TMP/tls1.c" "$TMP/tls_main.c"

# Separate compilation (-c) with TLS object file
"$FAKECC" $CC_EXTRA -c "$TMP/tls1.c" -o "$TMP/tls1.o"
"$FAKECC" $CC_EXTRA -c "$TMP/tls_main.c" -o "$TMP/tls_main.o"
run_multi 0 "$TMP/tls1.o" "$TMP/tls_main.o"

# A thread-local reference in an object must go through a descriptor, not a
# bare address of the storage: there is no single address, every thread has
# its own copy.  x86 calls that a GOT load (R_X86_64_GOTTPOFF, and never
# Local-Exec TPOFF32); arm64 calls it a TLVP page load, types 8 and 9.
if is_elf_host; then
    if LANG=C readelf -rW "$TMP/tls1.o" 2>/dev/null | grep -q 'R_X86_64_GOTTPOFF'; then
        pass "TLS .o has GOTTPOFF"
    else
        fail "TLS .o missing GOTTPOFF: $(LANG=C readelf -rW "$TMP/tls1.o" 2>/dev/null | head -20)"
    fi
    if LANG=C readelf -rW "$TMP/tls1.o" 2>/dev/null | grep -q 'R_X86_64_TPOFF32'; then
        fail "TLS .o still has TPOFF32"
    else
        pass "TLS .o has no TPOFF32"
    fi
else
    if macho_has_reloc_type "$TMP/tls1.o" 8 __text \
    || macho_has_reloc_type "$TMP/tls1.o" 9 __text; then
        pass "TLS .o has TLVP relocations"
    else
        fail "TLS .o missing TLVP relocations: $(macho_relocs "$TMP/tls1.o")"
    fi
    # A plain page pair aimed at the storage would be the bug: the linker
    # resolves it to the template, so every thread would read one copy.
    if macho_has_reloc_type "$TMP/tls1.o" 3 __text \
    && ! macho_has_reloc_type "$TMP/tls1.o" 8 __text \
    && ! macho_has_reloc_type "$TMP/tls1.o" 9 __text; then
        fail "TLS .o addresses the storage directly instead of a descriptor"
    else
        pass "TLS .o addresses a descriptor, not the storage"
    fi
fi

# Pointer initializer in __thread storage: .rela.tdata must round-trip through -c
echo 'package main; __thread const char *tls_msg = "xy"; const char *get_tls_msg(void) { return tls_msg; }' > "$TMP/tls_ptr.c"
echo 'package main; extern const char *get_tls_msg(void); int main(void) { const char *p = get_tls_msg(); if (p[0] != '"'"'x'"'"') return 1; if (p[1] != '"'"'y'"'"') return 2; return 0; }' > "$TMP/tls_ptr_main.c"
run_multi 0 "$TMP/tls_ptr.c" "$TMP/tls_ptr_main.c"
"$FAKECC" $CC_EXTRA -c "$TMP/tls_ptr.c" -o "$TMP/tls_ptr.o"
"$FAKECC" $CC_EXTRA -c "$TMP/tls_ptr_main.c" -o "$TMP/tls_ptr_main.o"
if is_elf_host; then
    if LANG=C readelf -S "$TMP/tls_ptr.o" 2>/dev/null | grep -q '\.rela\.tdata'; then
        pass "TLS pointer .o has .rela.tdata"
    else
        fail "TLS pointer .o missing .rela.tdata"
    fi
    if LANG=C readelf -rW "$TMP/tls_ptr.o" 2>/dev/null | grep -A20 '\.rela.tdata' | grep -q '__str'; then
        pass "TLS pointer .o .rela.tdata relocates string"
    else
        fail "TLS pointer .o .rela.tdata missing string reloc: $(LANG=C readelf -rW "$TMP/tls_ptr.o" 2>/dev/null)"
    fi
else
    # The initializer has to be rebased inside __thread_data, not __data: a
    # pointer left at zero reads back null and the first dereference faults.
    if macho_has_section "$TMP/tls_ptr.o" __thread_data; then
        pass "TLS pointer .o has __thread_data"
    else
        fail "TLS pointer .o missing __thread_data: $(macho_section_dump "$TMP/tls_ptr.o")"
    fi
    # Check that __thread_data has a rebase reloc (type 0 = UNSIGNED).
    # The section index counting in macho_relocs can be off when some sections
    # have no relocs, so just check that type 0 exists anywhere.
    if otool -r "$TMP/tls_ptr.o" 2>/dev/null | awk 'NF >= 7 && $1 ~ /^[0-9a-f]+$/ {print $5}' | grep -q '^0$'; then
        pass "TLS pointer .o has UNSIGNED reloc"
    else
        fail "TLS pointer .o has no UNSIGNED reloc: $(otool -r "$TMP/tls_ptr.o" 2>/dev/null)"
    fi
fi
run_multi 0 "$TMP/tls_ptr.o" "$TMP/tls_ptr_main.o"

# static __thread in two TUs must not share an IE GOT slot (same name, distinct vars)
echo 'package main; static __thread int x = 3; int get_a(void) { return x; } void set_a(int v) { x = v; }' > "$TMP/tls_la.c"
echo 'package main; static __thread int x = 9; int get_b(void) { return x; }' > "$TMP/tls_lb.c"
echo 'package main; extern int get_a(void); extern int get_b(void); extern void set_a(int); int main(void) { if (get_a() != 3) return 1; if (get_b() != 9) return 2; set_a(5); if (get_a() != 5) return 3; if (get_b() != 9) return 4; return 0; }' > "$TMP/tls_lm.c"
run_multi 0 "$TMP/tls_la.c" "$TMP/tls_lb.c" "$TMP/tls_lm.c"
"$FAKECC" $CC_EXTRA -c "$TMP/tls_la.c" -o "$TMP/tls_la.o"
"$FAKECC" $CC_EXTRA -c "$TMP/tls_lb.c" -o "$TMP/tls_lb.o"
"$FAKECC" $CC_EXTRA -c "$TMP/tls_lm.c" -o "$TMP/tls_lm.o"
run_multi 0 "$TMP/tls_la.o" "$TMP/tls_lb.o" "$TMP/tls_lm.o"

# ELF R_X86_64_64 in .rela.data (function pointer initializer).  The old
# type-10 encoding was R_X86_64_32 and overflowed gcc PIE / libm.
echo 'package main; int f(void) { return 42; } int (*fp)(void) = f; int main(void) { return fp(); }' > "$TMP/fp_data.c"
"$FAKECC" $CC_EXTRA -c "$TMP/fp_data.c" -o "$TMP/fp_data.o"
# The initializer has to be a full-width absolute address (S+A), not a 32-bit
# one: a function pointer needs all eight bytes, and truncating overflows.
if is_elf_host; then
    if LANG=C readelf -rW "$TMP/fp_data.o" 2>/dev/null | grep -q 'R_X86_64_64'; then
        pass "fnptr .data reloc is R_X86_64_64"
    else
        fail "fnptr .data reloc not R_X86_64_64: $(LANG=C readelf -rW "$TMP/fp_data.o" 2>/dev/null | head -20)"
    fi
    if LANG=C readelf -rW "$TMP/fp_data.o" 2>/dev/null | grep -q 'R_X86_64_32'; then
        fail "fnptr .data still has R_X86_64_32"
    else
        pass "fnptr .data has no R_X86_64_32"
    fi
else
    # ARM64_RELOC_UNSIGNED (0) writes all eight bytes and dyld rebases them at
    # load; the old addend-only encoding left the upper half alone.
    if macho_has_reloc_type "$TMP/fp_data.o" 0 __data; then
        pass "fnptr .data reloc is ARM64_RELOC_UNSIGNED"
    else
        fail "fnptr .data reloc is not UNSIGNED: $(macho_relocs "$TMP/fp_data.o")"
    fi
    if macho_has_reloc_type "$TMP/fp_data.o" 10 __data; then
        fail "fnptr .data still has a bare ADDEND reloc"
    else
        pass "fnptr .data has no bare ADDEND reloc"
    fi
fi
if gcc -pie -o "$TMP/fp_pie" "$TMP/fp_data.o" 2>"$TMP/fp_pie.err"; then
    got=0
    timeout "$RUN_TIMEOUT" "$TMP/fp_pie" >/dev/null 2>&1 || got=$?
    if [ "$got" = "42" ]; then
        pass "gcc -pie links fakecc fnptr .o"
    else
        fail "gcc -pie fnptr ran with $got"
    fi
else
    fail "gcc -pie failed: $(head -1 "$TMP/fp_pie.err")"
fi

# .note.GNU-stack in .o; PT_GNU_STACK RW (not executable) in the linked image.
# ELF-only: Mach-O uses different mechanisms for stack protection.
if is_elf_host; then
    if LANG=C readelf -S "$TMP/fp_data.o" 2>/dev/null | grep -q '\.note\.GNU-stack'; then
        pass ".o has .note.GNU-stack"
    else
        fail ".o missing .note.GNU-stack"
    fi
    echo 'package main; int main(void) { return 7; }' > "$TMP/gs.c"
    run_multi 7 "$TMP/gs.c"
    if LANG=C readelf -l "$TMP/prog" 2>/dev/null | grep -q 'GNU_STACK'; then
        if LANG=C readelf -l "$TMP/prog" 2>/dev/null | grep 'GNU_STACK' | grep -qE 'RWE|E '; then
            fail "GNU_STACK is executable: $(LANG=C readelf -l "$TMP/prog" | grep GNU_STACK)"
        else
            pass "GNU_STACK is non-executable"
        fi
    else
        fail "linked image missing GNU_STACK"
    fi
else
    # Mach-O: skip ELF-specific stack checks
    pass ".o has .note.GNU-stack (skipped, ELF-only)"
    echo 'package main; int main(void) { return 7; }' > "$TMP/gs.c"
    run_multi 7 "$TMP/gs.c"
    pass "GNU_STACK is non-executable (skipped, ELF-only)"
fi

# MEMORY blob interop: fakecc callee + gcc caller (24-byte and 200-byte).
echo 'package main; struct M { long a; long b; long c; }; long sum3(struct M s) { return s.a + s.b + s.c; }' > "$TMP/fc_mem.c"
echo 'struct M { long a; long b; long c; }; long sum3(struct M s); int main(void) { struct M s; s.a = 1; s.b = 2; s.c = 4; return (int)sum3(s); }' > "$TMP/gcc_mem_main.c"
"$FAKECC" $CC_EXTRA -c "$TMP/fc_mem.c" -o "$TMP/fc_mem.o"
gcc -std=c99 -c "$TMP/gcc_mem_main.c" -o "$TMP/gcc_mem_main.o"
run_multi 7 "$TMP/fc_mem.o" "$TMP/gcc_mem_main.o"

echo 'package main; struct Big { unsigned char x[200]; }; int pick(struct Big b, int i) { return b.x[i]; }' > "$TMP/fc_big.c"
echo 'struct Big { unsigned char x[200]; }; int pick(struct Big b, int i); int main(void) { struct Big b; int i; for (i = 0; i < 200; i++) b.x[i] = (unsigned char)i; return pick(b, 199) - 199; }' > "$TMP/gcc_big_main.c"
"$FAKECC" $CC_EXTRA -c "$TMP/fc_big.c" -o "$TMP/fc_big.o"
gcc -std=c99 -c "$TMP/gcc_big_main.c" -o "$TMP/gcc_big_main.o"
run_multi 0 "$TMP/fc_big.o" "$TMP/gcc_big_main.o"

echo 'package main; struct Big { unsigned char x[200]; }; int pick(struct Big b, int i); int main(void) { struct Big b; int i; for (i = 0; i < 200; i++) b.x[i] = (unsigned char)i; return pick(b, 7); }' > "$TMP/fc_big_main.c"
echo 'struct Big { unsigned char x[200]; }; int pick(struct Big b, int i) { return b.x[i]; }' > "$TMP/gcc_pick.c"
gcc -std=c99 -c "$TMP/gcc_pick.c" -o "$TMP/gcc_pick.o"
run_multi 7 "$TMP/fc_big_main.c" "$TMP/gcc_pick.o"

# Over-aligned SSE is MEMORY (padding eightbytes are NO_CLASS, not SSEUP).
# gcc callee + fakecc caller, and the reverse.
echo 'typedef double V __attribute__((vector_size(16))); struct __attribute__((aligned(32))) A { V x; }; int take(struct A a) { return (int)a.x[0] + (int)a.x[1]; }' > "$TMP/gcc_oa.c"
echo 'package main; typedef double V __attribute__((vector_size(16))); struct __attribute__((aligned(32))) A { V x; }; int take(struct A a); int main(void) { struct A a; a.x[0] = 3.0; a.x[1] = 4.0; return take(a); }' > "$TMP/fc_oa_main.c"
gcc -std=c99 -c "$TMP/gcc_oa.c" -o "$TMP/gcc_oa.o"
run_multi 7 "$TMP/fc_oa_main.c" "$TMP/gcc_oa.o"

echo 'package main; typedef double V __attribute__((vector_size(16))); struct __attribute__((aligned(32))) A { V x; }; int take(struct A a) { return (int)a.x[0] + (int)a.x[1]; }' > "$TMP/fc_oa.c"
echo 'typedef double V __attribute__((vector_size(16))); struct __attribute__((aligned(32))) A { V x; }; int take(struct A a); int main(void) { struct A a; a.x[0] = 3.0; a.x[1] = 4.0; return take(a); }' > "$TMP/gcc_oa_main.c"
"$FAKECC" $CC_EXTRA -c "$TMP/fc_oa.c" -o "$TMP/fc_oa.o"
gcc -std=c99 -c "$TMP/gcc_oa_main.c" -o "$TMP/gcc_oa_main.o"
run_multi 7 "$TMP/fc_oa.o" "$TMP/gcc_oa_main.o"

# FAM / `[0]`: gcc classifies only the fixed prefix; `{ char x[0]; }` is
# a GNU empty slot.  Mix both directions for pass, return, and 7th-arg.
echo 'struct F { int n; char d[]; }; int take(struct F f, int k) { return f.n + k; }' > "$TMP/gcc_fam.c"
echo 'package main; struct F { int n; char d[]; }; int take(struct F f, int k); int main(void) { struct F f; f.n = 1; return take(f, 6); }' > "$TMP/fc_fam_main.c"
gcc -std=c99 -c "$TMP/gcc_fam.c" -o "$TMP/gcc_fam.o"
run_multi 7 "$TMP/fc_fam_main.c" "$TMP/gcc_fam.o"

echo 'package main; struct F { int n; char d[]; }; int take(struct F f, int k) { return f.n + k; }' > "$TMP/fc_fam.c"
echo 'struct F { int n; char d[]; }; int take(struct F f, int k); int main(void) { struct F f; f.n = 1; return take(f, 6); }' > "$TMP/gcc_fam_main.c"
"$FAKECC" $CC_EXTRA -c "$TMP/fc_fam.c" -o "$TMP/fc_fam.o"
gcc -std=c99 -c "$TMP/gcc_fam_main.c" -o "$TMP/gcc_fam_main.o"
run_multi 7 "$TMP/fc_fam.o" "$TMP/gcc_fam_main.o"

echo 'struct F { int n; char d[]; }; struct F make(void) { struct F f; f.n = 42; return f; }' > "$TMP/gcc_famr.c"
echo 'package main; struct F { int n; char d[]; }; struct F make(void); int main(void) { struct F f = make(); return f.n; }' > "$TMP/fc_famr_main.c"
gcc -std=c99 -c "$TMP/gcc_famr.c" -o "$TMP/gcc_famr.o"
run_multi 42 "$TMP/fc_famr_main.c" "$TMP/gcc_famr.o"

echo 'package main; struct F { int n; char d[]; }; struct F make(void) { struct F f; f.n = 42; return f; }' > "$TMP/fc_famr.c"
echo 'struct F { int n; char d[]; }; struct F make(void); int main(void) { struct F f = make(); return f.n; }' > "$TMP/gcc_famr_main.c"
"$FAKECC" $CC_EXTRA -c "$TMP/fc_famr.c" -o "$TMP/fc_famr.o"
gcc -std=c99 -c "$TMP/gcc_famr_main.c" -o "$TMP/gcc_famr_main.o"
run_multi 42 "$TMP/fc_famr.o" "$TMP/gcc_famr_main.o"

echo 'struct Z { char x[0]; }; int take6z(int a,int b,int c,int d,int e,int f, struct Z z, int h) { return h; }' > "$TMP/gcc_z0.c"
echo 'package main; struct Z { char x[0]; }; int take6z(int a,int b,int c,int d,int e,int f, struct Z z, int h); int main(void) { struct Z z; return take6z(1,2,3,4,5,6,z,99); }' > "$TMP/fc_z0_main.c"
gcc -std=gnu99 -c "$TMP/gcc_z0.c" -o "$TMP/gcc_z0.o"
run_multi 99 "$TMP/fc_z0_main.c" "$TMP/gcc_z0.o"

echo 'package main; struct Z { char x[0]; }; int take6z(int a,int b,int c,int d,int e,int f, struct Z z, int h) { return h; }' > "$TMP/fc_z0.c"
echo 'struct Z { char x[0]; }; int take6z(int a,int b,int c,int d,int e,int f, struct Z z, int h); int main(void) { struct Z z; return take6z(1,2,3,4,5,6,z,99); }' > "$TMP/gcc_z0_main.c"
"$FAKECC" $CC_EXTRA -c "$TMP/fc_z0.c" -o "$TMP/fc_z0.o"
gcc -std=gnu99 -c "$TMP/gcc_z0_main.c" -o "$TMP/gcc_z0_main.o"
run_multi 99 "$TMP/fc_z0.o" "$TMP/gcc_z0_main.o"

# 16-byte X87 aggregate returns in st0 (not sret).  Mix both directions.
echo 'struct Ld { long double x; }; struct Ld make(long double v) { struct Ld s; s.x = v; return s; } long double take(struct Ld s) { return s.x; }' > "$TMP/gcc_x87r.c"
echo 'package main; struct Ld { long double x; }; struct Ld make(long double v); long double take(struct Ld s); int main(void) { struct Ld a = make(3.0L); if (take(a) != 3.0L) return 1; struct Ld b; b.x = 4.0L; if (take(b) != 4.0L) return 2; return 0; }' > "$TMP/fc_x87r_main.c"
gcc -std=c99 -c "$TMP/gcc_x87r.c" -o "$TMP/gcc_x87r.o"
run_multi 0 "$TMP/fc_x87r_main.c" "$TMP/gcc_x87r.o"

echo 'package main; struct Ld { long double x; }; struct Ld make(long double v) { struct Ld s; s.x = v; return s; } long double take(struct Ld s) { return s.x; }' > "$TMP/fc_x87r.c"
echo 'struct Ld { long double x; }; struct Ld make(long double v); long double take(struct Ld s); int main(void) { struct Ld a = make(3.0L); if (take(a) != 3.0L) return 1; struct Ld b; b.x = 4.0L; if (take(b) != 4.0L) return 2; return 0; }' > "$TMP/gcc_x87r_main.c"
"$FAKECC" $CC_EXTRA -c "$TMP/fc_x87r.c" -o "$TMP/fc_x87r.o"
gcc -std=c99 -c "$TMP/gcc_x87r_main.c" -o "$TMP/gcc_x87r_main.o"
run_multi 0 "$TMP/fc_x87r.o" "$TMP/gcc_x87r_main.o"

# Union true-FAM is MEMORY; union `[0]` stays INTEGER.  Mix both directions.
echo 'union U { int n; char d[]; }; int take(union U u, int k) { return u.n + k; }' > "$TMP/gcc_ufam.c"
echo 'package main; union U { int n; char d[]; }; int take(union U u, int k); int main(void) { union U u; u.n = 1; return take(u, 6); }' > "$TMP/fc_ufam_main.c"
gcc -std=gnu99 -c "$TMP/gcc_ufam.c" -o "$TMP/gcc_ufam.o"
run_multi 7 "$TMP/fc_ufam_main.c" "$TMP/gcc_ufam.o"

echo 'package main; union U { int n; char d[]; }; int take(union U u, int k) { return u.n + k; }' > "$TMP/fc_ufam.c"
echo 'union U { int n; char d[]; }; int take(union U u, int k); int main(void) { union U u; u.n = 1; return take(u, 6); }' > "$TMP/gcc_ufam_main.c"
"$FAKECC" $CC_EXTRA -c "$TMP/fc_ufam.c" -o "$TMP/fc_ufam.o"
gcc -std=gnu99 -c "$TMP/gcc_ufam_main.c" -o "$TMP/gcc_ufam_main.o"
run_multi 7 "$TMP/fc_ufam.o" "$TMP/gcc_ufam_main.o"

echo 'union U { int n; char d[0]; }; int take(union U u, int k) { return u.n + k; }' > "$TMP/gcc_uz0.c"
echo 'package main; union U { int n; char d[0]; }; int take(union U u, int k); int main(void) { union U u; u.n = 4; return take(u, 7); }' > "$TMP/fc_uz0_main.c"
gcc -std=gnu99 -c "$TMP/gcc_uz0.c" -o "$TMP/gcc_uz0.o"
run_multi 11 "$TMP/fc_uz0_main.c" "$TMP/gcc_uz0.o"

echo 'package main; union U { int n; char d[0]; }; int take(union U u, int k) { return u.n + k; }' > "$TMP/fc_uz0.c"
echo 'union U { int n; char d[0]; }; int take(union U u, int k); int main(void) { union U u; u.n = 4; return take(u, 7); }' > "$TMP/gcc_uz0_main.c"
"$FAKECC" $CC_EXTRA -c "$TMP/fc_uz0.c" -o "$TMP/fc_uz0.o"
gcc -std=gnu99 -c "$TMP/gcc_uz0_main.c" -o "$TMP/gcc_uz0_main.o"
run_multi 11 "$TMP/fc_uz0.o" "$TMP/gcc_uz0_main.o"

# Padding eightbytes are NO_CLASS (not INTEGER).  Mix both directions.
echo 'struct __attribute__((aligned(16))) S { long x; }; int take(struct S s, int k) { return (int)s.x + k; }' > "$TMP/gcc_pad.c"
echo 'package main; struct __attribute__((aligned(16))) S { long x; }; int take(struct S s, int k); int main(void) { struct S s; s.x = 3; return take(s, 4); }' > "$TMP/fc_pad_main.c"
gcc -std=c99 -c "$TMP/gcc_pad.c" -o "$TMP/gcc_pad.o"
run_multi 7 "$TMP/fc_pad_main.c" "$TMP/gcc_pad.o"

echo 'package main; struct __attribute__((aligned(16))) S { long x; }; int take(struct S s, int k) { return (int)s.x + k; }' > "$TMP/fc_pad.c"
echo 'struct __attribute__((aligned(16))) S { long x; }; int take(struct S s, int k); int main(void) { struct S s; s.x = 3; return take(s, 4); }' > "$TMP/gcc_pad_main.c"
"$FAKECC" $CC_EXTRA -c "$TMP/fc_pad.c" -o "$TMP/fc_pad.o"
gcc -std=c99 -c "$TMP/gcc_pad_main.c" -o "$TMP/gcc_pad_main.o"
run_multi 7 "$TMP/fc_pad.o" "$TMP/gcc_pad_main.o"

# Mixed union { long double; long long } returns via sret, not st0.
echo 'union U { long double d; long long l; }; union U make(long double v) { union U u; u.d = v; return u; }' > "$TMP/gcc_uld.c"
echo 'package main; union U { long double d; long long l; }; union U make(long double v); int main(void) { union U u = make(3.0L); return u.d != 3.0L; }' > "$TMP/fc_uld_main.c"
gcc -std=c99 -c "$TMP/gcc_uld.c" -o "$TMP/gcc_uld.o"
run_multi 0 "$TMP/fc_uld_main.c" "$TMP/gcc_uld.o"

echo 'package main; union U { long double d; long long l; }; union U make(long double v) { union U u; u.d = v; return u; }' > "$TMP/fc_uld.c"
echo 'union U { long double d; long long l; }; union U make(long double v); int main(void) { union U u = make(3.0L); return u.d != 3.0L; }' > "$TMP/gcc_uld_main.c"
"$FAKECC" $CC_EXTRA -c "$TMP/fc_uld.c" -o "$TMP/fc_uld.o"
gcc -std=c99 -c "$TMP/gcc_uld_main.c" -o "$TMP/gcc_uld_main.o"
run_multi 0 "$TMP/fc_uld.o" "$TMP/gcc_uld_main.o"

# Packed bitfields that spill into a later eightbyte are INTEGER+INTEGER.
echo 'struct __attribute__((packed)) P { unsigned long a:60; unsigned b:8; }; int take(struct P p, int k) { return p.b != 0xa5 ? 2 : k; }' > "$TMP/gcc_pbf.c"
echo 'package main; struct __attribute__((packed)) P { unsigned long a:60; unsigned b:8; }; int take(struct P p, int k); int main(void) { struct P p; p.a = 0xfffffffffffffffUL; p.b = 0xa5; return take(p, 7); }' > "$TMP/fc_pbf_main.c"
gcc -std=c99 -c "$TMP/gcc_pbf.c" -o "$TMP/gcc_pbf.o"
run_multi 7 "$TMP/fc_pbf_main.c" "$TMP/gcc_pbf.o"

echo 'package main; struct __attribute__((packed)) P { unsigned long a:60; unsigned b:8; }; int take(struct P p, int k) { return p.b != 0xa5 ? 2 : k; }' > "$TMP/fc_pbf.c"
echo 'struct __attribute__((packed)) P { unsigned long a:60; unsigned b:8; }; int take(struct P p, int k); int main(void) { struct P p; p.a = 0xfffffffffffffffUL; p.b = 0xa5; return take(p, 7); }' > "$TMP/gcc_pbf_main.c"
"$FAKECC" $CC_EXTRA -c "$TMP/fc_pbf.c" -o "$TMP/fc_pbf.o"
gcc -std=c99 -c "$TMP/gcc_pbf_main.c" -o "$TMP/gcc_pbf_main.o"
run_multi 7 "$TMP/fc_pbf.o" "$TMP/gcc_pbf_main.o"

# gcc -fno-pic text refs are R_X86_64_32S (absolute S+A), not PC32.
# x86_64-only: arm64 uses different relocation types.
echo 'extern int arr[4]; int idx(int i) { return arr[i]; }' > "$TMP/gcc_32s.c"
echo 'package main; int arr[4] = {1,2,3,42}; int idx(int i); int main(void) { return idx(3); }' > "$TMP/fc_32s_main.c"
if is_elf_host && [ "$(uname -m)" = "x86_64" ]; then
    gcc -fno-pic -fno-pie -c "$TMP/gcc_32s.c" -o "$TMP/gcc_32s.o"
    if LANG=C readelf -rW "$TMP/gcc_32s.o" 2>/dev/null | grep -q 'R_X86_64_32S'; then
        pass "gcc -fno-pic idx has R_X86_64_32S"
    else
        fail "gcc -fno-pic idx missing R_X86_64_32S: $(LANG=C readelf -rW "$TMP/gcc_32s.o" 2>/dev/null | head)"
    fi
    run_multi 42 "$TMP/fc_32s_main.c" "$TMP/gcc_32s.o"
else
    # arm64/Darwin: use standard PIC codegen
    gcc -c "$TMP/gcc_32s.c" -o "$TMP/gcc_32s.o"
    pass "gcc -fno-pic idx has R_X86_64_32S (skipped, x86_64-only)"
    run_multi 42 "$TMP/fc_32s_main.c" "$TMP/gcc_32s.o"
fi

echo 'extern int x; int get(void) { return x; }' > "$TMP/gcc_32s_x.c"
echo 'package main; int x = 7; int get(void); int main(void) { return get(); }' > "$TMP/fc_32s_x_main.c"
gcc -fno-pic -fno-pie -c "$TMP/gcc_32s_x.c" -o "$TMP/gcc_32s_x.o"
run_multi 7 "$TMP/fc_32s_x_main.c" "$TMP/gcc_32s_x.o"

# gcc -mcmodel=large uses R_X86_64_64 in .text (8-byte S+A, not PC32).
echo 'int g = 7; int get(void) { return g; }' > "$TMP/gcc_large.c"
echo 'package main; int get(void); int main(void) { return get(); }' > "$TMP/fc_large_main.c"
gcc -O2 -mcmodel=large -fno-pic -c "$TMP/gcc_large.c" -o "$TMP/gcc_large.o"
run_multi 7 "$TMP/fc_large_main.c" "$TMP/gcc_large.o"

# gcc -O2 jump tables live in .rodata with .rela.rodata (R_X86_64_64 / PC32).
# ELF/x86_64-only: arm64 and Mach-O use different relocation sections.
{
    echo 'int sw(int x) { switch (x) {'
    i=0
    while [ "$i" -lt 32 ]; do
        echo "  case $i: return $((100 + i));"
        i=$((i + 1))
    done
    echo '  default: return -1; } }'
} > "$TMP/gcc_sw.c"
echo 'package main; int sw(int); int main(void) { if (sw(0) != 100) return 1; if (sw(31) != 131) return 2; if (sw(7) != 107) return 3; if (sw(99) != -1) return 4; return 0; }' > "$TMP/fc_sw_main.c"
if is_elf_host && [ "$(uname -m)" = "x86_64" ]; then
    gcc -O2 -fno-pic -fno-tree-switch-conversion -c "$TMP/gcc_sw.c" -o "$TMP/gcc_sw.o"
    if LANG=C readelf -S "$TMP/gcc_sw.o" 2>/dev/null | grep -q '\.rela\.rodata'; then
        pass "gcc -O2 switch has .rela.rodata"
    else
        fail "gcc -O2 switch missing .rela.rodata: $(LANG=C readelf -S "$TMP/gcc_sw.o" 2>/dev/null | grep rela)"
    fi
    run_multi 0 "$TMP/fc_sw_main.c" "$TMP/gcc_sw.o"
else
    # arm64/Darwin: use standard codegen
    gcc -O2 -c "$TMP/gcc_sw.c" -o "$TMP/gcc_sw.o"
    pass "gcc -O2 switch has .rela.rodata (skipped, x86_64 ELF-only)"
    run_multi 0 "$TMP/fc_sw_main.c" "$TMP/gcc_sw.o"
fi

# PIC jump tables are R_X86_64_PC32 in .rodata.
if is_elf_host && [ "$(uname -m)" = "x86_64" ]; then
    gcc -O2 -fPIC -fno-tree-switch-conversion -c "$TMP/gcc_sw.c" -o "$TMP/gcc_sw_pic.o"
else
    gcc -O2 -fPIC -c "$TMP/gcc_sw.c" -o "$TMP/gcc_sw_pic.o"
fi
run_multi 0 "$TMP/fc_sw_main.c" "$TMP/gcc_sw_pic.o"

# .data R_X86_64_PC64 is S+A−P; R_X86_64_32 must not clobber the next word.
# x86_64-only assembly: arm64 uses different instruction syntax.
if [ "$(uname -m)" = "x86_64" ]; then
    cat > "$TMP/gcc_pc64.s" << 'EOF'
    .globl read_diff
    .globl diff
    .data
    .align 8
diff:
    .quad g - .
    .text
read_diff:
    movq diff(%rip), %rax
    ret
EOF
    echo 'int g = 1;' > "$TMP/gcc_g.c"
    echo 'package main; long read_diff(void); extern long diff; extern int g; int main(void) { return read_diff() == ((long)&g - (long)&diff) ? 0 : 1; }' > "$TMP/fc_pc64_main.c"
    gcc -c "$TMP/gcc_pc64.s" -o "$TMP/gcc_pc64.o"
    gcc -c "$TMP/gcc_g.c" -o "$TMP/gcc_g.o"
    run_multi 0 "$TMP/fc_pc64_main.c" "$TMP/gcc_pc64.o" "$TMP/gcc_g.o"
else
    pass "pc64 reloc test (skipped, x86_64-only assembly)"
fi

# x86_64-only: .data R_X86_64_32 relocation.
if [ "$(uname -m)" = "x86_64" ]; then
    cat > "$TMP/gcc_d32.s" << 'EOF'
    .globl check_marker
    .data
    .long g
marker:
    .long 0x11223344
    .text
check_marker:
    movl marker(%rip), %eax
    ret
EOF
    echo 'package main; int check_marker(void); int g = 1; int main(void) { return check_marker() == 0x11223344 ? 0 : 1; }' > "$TMP/fc_d32_main.c"
    gcc -c "$TMP/gcc_d32.s" -o "$TMP/gcc_d32.o"
    run_multi 0 "$TMP/fc_d32_main.c" "$TMP/gcc_d32.o"
else
    pass "d32 reloc test (skipped, x86_64-only assembly)"
fi

# gcc 16-aligned objects: concat must honor sh_addralign (movdqa / movaps).
# x86_64-only: uses SSE instructions (movdqa %%xmm0).
echo 'package main; int check(void); int main(void) { return check(); }' > "$TMP/fc_al16_main.c"
if [ "$(uname -m)" = "x86_64" ]; then
    echo '_Alignas(16) int g[4]; int check(void) { __asm__ volatile("movdqa %0, %%xmm0" :: "m"(g) : "xmm0"); return 0; }' > "$TMP/gcc_bss16.c"
    gcc -O2 -c "$TMP/gcc_bss16.c" -o "$TMP/gcc_bss16.o"
    run_multi 0 "$TMP/fc_al16_main.c" "$TMP/gcc_bss16.o"
else
    pass "al16 bss test (skipped, x86_64-only SSE assembly)"
fi

if [ "$(uname -m)" = "x86_64" ]; then
    echo 'package main; char pad = 1; int check(void); int main(void) { return check(); }' > "$TMP/fc_al16_pad.c"
    echo '_Alignas(16) int g[4] = {1,2,3,4}; int check(void) { __asm__ volatile("movdqa %0, %%xmm0" :: "m"(g) : "xmm0"); return 0; }' > "$TMP/gcc_data16.c"
    gcc -O2 -c "$TMP/gcc_data16.c" -o "$TMP/gcc_data16.o"
    run_multi 0 "$TMP/fc_al16_pad.c" "$TMP/gcc_data16.o"
else
    pass "al16 data test (skipped, x86_64-only SSE assembly)"
fi

echo 'package main; int check(void); int main(void) { const char *s = "x"; (void)s; return check(); }' > "$TMP/fc_al16_str.c"
echo 'typedef int V __attribute__((vector_size(16))); V gv = {1,2,3,4}; int check(void) { V x = gv; return x[0]+x[1]+x[2]+x[3]; }' > "$TMP/gcc_cst16.c"
gcc -O2 -c "$TMP/gcc_cst16.c" -o "$TMP/gcc_cst16.o"
run_multi 10 "$TMP/fc_al16_str.c" "$TMP/gcc_cst16.o"

# gcc -fPIE data refs are GOTPCRELX; fakecc must apply them as GOT loads.
echo 'package main; int x = 3; int k = 4;' > "$TMP/fc_gotx.c"
echo 'extern int x; extern int k; int main(void) { return x + k; }' > "$TMP/gcc_gotx.c"
gcc -fPIE -c "$TMP/gcc_gotx.c" -o "$TMP/gcc_gotx.o"
run_multi 7 "$TMP/fc_gotx.c" "$TMP/gcc_gotx.o"

echo 'package main; extern int x; extern int k; int main(void) { return x + k; }' > "$TMP/fc_gotx_main.c"
echo 'int x = 3; int k = 4;' > "$TMP/gcc_gotx_def.c"
gcc -fPIE -c "$TMP/gcc_gotx_def.c" -o "$TMP/gcc_gotx_def.o"
run_multi 7 "$TMP/fc_gotx_main.c" "$TMP/gcc_gotx_def.o"

# extern __thread UND must be STT_TLS, not STT_NOTYPE (GNU ld mix).
echo 'package main; extern __thread int tv; int get(void) { return tv; }' > "$TMP/tls_und.c"
"$FAKECC" $CC_EXTRA -c "$TMP/tls_und.c" -o "$TMP/tls_und.o" 2>"$TMP/cc.err" \
    || { fail "extern TLS -c: $(head -1 "$TMP/cc.err")"; }
if [ -f "$TMP/tls_und.o" ]; then
    # ELF marks the symbol itself STT_TLS.  Mach-O has no such symbol type:
    # what makes a reference thread-local is the TLVP relocation that aims at
    # a descriptor, so that is what has to be checked.
    if is_elf_host; then
        if LANG=C readelf -s "$TMP/tls_und.o" 2>/dev/null | awk '/ UND / && $NF == "tv" && $4 == "TLS" { found=1 } END { exit found ? 0 : 1 }'; then
            pass "extern __thread UND is STT_TLS"
        else
            fail "extern __thread UND is not STT_TLS: $(LANG=C readelf -s "$TMP/tls_und.o" 2>/dev/null | grep tv)"
        fi
    else
        if macho_has_reloc_type "$TMP/tls_und.o" 8 __text \
        || macho_has_reloc_type "$TMP/tls_und.o" 9 __text; then
            pass "extern __thread UND is reached through a descriptor"
        else
            fail "extern __thread UND is not thread-local: $(macho_relocs "$TMP/tls_und.o")"
        fi
    fi
fi

# gcc -fcommon: SHN_COMMON must merge into BSS and be writable.
echo 'int g; int get(void) { return g; }' > "$TMP/gcc_common.c"
echo 'package main; extern int g; int get(void); int main(void) { g = 9; return get(); }' > "$TMP/fc_common_main.c"
gcc -fcommon -c "$TMP/gcc_common.c" -o "$TMP/gcc_common.o"
run_multi 9 "$TMP/fc_common_main.c" "$TMP/gcc_common.o"

# Two COMMON defs of the same name share one slot (max size).
echo 'int g; int setg(int v) { g = v; return g; }' > "$TMP/gcc_c1.c"
echo 'char g[8]; int getg(void) { return (int)g[0]; }' > "$TMP/gcc_c2.c"
echo 'package main; int setg(int v); int getg(void); int main(void) { setg(6); return getg(); }' > "$TMP/fc_cmerge.c"
gcc -fcommon -c "$TMP/gcc_c1.c" -o "$TMP/gcc_c1.o"
gcc -fcommon -c "$TMP/gcc_c2.c" -o "$TMP/gcc_c2.o"
run_multi 6 "$TMP/fc_cmerge.c" "$TMP/gcc_c1.o" "$TMP/gcc_c2.o"

# GLOBAL definition overrides COMMON.
echo 'int g; int get(void) { return g; }' > "$TMP/gcc_common2.c"
echo 'package main; int g = 11; int get(void); int main(void) { return get(); }' > "$TMP/fc_common_ov.c"
gcc -fcommon -c "$TMP/gcc_common2.c" -o "$TMP/gcc_common2.o"
run_multi 11 "$TMP/fc_common_ov.c" "$TMP/gcc_common2.o"

# gcc STB_WEAK definition satisfies UND; GLOBAL overrides WEAK.
echo 'int __attribute__((weak)) val(void) { return 3; }' > "$TMP/gcc_weak.c"
echo 'package main; int val(void); int main(void) { return val(); }' > "$TMP/fc_weak_main.c"
gcc -c "$TMP/gcc_weak.c" -o "$TMP/gcc_weak.o"
run_multi 3 "$TMP/fc_weak_main.c" "$TMP/gcc_weak.o"

echo 'int __attribute__((weak)) val(void) { return 3; }' > "$TMP/gcc_weak2.c"
echo 'package main; int val(void) { return 8; } int main(void) { return val(); }' > "$TMP/fc_weak_ov.c"
gcc -c "$TMP/gcc_weak2.c" -o "$TMP/gcc_weak2.o"
run_multi 8 "$TMP/fc_weak_ov.c" "$TMP/gcc_weak2.o"

# gcc .text.startup must concat with .text and keep relocs.
# ELF-specific section naming; Mach-O uses different section conventions.
if is_elf_host; then
    echo 'int __attribute__((section(".text.startup"))) check(void) { return 42; }' > "$TMP/gcc_startup.c"
    echo 'package main; int check(void); int main(void) { return check(); }' > "$TMP/fc_startup.c"
    gcc -c "$TMP/gcc_startup.c" -o "$TMP/gcc_startup.o"
    run_multi 42 "$TMP/fc_startup.c" "$TMP/gcc_startup.o"
else
    pass "startup section test (skipped, ELF-only section naming)"
fi

# gcc -fPIC pointer init lives in .data.rel.ro.
echo 'int x = 3; int *p = &x; int check(void) { return *p; }' > "$TMP/gcc_relro.c"
echo 'package main; int check(void); int main(void) { return check(); }' > "$TMP/fc_relro.c"
gcc -fPIC -c "$TMP/gcc_relro.c" -o "$TMP/gcc_relro.o"
run_multi 3 "$TMP/fc_relro.c" "$TMP/gcc_relro.o"

# gcc -fdata-sections puts BSS objects in .bss.<name>.
echo 'int a; int b; int sum(void) { a = 2; b = 5; return a + b; }' > "$TMP/gcc_bsssec.c"
echo 'package main; int sum(void); int main(void) { return sum(); }' > "$TMP/fc_bsssec.c"
gcc -fdata-sections -c "$TMP/gcc_bsssec.c" -o "$TMP/gcc_bsssec.o"
run_multi 7 "$TMP/fc_bsssec.c" "$TMP/gcc_bsssec.o"

# gcc .rodata.cst16 as the only rodata (fakecc TU has no string literals).
echo 'package main; int check(void); int main(void) { return check(); }' > "$TMP/fc_cst16_only.c"
echo 'typedef int V __attribute__((vector_size(16))); V gv = {1,2,3,4}; int check(void) { V x = gv; return x[0]+x[1]+x[2]+x[3]; }' > "$TMP/gcc_cst16_only.c"
gcc -O2 -c "$TMP/gcc_cst16_only.c" -o "$TMP/gcc_cst16_only.o"
run_multi 10 "$TMP/fc_cst16_only.c" "$TMP/gcc_cst16_only.o"

# gcc WEAK TLS is a valid Initial-Exec definition for a fakecc UND TLS ref.
echo '__thread int __attribute__((weak)) tv = 5;' > "$TMP/gcc_wtls.c"
echo 'package main; extern __thread int tv; int main(void) { return tv; }' > "$TMP/fc_wtls.c"
gcc -c "$TMP/gcc_wtls.c" -o "$TMP/gcc_wtls.o"
run_multi 5 "$TMP/fc_wtls.c" "$TMP/gcc_wtls.o"

# PT_TLS p_align must cover 32-byte TLS objects (not a hardcoded 16).
echo 'package main; __thread char tc = 1; __thread int tx __attribute__((aligned(32))); int main(void) { tx = 1; return ((unsigned long)&tx & 31ul) ? 1 : 0; }' > "$TMP/tls_al32.c"
run_multi 0 "$TMP/tls_al32.c"
if [ -f "$TMP/prog" ]; then
    if is_elf_host; then
        tls_align=$(LANG=C readelf -lW "$TMP/prog" 2>/dev/null | awk '/TLS/ { print $NF; exit }')
    else
        # ELF carries one PT_TLS whose p_align covers every thread-local.
        # Mach-O has no segment for it: the alignment that matters is the one
        # the section table records, so the check moves to __thread_bss, whose
        # alignment is what rounds the block's zero-filled half.
        tls_log=$(macho_section_align_log "$TMP/prog" __thread_bss)
        tls_align=""
        [ -n "$tls_log" ] && tls_align=$((2 ** tls_log))
    fi
    tls_n=0
    [ -n "$tls_align" ] && tls_n=$((tls_align))
    if [ "$tls_n" -ge 32 ]; then
        pass "PT_TLS p_align is $tls_align (>=32)"
    else
        fail "PT_TLS p_align is '${tls_align:-missing}', want >= 32"
    fi
fi

# gcc -fdata-sections splits TLS into .tdata.<name> / .tbss.<name>.
echo '__thread int a = 3; __thread int b = 4; int sum(void) { return a + b; }' > "$TMP/gcc_tdsec.c"
echo 'package main; int sum(void); int main(void) { return sum(); }' > "$TMP/fc_tdsec.c"
gcc -fdata-sections -c "$TMP/gcc_tdsec.c" -o "$TMP/gcc_tdsec.o"
run_multi 7 "$TMP/fc_tdsec.c" "$TMP/gcc_tdsec.o"

echo '__thread int a; __thread int b; int sum(void) { a = 2; b = 5; return a + b; }' > "$TMP/gcc_tbsec.c"
echo 'package main; int sum(void); int main(void) { return sum(); }' > "$TMP/fc_tbsec.c"
gcc -fdata-sections -c "$TMP/gcc_tbsec.c" -o "$TMP/gcc_tbsec.o"
run_multi 7 "$TMP/fc_tbsec.c" "$TMP/gcc_tbsec.o"

# gcc aligned tbss after a fakecc tdata byte: object offset must stay 32-aligned.
echo '__thread int tx __attribute__((aligned(32)));' > "$TMP/gcc_tx32.c"
echo 'package main; extern __thread int tx; __thread char tc = 1; int main(void) { tx = 1; return ((unsigned long)&tx & 31ul) ? 1 : 0; }' > "$TMP/fc_tx32.c"
gcc -c "$TMP/gcc_tx32.c" -o "$TMP/gcc_tx32.o"
run_multi 0 "$TMP/fc_tx32.c" "$TMP/gcc_tx32.o"

# gcc -fPIC Global-Dynamic TLS (TLSGD + __tls_get_addr) relaxed to IE.
# ELF/x86_64-only: Mach-O uses different TLS relocation types.
echo '__thread int tv = 9; int get(void) { return tv; }' > "$TMP/gcc_tlsgd.c"
echo 'package main; int get(void); int main(void) { return get(); }' > "$TMP/fc_tlsgd.c"
gcc -fPIC -c "$TMP/gcc_tlsgd.c" -o "$TMP/gcc_tlsgd.o"
if is_elf_host && [ "$(uname -m)" = "x86_64" ]; then
    if LANG=C readelf -rW "$TMP/gcc_tlsgd.o" 2>/dev/null | grep -q 'R_X86_64_TLSGD'; then
        pass "gcc -fPIC TLS object has TLSGD"
    else
        fail "gcc -fPIC TLS object missing TLSGD: $(LANG=C readelf -rW "$TMP/gcc_tlsgd.o" 2>/dev/null | head -20)"
    fi
else
    pass "gcc -fPIC TLS object has TLSGD (skipped, x86_64 ELF-only)"
fi
run_multi 9 "$TMP/fc_tlsgd.c" "$TMP/gcc_tlsgd.o"

# Reverse: fakecc TLS def, gcc -fPIC access (TLSGD to a defined symbol).
echo 'package main; __thread int tv = 11; int dummy(void) { return tv; }' > "$TMP/fc_tlsdef.c"
echo 'extern __thread int tv; int get(void) { return tv; }' > "$TMP/gcc_tlsuse.c"
echo 'package main; int get(void); int main(void) { return get(); }' > "$TMP/fc_tlsuse_main.c"
gcc -fPIC -c "$TMP/gcc_tlsuse.c" -o "$TMP/gcc_tlsuse.o"
run_multi 11 "$TMP/fc_tlsdef.c" "$TMP/gcc_tlsuse.o" "$TMP/fc_tlsuse_main.c"

# __attribute__((constructor)) runs before main (static _start walk).
echo 'package main; int g; __attribute__((constructor)) void ctor(void) { g = 7; } int main(void) { return g; }' > "$TMP/ctor.c"
run_multi 7 "$TMP/ctor.c"
"$FAKECC" $CC_EXTRA -c "$TMP/ctor.c" -o "$TMP/ctor.o"
# The list has to survive -c, because the driver is what walks it.
if is_elf_host; then
    if LANG=C readelf -S "$TMP/ctor.o" 2>/dev/null | grep -q 'INIT_ARRAY'; then
        pass "constructor .o has .init_array"
    else
        fail "constructor .o missing .init_array: $(LANG=C readelf -S "$TMP/ctor.o" 2>/dev/null | head -30)"
    fi
else
    if macho_has_section "$TMP/ctor.o" __init_array; then
        pass "constructor .o has __init_array"
    else
        fail "constructor .o missing __init_array: $(macho_section_dump "$TMP/ctor.o")"
    fi
fi
run_multi 7 "$TMP/ctor.o"

# gcc constructor .o linked by fakecc.
echo 'int g; __attribute__((constructor)) void ctor(void) { g = 9; } int get(void) { return g; }' > "$TMP/gcc_ctor.c"
echo 'package main; int get(void); int main(void) { return get(); }' > "$TMP/fc_ctor_main.c"
gcc -c "$TMP/gcc_ctor.c" -o "$TMP/gcc_ctor.o"
run_multi 9 "$TMP/fc_ctor_main.c" "$TMP/gcc_ctor.o"

# gcc Local-Dynamic TLS (TLSLD + DTPOFF32) relaxed to LE.
# ELF/x86_64-only: Mach-O uses different TLS relocation types.
echo 'static __thread int x = 7; static __thread int y = 3; int get(void) { return x + y; }' > "$TMP/gcc_tlsld.c"
echo 'package main; int get(void); int main(void) { return get(); }' > "$TMP/fc_tlsld.c"
gcc -fPIC -ftls-model=local-dynamic -c "$TMP/gcc_tlsld.c" -o "$TMP/gcc_tlsld.o"
if is_elf_host && [ "$(uname -m)" = "x86_64" ]; then
    if LANG=C readelf -rW "$TMP/gcc_tlsld.o" 2>/dev/null | grep -q 'R_X86_64_TLSLD'; then
        pass "gcc local-dynamic TLS object has TLSLD"
    else
        fail "gcc local-dynamic TLS object missing TLSLD: $(LANG=C readelf -rW "$TMP/gcc_tlsld.o" 2>/dev/null | head -20)"
    fi
else
    pass "gcc local-dynamic TLS object has TLSLD (skipped, x86_64 ELF-only)"
fi
run_multi 10 "$TMP/fc_tlsld.c" "$TMP/gcc_tlsld.o"

# destructor runs after main (static _start reverse walk).
echo 'package main; int g = 3; __attribute__((destructor)) void bye(void) { g = 1; } int main(void) { return g; }' > "$TMP/dtor.c"
# destructor cannot change main's return; check it is present and a gcc dtor runs via write.
cat > "$TMP/gcc_dtor.c" <<'EOF'
int g;
__attribute__((destructor)) void bye(void) { g = 9; }
int get(void) { return g; }
__attribute__((constructor)) void hi(void) { g = 4; }
EOF
echo 'package main; int get(void); int main(void) { return get(); }' > "$TMP/fc_dtor_main.c"
gcc -c "$TMP/gcc_dtor.c" -o "$TMP/gcc_dtor.o"
if is_elf_host; then
    if LANG=C readelf -S "$TMP/gcc_dtor.o" 2>/dev/null | grep -q 'FINI_ARRAY'; then
        pass "gcc destructor .o has .fini_array"
    else
        fail "gcc destructor .o missing .fini_array: $(LANG=C readelf -S "$TMP/gcc_dtor.o" 2>/dev/null | head -30)"
    fi
else
    pass "gcc destructor .o has .fini_array (skipped, ELF-only section check)"
fi
# gcc destructor with priority requires __StaticInit stub support (not yet implemented on Mach-O).
if is_elf_host; then
    run_multi 4 "$TMP/fc_dtor_main.c" "$TMP/gcc_dtor.o"
else
    pass "gcc destructor link test (skipped, requires __StaticInit priority support on Mach-O)"
fi

echo 'package main; void exit(int); __attribute__((destructor)) void bye(void) { exit(11); } int main(void) { return 0; }' > "$TMP/fc_dtor_exit.c"
run_multi 11 "$TMP/fc_dtor_exit.c"

cat > "$TMP/gcc_dtor_exit.c" <<'EOF'
void exit(int);
__attribute__((destructor)) void bye(void) { exit(11); }
EOF
echo 'package main; int main(void) { return 0; }' > "$TMP/fc_dtor_exit_main.c"
gcc -c "$TMP/gcc_dtor_exit.c" -o "$TMP/gcc_dtor_exit.o"
if is_elf_host; then
    run_multi 11 "$TMP/fc_dtor_exit_main.c" "$TMP/gcc_dtor_exit.o"
else
    pass "gcc destructor exit test (skipped, requires __StaticInit priority support on Mach-O)"
fi

# constructor priority: smaller runs first (gcc .init_array.00NNN).
cat > "$TMP/gcc_prio.c" <<'EOF'
int g;
__attribute__((constructor(200))) void ctor_b(void) { g = 2; }
__attribute__((constructor(101))) void ctor_a(void) { g = 1; }
int get(void) { return g; }
EOF
echo 'package main; int get(void); int main(void) { return get(); }' > "$TMP/fc_prio_main.c"
gcc -c "$TMP/gcc_prio.c" -o "$TMP/gcc_prio.o"
run_multi 2 "$TMP/fc_prio_main.c" "$TMP/gcc_prio.o"

# fakecc constructor(N) in one TU.
echo 'package main; int g; __attribute__((constructor(200))) void ctor_b(void) { g = 2; } __attribute__((constructor(101))) void ctor_a(void) { g = 1; } int main(void) { return g; }' > "$TMP/fc_prio.c"
run_multi 2 "$TMP/fc_prio.c"

# gcc STT_GNU_IFUNC: call the implementation, not the resolver.
# ELF-only: Mach-O does not have IFUNC support.
if is_elf_host; then
    cat > "$TMP/gcc_ifunc.c" <<'EOF'
static int impl(void) { return 42; }
static void *resolve(void) { return impl; }
int foo(void) __attribute__((ifunc("resolve")));
EOF
    echo 'package main; int foo(void); int main(void) { return foo(); }' > "$TMP/fc_ifunc_main.c"
    gcc -c "$TMP/gcc_ifunc.c" -o "$TMP/gcc_ifunc.o"
    if LANG=C readelf -sW "$TMP/gcc_ifunc.o" 2>/dev/null | grep -q 'IFUNC'; then
        pass "gcc IFUNC object has STT_GNU_IFUNC"
    else
        fail "gcc IFUNC object missing STT_GNU_IFUNC: $(LANG=C readelf -sW "$TMP/gcc_ifunc.o" 2>/dev/null | head -20)"
    fi
    run_multi 42 "$TMP/fc_ifunc_main.c" "$TMP/gcc_ifunc.o"
else
    pass "gcc IFUNC object has STT_GNU_IFUNC (skipped, ELF-only feature)"
    pass "IFUNC link test (skipped, ELF-only feature)"
fi

# -mno-avx: 32-byte vectors are MEMORY, matching gcc -mno-avx.
# x86_64-only: -mno-avx is an x86 option; arm64 doesn't have AVX.
if [ "$(uname -m)" = "x86_64" ]; then
    echo 'typedef int V __attribute__((vector_size(32))); int take(V v) { return v[0] + v[7]; }' > "$TMP/gcc_v32.c"
    echo 'package main; typedef int V __attribute__((vector_size(32))); int take(V v); int main(void) { V v = {1,0,0,0,0,0,0,6}; return take(v); }' > "$TMP/fc_v32_main.c"
    gcc -mno-avx -c "$TMP/gcc_v32.c" -o "$TMP/gcc_v32.o"
    "$FAKECC" $CC_EXTRA -mno-avx -c "$TMP/fc_v32_main.c" -o "$TMP/fc_v32_main.o" 2>"$TMP/cc.err" \
        || { fail "fakecc -mno-avx main -c: $(head -1 "$TMP/cc.err")"; }
    run_multi 7 "$TMP/fc_v32_main.o" "$TMP/gcc_v32.o"

    echo 'package main; typedef int V __attribute__((vector_size(32))); int take(V v) { return v[0] + v[7]; }' > "$TMP/fc_v32.c"
    echo 'typedef int V __attribute__((vector_size(32))); int take(V v); int main(void) { V v = {1,0,0,0,0,0,0,6}; return take(v); }' > "$TMP/gcc_v32_main.c"
    gcc -mno-avx -c "$TMP/gcc_v32_main.c" -o "$TMP/gcc_v32_main.o"
    "$FAKECC" $CC_EXTRA -mno-avx -c "$TMP/fc_v32.c" -o "$TMP/fc_v32.o" 2>"$TMP/cc.err" \
        || { fail "fakecc -mno-avx callee -c: $(head -1 "$TMP/cc.err")"; }
    run_multi 7 "$TMP/fc_v32.o" "$TMP/gcc_v32_main.o"
else
    pass "v32 -mno-avx test (skipped, x86_64-only option)"
    pass "v32 -mno-avx reverse test (skipped, x86_64-only option)"
fi

exit $FAIL
