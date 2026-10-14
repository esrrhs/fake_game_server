#!/usr/bin/env bash
# Differential testing: compile the same program with gcc and with fakecc,
# run both, compare exit codes.
#
#   tools/difftest.sh [-c FAKECC] [-f "FAKECC_FLAGS"] file.c [file.c ...]
#
# gcc is the oracle, so a case never needs a hand-computed expected value.
# That matters more than it sounds: a wrong expectation looks exactly like a
# compiler bug, and chasing one costs more than writing the whole harness.
#
# Input files are in the fakecc dialect (a leading `package main;`); the line
# is stripped before handing the source to gcc.
set -uo pipefail
# Portability helpers (timeout shim for non-GNU hosts).
_COMPAT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/../test" && pwd)
. "$_COMPAT_DIR/compat.sh" || { echo "cannot source compat.sh from $_COMPAT_DIR" >&2; exit 2; }

FAKECC=${FAKECC:-./build/fakecc}
RUN_TIMEOUT=${RUN_TIMEOUT:-10}
# Flags for fakecc only: the oracle stays unmodified regardless of the
# optimization level we are exercising.
FCC_FLAGS=${FCC_FLAGS:-}
# Oracle compiler: gcc on Linux, clang on macOS (both understand gnu99).
ORACLE=${ORACLE:-gcc}

JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}

while getopts "c:f:o:j:" opt; do
    case "$opt" in
        c) FAKECC=$OPTARG ;;
        f) FCC_FLAGS=$OPTARG ;;
        o) ORACLE=$OPTARG ;;
        j) JOBS=$OPTARG ;;
        *) echo "usage: $0 [-c FAKECC] [-f FLAGS] [-o ORACLE] [-j JOBS] file.c ..." >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ $# -gt 0 ] || { echo "usage: $0 [-c FAKECC] file.c ..." >&2; exit 2; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
TOOLS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

export FAKECC RUN_TIMEOUT FCC_FLAGS WORK TOOLS_DIR ORACLE
# The worker subshell only sees exported functions; re-export the timeout
# shim from compat.sh so it is available inside difftest_one.
export -f timeout nproc 2>/dev/null || true

difftest_one() {
    local src="$1"
    local base
    base=$(basename "$src" .c)
    local hash
    hash=$(printf '%s' "$src" | md5sum | cut -c1-8)
    local name="${base}_${hash}"

    local gcc_extra
    local fcc_extra
    local file_flags
    local gcc_only_flags
    local extra_flags
    gcc_extra=$(sed -n 's|^//[[:space:]]*link:[[:space:]]*\(.*\)|\1|p; s|^//[[:space:]]*libs:[[:space:]]*\(.*\)|\1|p' "$src" | head -1)
    file_flags=$(sed -n 's|^//[[:space:]]*flags:[[:space:]]*\(.*\)|\1|p' "$src" | head -1)
    gcc_only_flags=$(sed -n 's|^//[[:space:]]*gcc_flags:[[:space:]]*\(.*\)|\1|p' "$src" | head -1)
    # Sanitizer flags are fakecc-only: gcc's ASan runtime is a different
    # implementation and often exits 1 ("runtime does not come first").
    fcc_extra="$file_flags $gcc_extra"
    extra_flags="$gcc_extra $gcc_only_flags"
    for tok in $file_flags; do
        case "$tok" in
            -fsanitize=*|-fno-sanitize=*) ;;
            *) extra_flags="$extra_flags $tok" ;;
        esac
    done

    sed -E \
        -e 's/^package[[:space:]]+[A-Za-z_][A-Za-z0-9_]*;//' \
        -e 's/^import[[:space:]]+[A-Za-z_][A-Za-z0-9_]*;//' \
        -e 's/\b(runtime|fmt|io|ctype)\.//g' \
        "$src" > "$WORK/$name.body.c"

    # Prefer a header-free translation: torture ports ship their own libc
    # prototypes (`fprintf(void*, ...)`, `isprint`) which clash with glibc
    # headers and used to fail the gcc translation.  gcc still needs <stdarg.h>
    # for va_arg; drop fakecc's in-source SysV va_list typedef so gcc sees
    # its own va_list (otherwise the stdio.h fallback fights fprintf).
    python3 "$TOOLS_DIR/gcc_stdarg_prep.py" < "$WORK/$name.body.c" > "$WORK/$name.prep.c"
    {
        echo '#define _GNU_SOURCE 1'
        # fakecc lexer folds these to (-MAX-1); gcc does not predefine them
        # because the magnitude is not a valid signed literal of that rank.
        echo '#ifndef __INT_MIN__'
        echo '#define __INT_MIN__ (-2147483647 - 1)'
        echo '#endif'
        echo '#ifndef __LONG_MIN__'
        echo '#define __LONG_MIN__ (-9223372036854775807l - 1l)'
        echo '#endif'
        if grep -qE '\b(va_(list|start|arg|end|copy)|__builtin_va_(list|start|arg|end|copy))\b' \
               "$WORK/$name.prep.c"; then
            echo '#include <stdarg.h>'
        fi
        if grep -qE '\bthread_local\b' "$WORK/$name.prep.c"; then
            echo '#define thread_local _Thread_local'
        fi
        if grep -qE '\b(pthread_create|thread_create)\b' "$WORK/$name.prep.c"; then
            echo '#include <pthread.h>'
            echo '#define thread_t pthread_t'
            echo '#define thread_create(t, fn, arg) pthread_create((pthread_t*)(t), NULL, (void*(*)(void*))(fn), (void*)(arg))'
            echo '#define thread_join(t, res) pthread_join((pthread_t)(t), (void**)(res))'
            echo '#define thread_exit(res) pthread_exit((void*)(res))'
            echo '#define thread_self() pthread_self()'
        fi
        # Ports call alloca without <alloca.h>.  gcc warns "implicit
        # declaration", the harness treats that as failure, and the stdio.h
        # fallback then clashes with `fprintf(void*, ...)`.
        if grep -qE '\balloca[[:space:]]*\(' "$WORK/$name.prep.c"; then
            echo '#define alloca __builtin_alloca'
        fi
        cat "$WORK/$name.prep.c"
    } > "$WORK/$name.gcc.c"
    # Header-free gcc must not accept implicit libc decls (`int getenv()`).
    # Do not pass -w: we need the diagnostic.  Fall back to glibc headers
    # when gcc fails or only succeeded by assuming implicit functions.
    #
    # GCC 16 promotes several historically soft diagnostics to errors
    # (-Wint-conversion, -Wimplicit-int).  Torture ports intentionally use
    # those constructs; keep them as warnings so the header-free path still
    # works, while implicit-function-declaration remains detectable.
    # Do not pass -Wno-*-declaration-missing-parameter-type: that option
    # exists only on GCC 14+, and unknown -Wno-error=* is a hard error on
    # Ubuntu 24.04's GCC 13.  K&R forwards are rewritten in gcc_stdarg_prep.py.
    if ! $ORACLE -std=gnu99 -D_GNU_SOURCE \
            -Wno-error=int-conversion -Wno-error=implicit-int \
            -Wno-error=incompatible-pointer-types \
            $extra_flags -o "$WORK/$name.gcc" "$WORK/$name.gcc.c" -lm 2>"$WORK/$name.gcc.err" \
       || grep -qE 'implicit declaration|隐式声明' "$WORK/$name.gcc.err"; then
        {
            echo '#define _GNU_SOURCE 1'
            echo '#ifndef __INT_MIN__'
            echo '#define __INT_MIN__ (-2147483647 - 1)'
            echo '#endif'
            echo '#ifndef __LONG_MIN__'
            echo '#define __LONG_MIN__ (-9223372036854775807l - 1l)'
            echo '#endif'
            echo '#include <stdio.h>'
            echo '#include <stdlib.h>'
            echo '#include <string.h>'
            echo '#include <ctype.h>'
            echo '#include <stdarg.h>'
            echo '#include <stdint.h>'
            echo '#include <unistd.h>'
            echo '#include <sys/stat.h>'
            echo '#include <errno.h>'
            # runtime.* math (sqrt/fabs/copysignl/sin/...) becomes a bare
            # libc call after the prefix strip.  GCC 16 treats implicit
            # decls as errors even with -w, so libm headers belong here.
            echo '#include <math.h>'
            echo '#define __syscall syscall'
            # Drop port libc prototypes that clash with glibc (GCC 16+).
            # fprintf(void*,...) vs FILE* is the common one; strip the whole
            # extern-decl line for names the headers above already provide.
            python3 - "$WORK/$name.body.c" <<'PY'
import re, sys
src = open(sys.argv[1], encoding="latin-1").read()
libc = (
    "abort|abs|atoi|atol|atof|calloc|ceil|ceill|ceilf|exit|"
    "copysign|copysignf|copysignl|fabs|fabsf|fabsl|floor|floorf|floorl|"
    "fprintf|free|"
    "isalnum|isalpha|isdigit|islower|isprint|isspace|isupper|isxdigit|"
    "labs|malloc|memchr|memcmp|memcpy|memmove|memset|pow|printf|putchar|"
    "puts|realloc|sin|sinf|sinl|cos|cosf|cosl|tan|tanf|tanl|"
    "snprintf|sprintf|sqrt|sqrtf|sqrtl|strcat|strchr|strcmp|strcpy|"
    "strlen|strncat|strncmp|strncpy|strrchr|strstr|"
    "strtod|strtof|strtold|"
    "rand|srand|stdin|stdout|stderr|open|close|read|write|unlink|tmpnam"
)
src = re.sub(
    rf"^extern\s+[^;\n]*\b(?:{libc})\s*(?:\([^;\n]*\))?;\s*\n?",
    "",
    src,
    flags=re.M,
)
src = re.sub(r"typedef\s+[^;]*\bfpos_t\s*;", "", src)
src = re.sub(r"typedef\s+struct\s+_?IO_FILE\s+FILE\s*;", "", src)
src = re.sub(r"typedef\s+struct\s+FILE\s+FILE\s*;", "", src)
sys.stdout.buffer.write(src.encode("latin-1"))
PY
        } > "$WORK/$name.gcc.c"
        if ! $ORACLE -std=gnu99 -D_GNU_SOURCE -w \
                -Wno-error=int-conversion -Wno-error=implicit-int \
                -Wno-error=incompatible-pointer-types \
                $extra_flags -o "$WORK/$name.gcc" "$WORK/$name.gcc.c" -lm 2>"$WORK/$name.gcc.err"; then
            printf '%-28s FAIL (%s rejected: %s)\n' "$base" "$ORACLE" "$(head -1 "$WORK/$name.gcc.err")"
            return 1
        fi
    fi
    local gcc_rc=0
    timeout "$RUN_TIMEOUT" "$WORK/$name.gcc" >"$WORK/$name.gcc.out" 2>"$WORK/$name.gcc.stderr" || gcc_rc=$?

    if ! "$FAKECC" $FCC_FLAGS $fcc_extra "$src" -o "$WORK/$name.fcc" 2>"$WORK/$name.fcc.err"; then
        local rc=$?
        if [ "$rc" -ge 128 ]; then
            printf '%-28s DIFF (fakecc killed by signal %s; gcc exits %s)\n' \
                   "$base" "$((rc - 128))" "$gcc_rc"
        else
            printf '%-28s DIFF (fakecc rejected: %s)\n' \
                   "$base" "$(head -1 "$WORK/$name.fcc.err")"
        fi
        return 1
    fi
    local fcc_rc=0
    timeout "$RUN_TIMEOUT" "$WORK/$name.fcc" >"$WORK/$name.fcc.out" 2>"$WORK/$name.fcc.stderr" || fcc_rc=$?

    if [ "$fcc_rc" = "124" ] && [ "$gcc_rc" != "124" ]; then
        printf '%-28s DIFF (fakecc build did not terminate; gcc exits %s)\n' "$base" "$gcc_rc"
        return 1
    elif [ "$gcc_rc" != "$fcc_rc" ]; then
        printf '%-28s DIFF (gcc exits %s, fakecc exits %s)\n' "$base" "$gcc_rc" "$fcc_rc"
        return 1
    elif ! cmp -s "$WORK/$name.gcc.out" "$WORK/$name.fcc.out"; then
        printf '%-28s DIFF (same exit %s, different stdout)\n' "$base" "$gcc_rc"
        return 1
    else
        printf '%-28s OK (exit %s)\n' "$base" "$gcc_rc"
        return 0
    fi
}
export -f difftest_one

printf "%s\n" "$@" | xargs -P "$JOBS" -n 1 bash -c 'difftest_one "$@"' _ > "$WORK/results.log"
cat "$WORK/results.log" | sort

if grep -qE ' (FAIL|DIFF) ' "$WORK/results.log"; then
    exit 1
fi
exit 0
