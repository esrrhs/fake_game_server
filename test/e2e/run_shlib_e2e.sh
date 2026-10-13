#!/usr/bin/env bash
# Shared-library link tests: -l / -l: / -nostdlib / -nodefaultlibs / .so path.
# Default link is freestanding (builtin runtime/, no DT_NEEDED).  -lc is opt-in.
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
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; FAIL=1; }

# Everything below is ELF dynamic linking: -soname, DT_NEEDED, DT_RUNPATH,
# -l:, and .so paths.  None of them exist on Mach-O, where the equivalents are
# -install_name, LC_LOAD_DYLIB, LC_RPATH, and .dylib -- and the underlying
# capability is missing too, since -shared is refused for arm64-macos.  Skip
# as a whole rather than failing on the first `gcc -Wl,-soname`, so the
# suite's status stays legible next to the other three.
HOST_OS=$(uname -s | tr 'A-Z' 'a-z')
if [ "$HOST_OS" != "linux" ]; then
    echo "SKIP shared-library suite is ELF-only (host $HOST_OS); Mach-O uses"
    echo "SKIP -install_name / LC_LOAD_DYLIB / LC_RPATH / .dylib instead"
    exit 0
fi

# Locale-independent dynamic-tag helpers (has_needed / has_runpath) come from
# the sourced compat.sh, which prefers readelf and falls back to otool.

# Build a tiny shared library with gcc (oracle object code; fakecc only records
# DT_NEEDED and resolves via the dynamic linker at run time).
cat > "$TMP/add.c" <<'EOF'
int add(int a, int b) { return a + b; }
int mul(int a, int b) { return a * b; }
EOF
gcc -shared -fPIC -Wl,-soname,libadd.so -o "$TMP/libadd.so" "$TMP/add.c" \
    || { echo "FAIL could not build libadd.so with gcc"; exit 1; }

cat > "$TMP/main_add.c" <<'EOF'
package main;
extern int add(int a, int b);
int main(void) { return add(20, 22); }
EOF

cat > "$TMP/main_printf.c" <<'EOF'
package main;
extern int printf(const char *fmt, ...);
int main(void) {
    int n = printf("hi\n");
    if (n != 3) return 1;
    return 0;
}
EOF

cat > "$TMP/main_fmt.c" <<'EOF'
package main;
import runtime;
int main(void) {
    int n = runtime.printf("hi\n");
    if (n != 3) return 1;
    return 0;
}
EOF

cat > "$TMP/main_missing.c" <<'EOF'
package main;
extern int definitely_missing_xyz(void);
int main(void) { return definitely_missing_xyz(); }
EOF

# 1) Opt-in libc via -nostdlib -lc (cgo-style escape hatch).
rm -f "$TMP/p"
timeout "$CC_TIMEOUT" "$FAKECC" $CC_EXTRA "$TMP/main_printf.c" -nostdlib -lc -o "$TMP/p" 2>"$TMP/err" \
    || { fail "nostdlib -lc compile: $(head -1 "$TMP/err")"; }
if [ -x "$TMP/p" ]; then
    got=0; timeout "$RUN_TIMEOUT" "$TMP/p" >/dev/null || got=$?
    if [ "$got" = "0" ]; then pass "nostdlib -lc printf"; else fail "nostdlib -lc run (exit $got)"; fi
    if has_needed "$TMP/p" "libc.so.6"; then
        pass "nostdlib -lc DT_NEEDED libc.so.6"
    else
        fail "nostdlib -lc missing DT_NEEDED libc.so.6"
    fi
fi

# 2) Default freestanding: printf via runtime/, no DT_NEEDED.
rm -f "$TMP/p"
timeout "$CC_TIMEOUT" "$FAKECC" $CC_EXTRA "$TMP/main_fmt.c" -o "$TMP/p" 2>"$TMP/err" \
    || { fail "default rt compile: $(head -1 "$TMP/err")"; }
if [ -x "$TMP/p" ]; then
    got=0; out=$(timeout "$RUN_TIMEOUT" "$TMP/p" 2>/dev/null) || got=$?
    if [ "$got" = "0" ] && [ "$out" = "hi" ]; then
        pass "default rt printf"
    else
        fail "default rt printf (exit $got out='$out')"
    fi
    if has_needed "$TMP/p" "libc.so.6"; then
        fail "default should not DT_NEEDED libc.so.6"
    else
        pass "default omits libc.so.6"
    fi
    if ! LANG=C readelf -l "$TMP/p" 2>/dev/null | grep -q 'INTERP'; then
        pass "default is static ELF"
    else
        fail "default should be static (has INTERP)"
    fi
fi

# 3) Custom shared library via -L + -ladd (DT_RUNPATH; no libc by default).
rm -f "$TMP/p"
timeout "$CC_TIMEOUT" "$FAKECC" $CC_EXTRA "$TMP/main_add.c" -L"$TMP" -ladd -o "$TMP/p" 2>"$TMP/err" \
    || { fail "-L -ladd compile: $(head -1 "$TMP/err")"; }
if [ -x "$TMP/p" ]; then
    if has_needed "$TMP/p" "libadd.so"; then
        pass "-L -ladd DT_NEEDED libadd.so"
    else
        fail "-L -ladd missing DT_NEEDED libadd.so"
    fi
    if has_needed "$TMP/p" "libc.so.6"; then
        fail "-L -ladd should not pull libc.so.6"
    else
        pass "-L -ladd omits libc.so.6"
    fi
    if has_runpath "$TMP/p" "$TMP"; then
        pass "-L -ladd DT_RUNPATH"
    else
        fail "-L -ladd missing DT_RUNPATH $TMP"
    fi
    got=0
    env -u LD_LIBRARY_PATH timeout "$RUN_TIMEOUT" "$TMP/p" >/dev/null || got=$?
    if [ "$got" = "42" ]; then pass "-L -ladd run via RUNPATH"; else fail "-L -ladd run (exit $got)"; fi
fi

# 3b) -L with a missing library must fail at link time.
rm -f "$TMP/p"
if timeout "$CC_TIMEOUT" "$FAKECC" $CC_EXTRA "$TMP/main_add.c" -L"$TMP/does-not-exist" -ladd -o "$TMP/p" 2>"$TMP/err"; then
    fail "-L missing dir should reject -ladd"
else
    pass "-L missing dir rejects -ladd"
fi

# 4) Exact soname form -l:libadd.so with -L, and passing the .so path.
rm -f "$TMP/p"
timeout "$CC_TIMEOUT" "$FAKECC" $CC_EXTRA "$TMP/main_add.c" -L"$TMP" -l:libadd.so -o "$TMP/p" 2>"$TMP/err" \
    || { fail "-l:libadd.so compile"; }
if [ -x "$TMP/p" ]; then
    got=0
    env -u LD_LIBRARY_PATH timeout "$RUN_TIMEOUT" "$TMP/p" >/dev/null || got=$?
    if [ "$got" = "42" ]; then pass "-L -l:libadd.so run"; else fail "-L -l:libadd.so run (exit $got)"; fi
fi

rm -f "$TMP/p"
timeout "$CC_TIMEOUT" "$FAKECC" $CC_EXTRA "$TMP/main_add.c" "$TMP/libadd.so" -o "$TMP/p" 2>"$TMP/err" \
    || { fail ".so path compile"; }
if [ -x "$TMP/p" ]; then
    if has_needed "$TMP/p" "libadd.so"; then
        pass ".so path DT_NEEDED"
    else
        fail ".so path missing DT_NEEDED libadd.so"
    fi
    if has_runpath "$TMP/p" "$TMP"; then
        pass ".so path DT_RUNPATH from dirname"
    else
        fail ".so path missing DT_RUNPATH"
    fi
    got=0
    env -u LD_LIBRARY_PATH timeout "$RUN_TIMEOUT" "$TMP/p" >/dev/null || got=$?
    if [ "$got" = "42" ]; then pass ".so path run via RUNPATH"; else fail ".so path run (exit $got)"; fi
fi

# 5) -nodefaultlibs + -L -ladd: still no libc (alias / compatibility).
rm -f "$TMP/p"
timeout "$CC_TIMEOUT" "$FAKECC" $CC_EXTRA "$TMP/main_add.c" -nodefaultlibs -L"$TMP" -ladd -o "$TMP/p" 2>"$TMP/err" \
    || { fail "-nodefaultlibs -L -ladd compile"; }
if [ -x "$TMP/p" ]; then
    if has_needed "$TMP/p" "libc.so.6"; then
        fail "-nodefaultlibs still has libc.so.6"
    else
        pass "-nodefaultlibs omits libc.so.6"
    fi
    if has_needed "$TMP/p" "libadd.so"; then
        pass "-nodefaultlibs keeps libadd.so"
    else
        fail "-nodefaultlibs -ladd missing libadd.so"
    fi
    got=0
    env -u LD_LIBRARY_PATH timeout "$RUN_TIMEOUT" "$TMP/p" >/dev/null || got=$?
    if [ "$got" = "42" ]; then pass "-nodefaultlibs -L -ladd run"; else fail "-nodefaultlibs -L -ladd run (exit $got)"; fi
fi

# 6) -nostdlib with an unresolved symbol: runtime must fail (no rt, no libc).
rm -f "$TMP/p"
timeout "$CC_TIMEOUT" "$FAKECC" $CC_EXTRA "$TMP/main_missing.c" -nostdlib -o "$TMP/p" 2>"$TMP/err"
cc_rc=$?
if [ "$cc_rc" != "0" ]; then
    pass "-nostdlib missing-sym rejected at link"
elif [ -x "$TMP/p" ]; then
    if has_needed "$TMP/p" "libc.so.6"; then
        fail "-nostdlib missing-sym still linked libc"
    else
        pass "-nostdlib missing-sym omits libc"
    fi
    got=0
    timeout "$RUN_TIMEOUT" "$TMP/p" >/dev/null 2>"$TMP/run.err" || got=$?
    if [ "$got" = "0" ]; then
        fail "-nostdlib missing-sym unexpectedly ran OK"
    else
        pass "-nostdlib missing-sym fails at runtime (exit $got)"
    fi
else
    fail "-nostdlib missing-sym: no binary and no error"
fi

# 7) fakecc can produce a shared library (-shared), and the three interop
#    directions all work:
#      fakecc exe  + fakecc .so
#      gcc    exe  + fakecc .so
#      fakecc exe  + gcc    .so  (already covered above; re-check with -shared)
cat > "$TMP/lib.c" <<'EOF'
package main;
int add(int a, int b) { return a + b; }
int mul(int a, int b) { return a * b; }
EOF
cat > "$TMP/lib_gcc.c" <<'EOF'
int add(int a, int b) { return a + b; }
int mul(int a, int b) { return a * b; }
EOF
cat > "$TMP/use.c" <<'EOF'
package main;
extern int add(int a, int b);
extern int mul(int a, int b);
int main(void) { return add(20, 22) + mul(0, 0); }
EOF
cat > "$TMP/use_gcc.c" <<'EOF'
extern int add(int a, int b);
extern int mul(int a, int b);
int main(void) { return add(20, 22) + mul(0, 0); }
EOF

rm -f "$TMP/libfcc.so" "$TMP/libgcc.so" "$TMP/p"
timeout "$CC_TIMEOUT" "$FAKECC" $CC_EXTRA -shared "$TMP/lib.c" -o "$TMP/libfcc.so" 2>"$TMP/err" \
    || { fail "fakecc -shared compile: $(head -1 "$TMP/err")"; }
if [ -f "$TMP/libfcc.so" ]; then
    if LANG=C readelf -h "$TMP/libfcc.so" 2>/dev/null | grep -q 'DYN'; then
        pass "fakecc -shared produces ET_DYN"
    else
        fail "fakecc -shared not ET_DYN"
    fi
    if has_needed "$TMP/libfcc.so" "libc.so.6"; then
        fail "fakecc -shared should not DT_NEEDED libc by default"
    else
        pass "fakecc -shared omits libc.so.6"
    fi
    if LANG=C readelf -d "$TMP/libfcc.so" 2>/dev/null | grep '(SONAME)' | grep -F '[libfcc.so]' >/dev/null; then
        pass "fakecc -shared DT_SONAME"
    else
        fail "fakecc -shared missing DT_SONAME libfcc.so"
    fi
    if LANG=C readelf -sW "$TMP/libfcc.so" 2>/dev/null | grep -E 'GLOBAL.*\<add\>' >/dev/null \
       && LANG=C readelf -sW "$TMP/libfcc.so" 2>/dev/null | grep -E 'GLOBAL.*\<mul\>' >/dev/null; then
        pass "fakecc -shared exports add/mul"
    else
        fail "fakecc -shared missing exported symbols"
    fi
fi

# 7a) fakecc exe loads fakecc .so
rm -f "$TMP/p"
timeout "$CC_TIMEOUT" "$FAKECC" $CC_EXTRA "$TMP/use.c" -L"$TMP" -lfcc -o "$TMP/p" 2>"$TMP/err" \
    || { fail "fakecc←fakecc.so compile: $(head -1 "$TMP/err")"; }
if [ -x "$TMP/p" ]; then
    got=0
    env -u LD_LIBRARY_PATH timeout "$RUN_TIMEOUT" "$TMP/p" >/dev/null || got=$?
    if [ "$got" = "42" ]; then pass "fakecc exe ← fakecc .so"; else fail "fakecc exe ← fakecc .so (exit $got)"; fi
fi

# 7b) gcc exe loads fakecc .so
rm -f "$TMP/p"
if gcc $CC_EXTRA "$TMP/use_gcc.c" -L"$TMP" -lfcc -Wl,-rpath,"$TMP" -o "$TMP/p" 2>"$TMP/err"; then
    got=0
    timeout "$RUN_TIMEOUT" "$TMP/p" >/dev/null || got=$?
    if [ "$got" = "42" ]; then pass "gcc exe ← fakecc .so"; else fail "gcc exe ← fakecc .so (exit $got)"; fi
else
    fail "gcc←fakecc.so compile: $(head -1 "$TMP/err")"
fi

# 7c) fakecc exe loads gcc .so (explicit -shared counterpart of test 3)
gcc -shared -fPIC -Wl,-soname,libaddg.so -o "$TMP/libaddg.so" "$TMP/lib_gcc.c" \
    || { echo "FAIL could not build libaddg.so with gcc"; exit 1; }
rm -f "$TMP/p"
timeout "$CC_TIMEOUT" "$FAKECC" $CC_EXTRA "$TMP/use.c" -L"$TMP" -laddg -o "$TMP/p" 2>"$TMP/err" \
    || { fail "fakecc←gcc.so compile: $(head -1 "$TMP/err")"; }
if [ -x "$TMP/p" ]; then
    got=0
    env -u LD_LIBRARY_PATH timeout "$RUN_TIMEOUT" "$TMP/p" >/dev/null || got=$?
    if [ "$got" = "42" ]; then pass "fakecc exe ← gcc .so"; else fail "fakecc exe ← gcc .so (exit $got)"; fi
fi

# 8) fakecc .so with a .data pointer initializer needs R_X86_64_RELATIVE
#    so the pointer is valid after ASLR.
cat > "$TMP/libptr.c" <<'EOF'
package main;
int x = 7;
int *p = &x;
int get(void) { return *p; }
EOF
cat > "$TMP/useptr.c" <<'EOF'
package main;
extern int get(void);
int main(void) { return get(); }
EOF
rm -f "$TMP/libptr.so" "$TMP/p"
timeout "$CC_TIMEOUT" "$FAKECC" $CC_EXTRA -shared "$TMP/libptr.c" -o "$TMP/libptr.so" 2>"$TMP/err" \
    || { fail "fakecc -shared data-ptr compile: $(head -1 "$TMP/err")"; }
if [ -f "$TMP/libptr.so" ]; then
    rela=$(LANG=C readelf -r "$TMP/libptr.so" 2>/dev/null | grep -c 'R_X86_64_RELATIVE' || true)
    if [ "$rela" -ge 1 ]; then
        pass "fakecc -shared emits R_X86_64_RELATIVE"
    else
        fail "fakecc -shared missing R_X86_64_RELATIVE (got $rela)"
    fi
    timeout "$CC_TIMEOUT" "$FAKECC" $CC_EXTRA "$TMP/useptr.c" -L"$TMP" -lptr -o "$TMP/p" 2>"$TMP/err" \
        || { fail "fakecc←libptr.so compile: $(head -1 "$TMP/err")"; }
    if [ -x "$TMP/p" ]; then
        got=0
        env -u LD_LIBRARY_PATH timeout "$RUN_TIMEOUT" "$TMP/p" >/dev/null || got=$?
        if [ "$got" = "7" ]; then pass "fakecc .so data pointer RELATIVE"; else fail "fakecc .so data pointer (exit $got)"; fi
    fi
fi

# 9) 16-byte vector SysV class is one XMM (SSE+SSEUP), matching GCC.
cat > "$TMP/vec_gcc.c" <<'EOF'
typedef float V __attribute__((vector_size(16)));
V vid(V v) { return v; }
EOF
cat > "$TMP/vec_use.c" <<'EOF'
package main;
typedef float V __attribute__((vector_size(16)));
extern V vid(V v);
int main(void) {
    V a = { 1.0f, 2.0f, 3.0f, 4.0f };
    V b = vid(a);
    if (b[0] != 1.0f) return 1;
    if (b[1] != 2.0f) return 2;
    if (b[2] != 3.0f) return 3;
    if (b[3] != 4.0f) return 4;
    return 0;
}
EOF
gcc -shared -fPIC -o "$TMP/libvec.so" "$TMP/vec_gcc.c" \
    || { fail "gcc -shared vector lib"; }
rm -f "$TMP/p"
timeout "$CC_TIMEOUT" "$FAKECC" $CC_EXTRA "$TMP/vec_use.c" -L"$TMP" -lvec -o "$TMP/p" 2>"$TMP/err" \
    || { fail "fakecc←gcc vector .so compile: $(head -1 "$TMP/err")"; }
if [ -x "$TMP/p" ]; then
    got=0
    env -u LD_LIBRARY_PATH timeout "$RUN_TIMEOUT" "$TMP/p" >/dev/null || got=$?
    if [ "$got" = "0" ]; then pass "16-byte vector ABI vs gcc .so"; else fail "16-byte vector ABI vs gcc .so (exit $got)"; fi
fi

# 10) A TLS .so without __fakecc_tls_image must not emit a leftover
#     R_X86_64_RELATIVE at r_offset 0 (that reloc would rewrite e_ident).
cat > "$TMP/libtls.c" <<'EOF'
package main;
__thread int tv = 7;
int get(void) { return tv; }
EOF
rm -f "$TMP/libtls.so"
timeout "$CC_TIMEOUT" "$FAKECC" $CC_EXTRA -shared "$TMP/libtls.c" -o "$TMP/libtls.so" 2>"$TMP/err" \
    || { fail "fakecc -shared TLS compile: $(head -1 "$TMP/err")"; }
if [ -f "$TMP/libtls.so" ]; then
    bad=$(LANG=C readelf -rW "$TMP/libtls.so" 2>/dev/null | awk '/R_X86_64_RELATIVE/ && $1 ~ /^(0+|0000000000000000)$/ { print }' || true)
    if [ -z "$bad" ]; then
        pass "TLS .so has no RELATIVE at r_offset 0"
    else
        fail "TLS .so leftover RELATIVE at 0: $bad"
    fi
fi

# 10b) fakecc DSO TLS uses Initial-Exec (GOTTPOFF + TPOFF64), not TPOFF32.
# Two __thread vars plus a pointer initializer, including a -c round-trip
# so .rela.tdata is read back before the shared link.
cat > "$TMP/libtlsie.c" <<'EOF'
package main;
__thread int x = 1;
__thread int y = 2;
__thread const char *msg = "ok";
int getx(void) { return x; }
int gety(void) { return y; }
int *px(void) { return &x; }
int *py(void) { return &y; }
const char *getmsg(void) { return msg; }
void setx(int v) { x = v; }
EOF
cat > "$TMP/tlsie_main.c" <<'EOF'
extern int getx(void);
extern int gety(void);
extern int *px(void);
extern int *py(void);
extern const char *getmsg(void);
extern void setx(int v);
int main(void) {
    if (getx() != 1) return 1;
    if (gety() != 2) return 2;
    if (px() == py()) return 3;
    if (getmsg()[0] != 'o' || getmsg()[1] != 'k') return 4;
    setx(9);
    if (getx() != 9) return 5;
    if (gety() != 2) return 6;
    return 0;
}
EOF
rm -f "$TMP/libtlsie.o" "$TMP/libtlsie.so" "$TMP/tlsie_p"
timeout "$CC_TIMEOUT" "$FAKECC" $CC_EXTRA -c "$TMP/libtlsie.c" -o "$TMP/libtlsie.o" 2>"$TMP/err" \
    || { fail "fakecc -c TLS IE lib: $(head -1 "$TMP/err")"; }
timeout "$CC_TIMEOUT" "$FAKECC" $CC_EXTRA -shared "$TMP/libtlsie.o" -o "$TMP/libtlsie.so" 2>"$TMP/err" \
    || { fail "fakecc -shared TLS IE lib: $(head -1 "$TMP/err")"; }
if [ -f "$TMP/libtlsie.so" ]; then
    if LANG=C readelf -rW "$TMP/libtlsie.so" 2>/dev/null | grep -q 'R_X86_64_TPOFF32'; then
        fail "TLS .so still has TPOFF32 (ld.so reloc 0x17)"
    else
        pass "TLS .so has no TPOFF32"
    fi
    if LANG=C readelf -rW "$TMP/libtlsie.so" 2>/dev/null | grep -q 'R_X86_64_TPOFF64'; then
        pass "TLS .so has TPOFF64"
    else
        fail "TLS .so missing TPOFF64: $(LANG=C readelf -rW "$TMP/libtlsie.so" 2>/dev/null)"
    fi
    gcc -o "$TMP/tlsie_p" "$TMP/tlsie_main.c" -L"$TMP" -ltlsie -Wl,-rpath,"$TMP" 2>"$TMP/err" \
        || { fail "gcc link vs fakecc TLS .so: $(head -1 "$TMP/err")"; }
    if [ -x "$TMP/tlsie_p" ]; then
        got=0
        timeout "$RUN_TIMEOUT" "$TMP/tlsie_p" >/dev/null || got=$?
        if [ "$got" = "0" ]; then pass "fakecc DSO TLS IE runtime"; else fail "fakecc DSO TLS IE runtime (exit $got)"; fi
    fi
    if LANG=C readelf -d "$TMP/libtlsie.so" 2>/dev/null | grep -q 'STATIC_TLS'; then
        pass "TLS IE .so has DF_STATIC_TLS"
    else
        fail "TLS IE .so missing DF_STATIC_TLS: $(LANG=C readelf -d "$TMP/libtlsie.so" 2>/dev/null | head -40)"
    fi
fi

# Undef IE in an executable: TPOFF64 against a DSO STT_TLS symbol.
cat > "$TMP/libtv.c" <<'EOF'
package main;
__thread int tv = 7;
int dummy(void) { return 0; }
EOF
cat > "$TMP/use_tv.c" <<'EOF'
package main;
extern __thread int tv;
int main(void) { return tv; }
EOF
rm -f "$TMP/libtv.so" "$TMP/use_tv_p"
timeout "$CC_TIMEOUT" "$FAKECC" $CC_EXTRA -shared "$TMP/libtv.c" -o "$TMP/libtv.so" 2>"$TMP/err" \
    || { fail "fakecc -shared undef-IE lib: $(head -1 "$TMP/err")"; }
timeout "$CC_TIMEOUT" "$FAKECC" $CC_EXTRA "$TMP/use_tv.c" -L"$TMP" -ltv -o "$TMP/use_tv_p" 2>"$TMP/err" \
    || { fail "undef IE exe compile: $(head -1 "$TMP/err")"; }
if [ -x "$TMP/use_tv_p" ]; then
    if LANG=C readelf -rW "$TMP/use_tv_p" 2>/dev/null | grep -q 'R_X86_64_TPOFF64'; then
        pass "undef IE exe has TPOFF64"
    else
        fail "undef IE exe missing TPOFF64: $(LANG=C readelf -rW "$TMP/use_tv_p" 2>/dev/null)"
    fi
    got=0
    env -u LD_LIBRARY_PATH timeout "$RUN_TIMEOUT" "$TMP/use_tv_p" >/dev/null || got=$?
    if [ "$got" = "7" ]; then pass "undef IE exe runtime"; else fail "undef IE exe runtime (exit $got)"; fi
fi

# R_X86_64_COPY: gcc -fno-pic executable access to a DSO data object.
cat > "$TMP/gcopy.c" <<'EOF'
int g = 42;
EOF
gcc -shared -fPIC -Wl,-soname,libgcopy.so -o "$TMP/libgcopy.so" "$TMP/gcopy.c" \
    || { fail "gcc -shared COPY lib"; }
cat > "$TMP/copy_main.c" <<'EOF'
extern int g;
int main(void) { return g; }
EOF
gcc -fno-pic -fno-pie -c "$TMP/copy_main.c" -o "$TMP/copy_main.o" \
    || { fail "gcc -fno-pic COPY user"; }
rm -f "$TMP/copy_p"
timeout "$CC_TIMEOUT" "$FAKECC" $CC_EXTRA "$TMP/copy_main.o" -L"$TMP" -lgcopy -o "$TMP/copy_p" 2>"$TMP/err" \
    || { fail "COPY link: $(head -1 "$TMP/err")"; }
if [ -x "$TMP/copy_p" ]; then
    if LANG=C readelf -rW "$TMP/copy_p" 2>/dev/null | grep -q 'R_X86_64_COPY'; then
        pass "exe has R_X86_64_COPY"
    else
        fail "exe missing R_X86_64_COPY: $(LANG=C readelf -rW "$TMP/copy_p" 2>/dev/null)"
    fi
    got=0
    env -u LD_LIBRARY_PATH timeout "$RUN_TIMEOUT" "$TMP/copy_p" >/dev/null || got=$?
    if [ "$got" = "42" ]; then pass "COPY runtime"; else fail "COPY runtime (exit $got)"; fi
fi

# COPY slot uses DSO alignment (vmovaps needs 32).
cat > "$TMP/libal.c" <<'EOF'
_Alignas(32) int buf[8] = {1,2,3,4,5,6,7,8};
EOF
gcc -shared -fPIC -Wl,-soname,libal.so -o "$TMP/libal.so" "$TMP/libal.c" \
    || { fail "gcc -shared aligned COPY lib"; }
cat > "$TMP/mal.c" <<'EOF'
typedef int V __attribute__((vector_size(32)));
extern V buf;
V get(void) { return buf; }
int main(void) { V v = get(); return v[0]; }
EOF
gcc -fno-pic -fno-pie -mavx -c "$TMP/mal.c" -o "$TMP/mal.o" \
    || { fail "gcc -fno-pic aligned COPY user"; }
rm -f "$TMP/mal_p"
timeout "$CC_TIMEOUT" "$FAKECC" $CC_EXTRA "$TMP/mal.o" -L"$TMP" -lal -o "$TMP/mal_p" 2>"$TMP/err" \
    || { fail "aligned COPY link: $(head -1 "$TMP/err")"; }
if [ -x "$TMP/mal_p" ]; then
    got=0
    env -u LD_LIBRARY_PATH timeout "$RUN_TIMEOUT" "$TMP/mal_p" >/dev/null || got=$?
    if [ "$got" = "1" ]; then pass "COPY 32-align runtime"; else fail "COPY 32-align runtime (exit $got)"; fi
fi

# Weak undef TLS: STB_WEAK so ld.so resolves a missing symbol to 0.
cat > "$TMP/weak_tls.c" <<'EOF'
extern __thread int __attribute__((weak)) x;
int main(void) { volatile void *p = &x; (void)p; return 0; }
EOF
gcc -fPIC -c "$TMP/weak_tls.c" -o "$TMP/weak_tls.o" \
    || { fail "gcc -fPIC weak TLS -c"; }
rm -f "$TMP/weak_tls_p"
timeout "$CC_TIMEOUT" "$FAKECC" $CC_EXTRA "$TMP/weak_tls.o" -o "$TMP/weak_tls_p" 2>"$TMP/err" \
    || { fail "weak undef TLS link: $(head -1 "$TMP/err")"; }
if [ -x "$TMP/weak_tls_p" ]; then
    if LANG=C readelf -sW "$TMP/weak_tls_p" 2>/dev/null | grep -E 'WEAK' | grep -q 'x'; then
        pass "weak undef TLS is STB_WEAK"
    else
        fail "weak undef TLS not STB_WEAK: $(LANG=C readelf -sW "$TMP/weak_tls_p" 2>/dev/null | grep -E 'TLS|WEAK| x' | head -20)"
    fi
    got=0
    timeout "$RUN_TIMEOUT" "$TMP/weak_tls_p" >/dev/null || got=$?
    if [ "$got" = "0" ]; then pass "weak undef TLS runtime"; else fail "weak undef TLS runtime (exit $got)"; fi
fi

# 11) GNU empty-struct return takes no hidden sret (RDI is the first real arg).
cat > "$TMP/empty_gcc.c" <<'EOF'
#include <stdlib.h>
struct E {};
struct E make(int x) { struct E e; if (x != 42) abort(); return e; }
int peek(struct E z, int y) { (void)z; return y; }
EOF
cat > "$TMP/empty_use.c" <<'EOF'
package main;
struct E {};
extern struct E make(int x);
extern int peek(struct E z, int y);
int main(void) {
    struct E e = make(42);
    if (peek(e, 7) != 7) return 1;
    if (peek(make(42), 9) != 9) return 2;
    return 0;
}
EOF
gcc -shared -fPIC -o "$TMP/libempty.so" "$TMP/empty_gcc.c" \
    || { fail "gcc -shared empty-struct lib"; }
rm -f "$TMP/p"
timeout "$CC_TIMEOUT" "$FAKECC" $CC_EXTRA "$TMP/empty_use.c" -L"$TMP" -lempty -o "$TMP/p" 2>"$TMP/err" \
    || { fail "fakecc←gcc empty-struct .so compile: $(head -1 "$TMP/err")"; }
if [ -x "$TMP/p" ]; then
    got=0
    env -u LD_LIBRARY_PATH timeout "$RUN_TIMEOUT" "$TMP/p" >/dev/null || got=$?
    if [ "$got" = "0" ]; then pass "GNU empty-struct return ABI vs gcc .so"; else fail "GNU empty-struct return ABI vs gcc .so (exit $got)"; fi
fi

# 12) `: 0` bitfield is not a SysV eightbyte — following float is XMM vs gcc.
cat > "$TMP/zbf_gcc.c" <<'EOF'
struct S { int a; int : 0; float f; };
float getf(struct S s) { return s.f; }
EOF
cat > "$TMP/zbf_use.c" <<'EOF'
package main;
struct S { int a; int : 0; float f; };
extern float getf(struct S s);
int main(void) {
    struct S s;
    s.a = 1;
    s.f = 42.0f;
    if (getf(s) != 42.0f) return 1;
    return 0;
}
EOF
gcc -shared -fPIC -o "$TMP/libzbf.so" "$TMP/zbf_gcc.c" \
    || { fail "gcc -shared zero-bitfield lib"; }
rm -f "$TMP/p"
timeout "$CC_TIMEOUT" "$FAKECC" $CC_EXTRA "$TMP/zbf_use.c" -L"$TMP" -lzbf -o "$TMP/p" 2>"$TMP/err" \
    || { fail "fakecc←gcc :0 bitfield .so compile: $(head -1 "$TMP/err")"; }
if [ -x "$TMP/p" ]; then
    got=0
    env -u LD_LIBRARY_PATH timeout "$RUN_TIMEOUT" "$TMP/p" >/dev/null || got=$?
    if [ "$got" = "0" ]; then pass "SysV :0 bitfield ABI vs gcc .so"; else fail "SysV :0 bitfield ABI vs gcc .so (exit $got)"; fi
fi

exit $FAIL
